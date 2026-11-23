// scip-cli: feed frames from a source through a pipeline into zero or more outputs,
// and report where the time goes.
//
//   scip-cli -i pattern:1920x1080 -p gray,blur:5 -n 200
//   scip-cli -i video.mp4 -p gray,blur:5 -o display --realtime
//   scip-cli -i video.mp4 -p gray -o out.mp4 -o udp:192.168.50.1:5000

#include "scip/build_info.hpp"
#include "scip/io.hpp"
#include "scip/pipeline.hpp"
#include "scip/stages.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stopRequested{false};

extern "C" void onSignal(int /*signal*/)
{
    g_stopRequested.store(true);
}

struct Options {
    std::string input = "pattern:1280x720";
    std::string pipeline = "gray";
    std::vector<std::string> outputs;
    std::optional<std::uint64_t> frames;  // unset: 100 for still images, all for videos
    std::uint64_t warmup = 5;
    bool realtime = false;
    bool grayInput = false;
};

void printUsage()
{
    std::printf("Usage: scip-cli [options]\n"
                "\n"
                "Input (-i):\n"
                "  pattern:WxH[:gray]        generated test pattern (default pattern:1280x720)\n"
                "  photo.png | .jpg ...      still image, repeated\n"
                "  video.mp4 | .mkv ...      video file (hardware decoder when available)\n"
                "  camera:/dev/video0[:WxH]  V4L2 camera\n"
                "  gst:'... ! appsink name=scip_sink'   any GStreamer pipeline\n"
                "\n"
                "Outputs (-o, repeatable):\n"
                "  display[:CONNECTOR]       window on the PC, the screen (KMS) on the board\n"
                "                            (board: 39 = LVDS panel, 40 = HDMI)\n"
                "  udp:HOST:PORT             H.264 RTP stream (watch the board on the PC)\n"
                "  out.mp4 | out.mkv         record H.264 video\n"
                "  out.png                   last frame as an image\n"
                "  gst:'appsrc name=scip_src ! ...'     any GStreamer pipeline\n"
                "\n"
                "Options:\n"
                "  -p, --pipeline SPEC   comma-separated stages, or 'none'  (default gray)\n"
                "  -n, --frames N        measured frames; 0 = until the input ends / Ctrl+C\n"
                "                        (default: 100 for still images, 0 for videos)\n"
                "  -w, --warmup N        untimed frames before measuring    (default 5)\n"
                "      --realtime        play video files at their own frame rate\n"
                "      --gray-input      decode video/camera straight to gray8\n"
                "      --list-stages     show available stages\n"
                "      --version         show build information\n"
                "  -h, --help\n");
}

void printVersion()
{
    std::printf("scip %s (%s) %s %s, %s%s\n",
                scip::build::kVersion,
                scip::build::kGitHash,
                scip::build::kTargetCpu,
                scip::build::kBuildType,
                scip::build::kCompiler,
                scip::io::hasGStreamer() ? ", GStreamer" : "");
}

std::uint64_t parseCount(const std::string& text, const char* option)
{
    try {
        std::size_t used = 0;
        const unsigned long long value = std::stoull(text, &used);
        if (used == text.size()) {
            return value;
        }
    } catch (const std::exception&) {}
    throw std::invalid_argument(std::string(option) + " expects a non-negative integer");
}

/// Returns false if the program should exit successfully right away (--help...).
bool parseArgs(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string(arg) + " needs a value");
            }
            return argv[++i];
        };

        if (arg == "-i" || arg == "--input") {
            options.input = value();
        } else if (arg == "-p" || arg == "--pipeline") {
            options.pipeline = value();
        } else if (arg == "-o" || arg == "--output") {
            options.outputs.push_back(value());
        } else if (arg == "-n" || arg == "--frames") {
            options.frames = parseCount(value(), "--frames");
        } else if (arg == "-w" || arg == "--warmup") {
            options.warmup = parseCount(value(), "--warmup");
        } else if (arg == "--realtime") {
            options.realtime = true;
        } else if (arg == "--gray-input") {
            options.grayInput = true;
        } else if (arg == "--list-stages") {
            std::printf("%s", scip::stages::help().c_str());
            return false;
        } else if (arg == "--version") {
            printVersion();
            return false;
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return false;
        } else {
            throw std::invalid_argument("unknown option '" + std::string(arg) + "' (see --help)");
        }
    }

    const bool still = scip::io::isStillSource(options.input);
    if (!options.frames) {
        options.frames = still ? 100 : 0;
    }
    if (still && *options.frames == 0) {
        throw std::invalid_argument("a still image never ends: give --frames N (N > 0)");
    }
    return true;
}

std::string describe(const scip::FrameInfo& info)
{
    return std::to_string(info.width) + "x" + std::to_string(info.height) + " " +
           std::string(scip::toString(info.format));
}

void printRow(const std::string& name, const scip::LatencyStats& s)
{
    std::printf("  %-24s %9.3f %9.3f %9.3f %9.3f %9.3f\n",
                name.c_str(),
                s.meanMs(),
                s.minMs(),
                s.percentileMs(50),
                s.percentileMs(95),
                s.maxMs());
}

struct RunStats {
    scip::LatencyStats read;   // waiting for / decoding the input
    scip::LatencyStats write;  // all outputs together
    Clock::time_point start = Clock::now();
    std::uint64_t frames = 0;

    void reset()
    {
        read.reset();
        write.reset();
        start = Clock::now();
        frames = 0;
    }
};

void printReport(const scip::Pipeline& pipeline, const RunStats& run, bool haveOutputs)
{
    std::printf(
        "\n  %-24s %9s %9s %9s %9s %9s\n", "[ms per frame]", "mean", "min", "p50", "p95", "max");
    printRow("source (read)", run.read);
    for (std::size_t i = 0; i < pipeline.size(); ++i) {
        printRow("  " + pipeline.stage(i).name(), pipeline.stageStats(i));
    }
    printRow("pipeline total", pipeline.totalStats());
    if (haveOutputs) {
        printRow("outputs (write)", run.write);
    }

    const double processMs = pipeline.totalStats().meanMs();
    if (processMs > 0.0) {
        std::printf("\n  pipeline capacity : %7.1f fps (processing only, one thread)\n",
                    1000.0 / processMs);
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - run.start).count();
    if (run.frames > 0 && seconds > 0.0) {
        std::printf("  end to end        : %7.1f fps (%llu frames in %.2f s, incl. input/output)\n",
                    static_cast<double>(run.frames) / seconds,
                    static_cast<unsigned long long>(run.frames),
                    seconds);
    }
}

int run(const Options& options)
{
    printVersion();

    scip::io::SourceOptions sourceOptions;
    sourceOptions.maxFrames = *options.frames == 0 ? 0 : options.warmup + *options.frames;
    sourceOptions.realtime = options.realtime;
    sourceOptions.gray = options.grayInput;
    scip::FrameSourcePtr source = scip::io::openSource(options.input, sourceOptions);

    scip::Pipeline pipeline;
    if (!options.pipeline.empty() && options.pipeline != "none") {
        for (scip::StagePtr& stage : scip::stages::createChain(options.pipeline)) {
            pipeline.add(std::move(stage));
        }
    }
    pipeline.configure(source->info());

    std::vector<scip::FrameSinkPtr> sinks;
    for (const std::string& uri : options.outputs) {
        sinks.push_back(scip::io::openSink(uri, pipeline.outputInfo(), source->frameRate()));
    }

    std::printf("input   : %s (%s", options.input.c_str(), describe(source->info()).c_str());
    if (source->frameRate() > 0.0) {
        std::printf(", %.2f fps", source->frameRate());
    }
    std::printf(")\noutput  : %s\n", describe(pipeline.outputInfo()).c_str());
    for (const auto& sink : sinks) {
        std::printf("  -> %s\n", sink->name().c_str());
    }
    for (const std::string& uri : options.outputs) {
        const std::string hint = scip::io::viewerHint(uri);
        if (!hint.empty()) {
            std::printf("  watch it on the receiving PC with:\n    %s\n", hint.c_str());
        }
    }
    if (*options.frames == 0) {
        std::printf("frames  : until the input ends (Ctrl+C to stop)\n");
    } else {
        std::printf("frames  : %llu (+%llu warm-up)\n",
                    static_cast<unsigned long long>(*options.frames),
                    static_cast<unsigned long long>(options.warmup));
    }
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    RunStats stats;
    scip::Frame frame;
    std::uint64_t index = 0;
    while (!g_stopRequested.load()) {
        const auto readStart = Clock::now();
        if (!source->read(frame)) {
            break;
        }
        if (index == options.warmup) {
            pipeline.resetStats();  // measure only once caches and clocks have settled
            stats.reset();
        }
        stats.read.add(Clock::now() - readStart);

        const scip::Frame& result = pipeline.process(frame);

        const auto writeStart = Clock::now();
        for (auto& sink : sinks) {
            sink->write(result);
        }
        stats.write.add(Clock::now() - writeStart);
        ++stats.frames;
        ++index;
    }
    if (g_stopRequested.load()) {
        std::printf("\n  stopped by signal\n");
    }

    printReport(pipeline, stats, !sinks.empty());

    // Shut down producer first, then consumers: stop decoding before the outputs
    // finish. (On the i.MX, closing the KMS display while the G2D converter in the
    // decode pipeline still has work queued leaves the GPU driver waiting forever.)
    frame = scip::Frame{};
    source.reset();
    for (auto& sink : sinks) {
        sink->close();  // finishes files; errors surface here
    }
    for (const std::string& uri : options.outputs) {
        if (uri.rfind("display", 0) != 0 && uri.rfind("udp:", 0) != 0 &&
            uri.rfind("gst:", 0) != 0) {
            std::printf("  wrote %s\n", uri.c_str());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        Options options;
        if (!parseArgs(argc, argv, options)) {
            return 0;
        }
        return run(options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scip-cli: error: %s\n", e.what());
        return 1;
    }
}

// scip-cli: run a pipeline on a source and report per-stage latency.
//
//   scip-cli -i pattern:1920x1080 -p gray,blur:5 -n 200
//   scip-cli -i photo.jpg -p resize:640x480,gray -o out.png

#include "scip/build_info.hpp"
#include "scip/io.hpp"
#include "scip/pipeline.hpp"
#include "scip/stages.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Options {
    std::string input = "pattern:1280x720";
    std::string pipeline = "gray";
    std::string output;
    std::uint64_t frames = 100;
    std::uint64_t warmup = 5;
};

void printUsage()
{
    std::printf(
        "Usage: scip-cli [options]\n"
        "  -i, --input URI      image file or pattern:WxH[:gray]   (default pattern:1280x720)\n"
        "  -p, --pipeline SPEC  comma-separated stages             (default gray)\n"
        "  -o, --output PATH    write the last output frame to an image file\n"
        "  -n, --frames N       measured frames                    (default 100)\n"
        "  -w, --warmup N       untimed frames before measuring    (default 5)\n"
        "      --list-stages    show available stages\n"
        "      --version        show build information\n"
        "  -h, --help\n");
}

void printVersion()
{
    std::printf("scip %s (%s) %s %s, %s\n",
                scip::build::kVersion,
                scip::build::kGitHash,
                scip::build::kTargetCpu,
                scip::build::kBuildType,
                scip::build::kCompiler);
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
            options.output = value();
        } else if (arg == "-n" || arg == "--frames") {
            options.frames = parseCount(value(), "--frames");
        } else if (arg == "-w" || arg == "--warmup") {
            options.warmup = parseCount(value(), "--warmup");
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
    if (options.frames == 0) {
        throw std::invalid_argument("--frames must be at least 1");
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
    std::printf("  %-22s %9.3f %9.3f %9.3f %9.3f %9.3f\n",
                name.c_str(),
                s.meanMs(),
                s.minMs(),
                s.percentileMs(50),
                s.percentileMs(95),
                s.maxMs());
}

void printReport(const scip::Pipeline& pipeline)
{
    std::printf(
        "\n  %-22s %9s %9s %9s %9s %9s\n", "stage [ms]", "mean", "min", "p50", "p95", "max");
    for (std::size_t i = 0; i < pipeline.size(); ++i) {
        printRow(pipeline.stage(i).name(), pipeline.stageStats(i));
    }
    const scip::LatencyStats& total = pipeline.totalStats();
    printRow("total", total);

    const double mean = total.meanMs();
    if (mean > 0.0) {
        std::printf("\n  throughput: %.1f fps (pipeline only, single thread)\n", 1000.0 / mean);
    }
}

int run(const Options& options)
{
    printVersion();

    scip::FrameSourcePtr source =
        scip::io::openSource(options.input, options.warmup + options.frames);
    scip::Pipeline pipeline;
    for (scip::StagePtr& stage : scip::stages::createChain(options.pipeline)) {
        pipeline.add(std::move(stage));
    }
    pipeline.configure(source->info());

    std::printf("input   : %s (%s)\n", options.input.c_str(), describe(source->info()).c_str());
    std::printf("output  : %s\n", describe(pipeline.outputInfo()).c_str());
    std::printf("frames  : %llu (+%llu warm-up)\n",
                static_cast<unsigned long long>(options.frames),
                static_cast<unsigned long long>(options.warmup));

    scip::Frame frame;
    const scip::Frame* result = nullptr;
    std::uint64_t index = 0;
    while (source->read(frame)) {
        if (index == options.warmup) {
            pipeline.resetStats();  // measure only after caches and clocks have settled
        }
        result = &pipeline.process(frame);
        ++index;
    }

    printReport(pipeline);

    if (!options.output.empty() && result != nullptr) {
        scip::io::writeImage(options.output, *result);
        std::printf("  wrote %s\n", options.output.c_str());
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

// scipd: the image processing daemon of a target (PC or board).
//
// Runs the engine (input -> functions -> outputs) and lets hosts control it
// over the network with AIPC (docs/aipc-protocol.md). Everything given on the
// command line is only the starting configuration; a host can change all of
// it while the video runs.
//
//   scipd                                        # wait for a host to configure it
//   scipd --input rtp://:5000 --output display   # start showing what a host sends
//   scipd --name board-1 --input camera:/dev/video0 --pipeline 'gray ! edges'

#include "scip/aipc.hpp"
#include "scip/build_info.hpp"
#include "scip/engine.hpp"
#include "scip/gst_stages.hpp"
#include "scip/io.hpp"
#include "scip/registry.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stopRequested{false};

extern "C" void onSignal(int /*signal*/)
{
    g_stopRequested.store(true);
}

struct Options {
    scip::aipc::ServerConfig server;
    scip::InputConfig input;
    std::string pipeline;
    std::vector<std::string> outputs;
    bool quiet = false;
};

void printUsage()
{
    std::printf("Usage: scipd [options]\n"
                "\n"
                "Serves AIPC, the AMC Image Proc Control protocol (docs/aipc-protocol.md).\n"
                "\n"
                "Control:\n"
                "      --bind ADDRESS         listen on this IPv4 address (default 0.0.0.0 = all)\n"
                "      --port N               TCP control port (default %d, 0 = any free port)\n"
                "      --discovery-port N     UDP discovery port (default %d, 0 = off)\n"
                "      --name NAME            shown in host GUIs (default: host name)\n"
                "      --max-clients N        simultaneous control connections (default 8)\n"
                "\n"
                "Starting configuration (a host can change all of it):\n"
                "  -i, --input URI            e.g. rtp://:5000, camera:/dev/video0, clip.mp4\n"
                "  -p, --pipeline TEXT        e.g. 'gray ! blur ksize=5'\n"
                "  -o, --output URI           repeatable: display[:ID], udp:HOST:PORT, out.mp4\n"
                "      --loop                 restart video files at the end\n"
                "      --realtime             play video files at their frame rate\n"
                "      --gray-input           decode straight to gray8\n"
                "      --max-fps N            limit non-live inputs\n"
                "\n"
                "      --list-stages          show available functions and their parameters\n"
                "  -q, --quiet                do not log state changes\n"
                "      --version\n"
                "  -h, --help\n",
                scip::aipc::kDefaultPort,
                scip::aipc::kDefaultDiscoveryPort);
}

void printVersion()
{
    const scip::build::Info& build = scip::build::info();
    std::printf("scipd %s (%s%s%s) %s %s, AIPC %s\n",
                build.version,
                build.describe,
                build.branch[0] != '\0' ? " on " : "",
                build.branch,
                build.targetCpu,
                build.buildType,
                scip::aipc::kProtocolVersion);
}

int parseNumber(std::string_view text, const char* option, int min, int max)
{
    try {
        std::size_t used = 0;
        const int value = std::stoi(std::string(text), &used);
        if (used == text.size() && value >= min && value <= max) {
            return value;
        }
    } catch (const std::exception&) {}
    throw std::invalid_argument(std::string(option) + " expects a number from " +
                                std::to_string(min) + " to " + std::to_string(max));
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

        if (arg == "--bind") {
            options.server.bind = value();
        } else if (arg == "--port") {
            options.server.port = parseNumber(value(), "--port", 0, 65535);
        } else if (arg == "--discovery-port") {
            const int port = parseNumber(value(), "--discovery-port", 0, 65535);
            options.server.discoveryPort = port == 0 ? -1 : port;
        } else if (arg == "--name") {
            options.server.name = value();
        } else if (arg == "--max-clients") {
            options.server.maxClients = parseNumber(value(), "--max-clients", 1, 64);
        } else if (arg == "-i" || arg == "--input") {
            options.input.uri = value();
        } else if (arg == "-p" || arg == "--pipeline") {
            options.pipeline = value();
        } else if (arg == "-o" || arg == "--output") {
            options.outputs.push_back(value());
        } else if (arg == "--loop") {
            options.input.options.loop = true;
        } else if (arg == "--realtime") {
            options.input.options.realtime = true;
        } else if (arg == "--gray-input") {
            options.input.options.gray = true;
        } else if (arg == "--max-fps") {
            options.input.maxFps = parseNumber(value(), "--max-fps", 1, 1000);
        } else if (arg == "-q" || arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "--list-stages") {
            std::printf("%s", scip::StageRegistry::instance().help().c_str());
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
    return true;
}

/// One line per state change, so `journalctl` / the console shows what happens.
void logChanges(const scip::Engine& engine, scip::EngineState& state, std::string& message)
{
    const scip::EngineStatus status = engine.status();
    if (status.state == state && status.message == message) {
        return;
    }
    state = status.state;
    message = status.message;
    std::string line = "scipd: " + std::string(scip::toString(state));
    if (state == scip::EngineState::Running) {
        line += " " + status.inputUri + " " + std::to_string(status.inputInfo.width) + "x" +
                std::to_string(status.inputInfo.height);
    }
    if (!message.empty()) {
        line += ": " + message;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

int run(const Options& options)
{
    scip::StageRegistry& registry = scip::StageRegistry::instance();
    scip::Engine engine(registry);
    // Apply the starting configuration; a mistake here stops at once.
    if (!options.pipeline.empty()) {
        engine.setPipeline(registry.parseChain(options.pipeline));
    }
    engine.setOutputs(options.outputs);
    if (!options.input.uri.empty()) {
        engine.setInput(options.input);
    }

    scip::aipc::Server server(engine, registry, options.server);
    server.start();
    engine.start();
    std::fprintf(stderr,
                 "scipd: AIPC %s on %s:%d",
                 scip::aipc::kProtocolVersion,
                 options.server.bind.c_str(),
                 server.port());
    if (server.discoveryPort() > 0) {
        std::fprintf(stderr, ", discovery on udp %d", server.discoveryPort());
    }
    std::fprintf(stderr, " (%s)\n", scip::build::info().describe);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    scip::EngineState state = scip::EngineState::Idle;
    std::string message = "stopped";
    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!options.quiet) {
            logChanges(engine, state, message);
        }
    }
    std::fprintf(stderr, "scipd: stopping\n");
    server.stop();  // no more commands
    engine.stop();  // source first, then outputs (recordings are finished)
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        scip::gst::registerStages(scip::StageRegistry::instance());
        Options options;
        if (!parseArgs(argc, argv, options)) {
            return 0;
        }
        return run(options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scipd: error: %s\n", e.what());
        return 1;
    }
}

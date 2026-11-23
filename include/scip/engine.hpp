#pragma once

// The processing engine of the target: input -> stages -> outputs, running on
// its own thread, reconfigurable at any time from other threads (the control
// interface).
//
// Commands validate what they can immediately (unknown function, bad parameter
// -> std::invalid_argument) and are applied by the worker between two frames.
// When the input pauses, a change of functions or outputs is applied to the
// last frame at once, so a paused video can be tuned.
// Problems that only show with real frames (a stage rejecting the format, an
// output that cannot open) do not stop the video: the stage is bypassed or the
// output dropped, and the error is reported in status().

#include "scip/io.hpp"
#include "scip/registry.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace scip {

struct InputConfig {
    std::string uri;            ///< see io::openSource(); empty = no input
    io::SourceOptions options;  ///< realtime, gray, loop (maxFrames: 0 = all)
    double maxFps = 0.0;        ///< throttle non-live inputs (e.g. patterns); 0 = no limit
};

/// One function in the engine's pipeline.
struct StageEntry {
    int id = 0;  ///< stable identifier, unaffected by reordering
    StageSpec spec;
    bool enabled = true;
};

enum class EngineState {
    Idle,     ///< no input configured, or the input ended
    Waiting,  ///< input open, no frame yet (a live sender has not started)
    Running,  ///< frames are flowing
    Error,    ///< the input could not be opened or failed
};

std::string_view toString(EngineState state);

struct StageStatus {
    int id = 0;
    std::string name;  ///< "blur ksize=5"
    bool enabled = true;
    bool active = false;  ///< enabled and running (not bypassed because of an error)
    std::string error;
    double meanMs = 0.0;
    double p95Ms = 0.0;
};

struct OutputStatus {
    std::string uri;
    std::string description;  ///< e.g. "display (kmssink 1920x1080, connector 40)"
    bool active = false;
    std::string error;
};

struct EngineStatus {
    EngineState state = EngineState::Idle;
    std::string message;  ///< details for Error / Idle ("input ended")
    std::string inputUri;
    bool inputLive = false;
    FrameInfo inputInfo;       ///< 0x0 until the first frame
    FrameInfo outputInfo;      ///< after the pipeline
    double inputFps = 0.0;     ///< nominal, from the source
    double measuredFps = 0.0;  ///< frames processed per second, last second
    std::uint64_t frames = 0;  ///< since the input was opened
    double readMs = 0.0;       ///< mean wait/decode time per frame
    double processMs = 0.0;    ///< mean time for all stages
    double processP95Ms = 0.0;
    double writeMs = 0.0;  ///< mean time for all outputs
    std::vector<StageStatus> stages;
    std::vector<OutputStatus> outputs;
};

class Engine {
public:
    explicit Engine(const StageRegistry& registry = StageRegistry::instance());
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Start / stop the worker thread. The configuration is kept across restarts.
    void start();
    void stop();

    // --- input ---------------------------------------------------------------
    /// Opened by the worker; failures show up in status() as EngineState::Error.
    void setInput(InputConfig input);
    InputConfig input() const;

    // --- pipeline ------------------------------------------------------------
    std::vector<StageEntry> pipeline() const;
    /// Replace the whole pipeline. All specs are validated first; on error
    /// nothing changes. Returns the new entries with their ids.
    std::vector<StageEntry> setPipeline(const std::vector<StageSpec>& specs);
    /// Insert at `index` (-1 or past the end = append).
    StageEntry addStage(const StageSpec& spec, int index = -1, bool enabled = true);
    void removeStage(int id);
    /// Move to `index` (clamped to the pipeline).
    void moveStage(int id, int index);
    /// Change some parameters (merged into the current ones) and/or enable.
    StageEntry
    updateStage(int id, const std::optional<ParamMap>& params, std::optional<bool> enabled);
    void clearPipeline();

    // --- outputs -------------------------------------------------------------
    /// Replace the outputs (see io::openSink()). Opened with the first frame.
    void setOutputs(std::vector<std::string> uris);
    std::vector<std::string> outputs() const;

    /// A snapshot, safe to call from any thread.
    EngineStatus status() const;

private:
    struct Worker;

    StageEntry& findStage(int id);  // requires mutex_
    void changed();                 // requires mutex_

    const StageRegistry& registry_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    InputConfig input_;
    std::vector<StageEntry> stages_;
    std::vector<std::string> outputs_;
    int nextStageId_ = 1;
    std::uint64_t inputVersion_ = 0;
    std::uint64_t pipelineVersion_ = 0;
    std::uint64_t outputsVersion_ = 0;
    EngineStatus status_;

    bool running_ = false;
    std::thread thread_;
    std::unique_ptr<Worker> worker_;
};

}  // namespace scip

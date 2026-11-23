// The engine's worker thread owns everything that touches frames: the source,
// the stage instances and the sinks. Other threads only change the
// configuration (under mutex_) and bump a version number; the worker notices
// the new version between two frames and applies it. Nothing slow (opening a
// camera, starting an encoder) ever happens while the mutex is held, so
// commands and status() stay fast.

#include "scip/engine.hpp"

#include "scip/stats.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace scip {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kStatsWindow = 120;  // ~4 s of samples at 30 fps
constexpr auto kStatusInterval = 250ms;    // how often status() is refreshed
constexpr auto kReadTimeout = 100ms;       // worker checks for commands this often
constexpr auto kIdleWait = 200ms;          // ... and this often with no input
constexpr auto kStallAfter = 1s;           // no frame for this long -> Waiting
constexpr double kStillFps = 30.0;         // default pace of patterns / images

constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

std::string errorText(const std::exception_ptr& error)
{
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

/// A stage as the worker runs it.
struct ActiveStage {
    int id = 0;
    std::string label;
    bool enabled = true;
    StagePtr stage;  ///< null when creation or processing failed
    std::string error;
    bool active = false;  ///< configured and part of the running chain
    Frame output;
    LatencyStats time{kStatsWindow};
};

struct ActiveOutput {
    std::string uri;
    FrameSinkPtr sink;
    std::string error;
};

/// Validate `spec` (throws std::invalid_argument) and store each given value
/// in the type the function declares: 2 sent for a float parameter becomes
/// 2.0, so the pipeline reports the same types as the function's ParamSpecs.
StageSpec normalized(const StageRegistry& registry, StageSpec spec)
{
    const ParamMap resolved = registry.resolve(spec);
    for (auto& [name, value] : spec.params) {
        value = resolved.at(name);
    }
    return spec;
}

}  // namespace

std::string_view toString(EngineState state)
{
    switch (state) {
        case EngineState::Idle:
            return "idle";
        case EngineState::Waiting:
            return "waiting";
        case EngineState::Running:
            return "running";
        case EngineState::Paused:
            return "paused";
        case EngineState::Error:
            return "error";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

struct Engine::Worker {
    explicit Worker(Engine& owner) : engine(owner) {}

    void run();

    // Configuration changes, applied between frames.
    void applyInput(InputConfig config);
    void applyPipeline(const std::vector<StageEntry>& entries);
    void applyOutputs(const std::vector<std::string>& uris);
    void applyInputOutputs(const std::vector<std::string>& uris);
    void applyPlayback(const PlaybackConfig& config, std::optional<double> seekTo);
    bool playerPaused();

    void step();
    void render(const Frame& frame, bool fresh);
    void configure(const FrameInfo& info);
    bool process(const Frame& frame, const Frame*& result);
    void write(const Frame& frame);
    void writeInputOutputs(const Frame& frame);
    void openOutputs(const FrameInfo& info);
    void closeOutputs();
    void openSinks(std::vector<ActiveOutput>& sinks, const FrameInfo& info);
    void writeSinks(std::vector<ActiveOutput>& sinks, const Frame& frame);
    void closeSinks(std::vector<ActiveOutput>& sinks);
    void setState(EngineState next, std::string message = {});
    void publish(bool force);

    Engine& engine;

    InputConfig input;
    FrameSourcePtr source;
    std::vector<ActiveStage> stages;
    std::vector<ActiveOutput> outputs;
    std::vector<ActiveOutput> inputOutputs;  ///< get the input, before the stages
    std::uint64_t inputVersion = kNever;
    std::uint64_t pipelineVersion = kNever;
    std::uint64_t outputsVersion = kNever;
    std::uint64_t inputOutputsVersion = kNever;
    std::uint64_t playbackVersion = kNever;

    bool configured = false;
    FrameInfo configuredFor;
    FrameInfo outputInfo;
    bool outputsPending = true;  ///< (re)open with the next processed frame
    bool inputOutputsPending = true;
    FrameInfo inputOutputsInfo;
    bool playerEnded = false;  ///< the player input stopped at its end (reported once)
    double pace = 0.0;         ///< frames per second for non-live sources, 0 = no limit
    Clock::time_point nextFrame;
    Clock::time_point lastFrame;
    /// The last frame read (shared with the source, not copied). When the input
    /// pauses, a change of functions or outputs is shown on it, so a host can
    /// tune a paused video. Released before the source goes away.
    Frame lastInput;
    bool rerender = false;  ///< show changes on lastInput if no new frame comes

    EngineState state = EngineState::Idle;
    std::string message = "no input";
    std::uint64_t frames = 0;
    LatencyStats readTime{kStatsWindow};
    LatencyStats processTime{kStatsWindow};
    LatencyStats writeTime{kStatsWindow};
    std::deque<Clock::time_point> recent;  ///< frame times of the last second
    Clock::time_point lastPublish;
    bool dirty = true;  ///< something worth publishing at once
};

void Engine::Worker::run()
{
    while (true) {
        std::optional<InputConfig> newInput;
        std::optional<std::vector<StageEntry>> newPipeline;
        std::optional<std::vector<std::string>> newOutputs;
        std::optional<std::vector<std::string>> newInputOutputs;
        std::optional<PlaybackConfig> newPlayback;
        std::optional<double> seekTo;
        {
            std::unique_lock lock(engine.mutex_);
            const auto pendingChange = [&] {
                return !engine.running_ || engine.inputVersion_ != inputVersion ||
                       engine.pipelineVersion_ != pipelineVersion ||
                       engine.outputsVersion_ != outputsVersion ||
                       engine.inputOutputsVersion_ != inputOutputsVersion ||
                       engine.playbackVersion_ != playbackVersion;
            };
            if (!source) {
                engine.wake_.wait_for(lock, kIdleWait, pendingChange);
            }
            if (!engine.running_) {
                break;
            }
            if (engine.inputVersion_ != inputVersion ||
                engine.playbackVersion_ != playbackVersion) {
                // A new input gets the current player settings too.
                playbackVersion = engine.playbackVersion_;
                newPlayback = engine.playback_;
                seekTo = std::exchange(engine.seekTo_, std::nullopt);
            }
            if (engine.inputVersion_ != inputVersion) {
                inputVersion = engine.inputVersion_;
                newInput = engine.input_;
            }
            if (engine.pipelineVersion_ != pipelineVersion) {
                pipelineVersion = engine.pipelineVersion_;
                newPipeline = engine.stages_;
            }
            if (engine.outputsVersion_ != outputsVersion) {
                outputsVersion = engine.outputsVersion_;
                newOutputs = engine.outputs_;
            }
            if (engine.inputOutputsVersion_ != inputOutputsVersion) {
                inputOutputsVersion = engine.inputOutputsVersion_;
                newInputOutputs = engine.inputOutputs_;
            }
        }
        // Outputs first: a new input must not start writing into outputs that
        // were just removed.
        if (newOutputs) {
            applyOutputs(*newOutputs);
        }
        if (newInputOutputs) {
            applyInputOutputs(*newInputOutputs);
        }
        if (newPipeline) {
            applyPipeline(*newPipeline);
        }
        if (newInput) {
            applyInput(std::move(*newInput));
        }
        if (newPlayback) {
            applyPlayback(*newPlayback, seekTo);
        }
        if (source) {
            step();
        }
        publish(false);
    }
    // Source first: a pipeline that feeds a display through G2D must stop
    // producing before the display goes away.
    lastInput = Frame();
    source.reset();
    closeOutputs();
    closeSinks(inputOutputs);
    setState(EngineState::Idle, "stopped");
    publish(true);
}

void Engine::Worker::applyInput(InputConfig config)
{
    lastInput = Frame();
    source.reset();
    input = std::move(config);
    configured = false;
    playerEnded = false;
    frames = 0;
    readTime.reset();
    processTime.reset();
    writeTime.reset();
    recent.clear();
    if (input.uri.empty()) {
        setState(EngineState::Idle, "no input");
        return;
    }
    try {
        source = io::openSource(input.uri, input.options);
    } catch (...) {
        setState(EngineState::Error, errorText(std::current_exception()));
        return;
    }
    pace = input.maxFps;
    if (pace <= 0.0 && io::isStillSource(input.uri)) {
        pace = kStillFps;  // a pattern would otherwise spin a core at 1000s of fps
    }
    nextFrame = Clock::now();
    lastFrame = Clock::now();
    setState(EngineState::Waiting);
}

void Engine::Worker::applyPipeline(const std::vector<StageEntry>& entries)
{
    std::vector<ActiveStage> next;
    next.reserve(entries.size());
    for (const StageEntry& entry : entries) {
        const std::string label = formatSpec(entry.spec);
        auto old = std::find_if(stages.begin(), stages.end(), [&](const ActiveStage& s) {
            return s.id == entry.id && s.label == label && s.stage != nullptr;
        });
        if (old != stages.end()) {
            // Same function and parameters: keep the instance (and its timings).
            old->enabled = entry.enabled;
            next.push_back(std::move(*old));
            continue;
        }
        ActiveStage stage;
        stage.id = entry.id;
        stage.label = label;
        stage.enabled = entry.enabled;
        try {
            stage.stage = engine.registry_.create(entry.spec);
        } catch (...) {
            stage.error = errorText(std::current_exception());
        }
        next.push_back(std::move(stage));
    }
    stages = std::move(next);
    configured = false;
    rerender = true;
    dirty = true;
}

void Engine::Worker::applyOutputs(const std::vector<std::string>& uris)
{
    closeOutputs();
    outputs.clear();
    for (const std::string& uri : uris) {
        outputs.push_back(ActiveOutput{uri, nullptr, {}});
    }
    outputsPending = true;
    rerender = true;
    dirty = true;
}

void Engine::Worker::applyInputOutputs(const std::vector<std::string>& uris)
{
    closeSinks(inputOutputs);
    inputOutputs.clear();
    for (const std::string& uri : uris) {
        inputOutputs.push_back(ActiveOutput{uri, nullptr, {}});
    }
    inputOutputsPending = true;
    rerender = true;
    dirty = true;
}

void Engine::Worker::applyPlayback(const PlaybackConfig& config, std::optional<double> seekTo)
{
    Playback* player = source ? source->playback() : nullptr;
    if (player == nullptr) {
        return;
    }
    try {
        const PlaybackState now = player->state();
        if (config.rate != now.rate) {
            player->setRate(config.rate);
        }
        if (config.loop != now.loop || config.a != now.a || config.b != now.b) {
            player->setLoop(config.loop, config.a, config.b);
        }
        if (seekTo) {
            player->seek(*seekTo);
        }
        if (config.paused != player->state().paused) {
            player->setPaused(config.paused);
        }
    } catch (...) {
        setState(state, "player: " + errorText(std::current_exception()));
    }
    playerEnded = player->state().ended;
    dirty = true;
}

/// True when the input is a paused player. Keeps the engine's PlaybackConfig
/// in step when the video stopped at its end by itself, so "play" restarts it.
bool Engine::Worker::playerPaused()
{
    Playback* player = source ? source->playback() : nullptr;
    if (player == nullptr) {
        return false;
    }
    const PlaybackState st = player->state();
    if (st.ended != playerEnded) {
        playerEnded = st.ended;
        if (st.ended) {
            std::lock_guard lock(engine.mutex_);
            engine.playback_.paused = true;
        }
        dirty = true;
    }
    return st.paused;
}

void Engine::Worker::step()
{
    const auto now = Clock::now();
    if (pace > 0.0 && !source->isLive()) {
        if (now < nextFrame) {
            // Sleep in small slices so commands are still picked up quickly.
            std::this_thread::sleep_for(std::min<Clock::duration>(nextFrame - now, 50ms));
            return;
        }
        const auto period =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / pace));
        // Catch up at most one frame: after a slow frame, do not burst.
        nextFrame = std::max(nextFrame + period, now);
    }

    Frame frame;
    const auto readStart = Clock::now();
    ReadStatus status = ReadStatus::End;
    try {
        status = source->tryRead(frame, kReadTimeout);
    } catch (...) {
        lastInput = Frame();
        source.reset();
        setState(EngineState::Error, "input failed: " + errorText(std::current_exception()));
        return;
    }
    if (status == ReadStatus::Timeout) {
        if (playerPaused()) {
            setState(EngineState::Paused, playerEnded ? "end of the video" : "");
        } else if (Clock::now() - lastFrame > kStallAfter) {
            setState(EngineState::Waiting, frames == 0 ? "" : "no frames from the input");
        }
        if (rerender && !lastInput.empty()) {
            render(lastInput, false);  // paused input: show the change anyway
        }
        return;
    }
    if (status == ReadStatus::End) {
        lastInput = Frame();
        source.reset();
        // Finish recordings; outputs reopen when a new input starts.
        closeOutputs();
        closeSinks(inputOutputs);
        outputsPending = true;
        inputOutputsPending = true;
        setState(EngineState::Idle, "input ended");
        return;
    }
    const auto readEnd = Clock::now();
    readTime.add(readEnd - readStart);
    lastFrame = readEnd;
    lastInput = frame;
    render(frame, true);
    if (playerPaused()) {
        setState(EngineState::Paused);  // the frame a seek landed on
    }
}

/// Process a frame and write the result. `fresh` = just read; a repeat of the
/// last frame (input paused) is kept out of the frame count and timings.
void Engine::Worker::render(const Frame& frame, bool fresh)
{
    rerender = false;
    if (!configured || frame.info() != configuredFor) {
        configure(frame.info());
    }

    const auto tapStart = Clock::now();
    writeInputOutputs(frame);
    const auto tapTime = Clock::now() - tapStart;

    const auto start = Clock::now();
    const Frame* result = &frame;
    if (!process(frame, result)) {
        return;  // a stage failed on this frame; it is bypassed from the next one
    }
    const auto processEnd = Clock::now();
    write(*result);
    if (!fresh) {
        return;
    }
    const auto writeEnd = Clock::now();
    processTime.add(processEnd - start);
    writeTime.add(writeEnd - processEnd + tapTime);
    ++frames;
    recent.push_back(writeEnd);
    setState(EngineState::Running);
}

void Engine::Worker::configure(const FrameInfo& info)
{
    FrameInfo current = info;
    for (ActiveStage& s : stages) {
        s.active = false;
        if (!s.enabled || !s.stage) {
            continue;  // disabled, or failed earlier (s.error tells why)
        }
        try {
            const FrameInfo out = s.stage->configure(current);
            s.output = Frame(out);
            s.error.clear();
            s.active = true;
            current = out;
        } catch (...) {
            // Bypass: the next stage gets this stage's input.
            s.error = errorText(std::current_exception());
        }
    }
    configured = true;
    configuredFor = info;
    if (current != outputInfo) {
        outputInfo = current;
        outputsPending = true;  // sinks are opened for one format
    }
    dirty = true;
}

bool Engine::Worker::process(const Frame& frame, const Frame*& result)
{
    const Frame* current = &frame;
    for (ActiveStage& s : stages) {
        if (!s.active) {
            continue;
        }
        const auto start = Clock::now();
        try {
            s.stage->process(*current, s.output);
        } catch (...) {
            s.error = errorText(std::current_exception());
            s.stage.reset();
            s.active = false;
            configured = false;
            dirty = true;
            return false;
        }
        s.time.add(Clock::now() - start);
        current = &s.output;
    }
    result = current;
    return true;
}

void Engine::Worker::write(const Frame& frame)
{
    if (outputsPending) {
        openOutputs(frame.info());
    }
    writeSinks(outputs, frame);
}

void Engine::Worker::writeInputOutputs(const Frame& frame)
{
    if (inputOutputs.empty()) {
        return;
    }
    if (inputOutputsPending || frame.info() != inputOutputsInfo) {
        openSinks(inputOutputs, frame.info());
        inputOutputsInfo = frame.info();
        inputOutputsPending = false;
    }
    writeSinks(inputOutputs, frame);
}

void Engine::Worker::openOutputs(const FrameInfo& info)
{
    openSinks(outputs, info);
    outputsPending = false;
}

void Engine::Worker::closeOutputs()
{
    closeSinks(outputs);
}

void Engine::Worker::openSinks(std::vector<ActiveOutput>& sinks, const FrameInfo& info)
{
    closeSinks(sinks);
    double fps = pace > 0.0 ? pace : 0.0;
    if (source && source->frameRate() > 0.0) {
        fps = source->frameRate();
    }
    for (ActiveOutput& out : sinks) {
        try {
            out.sink = io::openSink(out.uri, info, fps);
            out.error.clear();
        } catch (...) {
            out.error = errorText(std::current_exception());
        }
    }
    dirty = true;
}

void Engine::Worker::writeSinks(std::vector<ActiveOutput>& sinks, const Frame& frame)
{
    for (ActiveOutput& out : sinks) {
        if (!out.sink) {
            continue;
        }
        try {
            out.sink->write(frame);
        } catch (...) {
            // Drop this output; the others keep going.
            out.error = errorText(std::current_exception());
            out.sink.reset();
            dirty = true;
        }
    }
}

void Engine::Worker::closeSinks(std::vector<ActiveOutput>& sinks)
{
    for (ActiveOutput& out : sinks) {
        if (!out.sink) {
            continue;
        }
        try {
            out.sink->close();
        } catch (...) {
            out.error = errorText(std::current_exception());
        }
        out.sink.reset();
    }
}

void Engine::Worker::setState(EngineState next, std::string text)
{
    if (next != state || text != message) {
        state = next;
        message = std::move(text);
        dirty = true;
    }
}

void Engine::Worker::publish(bool force)
{
    const auto now = Clock::now();
    if (!force && !dirty && now - lastPublish < kStatusInterval) {
        return;
    }
    lastPublish = now;
    dirty = false;
    while (!recent.empty() && now - recent.front() > 1s) {
        recent.pop_front();
    }

    EngineStatus s;
    s.state = state;
    s.message = message;
    s.inputUri = input.uri;
    s.inputLive = source && source->isLive();
    s.inputInfo = configured ? configuredFor : FrameInfo{};
    s.outputInfo = configured ? outputInfo : FrameInfo{};
    s.inputFps = source ? source->frameRate() : 0.0;
    if (s.inputFps <= 0.0 && source && pace > 0.0) {
        s.inputFps = pace;
    }
    s.measuredFps = static_cast<double>(recent.size());
    s.frames = frames;
    s.readMs = readTime.meanMs();
    s.processMs = processTime.meanMs();
    s.processP95Ms = processTime.percentileMs(95.0);
    s.writeMs = writeTime.meanMs();
    for (const ActiveStage& stage : stages) {
        StageStatus st;
        st.id = stage.id;
        st.name = stage.label;
        st.enabled = stage.enabled;
        st.active = stage.active;
        st.error = stage.error;
        st.meanMs = stage.time.meanMs();
        st.p95Ms = stage.time.percentileMs(95.0);
        s.stages.push_back(std::move(st));
    }
    for (const ActiveOutput& out : outputs) {
        s.outputs.push_back(OutputStatus{
            out.uri, out.sink ? out.sink->name() : "", out.sink != nullptr, out.error});
    }
    for (const ActiveOutput& out : inputOutputs) {
        s.inputOutputs.push_back(OutputStatus{
            out.uri, out.sink ? out.sink->name() : "", out.sink != nullptr, out.error});
    }
    if (Playback* player = source ? source->playback() : nullptr) {
        s.playback = player->state();
    }

    std::lock_guard lock(engine.mutex_);
    engine.status_ = std::move(s);
}

// ---------------------------------------------------------------------------
// Engine: configuration, called from any thread
// ---------------------------------------------------------------------------

Engine::Engine(const StageRegistry& registry) : registry_(registry) {}

Engine::~Engine()
{
    stop();
}

void Engine::start()
{
    std::lock_guard lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::make_unique<Worker>(*this);
    thread_ = std::thread([worker = worker_.get()] { worker->run(); });
}

void Engine::stop()
{
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    wake_.notify_all();
    thread_.join();
    worker_.reset();
}

void Engine::setInput(InputConfig config)
{
    if (!config.uri.empty()) {
        io::validateSourceUri(config.uri);
    }
    if (config.maxFps < 0.0) {
        throw std::invalid_argument("max_fps must be >= 0");
    }
    std::lock_guard lock(mutex_);
    if (config.options.player) {
        // A new video starts paused on its first frame, without an A-B range.
        playback_.paused = true;
        playback_.loop = config.options.loop;
        playback_.a.reset();
        playback_.b.reset();
        seekTo_.reset();
    }
    input_ = std::move(config);
    ++inputVersion_;
    changed();
}

InputConfig Engine::input() const
{
    std::lock_guard lock(mutex_);
    return input_;
}

std::vector<StageEntry> Engine::pipeline() const
{
    std::lock_guard lock(mutex_);
    return stages_;
}

std::vector<StageEntry> Engine::setPipeline(const std::vector<StageSpec>& specs)
{
    std::vector<StageSpec> checked;
    for (const StageSpec& spec : specs) {
        checked.push_back(normalized(registry_, spec));  // all or nothing
    }
    std::lock_guard lock(mutex_);
    stages_.clear();
    for (StageSpec& spec : checked) {
        stages_.push_back(StageEntry{nextStageId_++, std::move(spec), true});
    }
    ++pipelineVersion_;
    changed();
    return stages_;
}

StageEntry Engine::addStage(const StageSpec& spec, int index, bool enabled)
{
    StageSpec checked = normalized(registry_, spec);
    std::lock_guard lock(mutex_);
    const StageEntry entry{nextStageId_++, std::move(checked), enabled};
    const auto size = static_cast<int>(stages_.size());
    const int at = index < 0 || index > size ? size : index;
    stages_.insert(stages_.begin() + at, entry);
    ++pipelineVersion_;
    changed();
    return entry;
}

void Engine::removeStage(int id)
{
    std::lock_guard lock(mutex_);
    const StageEntry& entry = findStage(id);
    stages_.erase(stages_.begin() + (&entry - stages_.data()));
    ++pipelineVersion_;
    changed();
}

void Engine::moveStage(int id, int index)
{
    std::lock_guard lock(mutex_);
    const auto from = &findStage(id) - stages_.data();
    StageEntry entry = stages_[static_cast<std::size_t>(from)];
    stages_.erase(stages_.begin() + from);
    const auto size = static_cast<int>(stages_.size());
    const int to = std::clamp(index < 0 ? size : index, 0, size);
    stages_.insert(stages_.begin() + to, std::move(entry));
    ++pipelineVersion_;
    changed();
}

StageEntry
Engine::updateStage(int id, const std::optional<ParamMap>& params, std::optional<bool> enabled)
{
    std::lock_guard lock(mutex_);
    StageEntry& entry = findStage(id);
    StageSpec spec = entry.spec;
    if (params) {
        for (const auto& [name, value] : *params) {
            spec.params[name] = value;
        }
        spec = normalized(registry_, spec);  // validate before changing anything
    }
    entry.spec = std::move(spec);
    if (enabled) {
        entry.enabled = *enabled;
    }
    ++pipelineVersion_;
    changed();
    return entry;
}

void Engine::clearPipeline()
{
    std::lock_guard lock(mutex_);
    stages_.clear();
    ++pipelineVersion_;
    changed();
}

namespace {

void validateOutputs(const std::vector<std::string>& uris)
{
    for (std::size_t i = 0; i < uris.size(); ++i) {
        io::validateSinkUri(uris[i]);
        if (std::find(uris.begin(), uris.begin() + static_cast<std::ptrdiff_t>(i), uris[i]) !=
            uris.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw std::invalid_argument("output listed twice: '" + uris[i] + "'");
        }
    }
}

}  // namespace

void Engine::setOutputs(std::vector<std::string> uris)
{
    validateOutputs(uris);
    std::lock_guard lock(mutex_);
    outputs_ = std::move(uris);
    ++outputsVersion_;
    changed();
}

std::vector<std::string> Engine::outputs() const
{
    std::lock_guard lock(mutex_);
    return outputs_;
}

void Engine::setInputOutputs(std::vector<std::string> uris)
{
    validateOutputs(uris);
    std::lock_guard lock(mutex_);
    inputOutputs_ = std::move(uris);
    ++inputOutputsVersion_;
    changed();
}

std::vector<std::string> Engine::inputOutputs() const
{
    std::lock_guard lock(mutex_);
    return inputOutputs_;
}

void Engine::setPlayback(const PlaybackConfig& config)
{
    if (!(config.rate > 0.0)) {
        throw std::invalid_argument("rate must be > 0");
    }
    if (config.a.has_value() != config.b.has_value()) {
        throw std::invalid_argument("give both a and b, or neither");
    }
    if (config.a && (*config.a < 0.0 || *config.b <= *config.a)) {
        throw std::invalid_argument("the loop needs 0 <= a < b");
    }
    std::lock_guard lock(mutex_);
    playback_ = config;
    ++playbackVersion_;
    changed();
}

PlaybackConfig Engine::playback() const
{
    std::lock_guard lock(mutex_);
    return playback_;
}

void Engine::seek(double seconds)
{
    if (!(seconds >= 0.0)) {
        throw std::invalid_argument("position must be >= 0");
    }
    std::lock_guard lock(mutex_);
    seekTo_ = seconds;
    ++playbackVersion_;
    changed();
}

EngineStatus Engine::status() const
{
    std::lock_guard lock(mutex_);
    EngineStatus result = status_;
    // The configuration is always current; the worker's view may lag by a
    // frame. Report the configuration, with the worker's findings where they
    // match it.
    std::vector<StageStatus> stages;
    for (const StageEntry& entry : stages_) {
        StageStatus st;
        st.id = entry.id;
        st.name = formatSpec(entry.spec);
        st.enabled = entry.enabled;
        for (const StageStatus& seen : status_.stages) {
            if (seen.id == entry.id && seen.name == st.name) {
                st = seen;
                st.enabled = entry.enabled;
            }
        }
        stages.push_back(std::move(st));
    }
    result.stages = std::move(stages);
    std::vector<OutputStatus> outputs;
    for (const std::string& uri : outputs_) {
        OutputStatus out;
        out.uri = uri;
        for (const OutputStatus& seen : status_.outputs) {
            if (seen.uri == uri) {
                out = seen;
            }
        }
        outputs.push_back(std::move(out));
    }
    result.outputs = std::move(outputs);
    std::vector<OutputStatus> inputOutputs;
    for (const std::string& uri : inputOutputs_) {
        OutputStatus out;
        out.uri = uri;
        for (const OutputStatus& seen : status_.inputOutputs) {
            if (seen.uri == uri) {
                out = seen;
            }
        }
        inputOutputs.push_back(std::move(out));
    }
    result.inputOutputs = std::move(inputOutputs);
    if (!running_) {
        result.state = EngineState::Idle;
        result.message = "stopped";
    }
    return result;
}

StageEntry& Engine::findStage(int id)
{
    for (StageEntry& entry : stages_) {
        if (entry.id == id) {
            return entry;
        }
    }
    throw std::invalid_argument("no stage with id " + std::to_string(id));
}

void Engine::changed()
{
    wake_.notify_all();
}

}  // namespace scip

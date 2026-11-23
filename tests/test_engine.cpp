// The engine: frames flow while the configuration changes underneath.

#include "scip/engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <thread>

using scip::Engine;
using scip::EngineState;
using scip::EngineStatus;
using scip::FrameInfo;
using scip::PixelFormat;
using scip::StageRegistry;

namespace {

/// Poll status() until `done` holds; fails the test after `timeout`.
EngineStatus waitFor(const Engine& engine,
                     const std::function<bool(const EngineStatus&)>& done,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    EngineStatus status = engine.status();
    while (!done(status)) {
        if (std::chrono::steady_clock::now() > deadline) {
            ADD_FAILURE() << "timed out; state " << scip::toString(status.state) << " ("
                          << status.message << "), " << status.frames << " frames";
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        status = engine.status();
    }
    return status;
}

scip::InputConfig pattern(const std::string& uri = "pattern:64x48", std::uint64_t frames = 0)
{
    scip::InputConfig input;
    input.uri = uri;
    input.options.maxFrames = frames;
    input.maxFps = 500.0;  // fast tests
    return input;
}

/// Frames counted after `n` more have been processed.
auto atLeast(std::uint64_t n)
{
    return [n](const EngineStatus& s) { return s.state == EngineState::Running && s.frames >= n; };
}

const StageRegistry& registry()
{
    return StageRegistry::instance();
}

}  // namespace

TEST(Engine, RunsInputThroughThePipeline)
{
    Engine engine;
    engine.setInput(pattern());
    engine.setPipeline(registry().parseChain("gray ! resize width=32 height=24"));
    engine.start();
    const EngineStatus status = waitFor(engine, atLeast(5));
    EXPECT_EQ(status.inputInfo, (FrameInfo{64, 48, PixelFormat::Bgr8}));
    EXPECT_EQ(status.outputInfo, (FrameInfo{32, 24, PixelFormat::Gray8}));
    ASSERT_EQ(status.stages.size(), 2u);
    EXPECT_TRUE(status.stages[0].active);
    EXPECT_EQ(status.stages[1].name, "resize height=24 width=32");  // parameters sorted
    engine.stop();
    EXPECT_EQ(engine.status().state, EngineState::Idle);
}

TEST(Engine, ChangesThePipelineWhileRunning)
{
    Engine engine;
    engine.setInput(pattern());
    engine.start();
    waitFor(engine, atLeast(2));
    engine.addStage(scip::StageSpec{"gray", {}});
    const EngineStatus status = waitFor(
        engine, [](const EngineStatus& s) { return s.outputInfo.format == PixelFormat::Gray8; });
    EXPECT_EQ(status.state, EngineState::Running);
}

TEST(Engine, BypassesAStageThatRejectsTheFormat)
{
    Engine engine;
    engine.setInput(pattern());  // bgr8, but otsu needs gray8
    engine.setPipeline(registry().parseChain("threshold mode=otsu ! invert"));
    engine.start();
    const EngineStatus status = waitFor(engine, [](const EngineStatus& s) {
        return s.frames >= 3 && s.stages.size() == 2 && !s.stages[0].error.empty();
    });
    EXPECT_FALSE(status.stages[0].active);
    EXPECT_TRUE(status.stages[1].active);  // the rest of the chain still runs
    EXPECT_EQ(status.outputInfo.format, PixelFormat::Bgr8);
}

TEST(Engine, RejectsBadCommandsAtOnce)
{
    Engine engine;
    EXPECT_THROW(engine.addStage(scip::StageSpec{"no-such-function", {}}), std::invalid_argument);
    EXPECT_THROW(engine.addStage(scip::StageSpec{"blur", {{"ksize", std::int64_t{4}}}}),
                 std::invalid_argument);
    EXPECT_THROW(engine.setInput(pattern("/no/such/clip.mp4")), std::invalid_argument);
    EXPECT_THROW(engine.setOutputs({"out.avi"}), std::invalid_argument);
    EXPECT_THROW(engine.setOutputs({"a.png", "a.png"}), std::invalid_argument);
    EXPECT_THROW(engine.removeStage(42), std::invalid_argument);
    EXPECT_TRUE(engine.pipeline().empty());
}

TEST(Engine, UpdatesSomeParametersAndKeepsTheOthers)
{
    Engine engine;
    const auto blur = engine.addStage(registry().parseChain("blur ksize=5").front());
    const auto updated = engine.updateStage(blur.id, scip::ParamMap{{"sigma", 2.0}}, std::nullopt);
    EXPECT_EQ(scip::formatSpec(updated.spec), "blur ksize=5 sigma=2");

    EXPECT_THROW(engine.updateStage(blur.id, scip::ParamMap{{"ksize", std::int64_t{4}}}, false),
                 std::invalid_argument);
    EXPECT_TRUE(engine.pipeline().front().enabled);  // a failed update changes nothing

    engine.updateStage(blur.id, std::nullopt, false);
    EXPECT_FALSE(engine.pipeline().front().enabled);
}

TEST(Engine, MovesAndRemovesStagesById)
{
    Engine engine;
    const auto entries = engine.setPipeline(registry().parseChain("gray ! invert ! blur"));
    ASSERT_EQ(entries.size(), 3u);
    engine.moveStage(entries[2].id, 0);
    engine.removeStage(entries[1].id);
    const auto pipeline = engine.pipeline();
    ASSERT_EQ(pipeline.size(), 2u);
    EXPECT_EQ(pipeline[0].spec.name, "blur");
    EXPECT_EQ(pipeline[1].spec.name, "gray");
    EXPECT_EQ(pipeline[1].id, entries[0].id);
}

TEST(Engine, ReportsAnInputThatCannotOpen)
{
    const auto path = std::filesystem::temp_directory_path() / "scip_test_broken.png";
    std::ofstream(path) << "not an image";
    Engine engine;
    engine.setInput(pattern(path.string()));
    engine.start();
    const EngineStatus status =
        waitFor(engine, [](const EngineStatus& s) { return s.state == EngineState::Error; });
    EXPECT_FALSE(status.message.empty());

    engine.setInput(pattern());  // recovers with a new input
    waitFor(engine, atLeast(1));
    std::filesystem::remove(path);
}

TEST(Engine, StopsWhenTheInputEnds)
{
    Engine engine;
    engine.setInput(pattern("pattern:16x16", 3));
    engine.start();
    const EngineStatus status =
        waitFor(engine, [](const EngineStatus& s) { return s.message == "input ended"; });
    EXPECT_EQ(status.state, EngineState::Idle);
    EXPECT_EQ(status.frames, 3u);
}

TEST(Engine, WritesOutputsAndClosesThemWhenRemoved)
{
    const auto path = std::filesystem::temp_directory_path() / "scip_test_engine.png";
    std::filesystem::remove(path);
    Engine engine;
    engine.setInput(pattern());
    engine.setPipeline(registry().parseChain("gray"));
    engine.setOutputs({path.string()});
    engine.start();
    const EngineStatus status = waitFor(engine, [](const EngineStatus& s) {
        return s.frames >= 3 && !s.outputs.empty() && s.outputs[0].active;
    });
    EXPECT_TRUE(status.outputs[0].error.empty());
    engine.setOutputs({});  // the image sink writes its last frame on close
    waitFor(engine, [&](const EngineStatus&) { return std::filesystem::exists(path); });
    std::filesystem::remove(path);
}

TEST(Engine, ShowsChangesWhileTheInputIsPaused)
{
    // A host pauses its video and tunes the functions: the change must reach the
    // outputs on the last frame, without waiting for the next one.
    if (!scip::io::hasGStreamer()) {
        GTEST_SKIP() << "built without GStreamer";
    }
    const FrameInfo info{64, 64, PixelFormat::Bgr8};
    scip::FrameSinkPtr sender;
    try {
        sender = scip::io::openSink("udp:127.0.0.1:47394", info, 30.0);
    } catch (const std::exception& e) {
        GTEST_SKIP() << e.what();  // no H.264 encoder here
    }
    const auto path = std::filesystem::temp_directory_path() / "scip_test_paused.png";
    std::filesystem::remove(path);

    Engine engine;
    scip::InputConfig input;
    input.uri = "rtp://127.0.0.1:47394";
    engine.setInput(input);
    engine.setOutputs({path.string()});
    engine.start();

    const scip::Frame frame = scip::io::makeTestPattern(info);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engine.status().frames < 5 && std::chrono::steady_clock::now() < deadline) {
        sender->write(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    ASSERT_GE(engine.status().frames, 5u);
    // Paused: the sender stays open but sends nothing more.
    const auto framesBefore = waitFor(engine, [](const EngineStatus& s) {
                                  return s.state == EngineState::Waiting;
                              }).frames;

    engine.setPipeline(registry().parseChain("gray"));
    const EngineStatus status = waitFor(engine, [](const EngineStatus& s) {
        return s.stages.size() == 1 && s.stages[0].meanMs > 0.0;  // ran on the last frame
    });
    EXPECT_EQ(status.frames, framesBefore);  // a repeat, not a new frame

    // The format change reopened the image output, and closing the old one
    // wrote its last (BGR) frame: start from no file.
    std::filesystem::remove(path);
    engine.setOutputs({});  // the image output writes what it got last
    waitFor(engine, [&](const EngineStatus&) { return std::filesystem::exists(path); });
    EXPECT_EQ(scip::io::readImage(path.string()).format(), PixelFormat::Gray8);
    std::filesystem::remove(path);
    sender->close();
}

TEST(Engine, ControlsAVideoFileLikeAPlayer)
{
    if (!scip::io::hasGStreamer()) {
        GTEST_SKIP() << "built without GStreamer";
    }
    const auto dir = std::filesystem::temp_directory_path();
    const auto video = dir / "scip_engine_player.mp4";
    const auto inputShot = dir / "scip_engine_input.png";
    const FrameInfo info{96, 64, PixelFormat::Bgr8};
    try {
        auto sink = scip::io::openSink(video.string(), info, 25.0);
        for (int i = 0; i < 50; ++i) {  // 2 s
            sink->write(scip::io::makeTestPattern(info));
        }
        sink->close();
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << "cannot record a test file here: " << e.what();
    }
    std::filesystem::remove(inputShot);

    Engine engine(registry());
    engine.setPipeline(registry().parseChain("gray"));
    engine.setInputOutputs({inputShot.string()});  // the input, before "gray"
    scip::InputConfig input;
    input.uri = video.string();
    input.options.player = true;
    input.options.realtime = true;
    engine.setInput(input);
    engine.start();

    // Opens paused on the first frame.
    auto status = waitFor(engine, [](const EngineStatus& s) {
        return s.state == EngineState::Paused && s.frames == 1 && s.playback;
    });
    EXPECT_NEAR(status.playback->duration, 2.0, 0.1);
    EXPECT_TRUE(engine.playback().paused);

    // Seek while paused: the frame it lands on is processed.
    engine.seek(1.2);
    status = waitFor(engine, [](const EngineStatus& s) {
        return s.frames == 2 && s.playback && std::abs(s.playback->position - 1.2) < 0.05;
    });
    EXPECT_EQ(status.state, EngineState::Paused);

    // Play at double speed to the end: it stops there, still open.
    scip::PlaybackConfig play = engine.playback();
    play.paused = false;
    play.rate = 2.0;
    engine.setPlayback(play);
    waitFor(engine, [](const EngineStatus& s) { return s.state == EngineState::Running; });
    status = waitFor(engine, [](const EngineStatus& s) {
        return s.state == EngineState::Paused && s.playback && s.playback->ended;
    });
    EXPECT_EQ(status.message, "end of the video");
    EXPECT_TRUE(engine.playback().paused);  // in step, so "play" restarts it
    EXPECT_GT(status.frames, 10U);
    ASSERT_EQ(status.inputOutputs.size(), 1U);
    EXPECT_TRUE(status.inputOutputs[0].active) << status.inputOutputs[0].error;

    // Play again: from the start.
    engine.setPlayback(play);
    status = waitFor(engine, [](const EngineStatus& s) {
        return s.state == EngineState::Running && s.playback && s.playback->position < 1.0;
    });
    EXPECT_THROW(engine.seek(-1.0), std::invalid_argument);
    play.a = 1.0;
    EXPECT_THROW(engine.setPlayback(play), std::invalid_argument);  // A without B

    engine.setInputOutputs({});  // closes the PNG: the last input frame, in color
    waitFor(engine, [&](const EngineStatus&) { return std::filesystem::exists(inputShot); });
    engine.stop();
    EXPECT_EQ(scip::io::readImage(inputShot.string()).info(), info);
    std::filesystem::remove(video);
    std::filesystem::remove(inputShot);
}

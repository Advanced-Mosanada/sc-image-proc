// GStreamer sources and sinks. Uses only elements from gst-plugins-base / -good
// (videotestsrc, fakesink...) so the tests run on the PC, under QEMU and on the board.

#include "scip/io.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;

namespace {

#define SKIP_WITHOUT_GSTREAMER()                   \
    if (!scip::io::hasGStreamer()) {               \
        GTEST_SKIP() << "built without GStreamer"; \
    }

std::string testSource(int buffers, const char* format = "BGR")
{
    return "gst:videotestsrc num-buffers=" + std::to_string(buffers) +
           " ! video/x-raw,format=" + format +
           ",width=64,height=48,framerate=30/1 ! appsink name=scip_sink";
}

int drain(scip::FrameSource& source)
{
    Frame frame;
    int count = 0;
    while (source.read(frame)) {
        ++count;
    }
    return count;
}

}  // namespace

TEST(GstSource, ReadsAllFramesThenEnds)
{
    SKIP_WITHOUT_GSTREAMER();
    auto source = scip::io::openSource(testSource(5), {});
    EXPECT_EQ(source->info(), (FrameInfo{64, 48, PixelFormat::Bgr8}));
    EXPECT_DOUBLE_EQ(source->frameRate(), 30.0);
    EXPECT_EQ(drain(*source), 5);
}

TEST(GstSource, StopsAtMaxFrames)
{
    SKIP_WITHOUT_GSTREAMER();
    scip::io::SourceOptions options;
    options.maxFrames = 3;
    auto source = scip::io::openSource(testSource(50), options);
    EXPECT_EQ(drain(*source), 3);
}

TEST(GstSource, GrayFrames)
{
    SKIP_WITHOUT_GSTREAMER();
    auto source = scip::io::openSource(testSource(1, "GRAY8"), {});
    EXPECT_EQ(source->info().format, PixelFormat::Gray8);
}

TEST(GstSource, FramesStayValidAfterNextRead)
{
    // Zero-copy frames keep their GstSample alive: an older frame must still be
    // readable after newer ones were pulled.
    SKIP_WITHOUT_GSTREAMER();
    auto source = scip::io::openSource(testSource(3), {});
    Frame first;
    ASSERT_TRUE(source->read(first));
    const Frame copy = first.clone();
    Frame later;
    while (source->read(later)) {}
    EXPECT_EQ(std::memcmp(first.row(10), copy.row(10), first.info().rowBytes()), 0);
}

TEST(GstSource, ReportsBadPipelines)
{
    SKIP_WITHOUT_GSTREAMER();
    EXPECT_THROW(scip::io::openSource("gst:no-such-element ! appsink name=scip_sink", {}),
                 std::runtime_error);
    EXPECT_THROW(scip::io::openSource("gst:videotestsrc ! fakesink", {}), std::invalid_argument);
    // Format we cannot handle: error must say so instead of hanging.
    EXPECT_THROW(scip::io::openSource(testSource(1, "YUY2"), {}), std::runtime_error);
}

TEST(GstSink, PushesFramesAndCloses)
{
    SKIP_WITHOUT_GSTREAMER();
    const FrameInfo info{64, 48, PixelFormat::Bgr8};
    auto sink =
        scip::io::openSink("gst:appsrc name=scip_src ! videoconvert ! fakesink", info, 30.0);
    const Frame frame = scip::io::makeTestPattern(info);
    for (int i = 0; i < 10; ++i) {
        sink->write(frame);
    }
    EXPECT_NO_THROW(sink->close());
}

TEST(GstSink, RejectsWrongFormat)
{
    SKIP_WITHOUT_GSTREAMER();
    auto sink = scip::io::openSink(
        "gst:appsrc name=scip_src ! fakesink", FrameInfo{64, 48, PixelFormat::Bgr8}, 30.0);
    const Frame gray(FrameInfo{64, 48, PixelFormat::Gray8});
    EXPECT_THROW(sink->write(gray), std::invalid_argument);
}

class RecordedMp4 : public ::testing::TestWithParam<PixelFormat> {};

// Record with the H.264 encoder, then decode the file again. Covers gray input
// too: the encoder must still produce a standard 4:2:0 stream.
TEST_P(RecordedMp4, CanBeReadBack)
{
    SKIP_WITHOUT_GSTREAMER();
    const PixelFormat format = GetParam();
    const auto path = std::filesystem::temp_directory_path() /
                      ("scip_test_roundtrip_" + std::string(scip::toString(format)) + ".mp4");
    // 160x128: the i.MX VPU encoder accepts 64x64 .. 1920x1088 only.
    const FrameInfo info{160, 128, format};

    scip::FrameSinkPtr sink;
    try {
        // 25.004 like real recordings: outputs round to whole fps (the VPU needs it).
        sink = scip::io::openSink(path.string(), info, 25.004);
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()).find("no H.264 encoder") != std::string::npos) {
            GTEST_SKIP() << e.what();
        }
        throw;
    }
    const Frame frame = scip::io::makeTestPattern(info);
    for (int i = 0; i < 12; ++i) {
        sink->write(frame);
    }
    sink->close();

    scip::io::SourceOptions options;
    options.gray = format == PixelFormat::Gray8;
    auto source = scip::io::openSource(path.string(), options);
    EXPECT_EQ(source->info(), info);
    EXPECT_NEAR(source->frameRate(), 25.0, 0.01);
    EXPECT_EQ(drain(*source), 12);
    source.reset();
    std::filesystem::remove(path);
}

INSTANTIATE_TEST_SUITE_P(GstSink,
                         RecordedMp4,
                         ::testing::Values(PixelFormat::Bgr8, PixelFormat::Gray8),
                         [](const auto& param) {
                             return std::string(scip::toString(param.param));
                         });

TEST(GstSource, LumaPlaneOfNv12IsGray)
{
    // Decoders output NV12/I420; with --gray-input we use the Y plane directly.
    SKIP_WITHOUT_GSTREAMER();
    for (const char* format : {"NV12", "I420"}) {
        auto source = scip::io::openSource(testSource(2, format), {});
        EXPECT_EQ(source->info(), (FrameInfo{64, 48, PixelFormat::Gray8})) << format;
        EXPECT_EQ(drain(*source), 2) << format;
    }
}

TEST(GstSource, BgrxIsConvertedToBgr)
{
    SKIP_WITHOUT_GSTREAMER();
    auto bgrx = scip::io::openSource(
        "gst:videotestsrc num-buffers=1 pattern=smpte ! video/x-raw,format=BGRx,width=64,height=48 "
        "! appsink name=scip_sink",
        {});
    auto bgr = scip::io::openSource(
        "gst:videotestsrc num-buffers=1 pattern=smpte ! video/x-raw,format=BGR,width=64,height=48 "
        "! appsink name=scip_sink",
        {});
    ASSERT_EQ(bgrx->info(), (FrameInfo{64, 48, PixelFormat::Bgr8}));
    Frame a;
    Frame b;
    ASSERT_TRUE(bgrx->read(a));
    ASSERT_TRUE(bgr->read(b));
    for (int y = 0; y < 48; ++y) {
        ASSERT_EQ(std::memcmp(a.row(y), b.row(y), a.info().rowBytes()), 0) << "row " << y;
    }
}

TEST(GstSource, LoopsAtTheEnd)
{
    SKIP_WITHOUT_GSTREAMER();
    const auto path = std::filesystem::temp_directory_path() / "scip_test_loop.mp4";
    const FrameInfo info{160, 128, PixelFormat::Bgr8};
    try {
        auto sink = scip::io::openSink(path.string(), info, 25.0);
        const Frame frame = scip::io::makeTestPattern(info);
        for (int i = 0; i < 3; ++i) {
            sink->write(frame);
        }
        sink->close();
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << "cannot record a test file here: " << e.what();
    }

    scip::io::SourceOptions options;
    options.loop = true;
    options.maxFrames = 8;  // the file has 3: needs two restarts
    auto source = scip::io::openSource(path.string(), options);
    EXPECT_EQ(drain(*source), 8);
    source.reset();
    std::filesystem::remove(path);
}

TEST(GstSource, LiveSourceWaitsForASender)
{
    SKIP_WITHOUT_GSTREAMER();
    auto source = scip::io::openSource("rtp://127.0.0.1:47391", {});
    EXPECT_TRUE(source->isLive());
    EXPECT_EQ(source->info().width, 0);  // unknown until the first frame
    Frame frame;
    EXPECT_EQ(source->tryRead(frame, std::chrono::milliseconds(50)), scip::ReadStatus::Timeout);
    EXPECT_THROW(scip::io::openSource("rtp://:99999", {}), std::invalid_argument);
}

TEST(GstSource, ReceivesRtpFromAUdpOutput)
{
    // The two ends the GUI and the target use: udp: output -> rtp:// input.
    SKIP_WITHOUT_GSTREAMER();
    auto source = scip::io::openSource("rtp://127.0.0.1:47392", {});
    const FrameInfo info{160, 128, PixelFormat::Bgr8};
    scip::FrameSinkPtr sink;
    try {
        sink = scip::io::openSink("udp:127.0.0.1:47392", info, 30.0);
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()).find("no H.264 encoder") != std::string::npos) {
            GTEST_SKIP() << e.what();
        }
        throw;
    }
    const Frame pattern = scip::io::makeTestPattern(info);
    Frame received;
    int frames = 0;
    for (int i = 0; i < 300 && frames < 5; ++i) {  // up to ~3 s
        sink->write(pattern);
        if (source->tryRead(received, std::chrono::milliseconds(10)) == scip::ReadStatus::Frame) {
            ++frames;
        }
    }
    sink->close();
    ASSERT_GE(frames, 5);
    EXPECT_EQ(received.width(), 160);
    EXPECT_EQ(received.height(), 128);
    EXPECT_EQ(source->info(), received.info());
}

namespace {

/// Frames to `port` the way the host GUI's raw mode sends them (GDP over TCP).
scip::FrameSinkPtr rawTcpSender(int port, const FrameInfo& info)
{
    return scip::io::openSink("gst:appsrc name=scip_src ! gdppay ! tcpclientsink host=127.0.0.1 "
                              "port=" +
                                  std::to_string(port),
                              info,
                              30.0);
}

/// Send `frame` until the source has delivered `wanted` frames (or ~3 s).
int sendAndReceive(scip::FrameSink& sink,
                   scip::FrameSource& source,
                   const Frame& frame,
                   int wanted,
                   Frame& received)
{
    int frames = 0;
    for (int i = 0; i < 300 && frames < wanted; ++i) {
        sink.write(frame);
        if (source.tryRead(received, std::chrono::milliseconds(10)) == scip::ReadStatus::Frame) {
            ++frames;
        }
    }
    return frames;
}

}  // namespace

TEST(GstSource, ReceivesRawFramesOverTcpLosslessly)
{
    SKIP_WITHOUT_GSTREAMER();
    if (!scip::io::hasGstElement("gdppay") || !scip::io::hasGstElement("tcpserversrc")) {
        GTEST_SKIP() << "needs gdppay and tcpserversrc";
    }
    auto source = scip::io::openSource("tcp://127.0.0.1:47396", {});
    EXPECT_TRUE(source->isLive());

    const FrameInfo info{160, 128, PixelFormat::Bgr8};
    const Frame pattern = scip::io::makeTestPattern(info);
    Frame received;
    {
        auto sink = rawTcpSender(47396, info);
        ASSERT_GE(sendAndReceive(*sink, *source, pattern, 3, received), 3);
        sink->close();
    }
    ASSERT_EQ(received.info(), info);
    for (int y = 0; y < info.height; ++y) {  // raw: every byte as sent
        ASSERT_EQ(std::memcmp(received.row(y), pattern.row(y), info.rowBytes()), 0) << "row " << y;
    }

    // The sender went away; the next one (another size) is accepted once the
    // source has noticed and listens again (a sender retries meanwhile).
    const FrameInfo other{96, 64, PixelFormat::Bgr8};
    scip::FrameSinkPtr sink;
    for (int attempt = 0; attempt < 50 && !sink; ++attempt) {
        try {
            sink = rawTcpSender(47396, other);
        } catch (const std::runtime_error&) {
            source->tryRead(received, std::chrono::milliseconds(20));
        }
    }
    ASSERT_TRUE(sink);
    ASSERT_GE(sendAndReceive(*sink, *source, scip::io::makeTestPattern(other), 3, received), 3);
    sink->close();
    EXPECT_EQ(received.info(), other);
    EXPECT_THROW(scip::io::openSource("tcp://:0", {}), std::invalid_argument);
}

namespace {

/// Positions of up to `limit` frames (5 s at most), fewer once `stop` is true.
template <typename Stop>
std::vector<double> playFor(scip::FrameSource& source, int limit, Stop stop)
{
    std::vector<double> positions;
    Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (static_cast<int>(positions.size()) < limit && !stop() &&
           std::chrono::steady_clock::now() < deadline) {
        const auto status = source.tryRead(frame, std::chrono::milliseconds(50));
        EXPECT_NE(status, scip::ReadStatus::End) << "a player never ends";
        if (status == scip::ReadStatus::Frame) {
            positions.push_back(source.playback()->state().position);
        }
    }
    return positions;
}

}  // namespace

TEST(GstSource, PlaysAFileLikeAPlayer)
{
    SKIP_WITHOUT_GSTREAMER();
    const auto path = std::filesystem::temp_directory_path() / "scip_test_player.mp4";
    const FrameInfo info{160, 128, PixelFormat::Bgr8};
    try {
        auto sink = scip::io::openSink(path.string(), info, 25.0);
        const Frame frame = scip::io::makeTestPattern(info);
        for (int i = 0; i < 50; ++i) {  // 2 s
            sink->write(frame);
        }
        sink->close();
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << "cannot record a test file here: " << e.what();
    }

    scip::io::SourceOptions options;
    options.player = true;  // not realtime: frames come as fast as they decode
    auto source = scip::io::openSource(path.string(), options);
    scip::Playback* player = source->playback();
    ASSERT_NE(player, nullptr);
    EXPECT_EQ(scip::io::openSource(path.string(), {})->playback(), nullptr);

    // Paused on the first frame; nothing more until it plays.
    Frame frame;
    ASSERT_EQ(source->tryRead(frame, std::chrono::milliseconds(500)), scip::ReadStatus::Frame);
    EXPECT_TRUE(player->state().paused);
    EXPECT_NEAR(player->state().duration, 2.0, 0.1);
    EXPECT_EQ(source->tryRead(frame, std::chrono::milliseconds(50)), scip::ReadStatus::Timeout);

    // A seek while paused delivers the frame it lands on, once.
    player->seek(1.0);
    ASSERT_EQ(source->tryRead(frame, std::chrono::milliseconds(500)), scip::ReadStatus::Frame);
    EXPECT_NEAR(player->state().position, 1.0, 0.05);
    EXPECT_EQ(source->tryRead(frame, std::chrono::milliseconds(50)), scip::ReadStatus::Timeout);

    // Playing to the end: it stops there, paused, and stays open.
    player->setPaused(false);
    const auto played = playFor(*source, 100, [&] { return player->state().ended; });
    ASSERT_TRUE(player->state().ended);
    EXPECT_TRUE(player->state().paused);
    ASSERT_GE(played.size(), 20U);
    EXPECT_NEAR(played.front(), 1.04, 0.05);  // no repeat of the paused frame
    EXPECT_NEAR(played.back(), 1.96, 0.05);

    // An A-B loop stays in its range; play starts again from A.
    player->setLoop(false, 0.4, 0.8);
    player->setPaused(false);
    const auto looped = playFor(*source, 40, [] { return false; });
    ASSERT_EQ(looped.size(), 40U);  // 10 frames per pass: several loops
    for (double position : looped) {
        EXPECT_GE(position, 0.39);
        EXPECT_LE(position, 0.81);
    }
    EXPECT_FALSE(player->state().ended);
    EXPECT_THROW(player->setLoop(false, 0.8, 0.4), std::invalid_argument);
    EXPECT_THROW(player->setRate(0.0), std::invalid_argument);
    source.reset();
    std::filesystem::remove(path);
}

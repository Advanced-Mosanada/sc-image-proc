// GStreamer sources and sinks. Uses only elements from gst-plugins-base / -good
// (videotestsrc, fakesink...) so the tests run on the PC, under QEMU and on the board.

#include "scip/io.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

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

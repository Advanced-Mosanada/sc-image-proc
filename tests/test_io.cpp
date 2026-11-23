#include "scip/cv.hpp"
#include "scip/io.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;

TEST(Sources, PatternSourceProducesCountFrames)
{
    auto source = scip::io::openSource("pattern:64x32", 3);
    EXPECT_EQ(source->info(), (FrameInfo{64, 32, PixelFormat::Bgr8}));

    Frame frame;
    int frames = 0;
    while (source->read(frame)) {
        EXPECT_EQ(frame.meta.sequence, static_cast<std::uint64_t>(frames));
        ++frames;
    }
    EXPECT_EQ(frames, 3);
}

TEST(Sources, PatternGrayOption)
{
    auto source = scip::io::openSource("pattern:16x16:gray", 1);
    EXPECT_EQ(source->info().format, PixelFormat::Gray8);
}

TEST(Sources, RejectsBadUris)
{
    EXPECT_THROW(scip::io::openSource("pattern:big", 1), std::invalid_argument);
    EXPECT_THROW(scip::io::openSource("pattern:16x16:rgb", 1), std::invalid_argument);
    EXPECT_THROW(scip::io::openSource("/no/such/file.png", 1), std::runtime_error);
}

TEST(ImageFile, PngRoundTripIsLossless)
{
    const Frame original = scip::io::makeTestPattern(FrameInfo{40, 30, PixelFormat::Bgr8});
    const auto path = std::filesystem::temp_directory_path() / "scip_test_roundtrip.png";

    scip::io::writeImage(path.string(), original);
    const Frame loaded = scip::io::readImage(path.string());
    std::filesystem::remove(path);

    ASSERT_EQ(loaded.info(), original.info());
    EXPECT_EQ(cv::norm(scip::asMat(loaded), scip::asMat(original), cv::NORM_INF), 0.0);
}

// Every stage is checked against OpenCV as the reference implementation.
// Accelerated versions (NEON, OpenCL, G2D) will be checked the same way.

#include "scip/cv.hpp"
#include "scip/io.hpp"
#include "scip/stages.hpp"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

#include <stdexcept>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;

namespace {

/// Run a single stage the way the pipeline does: configure, allocate, process.
Frame runStage(scip::Stage& stage, const Frame& input)
{
    Frame output(stage.configure(input.info()));
    stage.process(input, output);
    return output;
}

double maxAbsDiff(const Frame& a, const Frame& b)
{
    return cv::norm(scip::asMat(a), scip::asMat(b), cv::NORM_INF);
}

Frame randomFrame(const FrameInfo& info)
{
    Frame frame(info);
    cv::Mat view = scip::asMat(frame);
    cv::theRNG().state = 1234;  // deterministic
    cv::randu(view, cv::Scalar::all(0), cv::Scalar::all(256));
    return frame;
}

}  // namespace

TEST(ToGray, MatchesOpenCvWithinOneLevel)
{
    // Odd width on purpose: catches loops that assume multiples of 8 or 16.
    const Frame input = randomFrame(FrameInfo{333, 77, PixelFormat::Bgr8});
    auto stage = scip::stages::makeToGray();
    const Frame ours = runStage(*stage, input);

    cv::Mat reference;
    cv::cvtColor(scip::asMat(input), reference, cv::COLOR_BGR2GRAY);

    EXPECT_EQ(ours.format(), PixelFormat::Gray8);
    EXPECT_LE(cv::norm(scip::asMat(ours), reference, cv::NORM_INF), 1.0);
}

TEST(ToGray, GrayPixelsStayExact)
{
    Frame input(FrameInfo{256, 1, PixelFormat::Bgr8});
    for (int x = 0; x < 256; ++x) {
        for (int c = 0; c < 3; ++c) {
            input.row(0)[x * 3 + c] = static_cast<std::uint8_t>(x);
        }
    }
    auto stage = scip::stages::makeToGray();
    const Frame ours = runStage(*stage, input);
    for (int x = 0; x < 256; ++x) {
        ASSERT_EQ(ours.row(0)[x], x) << "at x=" << x;
    }
}

TEST(ToGray, GrayInputIsCopied)
{
    const Frame input = randomFrame(FrameInfo{50, 20, PixelFormat::Gray8});
    auto stage = scip::stages::makeToGray();
    EXPECT_EQ(maxAbsDiff(runStage(*stage, input), input), 0.0);
}

TEST(GaussianBlur, MatchesOpenCv)
{
    const Frame input = scip::io::makeTestPattern(FrameInfo{160, 90, PixelFormat::Bgr8});
    auto stage = scip::stages::makeGaussianBlur(5);
    const Frame ours = runStage(*stage, input);

    cv::Mat reference;
    cv::GaussianBlur(scip::asMat(input), reference, cv::Size(5, 5), 0, 0, cv::BORDER_REFLECT_101);
    EXPECT_EQ(cv::norm(scip::asMat(ours), reference, cv::NORM_INF), 0.0);
}

TEST(GaussianBlur, RejectsEvenKernel)
{
    EXPECT_THROW(scip::stages::makeGaussianBlur(4), std::invalid_argument);
    EXPECT_THROW(scip::stages::makeGaussianBlur(1), std::invalid_argument);
}

TEST(Resize, ProducesRequestedSize)
{
    const Frame input = scip::io::makeTestPattern(FrameInfo{64, 48, PixelFormat::Bgr8});
    auto stage = scip::stages::makeResize(32, 24);
    const Frame ours = runStage(*stage, input);
    EXPECT_EQ(ours.info(), (FrameInfo{32, 24, PixelFormat::Bgr8}));
}

TEST(Factory, CreatesStagesFromSpecs)
{
    EXPECT_EQ(scip::stages::create("gray")->name(), "gray");
    EXPECT_EQ(scip::stages::create(" blur:7 ")->name(), "blur:7");
    EXPECT_EQ(scip::stages::create("blur:7:1.5")->name(), "blur:7:1.5");
    EXPECT_EQ(scip::stages::create("resize:640x480")->name(), "resize:640x480");

    const auto chain = scip::stages::createChain("resize:320x240, gray ,blur:3");
    ASSERT_EQ(chain.size(), 3u);
    EXPECT_EQ(chain[1]->name(), "gray");
}

TEST(Factory, RejectsBadSpecs)
{
    EXPECT_THROW(scip::stages::create("nope"), std::invalid_argument);
    EXPECT_THROW(scip::stages::create("gray:1"), std::invalid_argument);
    EXPECT_THROW(scip::stages::create("blur"), std::invalid_argument);
    EXPECT_THROW(scip::stages::create("blur:abc"), std::invalid_argument);
    EXPECT_THROW(scip::stages::create("resize:640"), std::invalid_argument);
    EXPECT_THROW(scip::stages::createChain("gray,,blur:3"), std::invalid_argument);
}

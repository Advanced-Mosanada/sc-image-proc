// Every stage is checked against OpenCV as the reference implementation.
// Accelerated versions (NEON, OpenCL, G2D) will be checked the same way.

#include "scip/cv.hpp"
#include "scip/io.hpp"
#include "scip/registry.hpp"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

#include <stdexcept>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;

namespace {

/// Create one stage from pipeline text, through the registry like every app does.
scip::StagePtr make(const std::string& text)
{
    auto stages = scip::StageRegistry::instance().createChain(text);
    EXPECT_EQ(stages.size(), 1u);
    return std::move(stages.front());
}

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
    auto stage = make("gray");
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
    auto stage = make("gray");
    const Frame ours = runStage(*stage, input);
    for (int x = 0; x < 256; ++x) {
        ASSERT_EQ(ours.row(0)[x], x) << "at x=" << x;
    }
}

TEST(ToGray, GrayInputIsCopied)
{
    const Frame input = randomFrame(FrameInfo{50, 20, PixelFormat::Gray8});
    auto stage = make("gray");
    EXPECT_EQ(maxAbsDiff(runStage(*stage, input), input), 0.0);
}

TEST(GaussianBlur, MatchesOpenCv)
{
    const Frame input = scip::io::makeTestPattern(FrameInfo{160, 90, PixelFormat::Bgr8});
    auto stage = make("blur ksize=5");
    const Frame ours = runStage(*stage, input);

    cv::Mat reference;
    cv::GaussianBlur(scip::asMat(input), reference, cv::Size(5, 5), 0, 0, cv::BORDER_REFLECT_101);
    EXPECT_EQ(cv::norm(scip::asMat(ours), reference, cv::NORM_INF), 0.0);
}

TEST(GaussianBlur, RejectsEvenKernel)
{
    EXPECT_THROW(make("blur ksize=4"), std::invalid_argument);
    EXPECT_THROW(make("blur ksize=1"), std::invalid_argument);
}

TEST(Resize, ProducesRequestedSize)
{
    const Frame input = scip::io::makeTestPattern(FrameInfo{64, 48, PixelFormat::Bgr8});
    auto stage = make("resize width=32 height=24");
    const Frame ours = runStage(*stage, input);
    EXPECT_EQ(ours.info(), (FrameInfo{32, 24, PixelFormat::Bgr8}));
}

TEST(BuiltinStages, AreRegisteredWithDescriptions)
{
    const auto& registry = scip::StageRegistry::instance();
    for (const char* name : {"gray", "blur", "resize"}) {
        ASSERT_TRUE(registry.contains(name)) << name;
        const scip::StageInfo info = registry.info(name);
        EXPECT_FALSE(info.title.empty()) << name;
        EXPECT_FALSE(info.description.empty()) << name;
        EXPECT_EQ(info.backend, "native") << name;
    }
}

TEST(BuiltinStages, ChainFromText)
{
    const auto chain = scip::StageRegistry::instance().createChain(
        "resize width=320 height=240 ! gray ! blur ksize=3 sigma=0.8");
    ASSERT_EQ(chain.size(), 3u);
    EXPECT_EQ(chain[0]->name(), "resize height=240 width=320");
    EXPECT_EQ(chain[1]->name(), "gray");
    EXPECT_EQ(chain[2]->name(), "blur ksize=3 sigma=0.8");
}

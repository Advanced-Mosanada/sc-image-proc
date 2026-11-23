// GStreamer filters as stages: discovery, parameter introspection, results.

#include "scip/cv.hpp"
#include "scip/gst_stages.hpp"
#include "scip/io.hpp"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <stdexcept>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;
using scip::StageRegistry;

namespace {

#define SKIP_WITHOUT(element)                                \
    if (!registry().contains(element)) {                     \
        GTEST_SKIP() << element << " is not available here"; \
    }

StageRegistry& registry()
{
    static StageRegistry instance;
    static const int added = scip::gst::registerStages(instance);
    (void)added;
    return instance;
}

Frame run(const std::string& text, const Frame& input)
{
    auto stages = registry().createChain(text);
    EXPECT_EQ(stages.size(), 1u);
    Frame output(stages.front()->configure(input.info()));
    stages.front()->process(input, output);
    return output;
}

const scip::ParamSpec* findParam(const scip::StageInfo& info, const std::string& name)
{
    for (const auto& p : info.params) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace

TEST(GstStages, RegistersChainAndInstalledElements)
{
    if (!scip::io::hasGStreamer()) {
        GTEST_SKIP() << "built without GStreamer";
    }
    EXPECT_TRUE(registry().contains("gst"));
    EXPECT_EQ(registry().info("gst").backend, "gstreamer");
}

TEST(GstStages, ParametersComeFromElementProperties)
{
    SKIP_WITHOUT("videoflip");
    const scip::StageInfo flip = registry().info("videoflip");
    const scip::ParamSpec* method = findParam(flip, "method");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->type, scip::ParamType::Enum);
    EXPECT_NE(std::find(method->choices.begin(), method->choices.end(), "clockwise"),
              method->choices.end());
    EXPECT_EQ(findParam(flip, "name"), nullptr);  // hidden

    SKIP_WITHOUT("gamma");
    const scip::ParamSpec* gamma = findParam(registry().info("gamma"), "gamma");
    ASSERT_NE(gamma, nullptr);
    EXPECT_EQ(gamma->type, scip::ParamType::Float);
    EXPECT_TRUE(gamma->hasRange());
}

TEST(GstStages, FlipMatchesOpenCv)
{
    SKIP_WITHOUT("videoflip");
    const Frame input = scip::io::makeTestPattern(FrameInfo{64, 48, PixelFormat::Bgr8});
    const Frame flipped = run("videoflip method=horizontal-flip", input);
    cv::Mat reference;
    cv::flip(scip::asMat(input), reference, 1);
    EXPECT_EQ(cv::norm(scip::asMat(flipped), reference, cv::NORM_INF), 0.0);
}

TEST(GstStages, RotationChangesSize)
{
    SKIP_WITHOUT("videoflip");
    const Frame input = scip::io::makeTestPattern(FrameInfo{64, 48, PixelFormat::Gray8});
    const Frame rotated = run("videoflip method=clockwise", input);
    EXPECT_EQ(rotated.info(), (FrameInfo{48, 64, PixelFormat::Gray8}));
}

TEST(GstStages, ChainOfElements)
{
    SKIP_WITHOUT("gst");
    const Frame input = scip::io::makeTestPattern(FrameInfo{64, 48, PixelFormat::Bgr8});
    const Frame output =
        run("gst pipeline=\"videoflip method=horizontal-flip ! videoflip method=horizontal-flip\"",
            input);
    EXPECT_EQ(cv::norm(scip::asMat(output), scip::asMat(input), cv::NORM_INF), 0.0);
}

TEST(GstStages, BadChainsFail)
{
    SKIP_WITHOUT("gst");
    const Frame input(FrameInfo{16, 16, PixelFormat::Gray8});
    EXPECT_THROW(run("gst pipeline=\"no-such-element\"", input), std::runtime_error);
    EXPECT_THROW(registry().createChain("gst pipeline=\" \""), std::invalid_argument);
}

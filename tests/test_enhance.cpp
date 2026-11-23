// Contrast, noise reduction (spatial and temporal) and sharpening functions.
// Each test checks the property the function is for, on synthetic images where
// the right answer is known.

#include "scip/cv.hpp"
#include "scip/io.hpp"
#include "scip/registry.hpp"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>

#include <stdexcept>
#include <string>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;

namespace {

scip::StagePtr make(const std::string& text)
{
    auto stages = scip::StageRegistry::instance().createChain(text);
    EXPECT_EQ(stages.size(), 1u);
    return std::move(stages.front());
}

/// Configure and process one frame (stages with history keep it between calls).
Frame run(scip::Stage& stage, const Frame& input)
{
    Frame output(stage.configure(input.info()));
    stage.process(input, output);
    return output;
}

/// Process without configuring again: the next frame of the same stream.
Frame next(scip::Stage& stage, const Frame& input)
{
    Frame output(input.info());
    stage.process(input, output);
    return output;
}

Frame uniform(const FrameInfo& info, int value)
{
    Frame frame(info);
    frame.fill(static_cast<std::uint8_t>(value));
    return frame;
}

/// `value` plus Gaussian noise of standard deviation `sigma`.
Frame noisy(const FrameInfo& info, int value, double sigma, int seed)
{
    Frame frame(info);
    cv::Mat view = scip::asMat(frame);
    cv::RNG rng(static_cast<std::uint64_t>(seed));
    cv::Mat noise(view.size(), CV_32FC(view.channels()));
    rng.fill(noise, cv::RNG::NORMAL, value, sigma);
    noise.convertTo(view, CV_8U);
    return frame;
}

/// Left half `left`, right half `right`.
Frame step(const FrameInfo& info, int left, int right)
{
    Frame frame = uniform(info, left);
    cv::Mat view = scip::asMat(frame);
    view.colRange(info.width / 2, info.width).setTo(cv::Scalar::all(right));
    return frame;
}

double deviation(const cv::Mat& image)
{
    cv::Scalar mean;
    cv::Scalar stddev;
    cv::meanStdDev(image, mean, stddev);
    return stddev[0];
}

double mean(const cv::Mat& image)
{
    return cv::mean(image)[0];
}

const char* const kNewFunctions[] = {
    "equalize",
    "autolevels",
    "levels",
    "retinex",
    "clahe",
};

}  // namespace

// --- all of them ------------------------------------------------------------

TEST(Enhance, EveryFunctionTakesGrayAndColourOfAnySize)
{
    const auto& registry = scip::StageRegistry::instance();
    for (const char* name : kNewFunctions) {
        const scip::StageInfo info = registry.info(name);
        EXPECT_FALSE(info.title.empty()) << name;
        EXPECT_FALSE(info.description.empty()) << name;
        for (const PixelFormat format : {PixelFormat::Gray8, PixelFormat::Bgr8}) {
            const FrameInfo frameInfo{37, 21, format};  // odd sizes catch loop bugs
            auto stage = make(name);
            const Frame input = noisy(frameInfo, 120, 20.0, 1);
            Frame output;
            ASSERT_NO_THROW(output = run(*stage, input)) << name;
            EXPECT_EQ(output.info(), frameInfo) << name;
            ASSERT_NO_THROW(next(*stage, input)) << name << ": second frame";
        }
    }
}

// --- contrast ---------------------------------------------------------------

TEST(Contrast, EqualizeMatchesOpenCvAndKeepsHue)
{
    const Frame gray = noisy(FrameInfo{64, 48, PixelFormat::Gray8}, 100, 10.0, 2);
    cv::Mat reference;
    cv::equalizeHist(scip::asMat(gray), reference);
    EXPECT_EQ(cv::norm(scip::asMat(run(*make("equalize"), gray)), reference, cv::NORM_INF), 0.0);

    // Colour: the brightness is equalised, the colour (Cr, Cb) stays. A ramp of
    // one tint; chroma is compared on the mid-tones (near black or white the
    // colour is clipped by any brightness change).
    Frame colour(FrameInfo{200, 4, PixelFormat::Bgr8});
    for (int x = 0; x < 200; ++x) {
        const int v = 60 + x / 2;
        scip::asMat(colour).col(x).setTo(cv::Scalar(v - 20, v, v + 25));
    }
    const Frame result = run(*make("equalize"), colour);
    cv::Mat before;
    cv::Mat after;
    cv::cvtColor(scip::asMat(colour), before, cv::COLOR_BGR2YCrCb);
    cv::cvtColor(scip::asMat(result), after, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> b;
    std::vector<cv::Mat> a;
    cv::split(before, b);
    cv::split(after, a);
    EXPECT_GT(cv::norm(b[0], a[0], cv::NORM_INF), 40.0);  // brightness spread out
    cv::Mat midTones;
    cv::inRange(a[0], 50, 205, midTones);
    ASSERT_GT(cv::countNonZero(midTones), 200);
    cv::Mat crChange;
    cv::Mat cbChange;
    cv::absdiff(b[1], a[1], crChange);
    cv::absdiff(b[2], a[2], cbChange);
    double worst = 0;
    cv::minMaxLoc(crChange, nullptr, &worst, nullptr, nullptr, midTones);
    EXPECT_LE(worst, 3.0);  // hue kept (rounding only)
    cv::minMaxLoc(cbChange, nullptr, &worst, nullptr, nullptr, midTones);
    EXPECT_LE(worst, 3.0);
}

TEST(Contrast, ClaheWorksOnColourToo)
{
    const Frame gray = noisy(FrameInfo{64, 48, PixelFormat::Gray8}, 100, 10.0, 3);
    cv::Mat reference;
    cv::createCLAHE(3.0, cv::Size(4, 4))->apply(scip::asMat(gray), reference);
    EXPECT_EQ(
        cv::norm(scip::asMat(run(*make("clahe clip=3 tiles=4"), gray)), reference, cv::NORM_INF),
        0.0);
    EXPECT_NO_THROW(run(*make("clahe"), noisy(FrameInfo{64, 48, PixelFormat::Bgr8}, 100, 10, 3)));
}

TEST(Contrast, LevelsMapBlackWhiteAndGamma)
{
    Frame ramp(FrameInfo{256, 1, PixelFormat::Gray8});
    for (int x = 0; x < 256; ++x) {
        ramp.row(0)[x] = static_cast<std::uint8_t>(x);
    }
    const Frame out = run(*make("levels black=50 white=200"), ramp);
    EXPECT_EQ(out.row(0)[30], 0);
    EXPECT_EQ(out.row(0)[50], 0);
    EXPECT_EQ(out.row(0)[125], 128);
    EXPECT_EQ(out.row(0)[200], 255);
    EXPECT_EQ(out.row(0)[230], 255);

    const Frame brighter = run(*make("levels gamma=2"), ramp);
    EXPECT_GT(brighter.row(0)[64], 64 + 40);  // mid-tones lifted

    EXPECT_THROW(make("levels black=200 white=100"), std::invalid_argument);
}

TEST(Contrast, AutolevelsStretchesTheUsedRange)
{
    // Values only between 100 and 150: a flat-looking picture.
    Frame dull(FrameInfo{100, 10, PixelFormat::Gray8});
    for (int x = 0; x < 100; ++x) {
        scip::asMat(dull).col(x).setTo(100 + x / 2);
    }
    auto stage = make("autolevels low=0 high=0");
    const Frame outFrame = run(*stage, dull);
    const cv::Mat out = scip::asMat(outFrame);
    double lo = 0;
    double hi = 0;
    cv::minMaxLoc(out, &lo, &hi);
    EXPECT_LE(lo, 1.0);
    EXPECT_GE(hi, 254.0);

    // With smoothing, a sudden change of range moves the limits only halfway:
    // after a full-range frame the dull one is stretched less than before.
    auto smooth = make("autolevels low=0 high=0 smooth=0.5");
    run(*smooth, dull);
    next(*smooth, step(FrameInfo{100, 10, PixelFormat::Gray8}, 0, 255));
    const Frame afterFrame = next(*smooth, dull);
    const cv::Mat after = scip::asMat(afterFrame);
    cv::minMaxLoc(after, &lo, &hi);
    EXPECT_GT(lo, 10.0);
    EXPECT_LT(hi, 245.0);
}

TEST(Contrast, RetinexEvensOutUnevenLighting)
{
    // A fine texture under lighting that goes from dark (left) to bright (right).
    Frame scene(FrameInfo{256, 64, PixelFormat::Gray8});
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 256; ++x) {
            const double light = 0.2 + 0.8 * x / 255.0;
            const double texture = ((x / 4 + y / 4) % 2) ? 200 : 120;
            scene.row(y)[x] = static_cast<std::uint8_t>(light * texture);
        }
    }
    const cv::Mat in = scip::asMat(scene);
    const Frame outFrame = run(*make("retinex sigma=20 strength=1"), scene);
    const cv::Mat out = scip::asMat(outFrame);
    const double before = std::abs(mean(in.colRange(0, 64)) - mean(in.colRange(192, 256)));
    const double after = std::abs(mean(out.colRange(0, 64)) - mean(out.colRange(192, 256)));
    EXPECT_LT(after, before * 0.5);
    EXPECT_GT(deviation(out.colRange(0, 64)), deviation(in.colRange(0, 64)));  // shadow detail up
}

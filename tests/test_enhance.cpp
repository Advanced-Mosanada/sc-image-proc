// Contrast, noise reduction (spatial and temporal) and sharpening functions.
// Each test checks the property the function is for, on synthetic images where
// the right answer is known.

#include "scip/cv.hpp"
#include "scip/io.hpp"
#include "scip/registry.hpp"

#include <gtest/gtest.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

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
    "median",
    "bilateral",
    "guided",
    "nlmeans",
    "temporal_mean",
    "tnr",
    "unsharp",
    "sharpen",
    "temporal_median",
    "detail",
    "dde",
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

// --- spatial noise reduction ----------------------------------------------------

TEST(Denoise, MedianRemovesImpulseNoise)
{
    Frame frame = uniform(FrameInfo{64, 48, PixelFormat::Gray8}, 128);
    for (int y = 2; y < 48; y += 7) {
        for (int x = 3; x < 64; x += 9) {
            frame.row(y)[x] = (x + y) % 2 ? 255 : 0;  // isolated hot and dead pixels
        }
    }
    const Frame out = run(*make("median ksize=3"), frame);
    EXPECT_EQ(cv::countNonZero(scip::asMat(out) != 128), 0);
}

TEST(Denoise, BilateralMatchesOpenCv)
{
    const Frame input = noisy(FrameInfo{40, 30, PixelFormat::Bgr8}, 120, 15.0, 4);
    cv::Mat reference;
    cv::bilateralFilter(scip::asMat(input), reference, 5, 30.0, 5.0, cv::BORDER_REFLECT_101);
    const Frame out = run(*make("bilateral diameter=5 sigma_color=30 sigma_space=5"), input);
    EXPECT_EQ(cv::norm(scip::asMat(out), reference, cv::NORM_INF), 0.0);
}

TEST(Denoise, EdgePreservingFiltersSmoothNoiseButKeepEdges)
{
    // A step from 50 to 200 with noise of sigma 8 on top.
    const FrameInfo info{80, 40, PixelFormat::Gray8};
    Frame input = step(info, 50, 200);
    cv::Mat view = scip::asMat(input);
    cv::Mat withNoise;
    view.convertTo(withNoise, CV_16S);
    cv::Mat noise;
    scip::asMat(noisy(info, 128, 8.0, 5)).convertTo(noise, CV_16S, 1.0, -128.0);
    withNoise += noise;
    withNoise.convertTo(view, CV_8U);

    for (const char* filter : {"guided radius=4 eps=0.01", "bilateral", "nlmeans strength=10"}) {
        const Frame outFrame = run(*make(filter), input);
        const cv::Mat out = scip::asMat(outFrame);
        EXPECT_LT(deviation(out.colRange(5, 30)), deviation(view.colRange(5, 30)) * 0.6) << filter;
        // The edge stays a step: both sides keep their level.
        EXPECT_NEAR(mean(out.colRange(5, 30)), 50.0, 4.0) << filter;
        EXPECT_NEAR(mean(out.colRange(50, 75)), 200.0, 4.0) << filter;
        EXPECT_GT(mean(out.colRange(41, 43)) - mean(out.colRange(37, 39)), 120.0) << filter;
    }
}

TEST(Denoise, NlMeansMatchesOpenCv)
{
    const Frame input = noisy(FrameInfo{48, 32, PixelFormat::Gray8}, 100, 10.0, 6);
    cv::Mat reference;
    cv::fastNlMeansDenoising(scip::asMat(input), reference, 5.0f, 7, 11);
    const Frame out = run(*make("nlmeans strength=5 search=11"), input);
    EXPECT_EQ(cv::norm(scip::asMat(out), reference, cv::NORM_INF), 0.0);
}

// --- temporal noise reduction -----------------------------------------------------

TEST(Temporal, MeanAveragesOverFrames)
{
    const FrameInfo info{16, 8, PixelFormat::Bgr8};
    auto stage = make("temporal_mean alpha=0.5");
    EXPECT_EQ(mean(scip::asMat(run(*stage, uniform(info, 100)))), 100.0);
    EXPECT_NEAR(mean(scip::asMat(next(*stage, uniform(info, 200)))), 150.0, 0.5);
    EXPECT_NEAR(mean(scip::asMat(next(*stage, uniform(info, 200)))), 175.0, 0.5);
    // configure() starts a new history.
    EXPECT_EQ(mean(scip::asMat(run(*stage, uniform(info, 10)))), 10.0);
}

TEST(Temporal, MedianDropsFlickerWithoutAveraging)
{
    const FrameInfo info{16, 8, PixelFormat::Gray8};
    auto three = make("temporal_median frames=3");
    EXPECT_EQ(mean(scip::asMat(run(*three, uniform(info, 10)))), 10.0);   // no history yet
    next(*three, uniform(info, 200));                                     // a flash
    EXPECT_EQ(mean(scip::asMat(next(*three, uniform(info, 12)))), 12.0);  // median(10, 200, 12)

    auto five = make("temporal_median frames=5");
    run(*five, uniform(info, 5));
    for (const int v : {250, 7, 6}) {
        next(*five, uniform(info, v));
    }
    EXPECT_EQ(mean(scip::asMat(next(*five, uniform(info, 251)))), 7.0);  // median of 5
}

TEST(Temporal, TnrRemovesNoiseButFollowsMotion)
{
    const FrameInfo info{64, 48, PixelFormat::Bgr8};
    auto stage = make("tnr strength=0.8 noise=10");  // noise sigma 4: differences mostly < 10
    run(*stage, noisy(info, 128, 4.0, 100));
    cv::Mat out;
    for (int i = 1; i < 30; ++i) {
        out = scip::asMat(next(*stage, noisy(info, 128, 4.0, 100 + i))).clone();
    }
    EXPECT_LT(deviation(out), 4.0 * 0.5);  // still scene: at most half the noise

    // Something moves in: a change far above the noise level is taken at once.
    const Frame moved = uniform(info, 228);
    EXPECT_EQ(cv::norm(scip::asMat(next(*stage, moved)), scip::asMat(moved), cv::NORM_INF), 0.0);
}

TEST(Temporal, TnrReachesSmallChanges)
{
    // An 8-bit history rounded every blend away: with strength 0.8 the output
    // stayed at 100 for ever when the scene went to 102.
    const FrameInfo info{16, 8, PixelFormat::Gray8};
    auto stage = make("tnr strength=0.8 noise=10");
    run(*stage, uniform(info, 100));
    cv::Mat out;
    for (int i = 0; i < 40; ++i) {
        out = scip::asMat(next(*stage, uniform(info, 102))).clone();
    }
    EXPECT_EQ(mean(out), 102.0);
}

namespace {

/// A textured scene seen through a camera that pans `step` pixels per frame
/// to the right, plus fresh noise of `sigma` in every frame.
Frame panned(const cv::Mat& scene, int frame, int step, double sigma, int seed)
{
    const cv::Rect view(frame * step, 0, 192, 128);
    Frame out(FrameInfo{view.width, view.height, PixelFormat::Gray8});
    cv::Mat noise(view.size(), CV_32F);
    cv::RNG rng(static_cast<std::uint64_t>(seed));
    rng.fill(noise, cv::RNG::NORMAL, 0.0, sigma);
    cv::Mat sum;
    scene(view).convertTo(sum, CV_32F);
    sum += noise;
    sum.convertTo(scip::asMat(out), CV_8U);
    return out;
}

}  // namespace

TEST(Temporal, TnrAlignFollowsAPanningCamera)
{
    cv::Mat scene(128, 192 + 30 * 4, CV_8U);
    cv::RNG(5).fill(scene, cv::RNG::UNIFORM, 0, 256);
    cv::GaussianBlur(scene, scene, cv::Size(), 4.0);  // structures of ~10 px, as in real scenes
    cv::normalize(scene, scene, 30, 220, cv::NORM_MINMAX);
    const double sigma = 4.0;
    for (const bool align : {false, true}) {
        auto stage = make(std::string("tnr window=3 noise=6 align=") + (align ? "true" : "false"));
        run(*stage, panned(scene, 0, 4, sigma, 1000));
        double error = 0.0;
        for (int i = 1; i < 30; ++i) {
            const Frame out = next(*stage, panned(scene, i, 4, sigma, 1000 + i));
            if (i == 29) {
                cv::Mat clean;
                scene(cv::Rect(i * 4, 0, 192, 128)).convertTo(clean, CV_32F);
                cv::Mat result;
                scip::asMat(out).convertTo(result, CV_32F);
                const cv::Mat error32 = result - clean;
                error = deviation(error32(cv::Rect(8, 8, 160, 112)));  // away from the new edge
            }
        }
        if (align) {
            EXPECT_LT(error, sigma * 0.6) << "aligned: averaged although the scene moves";
        } else {
            EXPECT_GT(error, sigma * 0.6) << "not aligned: motion, so nothing is averaged";
        }
    }
}

TEST(Temporal, RepeatedFramesAreNotNewSamples)
{
    const FrameInfo info{16, 8, PixelFormat::Gray8};
    // Without skip_repeats the median of [200, 200(repeat), 12] is the repeat.
    auto plain = make("temporal_median frames=3");
    run(*plain, uniform(info, 10));
    next(*plain, uniform(info, 200));
    next(*plain, uniform(info, 200));
    EXPECT_EQ(mean(scip::asMat(next(*plain, uniform(info, 12)))), 200.0);

    auto skip = make("temporal_median frames=3 skip_repeats=true");
    run(*skip, uniform(info, 10));
    next(*skip, uniform(info, 200));
    EXPECT_EQ(mean(scip::asMat(next(*skip, uniform(info, 200)))), 200.0);  // the last output again
    EXPECT_EQ(mean(scip::asMat(next(*skip, uniform(info, 12)))), 12.0);    // median(10, 200, 12)

    // The recursive filters give exactly the previous output for a repeat.
    for (const char* name : {"tnr skip_repeats=true", "temporal_mean skip_repeats=true"}) {
        auto stage = make(name);
        run(*stage, noisy(info, 100, 3.0, 1));
        const Frame b = noisy(info, 101, 3.0, 2);
        const cv::Mat first = scip::asMat(next(*stage, b)).clone();
        EXPECT_EQ(cv::norm(scip::asMat(next(*stage, b)), first, cv::NORM_INF), 0.0) << name;
    }
}

// --- sharpening -----------------------------------------------------------------

TEST(Sharpen, UnsharpMatchesTheFormulaAndRespectsTheThreshold)
{
    const Frame input = noisy(FrameInfo{40, 30, PixelFormat::Gray8}, 120, 20.0, 7);
    cv::Mat blurred;
    cv::GaussianBlur(scip::asMat(input), blurred, cv::Size(), 1.5, 1.5, cv::BORDER_REFLECT_101);
    cv::Mat reference;
    cv::addWeighted(scip::asMat(input), 2.0, blurred, -1.0, 0.0, reference);
    const Frame out = run(*make("unsharp amount=1 sigma=1.5"), input);
    EXPECT_EQ(cv::norm(scip::asMat(out), reference, cv::NORM_INF), 0.0);

    // Small noise below the threshold is left exactly as it was.
    const Frame grain = noisy(FrameInfo{40, 30, PixelFormat::Gray8}, 120, 1.0, 8);
    const Frame kept = run(*make("unsharp amount=2 threshold=10"), grain);
    EXPECT_EQ(cv::norm(scip::asMat(kept), scip::asMat(grain), cv::NORM_INF), 0.0);
}

TEST(Sharpen, EdgesGetSteeperFlatAreasStay)
{
    const Frame edge = step(FrameInfo{40, 10, PixelFormat::Gray8}, 100, 150);
    for (const char* sharpener : {"sharpen amount=1",
                                  "sharpen amount=1 neighbours=8",
                                  "unsharp amount=1",
                                  "detail gain=3 radius=4 eps=0.1"}) {
        const Frame out = run(*make(sharpener), edge);
        EXPECT_LT(out.row(5)[19], 100) << sharpener;  // undershoot before the edge
        EXPECT_GT(out.row(5)[20], 150) << sharpener;  // overshoot after it
        EXPECT_EQ(out.row(5)[2], 100) << sharpener;   // flat areas unchanged
        EXPECT_EQ(out.row(5)[37], 150) << sharpener;
    }
}

TEST(Contrast, DdeNeutralSettingsChangeNothing)
{
    for (const PixelFormat format : {PixelFormat::Gray8, PixelFormat::Bgr8}) {
        const Frame input = noisy(FrameInfo{41, 33, format}, 120, 20.0, 11);
        const Frame out = run(*make("dde gain=1 fine_gain=1 noise=0 contrast=0"), input);
        // Gray: the layers add up to the input exactly; colour: YCrCb round trip.
        EXPECT_LE(cv::norm(scip::asMat(out), scip::asMat(input), cv::NORM_INF),
                  format == PixelFormat::Gray8 ? 0.0 : 2.0);
    }
}

TEST(Contrast, DdeSmoothsNoiseAndKeepsStrongEdgesWithoutHalos)
{
    // Flat area with noise below the `noise` level: comes out smoother.
    const Frame flat = noisy(FrameInfo{64, 48, PixelFormat::Gray8}, 120, 2.0, 12);
    const Frame calm = run(*make("dde contrast=0 gain=2 noise=4"), flat);
    EXPECT_LT(deviation(scip::asMat(calm)), deviation(scip::asMat(flat)) * 0.7);

    // A strong edge (150 levels, far above sqrt(eps)*255 = 25) is not boosted:
    // little ring, where the same boost from unsharp overshoots by ~40 levels.
    const Frame edge = step(FrameInfo{64, 16, PixelFormat::Gray8}, 50, 200);
    const auto ring = [&](const char* text) {
        double low = 0.0;
        double high = 0.0;
        cv::minMaxLoc(scip::asMat(run(*make(text), edge)).row(8), &low, &high);
        return std::max(50.0 - low, high - 200.0);
    };
    EXPECT_LE(ring("dde contrast=0 gain=3 fine_gain=2 noise=0 eps=0.01"), 150 * 0.1);
    EXPECT_GT(ring("unsharp amount=1 sigma=1"), 150 * 0.2);
}

TEST(Contrast, DdeLocalContrastLikeClaheWithoutBanding)
{
    // A slow gradient (a dark sky): 8 levels over 128 pixels, plus a bright
    // object so that the gradient's levels are a small part of the histogram.
    const FrameInfo info{128, 64, PixelFormat::Gray8};
    Frame frame(info);
    cv::Mat view = scip::asMat(frame);
    for (int x = 0; x < info.width; ++x) {
        view.col(x).setTo(cv::Scalar(20 + x / 16));
    }
    view(cv::Rect(96, 40, 32, 24)).setTo(cv::Scalar(200));
    const cv::Mat row = view.row(10);  // inside the gradient

    // cv::CLAHE stretches each 1-level step into a big one: visible bands.
    cv::Mat clahe;
    cv::createCLAHE(3.0, cv::Size(2, 2))->apply(view, clahe);
    cv::Mat steps;
    cv::absdiff(clahe.row(10).colRange(1, 96), clahe.row(10).colRange(0, 95), steps);
    double claheStep = 0.0;
    cv::minMaxLoc(steps, nullptr, &claheStep);

    // dde (radius 8 averages the gradient's steps into a ramp) applies the same
    // contrast to the unrounded base: as much contrast, in small steps.
    const Frame out = run(*make("dde radius=8 eps=0.01 contrast=1 clip=3 tiles=2 gain=1 fine_gain=1 "
                                "noise=8"),
                          frame);
    const cv::Mat result = scip::asMat(out).row(10);
    cv::absdiff(result.colRange(1, 96), result.colRange(0, 95), steps);
    double ddeStep = 0.0;
    cv::minMaxLoc(steps, nullptr, &ddeStep);
    const double claheRange = clahe.at<std::uint8_t>(10, 90) - clahe.at<std::uint8_t>(10, 4);
    const double ddeRange = result.at<std::uint8_t>(90) - result.at<std::uint8_t>(4);
    EXPECT_GE(claheStep, 4.0) << "the banding this guards against";
    EXPECT_LE(ddeStep, 2.0);
    EXPECT_GT(ddeRange, 0.6 * claheRange) << "contrast raised like CLAHE's";
    EXPECT_GT(ddeRange, 1.3 * (row.at<std::uint8_t>(90) - row.at<std::uint8_t>(4))) << "raised";
}

TEST(Sharpen, DetailWithGainOneChangesNothing)
{
    const Frame input = noisy(FrameInfo{40, 30, PixelFormat::Bgr8}, 120, 20.0, 9);
    const Frame out = run(*make("detail gain=1"), input);
    EXPECT_LE(cv::norm(scip::asMat(out), scip::asMat(input), cv::NORM_INF), 1.0);
    const Frame boosted = run(*make("detail gain=3"), input);
    EXPECT_GT(deviation(scip::asMat(boosted)), deviation(scip::asMat(input)));
}

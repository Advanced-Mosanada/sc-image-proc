#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace scip::stages {

namespace {

/// Single-scale Retinex: divides the image by an estimate of its illumination
/// (a wide blur), in the log domain. Evens out uneven lighting and lifts
/// shadows while keeping local contrast: dynamic range compression.
///
/// The illumination is smooth, so it is estimated on a reduced image and
/// scaled back: a wide blur stays cheap on the board.
class Retinex final : public Stage {
public:
    Retinex(double sigma, double strength) : sigma_(sigma), strength_(strength)
    {
        logLut_.create(1, 256, CV_32F);
        for (int v = 0; v < 256; ++v) {
            logLut_.at<float>(v) = std::log(float(v) + 1.0f);
        }
    }

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        luma_.apply(
            asMat(in), dst, [this](const cv::Mat& src, cv::Mat& result) { enhance(src, result); });
    }

private:
    void enhance(const cv::Mat& src, cv::Mat& result)
    {
        // Illumination: blur on an image reduced so the blur's sigma is ~8 px.
        const double factor = std::max(1.0, sigma_ / 8.0);
        const cv::Size small(std::max(1, int(src.cols / factor)),
                             std::max(1, int(src.rows / factor)));
        cv::resize(src, small_, small, 0, 0, cv::INTER_AREA);
        cv::GaussianBlur(small_, small_, cv::Size(), sigma_ / factor);
        cv::resize(small_, illumination_, src.size(), 0, 0, cv::INTER_LINEAR);

        // Reflectance = log(I) - log(L); spread mean +- 3 sigma over 0..255.
        cv::LUT(src, logLut_, logImage_);
        cv::LUT(illumination_, logLut_, logIllumination_);
        cv::subtract(logImage_, logIllumination_, reflectance_);
        cv::Scalar mean;
        cv::Scalar deviation;
        cv::meanStdDev(reflectance_, mean, deviation);
        const double span = std::max(6.0 * deviation[0], 1e-6);
        reflectance_.convertTo(enhanced_, CV_8U, 255.0 / span, 127.5 - mean[0] * 255.0 / span);
        cv::addWeighted(src, 1.0 - strength_, enhanced_, strength_, 0.0, result);
    }

    double sigma_;
    double strength_;
    cv::Mat logLut_;
    cv::Mat small_, illumination_, logImage_, logIllumination_, reflectance_, enhanced_;
    LumaProcessor luma_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "retinex";
    info.title = "Retinex (uneven lighting)";
    info.description = "Removes uneven illumination and lifts shadows while keeping local "
                       "contrast (single-scale Retinex on the brightness)";
    info.category = "contrast";
    info.params = {
        param::number("sigma", "Size of the illumination estimate", 40.0, 5.0, 200.0, 1.0, "px"),
        param::number("strength", "Blend with the original (1 = full effect)", 0.7, 0.0, 1.0, 0.05),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Retinex>(getFloat(p, "sigma"), getFloat(p, "strength"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

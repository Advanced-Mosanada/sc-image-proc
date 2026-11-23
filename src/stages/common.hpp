#pragma once

// Building blocks shared by several functions (internal to scip_stages).

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace scip::stages {

/// Runs a single-channel operation on the brightness of a frame: a gray frame
/// directly, a colour frame through its luma (Y of YCrCb), so contrast changes
/// keep the colours' hue instead of shifting them channel by channel.
///
/// `op(src, dst)` gets an 8-bit single-channel image and writes `dst` of the
/// same size and type. Scratch buffers are kept between frames.
class LumaProcessor {
public:
    template <typename Op>
    void apply(const cv::Mat& in, cv::Mat& out, Op&& op)
    {
        if (in.channels() == 1) {
            std::forward<Op>(op)(in, out);
            return;
        }
        cv::cvtColor(in, ycrcb_, cv::COLOR_BGR2YCrCb);
        cv::extractChannel(ycrcb_, luma_, 0);
        std::forward<Op>(op)(luma_, lumaOut_);
        cv::insertChannel(lumaOut_, ycrcb_, 0);
        cv::cvtColor(ycrcb_, out, cv::COLOR_YCrCb2BGR);
    }

    /// The luma of `in` (a view for gray frames), e.g. to compute a histogram.
    const cv::Mat& luma(const cv::Mat& in)
    {
        if (in.channels() == 1) {
            luma_ = in;
        } else {
            cv::cvtColor(in, luma_, cv::COLOR_BGR2GRAY);
        }
        return luma_;
    }

private:
    cv::Mat ycrcb_;
    cv::Mat luma_;
    cv::Mat lumaOut_;
};

/// Self-guided filter (He, Sun, Tang: "Guided Image Filtering", 2010): smooths
/// flat areas and keeps edges, at a cost independent of the radius (box filters).
/// Works on 8-bit images of any channel count (each channel guides itself);
/// `eps` is in normalised units (0..1 range squared), e.g. 0.01.
///
/// Speed, for the board's CPU:
/// - "fast guided filter" (He, Sun, 2015): the linear coefficients a, b of
///   q = a * I + b are smooth, so they are computed on an image reduced by
///   `subsample()`, then scaled back up;
/// - the full-resolution work is one fused pass, from 8-bit input to 8-bit
///   output.
/// On the A53 the float passes are limited by memory bandwidth (~2-3 ms per
/// pass over a 720p float image): a radius of 8 or more (4x reduction) keeps
/// it near 25 ms for 720p gray. Faster needs one fused row-streaming loop.
///
/// `gain` turns the smoother into a detail enhancer: out = q + gain * (I - q),
/// which is again linear in I, so it costs nothing extra. gain 0 = smoothed.
class GuidedFilter {
public:
    /// Reduction factor for a radius: the reduced radius stays >= 2 pixels.
    static int subsample(int radius) { return std::clamp(radius / 2, 1, 4); }

    void apply(const cv::Mat& src, cv::Mat& dst, int radius, double eps, double gain = 0.0)
    {
        CV_Assert(src.depth() == CV_8U && dst.type() == src.type() && dst.size() == src.size());
        const int factor = subsample(radius);
        if (factor > 1) {
            const cv::Size reduced(std::max(1, src.cols / factor), std::max(1, src.rows / factor));
            cv::resize(src, small8_, reduced, 0, 0, cv::INTER_AREA);
            small8_.convertTo(guide_, CV_32F);
        } else {
            src.convertTo(guide_, CV_32F);
        }
        const int r = std::max(1, radius / factor);
        const double e = eps * 255.0 * 255.0;
        const cv::Size window(2 * r + 1, 2 * r + 1);
        cv::boxFilter(guide_, mean_, CV_32F, window);
        cv::multiply(guide_, guide_, square_);
        cv::boxFilter(square_, meanSquare_, CV_32F, window);
        cv::multiply(mean_, mean_, variance_);
        cv::subtract(meanSquare_, variance_, variance_);  // var = E[I^2] - E[I]^2
        cv::add(variance_, cv::Scalar::all(e), denominator_);
        cv::divide(variance_, denominator_, a_);  // a = var / (var + eps)
        cv::multiply(a_, mean_, b_);
        cv::subtract(mean_, b_, b_);  // b = mean - a * mean
        cv::boxFilter(a_, a_, CV_32F, window);
        cv::boxFilter(b_, b_, CV_32F, window);
        if (gain != 0.0) {
            // q + gain * (I - q) = (gain + (1 - gain) * a) * I + (1 - gain) * b
            a_.convertTo(a_, -1, 1.0 - gain, gain);
            b_.convertTo(b_, -1, 1.0 - gain);
        }
        if (factor > 1) {
            cv::resize(a_, aFull_, src.size(), 0, 0, cv::INTER_LINEAR);
            cv::resize(b_, bFull_, src.size(), 0, 0, cv::INTER_LINEAR);
        } else {
            aFull_ = a_;
            bFull_ = b_;
        }
        // out = a * I + b, straight from and to 8 bits.
        const int count = src.cols * src.channels();
        for (int y = 0; y < src.rows; ++y) {
            const auto* in = src.ptr<std::uint8_t>(y);
            const auto* a = aFull_.ptr<float>(y);
            const auto* b = bFull_.ptr<float>(y);
            auto* out = dst.ptr<std::uint8_t>(y);
            for (int i = 0; i < count; ++i) {
                // Clamp and round without lrintf, so the loop vectorises.
                const float v = std::min(std::max(a[i] * float(in[i]) + b[i] + 0.5f, 0.0f), 255.0f);
                out[i] = static_cast<std::uint8_t>(v);
            }
        }
    }

private:
    cv::Mat small8_, guide_, mean_, square_, meanSquare_, variance_, denominator_, a_, b_;
    cv::Mat aFull_, bFull_;
};

/// A 256-entry lookup table, filled by `f(v)` for v = 0..255 (clamped to 0..255).
template <typename F>
cv::Mat makeLut(F&& f)
{
    cv::Mat lut(1, 256, CV_8U);
    for (int v = 0; v < 256; ++v) {
        lut.at<std::uint8_t>(v) = cv::saturate_cast<std::uint8_t>(f(v));
    }
    return lut;
}

}  // namespace scip::stages

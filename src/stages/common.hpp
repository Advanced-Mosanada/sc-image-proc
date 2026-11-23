#pragma once

// Building blocks shared by several functions (internal to scip_stages).

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

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

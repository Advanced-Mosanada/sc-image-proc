#include "common.hpp"
#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace scip::stages {

namespace {

/// Motion-adaptive temporal noise reduction, as camera ISPs do it: a recursive
/// filter that blends each pixel with its previous output. Changes up to the
/// `noise` level are taken as noise and blended with full `strength`; above
/// that the blend fades out, and changes of twice the noise level or more are
/// taken as motion and not blended at all. Still areas get the noise
/// reduction of a long average; moving objects stay sharp.
///
/// - The history is kept in 8.8 fixed point (16 bits). With 8 bits, a
///   recursive blend rounds every small change away: with strength 0.8 the
///   output stays up to 2 levels from the input for ever, which shows as
///   blotches and smeared low-contrast detail.
/// - The change is averaged over a `window` x `window` neighbourhood before it
///   is compared with `noise`: noise averages out, motion does not, so a low
///   threshold separates them (fewer trails, same noise reduction).
/// - `align` follows a moving camera: the shift between frames is measured
///   (phase correlation on a 1/4 size image, then refined to the pixel by a
///   +-2 px search at full size on a sparse grid) and the history is moved
///   with it before blending, so a panning scene is still averaged.
/// - `skip_repeats`: an exact repeat of the previous input (frame-rate
///   conversion, stalled source) gives the previous output again instead of
///   averaging the same noise twice.
///
/// The blend weight per difference comes from a table, and the blend is in
/// fixed point: cheap enough for the board's CPU.
class Tnr final : public Stage {
public:
    Tnr(double strength, int noise, int window, bool align, bool skipRepeats)
        : window_(window), align_(align), skipRepeats_(skipRepeats)
    {
        for (int d = 0; d < 256; ++d) {
            const double motion = std::clamp(double(d) / noise - 1.0, 0.0, 1.0);  // 0..1
            const double w = strength * (1.0 - motion);
            weight_[std::size_t(d)] = std::uint16_t(w * 256.0 + 0.5);  // 0..256 = 0..1
        }
    }

    FrameInfo configure(const FrameInfo& input) override
    {
        first_ = true;
        repeats_.reset();
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        const cv::Mat src = asMat(in);
        cv::Mat dst = asMat(out);
        const bool repeat = skipRepeats_ && repeats_.isRepeat(src);
        if (repeat && !first_ && lastOut_.size() == src.size() && lastOut_.type() == src.type()) {
            lastOut_.copyTo(dst);
            return;
        }
        if (first_ || history_.size() != src.size() || history_.channels() != src.channels()) {
            src.convertTo(history_, CV_16U, 256.0);
            src.copyTo(dst);
            if (align_) {
                measureShift(src);  // keeps the reduced frame for the next one
            }
            first_ = false;
        } else {
            if (align_) {
                const cv::Point shift = measureShift(src);
                if (shift != cv::Point(0, 0)) {
                    moveHistory(src, shift);
                }
            }
            if (src.channels() == 1) {
                blend<1>(src, dst);
            } else {
                blend<3>(src, dst);
            }
        }
        if (skipRepeats_) {
            dst.copyTo(lastOut_);
        }
    }

private:
    static constexpr int kReduction = 4;           // phase correlation on a 1/4 size image
    static constexpr double kMinResponse = 0.08;   // below: no reliable shift (cut, flat scene)

    /// Shift of the scene from the previous frame to this one, in full-size
    /// pixels, or (0, 0) when it cannot be measured reliably (scene cut, no
    /// structure). Coarse: phase correlation at 1/4 size, whose sub-pixel
    /// estimate is off by up to ~1 px there (4 px at full size); fine: the
    /// best of the +-2 px around it by the sum of absolute differences.
    cv::Point measureShift(const cv::Mat& src)
    {
        const cv::Mat* gray = &src;
        if (src.channels() != 1) {
            cv::cvtColor(src, gray_, cv::COLOR_BGR2GRAY);
            gray = &gray_;
        }
        const cv::Size reduced(src.cols / kReduction, src.rows / kReduction);
        if (reduced.width < 16 || reduced.height < 16) {
            return {0, 0};
        }
        cv::resize(*gray, small8_, reduced, 0, 0, cv::INTER_AREA);
        small8_.convertTo(small_, CV_32F);
        cv::Point shift(0, 0);
        if (!previousSmall_.empty() && previousSmall_.size() == small_.size()) {
            if (hann_.size() != small_.size()) {
                cv::createHanningWindow(hann_, small_.size(), CV_32F);
            }
            double response = 0.0;
            const cv::Point2d d = cv::phaseCorrelate(previousSmall_, small_, hann_, &response);
            const cv::Point coarse(int(std::lround(d.x * kReduction)), int(std::lround(d.y * kReduction)));
            if (response >= kMinResponse && std::abs(coarse.x) < src.cols / 4 &&
                std::abs(coarse.y) < src.rows / 4) {
                shift = refine(previousGray_, *gray, coarse);
            }
        }
        std::swap(previousSmall_, small_);
        gray->copyTo(previousGray_);
        return shift;
    }

    /// The shift within +-kRefine of `coarse` with the smallest mean absolute
    /// difference between `current` and the shifted `previous`, on every 4th
    /// pixel of every 4th row. Keeps `coarse` unless another is clearly better.
    static cv::Point refine(const cv::Mat& previous, const cv::Mat& current, cv::Point coarse)
    {
        constexpr int kRefine = 2;
        constexpr int kStep = 4;
        const int mx = std::abs(coarse.x) + kRefine + 1;
        const int my = std::abs(coarse.y) + kRefine + 1;
        if (current.cols <= 2 * mx + kStep || current.rows <= 2 * my + kStep) {
            return coarse;
        }
        const auto cost = [&](cv::Point s) {
            long sum = 0;
            for (int y = my; y < current.rows - my; y += kStep) {
                const std::uint8_t* c = current.ptr<std::uint8_t>(y);
                const std::uint8_t* p = previous.ptr<std::uint8_t>(y - s.y) - s.x;
                for (int x = mx; x < current.cols - mx; x += kStep) {
                    sum += std::abs(int(c[x]) - int(p[x]));
                }
            }
            return sum;
        };
        cv::Point best = coarse;
        long bestCost = cost(coarse);
        const long keep = bestCost - bestCost / 64;  // another must be > ~1.5 % better
        long candidateCost = keep;
        for (int dy = -kRefine; dy <= kRefine; ++dy) {
            for (int dx = -kRefine; dx <= kRefine; ++dx) {
                const cv::Point s(coarse.x + dx, coarse.y + dy);
                if (s == coarse) {
                    continue;
                }
                const long c = cost(s);
                if (c < candidateCost) {
                    candidateCost = c;
                    best = s;
                }
            }
        }
        return best;
    }

    /// Moves the history by `shift` (scene content at p moves to p + shift);
    /// the part that comes into view has no history: it starts from `src`.
    void moveHistory(const cv::Mat& src, cv::Point shift)
    {
        src.convertTo(moved_, CV_16U, 256.0);
        const int w = src.cols - std::abs(shift.x);
        const int h = src.rows - std::abs(shift.y);
        const cv::Rect from(std::max(-shift.x, 0), std::max(-shift.y, 0), w, h);
        const cv::Rect to(std::max(shift.x, 0), std::max(shift.y, 0), w, h);
        history_(from).copyTo(moved_(to));
        std::swap(history_, moved_);
    }

    /// The channel count is a template parameter so the compiler can unroll
    /// the per-channel loops (3x faster than a run-time count on the A53).
    template <int Channels>
    void blend(const cv::Mat& src, cv::Mat& dst)
    {
        // 1. Change per pixel: the largest of any channel, so colours move together.
        // (The size is copied to a local: writes through uint8_t* may alias
        // members, which would keep the compiler from vectorising this loop.)
        const int cols = src.cols;
        difference_.create(src.size(), CV_8U);
        for (int y = 0; y < src.rows; ++y) {
            const std::uint8_t* s = src.ptr<std::uint8_t>(y);
            const std::uint16_t* p = history_.ptr<std::uint16_t>(y);
            std::uint8_t* d = difference_.ptr<std::uint8_t>(y);
            for (int x = 0; x < cols; ++x, s += Channels, p += Channels) {
                int difference = 0;
                for (int c = 0; c < Channels; ++c) {
                    difference = std::max(difference, std::abs(int(s[c]) - ((p[c] + 128) >> 8)));
                }
                d[x] = std::uint8_t(std::min(difference, 255));
            }
        }
        // 2. Averaged over the window: noise averages out, motion stays.
        const cv::Mat* change = &difference_;
        if (window_ > 1) {
            cv::blur(difference_, averaged_, cv::Size(window_, window_), cv::Point(-1, -1),
                     cv::BORDER_REPLICATE);
            change = &averaged_;
        }
        // 3. Blend in 8.8 fixed point; the output is the history rounded to 8 bits.
        for (int y = 0; y < src.rows; ++y) {
            const std::uint8_t* s = src.ptr<std::uint8_t>(y);
            const std::uint8_t* d = change->ptr<std::uint8_t>(y);
            std::uint16_t* p = history_.ptr<std::uint16_t>(y);
            std::uint8_t* o = dst.ptr<std::uint8_t>(y);
            for (int x = 0; x < cols; ++x, s += Channels, p += Channels, o += Channels) {
                const std::uint32_t w = weight_[d[x]];
                for (int c = 0; c < Channels; ++c) {
                    const std::uint32_t v =
                        (std::uint32_t(s[c]) * 256u * (256u - w) + std::uint32_t(p[c]) * w + 128u) >> 8;
                    p[c] = std::uint16_t(v);  // recursive: the history of the next frame
                    o[c] = std::uint8_t((v + 128u) >> 8);
                }
            }
        }
    }

    std::array<std::uint16_t, 256> weight_{};
    int window_;
    bool align_;
    bool skipRepeats_;
    bool first_ = true;
    cv::Mat history_;  // CV_16UC(n): value * 256
    cv::Mat moved_, difference_, averaged_, lastOut_;
    cv::Mat gray_, small8_, small_, previousSmall_, previousGray_, hann_;
    RepeatDetector repeats_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "tnr";
    info.title = "Motion-adaptive temporal denoise";
    info.description = "Averages over time where nothing moves, not where it does: strong "
                       "noise reduction without motion trails; can follow a panning camera";
    info.category = "temporal";
    info.params = {
        param::number("strength", "Weight of the history on still pixels", 0.8, 0.0, 0.95, 0.05),
        param::integer("noise",
                       "Changes up to this many levels (averaged over the window) are noise; "
                       "twice that is motion",
                       10,
                       1,
                       64),
        param::integer("window", "Neighbourhood the change is averaged over (1 = per pixel)", 1, 1, 7, 2, "px"),
        param::boolean("align", "Follow camera motion (global shift) before blending", false),
        param::boolean("skip_repeats", "An exact repeat of the last frame gives the last output", false),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Tnr>(getFloat(p, "strength"),
                                     static_cast<int>(getInt(p, "noise")),
                                     static_cast<int>(getInt(p, "window")),
                                     getBool(p, "align"),
                                     getBool(p, "skip_repeats"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <algorithm>
#include <array>
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
/// The blend weight per difference comes from a table, and the blend is in
/// 8-bit fixed point: cheap enough for HD on the board's CPU.
class Tnr final : public Stage {
public:
    Tnr(double strength, int noise)
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
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        const cv::Mat src = asMat(in);
        cv::Mat dst = asMat(out);
        if (first_ || previous_.size() != src.size() || previous_.type() != src.type()) {
            src.copyTo(previous_);
            src.copyTo(dst);
            first_ = false;
            return;
        }
        if (src.channels() == 1) {
            blend<1>(src, dst);
        } else {
            blend<3>(src, dst);
        }
    }

private:
    /// The channel count is a template parameter so the compiler can unroll
    /// the per-channel loops (3x faster than a run-time count on the A53).
    template <int Channels>
    void blend(const cv::Mat& src, cv::Mat& dst)
    {
        for (int y = 0; y < src.rows; ++y) {
            const std::uint8_t* s = src.ptr<std::uint8_t>(y);
            std::uint8_t* p = previous_.ptr<std::uint8_t>(y);
            std::uint8_t* o = dst.ptr<std::uint8_t>(y);
            for (int x = 0; x < src.cols; ++x, s += Channels, p += Channels, o += Channels) {
                // Motion = the largest change of any channel, so colours move together.
                int difference = 0;
                for (int c = 0; c < Channels; ++c) {
                    difference = std::max(difference, std::abs(int(s[c]) - int(p[c])));
                }
                const unsigned w = weight_[std::size_t(difference)];
                for (int c = 0; c < Channels; ++c) {
                    const auto v = std::uint8_t((s[c] * (256u - w) + p[c] * w + 128u) >> 8);
                    o[c] = v;
                    p[c] = v;  // recursive: the output is the next frame's history
                }
            }
        }
    }

    std::array<std::uint16_t, 256> weight_{};
    bool first_ = true;
    cv::Mat previous_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "tnr";
    info.title = "Motion-adaptive temporal denoise";
    info.description = "Averages over time where nothing moves, not where it does: strong "
                       "noise reduction without motion trails";
    info.category = "temporal";
    info.params = {
        param::number("strength", "Weight of the history on still pixels", 0.8, 0.0, 0.95, 0.05),
        param::integer(
            "noise", "Changes up to this many levels are noise; twice that is motion", 10, 1, 64),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Tnr>(getFloat(p, "strength"), static_cast<int>(getInt(p, "noise")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <array>
#include <memory>

namespace scip::stages {

namespace {

/// Automatic contrast stretch: finds the darkest and brightest levels (ignoring
/// the extreme `low` / `high` percent of pixels, so a few hot pixels do not
/// decide) and stretches that range to 0..255.
///
/// On video the limits are smoothed over time (`smooth`), otherwise the
/// picture would flicker as the histogram changes from frame to frame.
class AutoLevels final : public Stage {
public:
    AutoLevels(double low, double high, double smooth) : low_(low), high_(high), smooth_(smooth) {}

    FrameInfo configure(const FrameInfo& input) override
    {
        first_ = true;  // new stream or format: no history to smooth with
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        const cv::Mat src = asMat(in);
        const cv::Mat& luma = luma_.luma(src);

        std::array<int, 256> histogram{};
        for (int y = 0; y < luma.rows; ++y) {
            const auto* row = luma.ptr<std::uint8_t>(y);
            for (int x = 0; x < luma.cols; ++x) {
                ++histogram[row[x]];
            }
        }
        const double total = double(luma.total());
        const double lo = level(histogram, total * low_ / 100.0, false);
        const double hi = level(histogram, total * high_ / 100.0, true);

        if (first_) {
            black_ = lo;
            white_ = hi;
            first_ = false;
        } else {
            black_ = smooth_ * black_ + (1.0 - smooth_) * lo;
            white_ = smooth_ * white_ + (1.0 - smooth_) * hi;
        }
        const double range = std::max(white_ - black_, 1.0);  // flat image: no division by 0
        const cv::Mat lut = makeLut([&](int v) { return (v - black_) * 255.0 / range + 0.5; });
        cv::Mat dst = asMat(out);
        cv::LUT(src, lut, dst);
    }

private:
    /// The level below which (or, from the top, above which) `count` pixels lie.
    static double level(const std::array<int, 256>& histogram, double count, bool fromTop)
    {
        double seen = 0.0;
        for (int i = 0; i < 256; ++i) {
            const int v = fromTop ? 255 - i : i;
            seen += histogram[static_cast<std::size_t>(v)];
            if (seen > count) {
                return v;
            }
        }
        return fromTop ? 0.0 : 255.0;
    }

    double low_;
    double high_;
    double smooth_;
    bool first_ = true;
    double black_ = 0.0;
    double white_ = 255.0;
    LumaProcessor luma_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "autolevels";
    info.title = "Auto contrast stretch";
    info.description = "Stretches the used brightness range to 0..255, ignoring the extreme "
                       "percent of pixels; limits smoothed over time against flicker";
    info.category = "contrast";
    info.params = {
        param::number(
            "low", "Percent of darkest pixels allowed to clip to black", 1.0, 0.0, 20.0, 0.1, "%"),
        param::number("high",
                      "Percent of brightest pixels allowed to clip to white",
                      1.0,
                      0.0,
                      20.0,
                      0.1,
                      "%"),
        param::number(
            "smooth", "Temporal smoothing of the limits (0 = per frame)", 0.9, 0.0, 0.99, 0.01),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<AutoLevels>(
            getFloat(p, "low"), getFloat(p, "high"), getFloat(p, "smooth"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

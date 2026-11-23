#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Unsharp mask, the classic sharpener: adds `amount` times the detail
/// (image minus its blur) back. `sigma` sets which detail size is boosted;
/// `threshold` leaves differences below it alone, so flat noisy areas are not
/// sharpened into grain.
class Unsharp final : public Stage {
public:
    Unsharp(double amount, double sigma, int threshold)
        : amount_(amount), sigma_(sigma), threshold_(threshold)
    {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        const cv::Mat src = asMat(in);
        cv::Mat dst = asMat(out);
        cv::GaussianBlur(src, blurred_, cv::Size(), sigma_, sigma_, cv::BORDER_REFLECT_101);
        // src + amount * (src - blurred), saturated to 0..255
        cv::addWeighted(src, 1.0 + amount_, blurred_, -amount_, 0.0, dst);
        if (threshold_ > 0) {
            cv::absdiff(src, blurred_, difference_);
            cv::compare(difference_, cv::Scalar::all(threshold_), keep_, cv::CMP_LE);
            src.copyTo(dst, keep_);
        }
    }

private:
    double amount_;
    double sigma_;
    int threshold_;
    cv::Mat blurred_, difference_, keep_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "unsharp";
    info.title = "Unsharp mask";
    info.description = "Sharpens by adding back the detail lost in a blur; threshold spares noise";
    info.category = "sharpen";
    info.params = {
        param::number("amount", "Strength", 1.0, 0.0, 5.0, 0.05),
        param::number("sigma", "Size of the detail to boost", 1.0, 0.3, 10.0, 0.1, "px"),
        param::integer("threshold", "Leave differences up to this many levels alone", 0, 0, 64),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Unsharp>(
            getFloat(p, "amount"), getFloat(p, "sigma"), static_cast<int>(getInt(p, "threshold")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

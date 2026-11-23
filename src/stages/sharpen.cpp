#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>
#include <string>

namespace scip::stages {

namespace {

/// Laplacian sharpening with one 3x3 kernel: subtracts `amount` times the
/// second derivative. The cheapest sharpener; boosts the finest detail (and
/// noise) only.
class Sharpen final : public Stage {
public:
    Sharpen(double amount, const std::string& neighbours)
    {
        const auto a = float(amount);
        if (neighbours == "8") {
            kernel_ = (cv::Mat_<float>(3, 3) << -a, -a, -a, -a, 1 + 8 * a, -a, -a, -a, -a);
        } else {
            kernel_ = (cv::Mat_<float>(3, 3) << 0, -a, 0, -a, 1 + 4 * a, -a, 0, -a, 0);
        }
    }

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::filter2D(asMat(in), dst, -1, kernel_, cv::Point(-1, -1), 0.0, cv::BORDER_REFLECT_101);
    }

private:
    cv::Mat kernel_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "sharpen";
    info.title = "Laplacian sharpen";
    info.description = "Fast 3x3 sharpening of the finest detail";
    info.category = "sharpen";
    info.params = {
        param::number("amount", "Strength", 0.5, 0.0, 3.0, 0.05),
        param::choice("neighbours", "4 (cross) is softer, 8 (square) stronger", "4", {"4", "8"}),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Sharpen>(getFloat(p, "amount"), getString(p, "neighbours"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

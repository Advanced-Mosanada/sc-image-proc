#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Bilateral filter: averages neighbours weighted by distance AND by
/// similarity, so pixels across an edge (very different values) are left out.
/// Smooths noise and keeps edges; cost grows with the diameter.
class Bilateral final : public Stage {
public:
    Bilateral(int diameter, double sigmaColor, double sigmaSpace)
        : diameter_(diameter), sigmaColor_(sigmaColor), sigmaSpace_(sigmaSpace)
    {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::bilateralFilter(
            asMat(in), dst, diameter_, sigmaColor_, sigmaSpace_, cv::BORDER_REFLECT_101);
    }

private:
    int diameter_;
    double sigmaColor_;
    double sigmaSpace_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "bilateral";
    info.title = "Bilateral filter";
    info.description = "Edge-preserving smoothing: averages only similar neighbours";
    info.category = "denoise";
    info.params = {
        param::integer("diameter", "Neighbourhood diameter", 5, 3, 15, 2, "px"),
        param::number("sigma_color",
                      "How different values may be and still be averaged",
                      30.0,
                      1.0,
                      150.0,
                      1.0),
        param::number("sigma_space", "How far neighbours count", 5.0, 0.5, 50.0, 0.5, "px"),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Bilateral>(static_cast<int>(getInt(p, "diameter")),
                                           getFloat(p, "sigma_color"),
                                           getFloat(p, "sigma_space"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

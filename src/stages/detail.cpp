#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <memory>

namespace scip::stages {

namespace {

/// Edge-aware detail enhancement: splits the image into a base layer (guided
/// filter: smooth, but with the strong edges kept) and the detail on top of
/// it, then boosts the detail. Because strong edges stay in the base, they do
/// not get the bright/dark halos an unsharp mask gives them.
class Detail final : public Stage {
public:
    Detail(double gain, int radius, double eps) : gain_(gain), radius_(radius), eps_(eps) {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        filter_.apply(asMat(in), dst, radius_, eps_, gain_);  // base + gain * detail
    }

private:
    double gain_;
    int radius_;
    double eps_;
    GuidedFilter filter_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "detail";
    info.title = "Detail enhancement";
    info.description = "Boosts texture and fine detail without halos at strong edges";
    info.category = "sharpen";
    info.params = {
        param::number("gain", "Detail boost (1 = unchanged)", 2.0, 1.0, 6.0, 0.1),
        param::integer("radius", "Size of the detail to boost", 8, 1, 32, 0, "px"),
        param::number("eps",
                      "Edges stronger than ~sqrt(eps)*255 are kept out of the boost",
                      0.01,
                      0.0001,
                      0.2,
                      0.0005),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Detail>(
            getFloat(p, "gain"), static_cast<int>(getInt(p, "radius")), getFloat(p, "eps"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

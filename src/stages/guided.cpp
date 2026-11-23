#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <memory>

namespace scip::stages {

namespace {

/// Guided filter used as an edge-preserving denoiser: like the bilateral
/// filter in effect, but built from box filters, so its cost does not depend
/// on the radius and it has no gradient-reversal artefacts.
class Guided final : public Stage {
public:
    Guided(int radius, double eps) : radius_(radius), eps_(eps) {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        filter_.apply(asMat(in), dst, radius_, eps_);
    }

private:
    int radius_;
    double eps_;
    GuidedFilter filter_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "guided";
    info.title = "Guided filter";
    info.description = "Edge-preserving smoothing at a cost independent of the radius";
    info.category = "denoise";
    info.params = {
        param::integer(
            "radius", "Window radius (8+ is fastest: computed at 1/4 size)", 8, 1, 32, 0, "px"),
        param::number("eps",
                      "Smoothing: variations below ~sqrt(eps)*255 are flattened",
                      0.01,
                      0.0001,
                      0.2,
                      0.0005),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Guided>(static_cast<int>(getInt(p, "radius")), getFloat(p, "eps"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

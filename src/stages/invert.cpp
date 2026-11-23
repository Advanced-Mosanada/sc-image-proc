#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/core.hpp>

#include <memory>

namespace scip::stages {

namespace {

class Invert final : public Stage {
public:
    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::bitwise_not(asMat(in), dst);
    }
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "invert";
    info.title = "Invert";
    info.description = "Negative image: every value v becomes 255 - v";
    info.category = "color";
    info.create = [](const ParamMap&) { return std::make_unique<Invert>(); };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

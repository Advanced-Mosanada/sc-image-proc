#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Global histogram equalisation: spreads the brightness levels so that each
/// is used about equally often. Strong and parameter-free; clahe is gentler on
/// images with large dark or bright areas.
class Equalize final : public Stage {
public:
    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        luma_.apply(asMat(in), dst, [](const cv::Mat& src, cv::Mat& result) {
            cv::equalizeHist(src, result);
        });
    }

private:
    LumaProcessor luma_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "equalize";
    info.title = "Histogram equalisation";
    info.description = "Spreads brightness over the full range (global); colour: on the "
                       "brightness only";
    info.category = "contrast";
    info.create = [](const ParamMap&) { return std::make_unique<Equalize>(); };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

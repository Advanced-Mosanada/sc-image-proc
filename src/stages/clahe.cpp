#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Contrast Limited Adaptive Histogram Equalisation: equalises small tiles
/// separately, which brings out detail in low-contrast images such as thermal
/// (IR) video far better than a global histogram equalisation. Colour frames
/// are equalised on their brightness, so colours keep their hue.
class Clahe final : public Stage {
public:
    Clahe(double clipLimit, int tiles) : clahe_(cv::createCLAHE(clipLimit, cv::Size(tiles, tiles)))
    {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        luma_.apply(asMat(in), dst, [this](const cv::Mat& src, cv::Mat& result) {
            clahe_->apply(src, result);
        });
    }

private:
    cv::Ptr<cv::CLAHE> clahe_;
    LumaProcessor luma_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "clahe";
    info.title = "Adaptive contrast (CLAHE)";
    info.description =
        "Local histogram equalisation with a contrast limit; brings out detail in dark "
        "or flat areas (thermal video). Colour: on the brightness only";
    info.category = "contrast";
    info.params = {
        param::number("clip", "Contrast limit; higher = stronger", 2.0, 0.1, 40.0, 0.1),
        param::integer("tiles", "Tiles per side of the grid", 8, 1, 32),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Clahe>(getFloat(p, "clip"), static_cast<int>(getInt(p, "tiles")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

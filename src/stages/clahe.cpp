#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>
#include <stdexcept>

namespace scip::stages {

namespace {

/// Contrast Limited Adaptive Histogram Equalisation: equalises small tiles
/// separately, which brings out detail in low-contrast images such as thermal
/// (IR) video far better than a global histogram equalisation.
class Clahe final : public Stage {
public:
    Clahe(double clipLimit, int tiles) : clahe_(cv::createCLAHE(clipLimit, cv::Size(tiles, tiles)))
    {}

    FrameInfo configure(const FrameInfo& input) override
    {
        if (input.format != PixelFormat::Gray8) {
            throw std::invalid_argument("needs gray8 input (put 'gray' before it)");
        }
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        clahe_->apply(asMat(in), dst);
    }

private:
    cv::Ptr<cv::CLAHE> clahe_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "clahe";
    info.title = "Adaptive contrast (CLAHE)";
    info.description =
        "Local histogram equalisation with a contrast limit; good for thermal images (gray8)";
    info.category = "color";
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

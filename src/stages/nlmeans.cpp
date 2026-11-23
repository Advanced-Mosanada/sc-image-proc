#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/photo.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Non-local means: each pixel is averaged with pixels whose surrounding
/// patches look alike, wherever they are in the search window. The best
/// quality of the spatial denoisers, and by far the slowest: a reference to
/// compare faster filters against, or for small frames.
class NlMeans final : public Stage {
public:
    NlMeans(double strength, double colorStrength, int search)
        : strength_(float(strength)), colorStrength_(float(colorStrength)), search_(search)
    {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        constexpr int kTemplate = 7;  // patch size recommended by OpenCV
        if (in.format() == PixelFormat::Gray8) {
            cv::fastNlMeansDenoising(asMat(in), dst, strength_, kTemplate, search_);
        } else {
            cv::fastNlMeansDenoisingColored(
                asMat(in), dst, strength_, colorStrength_, kTemplate, search_);
        }
    }

private:
    float strength_;
    float colorStrength_;
    int search_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "nlmeans";
    info.title = "Non-local means denoise";
    info.description = "Best-quality spatial denoising, very slow (OpenCV photo module)";
    info.category = "denoise";
    info.params = {
        param::number("strength",
                      "Filter strength; higher removes more noise and detail",
                      5.0,
                      1.0,
                      30.0,
                      0.5),
        param::number("color_strength", "Strength on colour (colour frames)", 5.0, 1.0, 30.0, 0.5),
        param::integer("search", "Search window (odd); larger is slower", 11, 5, 35, 2, "px"),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<NlMeans>(getFloat(p, "strength"),
                                         getFloat(p, "color_strength"),
                                         static_cast<int>(getInt(p, "search")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

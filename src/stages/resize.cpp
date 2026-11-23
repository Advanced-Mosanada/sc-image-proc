#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

int toCvInterpolation(const std::string& name)
{
    if (name == "nearest") {
        return cv::INTER_NEAREST;
    }
    if (name == "cubic") {
        return cv::INTER_CUBIC;
    }
    if (name == "area") {
        return cv::INTER_AREA;
    }
    return cv::INTER_LINEAR;
}

class Resize final : public Stage {
public:
    Resize(int width, int height, int interpolation)
        : width_(width), height_(height), interpolation_(interpolation)
    {}

    FrameInfo configure(const FrameInfo& input) override
    {
        return FrameInfo{width_, height_, input.format};
    }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::resize(asMat(in), dst, dst.size(), 0.0, 0.0, interpolation_);
    }

private:
    int width_;
    int height_;
    int interpolation_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "resize";
    info.title = "Resize";
    info.description = "Scales the image to a fixed size (OpenCV)";
    info.category = "geometry";
    info.params = {
        param::integer("width", "Output width", 640, 16, 7680, 0, "px"),
        param::integer("height", "Output height", 480, 16, 4320, 0, "px"),
        param::choice(
            "interpolation", "Resampling method", "linear", {"nearest", "linear", "cubic", "area"}),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Resize>(static_cast<int>(getInt(p, "width")),
                                        static_cast<int>(getInt(p, "height")),
                                        toCvInterpolation(getString(p, "interpolation")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

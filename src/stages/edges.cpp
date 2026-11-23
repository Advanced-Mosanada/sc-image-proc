#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>
#include <stdexcept>

namespace scip::stages {

namespace {

class Edges final : public Stage {
public:
    Edges(double low, double high, int aperture) : low_(low), high_(high), aperture_(aperture)
    {
        if (low > high) {
            throw std::invalid_argument("low must not exceed high");
        }
    }

    FrameInfo configure(const FrameInfo& input) override
    {
        return FrameInfo{input.width, input.height, PixelFormat::Gray8};
    }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat src = asMat(in);
        if (in.format() == PixelFormat::Bgr8) {
            cv::cvtColor(src, gray_, cv::COLOR_BGR2GRAY);  // gray_ is reused between frames
            src = gray_;
        }
        cv::Mat dst = asMat(out);
        cv::Canny(src, dst, low_, high_, aperture_);
    }

private:
    double low_;
    double high_;
    int aperture_;
    cv::Mat gray_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "edges";
    info.title = "Canny edges";
    info.description = "Edge map (255 on edges) using the Canny detector; outputs gray8";
    info.category = "features";
    info.params = {
        param::integer("low", "Lower hysteresis threshold", 50, 0, 1000),
        param::integer("high", "Upper hysteresis threshold", 150, 0, 1000),
        param::integer("aperture", "Sobel aperture size", 3, 3, 7, 2),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Edges>(static_cast<double>(getInt(p, "low")),
                                       static_cast<double>(getInt(p, "high")),
                                       static_cast<int>(getInt(p, "aperture")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

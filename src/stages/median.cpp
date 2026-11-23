#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Median filter: each pixel becomes the median of its neighbourhood. Removes
/// salt-and-pepper noise (dead / hot pixels) completely while keeping edges
/// sharp, which an averaging blur cannot do.
class Median final : public Stage {
public:
    explicit Median(int kernelSize) : kernelSize_(kernelSize) {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::medianBlur(asMat(in), dst, kernelSize_);
    }

private:
    int kernelSize_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "median";
    info.title = "Median filter";
    info.description = "Removes impulse (salt-and-pepper) noise, keeps edges; 3 and 5 are fast";
    info.category = "denoise";
    info.params = {
        param::integer("ksize", "Neighbourhood size (odd)", 3, 3, 15, 2, "px"),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Median>(static_cast<int>(getInt(p, "ksize")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

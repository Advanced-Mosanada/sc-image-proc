#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

class GaussianBlur final : public Stage {
public:
    GaussianBlur(int kernelSize, double sigma) : kernelSize_(kernelSize), sigma_(sigma) {}

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::GaussianBlur(asMat(in),
                         dst,
                         cv::Size(kernelSize_, kernelSize_),
                         sigma_,
                         sigma_,
                         cv::BORDER_REFLECT_101);
    }

private:
    int kernelSize_;
    double sigma_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "blur";
    info.title = "Gaussian blur";
    info.description = "Smooths the image with a Gaussian kernel (OpenCV)";
    info.category = "denoise";
    info.params = {
        param::integer("ksize", "Kernel size (odd)", 5, 3, 31, 2, "px"),
        param::number("sigma",
                      "Standard deviation; 0 = derived from the kernel size",
                      0.0,
                      0.0,
                      20.0,
                      0.1,
                      "px"),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<GaussianBlur>(static_cast<int>(getInt(p, "ksize")),
                                              getFloat(p, "sigma"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

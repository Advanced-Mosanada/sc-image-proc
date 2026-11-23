#include "scip/cv.hpp"
#include "scip/stages.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>
#include <sstream>
#include <stdexcept>

namespace scip::stages {

namespace {

class GaussianBlur final : public Stage {
public:
    GaussianBlur(int kernelSize, double sigma) : kernelSize_(kernelSize), sigma_(sigma)
    {
        if (kernelSize < 3 || kernelSize % 2 == 0) {
            throw std::invalid_argument("blur kernel size must be odd and >= 3, got " +
                                        std::to_string(kernelSize));
        }
        if (sigma < 0.0) {
            throw std::invalid_argument("blur sigma must be >= 0");
        }
    }

    std::string name() const override
    {
        std::ostringstream os;
        os << "blur:" << kernelSize_;
        if (sigma_ > 0.0) {
            os << ':' << sigma_;
        }
        return os.str();
    }

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

}  // namespace

StagePtr makeGaussianBlur(int kernelSize, double sigma)
{
    return std::make_unique<GaussianBlur>(kernelSize, sigma);
}

}  // namespace scip::stages

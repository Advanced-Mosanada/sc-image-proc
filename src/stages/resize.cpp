#include "scip/cv.hpp"
#include "scip/stages.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>
#include <stdexcept>

namespace scip::stages {

namespace {

class Resize final : public Stage {
public:
    Resize(int width, int height) : width_(width), height_(height)
    {
        if (width <= 0 || height <= 0) {
            throw std::invalid_argument("resize target must be positive");
        }
    }

    std::string name() const override
    {
        return "resize:" + std::to_string(width_) + "x" + std::to_string(height_);
    }

    FrameInfo configure(const FrameInfo& input) override
    {
        return FrameInfo{width_, height_, input.format};
    }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::resize(asMat(in), dst, dst.size(), 0.0, 0.0, cv::INTER_LINEAR);
    }

private:
    int width_;
    int height_;
};

}  // namespace

StagePtr makeResize(int width, int height)
{
    return std::make_unique<Resize>(width, height);
}

}  // namespace scip::stages

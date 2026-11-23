#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace scip::stages {

namespace {

class Threshold final : public Stage {
public:
    Threshold(int value, std::string mode, bool invert)
        : value_(value), mode_(std::move(mode)), invert_(invert)
    {}

    FrameInfo configure(const FrameInfo& input) override
    {
        if (mode_ == "otsu" && input.format != PixelFormat::Gray8) {
            throw std::invalid_argument("otsu needs gray8 input (put 'gray' before it)");
        }
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        int type = invert_ ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY;
        if (mode_ == "otsu") {
            type |= cv::THRESH_OTSU;  // picks the value from the histogram
        }
        cv::Mat dst = asMat(out);
        cv::threshold(asMat(in), dst, value_, 255, type);
    }

private:
    int value_;
    std::string mode_;
    bool invert_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "threshold";
    info.title = "Threshold";
    info.description = "Binary image: 255 above the threshold, 0 below (per channel)";
    info.category = "segmentation";
    info.params = {
        param::integer("value", "Threshold (ignored with mode=otsu)", 128, 0, 255),
        param::choice("mode",
                      "fixed value, or Otsu's automatic choice (gray8 only)",
                      "fixed",
                      {"fixed", "otsu"}),
        param::boolean("invert", "255 below the threshold instead of above", false),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Threshold>(
            static_cast<int>(getInt(p, "value")), getString(p, "mode"), getBool(p, "invert"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

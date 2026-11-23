#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace scip::stages {

namespace {

/// Manual levels, like in a photo editor: `black` becomes 0, `white` becomes
/// 255, and `gamma` bends the curve in between (> 1 brightens the mid-tones).
/// One lookup table, the same for every channel.
class Levels final : public Stage {
public:
    Levels(int black, int white, double gamma)
    {
        if (white <= black) {
            throw std::invalid_argument("white must be above black");
        }
        lut_ = makeLut([&](int v) {
            const double x = std::clamp((v - black) / double(white - black), 0.0, 1.0);
            return std::pow(x, 1.0 / gamma) * 255.0 + 0.5;
        });
    }

    FrameInfo configure(const FrameInfo& input) override { return input; }

    void process(const Frame& in, Frame& out) override
    {
        cv::Mat dst = asMat(out);
        cv::LUT(asMat(in), lut_, dst);
    }

private:
    cv::Mat lut_;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "levels";
    info.title = "Levels (black, white, gamma)";
    info.description = "Maps black..white to the full range and bends the mid-tones with gamma";
    info.category = "contrast";
    info.params = {
        param::integer("black", "Input level that becomes black", 0, 0, 254),
        param::integer("white", "Input level that becomes white", 255, 1, 255),
        param::number("gamma", "Mid-tones: > 1 brighter, < 1 darker", 1.0, 0.1, 5.0, 0.05),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<Levels>(static_cast<int>(getInt(p, "black")),
                                        static_cast<int>(getInt(p, "white")),
                                        getFloat(p, "gamma"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

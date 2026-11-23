#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/imgproc.hpp>

#include <memory>

namespace scip::stages {

namespace {

/// Running (exponential) average over time: out = alpha * frame + (1 - alpha)
/// * previous output. Removes noise on still scenes very effectively, but
/// anything that moves leaves a trail (see `tnr` for a motion-aware version).
class TemporalMean final : public Stage {
public:
    explicit TemporalMean(double alpha) : alpha_(alpha) {}

    FrameInfo configure(const FrameInfo& input) override
    {
        first_ = true;  // history restarts with a new stream or pipeline
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        const cv::Mat src = asMat(in);
        if (first_) {
            src.convertTo(average_, CV_32F);
            first_ = false;
        } else {
            cv::accumulateWeighted(src, average_, alpha_);
        }
        cv::Mat dst = asMat(out);
        average_.convertTo(dst, CV_8U);
    }

private:
    double alpha_;
    bool first_ = true;
    cv::Mat average_;  // CV_32F: 8-bit would lose the small steps
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "temporal_mean";
    info.title = "Temporal average";
    info.description = "Running average over frames: strong on still scenes, trails on motion";
    info.category = "temporal";
    info.params = {
        param::number(
            "alpha", "Weight of the new frame (smaller = more smoothing)", 0.25, 0.02, 1.0, 0.01),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<TemporalMean>(getFloat(p, "alpha"));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

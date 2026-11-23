#include "scip/cv.hpp"
#include "scip/registry.hpp"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scip::stages {

namespace {

/// Per-pixel median of the last 3 or 5 frames. Removes noise that flickers
/// from frame to frame (sensor noise, interference, compression sparkle)
/// without the trails of an average: a moving edge needs only half the window
/// to move, but the output lags by about that much.
///
/// The median is computed with min/max sorting networks over whole images,
/// which OpenCV vectorises.
class TemporalMedian final : public Stage {
public:
    explicit TemporalMedian(int frames) : history_(std::size_t(frames)), work_(std::size_t(frames))
    {}

    FrameInfo configure(const FrameInfo& input) override
    {
        stored_ = 0;
        next_ = 0;
        return input;
    }

    void process(const Frame& in, Frame& out) override
    {
        const cv::Mat src = asMat(in);
        src.copyTo(history_[next_]);
        next_ = (next_ + 1) % history_.size();
        stored_ = std::min(stored_ + 1, history_.size());
        cv::Mat dst = asMat(out);
        if (stored_ < history_.size()) {
            src.copyTo(dst);  // not enough history yet
            return;
        }
        for (std::size_t i = 0; i < history_.size(); ++i) {
            history_[i].copyTo(work_[i]);
        }
        if (work_.size() == 3) {
            sort(0, 1);
            sort(1, 2);
            sort(0, 1);
            work_[1].copyTo(dst);
        } else {
            // Median of 5 with 7 compare-exchanges (Devillard, "Fast median search").
            sort(0, 1);
            sort(3, 4);
            sort(0, 3);
            sort(1, 4);
            sort(1, 2);
            sort(2, 3);
            sort(1, 2);
            work_[2].copyTo(dst);
        }
    }

private:
    /// Compare-exchange of two whole images: a = min, b = max, per pixel.
    void sort(std::size_t a, std::size_t b)
    {
        cv::min(work_[a], work_[b], scratch_);
        cv::max(work_[a], work_[b], work_[b]);
        std::swap(work_[a], scratch_);
    }

    std::vector<cv::Mat> history_;
    std::vector<cv::Mat> work_;
    cv::Mat scratch_;
    std::size_t stored_ = 0;
    std::size_t next_ = 0;
};

StageInfo makeInfo()
{
    StageInfo info;
    info.name = "temporal_median";
    info.title = "Temporal median";
    info.description = "Median of the last frames: removes flickering noise without trails";
    info.category = "temporal";
    info.params = {
        param::choice("frames", "Frames in the window", "3", {"3", "5"}),
    };
    info.create = [](const ParamMap& p) {
        return std::make_unique<TemporalMedian>(std::stoi(getString(p, "frames")));
    };
    return info;
}

SCIP_REGISTER_STAGE(makeInfo());

}  // namespace

}  // namespace scip::stages

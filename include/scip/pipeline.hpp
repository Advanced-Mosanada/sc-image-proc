#pragma once

#include "scip/frame.hpp"
#include "scip/stage.hpp"
#include "scip/stats.hpp"

#include <cstddef>
#include <vector>

namespace scip {

/// A chain of stages run one after another on the calling thread.
///
/// configure() allocates one output buffer per stage up front, so process()
/// performs no allocation. Per-stage and total latency is recorded on every call.
class Pipeline {
public:
    /// Append a stage. Invalidates a previous configure().
    void add(StagePtr stage);

    /// Configure every stage for frames of `input` format and allocate buffers.
    void configure(const FrameInfo& input);

    /// Run all stages on `input`. The returned frame belongs to the pipeline and
    /// is overwritten by the next call (clone() it to keep it). With no stages,
    /// `input` itself is returned.
    const Frame& process(const Frame& input);

    bool configured() const { return configured_; }
    std::size_t size() const { return slots_.size(); }
    const Stage& stage(std::size_t index) const { return *slots_.at(index).stage; }
    const LatencyStats& stageStats(std::size_t index) const { return slots_.at(index).stats; }
    const LatencyStats& totalStats() const { return total_; }
    const FrameInfo& inputInfo() const { return inputInfo_; }
    const FrameInfo& outputInfo() const { return outputInfo_; }

    void resetStats();

private:
    struct Slot {
        StagePtr stage;
        Frame output;
        LatencyStats stats;
    };

    std::vector<Slot> slots_;
    FrameInfo inputInfo_;
    FrameInfo outputInfo_;
    LatencyStats total_;
    bool configured_ = false;
};

}  // namespace scip

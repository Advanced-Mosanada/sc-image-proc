#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace scip {

/// Collects latency samples and reports min / mean / percentiles / max.
///
/// Keeps every sample (8 bytes each), so percentiles are exact. That is fine for
/// benchmark runs of thousands of frames. Long-running processes give a `window`:
/// then only the most recent `window` samples are kept and reported.
class LatencyStats {
public:
    explicit LatencyStats(std::size_t window = 0) : window_(window) {}

    void add(std::chrono::nanoseconds duration);
    void reset();

    std::size_t count() const { return samplesNs_.size(); }
    double minMs() const;
    double maxMs() const;
    double meanMs() const;

    /// Nearest-rank percentile, `percent` in [0, 100]. Returns 0 with no samples.
    double percentileMs(double percent) const;

private:
    std::size_t window_;
    std::size_t next_ = 0;  // ring position once the window is full
    std::vector<std::int64_t> samplesNs_;
};

}  // namespace scip

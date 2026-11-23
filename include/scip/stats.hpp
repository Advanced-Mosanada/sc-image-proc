#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace scip {

/// Collects latency samples and reports min / mean / percentiles / max.
///
/// Keeps every sample (8 bytes each), so percentiles are exact. That is fine for
/// benchmark runs of thousands of frames; a streaming estimator can replace it later.
class LatencyStats {
public:
    void add(std::chrono::nanoseconds duration);
    void reset();

    std::size_t count() const { return samplesNs_.size(); }
    double minMs() const;
    double maxMs() const;
    double meanMs() const;

    /// Nearest-rank percentile, `percent` in [0, 100]. Returns 0 with no samples.
    double percentileMs(double percent) const;

private:
    std::vector<std::int64_t> samplesNs_;
};

}  // namespace scip

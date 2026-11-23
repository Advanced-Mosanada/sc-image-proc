#include "scip/stats.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scip {

namespace {

constexpr double kNsPerMs = 1e6;

}  // namespace

void LatencyStats::add(std::chrono::nanoseconds duration)
{
    samplesNs_.push_back(duration.count());
}

void LatencyStats::reset()
{
    samplesNs_.clear();
}

double LatencyStats::minMs() const
{
    if (samplesNs_.empty()) {
        return 0.0;
    }
    return static_cast<double>(*std::min_element(samplesNs_.begin(), samplesNs_.end())) / kNsPerMs;
}

double LatencyStats::maxMs() const
{
    if (samplesNs_.empty()) {
        return 0.0;
    }
    return static_cast<double>(*std::max_element(samplesNs_.begin(), samplesNs_.end())) / kNsPerMs;
}

double LatencyStats::meanMs() const
{
    if (samplesNs_.empty()) {
        return 0.0;
    }
    const double sum = std::accumulate(samplesNs_.begin(), samplesNs_.end(), 0.0);
    return sum / static_cast<double>(samplesNs_.size()) / kNsPerMs;
}

double LatencyStats::percentileMs(double percent) const
{
    if (samplesNs_.empty()) {
        return 0.0;
    }
    std::vector<std::int64_t> sorted = samplesNs_;
    std::sort(sorted.begin(), sorted.end());

    // Nearest-rank: the smallest sample with at least `percent`% of samples <= it.
    const double clamped = std::clamp(percent, 0.0, 100.0);
    const auto rank =
        static_cast<std::size_t>(std::ceil(clamped / 100.0 * static_cast<double>(sorted.size())));
    const std::size_t index = rank == 0 ? 0 : rank - 1;
    return static_cast<double>(sorted[index]) / kNsPerMs;
}

}  // namespace scip

#include "scip/stats.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;

TEST(LatencyStats, EmptyReportsZero)
{
    const scip::LatencyStats stats;
    EXPECT_EQ(stats.count(), 0u);
    EXPECT_EQ(stats.meanMs(), 0.0);
    EXPECT_EQ(stats.percentileMs(95), 0.0);
}

TEST(LatencyStats, Summary)
{
    scip::LatencyStats stats;
    for (int ms = 1; ms <= 10; ++ms) {
        stats.add(std::chrono::milliseconds(ms));
    }
    EXPECT_EQ(stats.count(), 10u);
    EXPECT_DOUBLE_EQ(stats.minMs(), 1.0);
    EXPECT_DOUBLE_EQ(stats.maxMs(), 10.0);
    EXPECT_DOUBLE_EQ(stats.meanMs(), 5.5);
    EXPECT_DOUBLE_EQ(stats.percentileMs(50), 5.0);   // nearest rank: ceil(0.5 * 10) = 5th
    EXPECT_DOUBLE_EQ(stats.percentileMs(95), 10.0);  // ceil(9.5) = 10th
    EXPECT_DOUBLE_EQ(stats.percentileMs(0), 1.0);
    EXPECT_DOUBLE_EQ(stats.percentileMs(100), 10.0);
}

TEST(LatencyStats, Reset)
{
    scip::LatencyStats stats;
    stats.add(3ms);
    stats.reset();
    EXPECT_EQ(stats.count(), 0u);
}

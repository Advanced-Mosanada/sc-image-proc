#include "scip/frame.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

using scip::Frame;
using scip::FrameInfo;
using scip::PixelFormat;

TEST(Frame, AllocatesAlignedRows)
{
    const Frame frame(FrameInfo{100, 7, PixelFormat::Bgr8});
    EXPECT_FALSE(frame.empty());
    EXPECT_EQ(frame.info().rowBytes(), 300u);
    EXPECT_EQ(frame.stride() % Frame::kRowAlignment, 0u);
    EXPECT_GE(frame.stride(), 300u);
    for (int y = 0; y < frame.height(); ++y) {
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(frame.row(y)) % Frame::kRowAlignment, 0u);
    }
}

TEST(Frame, RejectsInvalidSize)
{
    EXPECT_THROW(Frame(FrameInfo{0, 10, PixelFormat::Gray8}), std::invalid_argument);
    EXPECT_THROW(Frame(FrameInfo{10, -1, PixelFormat::Gray8}), std::invalid_argument);
}

TEST(Frame, CopySharesPixelsCloneDoesNot)
{
    Frame a(FrameInfo{8, 8, PixelFormat::Gray8});
    a.fill(10);

    Frame shared = a;
    Frame deep = a.clone();
    a.row(0)[0] = 99;

    EXPECT_EQ(shared.row(0)[0], 99);
    EXPECT_EQ(deep.row(0)[0], 10);
    EXPECT_NE(deep.data(), a.data());
}

TEST(Frame, WrapUsesExternalMemory)
{
    std::vector<std::uint8_t> buffer(4 * 16, 7);
    Frame frame = Frame::wrap(FrameInfo{10, 4, PixelFormat::Gray8}, buffer.data(), 16);
    EXPECT_EQ(frame.data(), buffer.data());
    EXPECT_EQ(frame.stride(), 16u);
    frame.fill(1);
    EXPECT_EQ(buffer[16], 1);  // row 1, pixel 0
    EXPECT_EQ(buffer[15], 7);  // padding of row 0 untouched
}

TEST(Frame, WrapRejectsShortStride)
{
    std::vector<std::uint8_t> buffer(100);
    EXPECT_THROW(Frame::wrap(FrameInfo{10, 2, PixelFormat::Bgr8}, buffer.data(), 20),
                 std::invalid_argument);
}

TEST(Frame, CloneOfEmptyIsEmpty)
{
    EXPECT_TRUE(Frame().clone().empty());
}

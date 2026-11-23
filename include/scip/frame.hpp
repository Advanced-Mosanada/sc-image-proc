#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace scip {

/// Pixel layouts understood by the pipeline. Packed, 8 bits per channel.
enum class PixelFormat : std::uint8_t {
    Gray8,  ///< 1 channel
    Bgr8,   ///< 3 channels in B,G,R byte order (OpenCV's default)
};

std::string_view toString(PixelFormat format);

/// Bytes per pixel of a packed format.
int bytesPerPixel(PixelFormat format);

/// Geometry and format of a frame, without the pixels.
struct FrameInfo {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Gray8;

    /// Bytes of real pixel data in one row (excludes stride padding).
    std::size_t rowBytes() const
    {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(bytesPerPixel(format));
    }
};

bool operator==(const FrameInfo& a, const FrameInfo& b);
bool operator!=(const FrameInfo& a, const FrameInfo& b);

/// Per-frame metadata carried through the pipeline.
struct FrameMeta {
    std::uint64_t sequence = 0;    ///< frame counter assigned by the source
    std::int64_t timestampNs = 0;  ///< capture time on the steady clock
};

/// An image buffer.
///
/// Copying a Frame is cheap and SHARES the pixels (like cv::Mat); use clone() for
/// a deep copy. Rows are stride() bytes apart and every row starts on a
/// kRowAlignment boundary, which suits SIMD loads and DMA engines.
class Frame {
public:
    static constexpr std::size_t kRowAlignment = 64;

    Frame() = default;

    /// Allocate a frame. Pixel contents are uninitialised.
    explicit Frame(const FrameInfo& info);

    /// Wrap memory owned by someone else (a decoder or camera buffer, a cv::Mat...).
    ///
    /// `owner` is kept alive for as long as this frame or any copy of it exists, so
    /// the memory can be released automatically (e.g. a GStreamer buffer is returned
    /// to its pool when the last Frame referencing it goes away). Without an owner,
    /// the caller must keep `data` valid. Wrapped rows need not be 64-byte aligned.
    static Frame wrap(const FrameInfo& info,
                      std::uint8_t* data,
                      std::size_t stride,
                      std::shared_ptr<void> owner = nullptr);

    const FrameInfo& info() const { return info_; }
    int width() const { return info_.width; }
    int height() const { return info_.height; }
    PixelFormat format() const { return info_.format; }
    std::size_t stride() const { return stride_; }
    bool empty() const { return data_ == nullptr; }

    std::uint8_t* data() { return data_; }
    const std::uint8_t* data() const { return data_; }
    std::uint8_t* row(int y) { return data_ + static_cast<std::size_t>(y) * stride_; }
    const std::uint8_t* row(int y) const { return data_ + static_cast<std::size_t>(y) * stride_; }

    /// Deep copy with its own, freshly allocated buffer.
    Frame clone() const;

    /// Set every pixel byte to `value`.
    void fill(std::uint8_t value);

    FrameMeta meta;

private:
    FrameInfo info_;
    std::size_t stride_ = 0;
    std::shared_ptr<void> owner_;  // keeps the pixel memory alive (own buffer or external owner)
    std::uint8_t* data_ = nullptr;
};

}  // namespace scip

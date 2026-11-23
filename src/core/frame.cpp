#include "scip/frame.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace scip {

namespace {

std::size_t alignUp(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

void validate(const FrameInfo& info)
{
    if (info.width <= 0 || info.height <= 0) {
        throw std::invalid_argument("frame size must be positive, got " +
                                    std::to_string(info.width) + "x" + std::to_string(info.height));
    }
}

}  // namespace

std::string_view toString(PixelFormat format)
{
    switch (format) {
        case PixelFormat::Gray8:
            return "gray8";
        case PixelFormat::Bgr8:
            return "bgr8";
    }
    return "unknown";
}

int bytesPerPixel(PixelFormat format)
{
    switch (format) {
        case PixelFormat::Gray8:
            return 1;
        case PixelFormat::Bgr8:
            return 3;
    }
    return 0;
}

bool operator==(const FrameInfo& a, const FrameInfo& b)
{
    return a.width == b.width && a.height == b.height && a.format == b.format;
}

bool operator!=(const FrameInfo& a, const FrameInfo& b)
{
    return !(a == b);
}

Frame::Frame(const FrameInfo& info) : info_(info)
{
    validate(info);
    stride_ = alignUp(info.rowBytes(), kRowAlignment);
    const std::size_t bytes = stride_ * static_cast<std::size_t>(info.height);

    // aligned_alloc requires the size to be a multiple of the alignment: it is,
    // because the stride is.
    void* memory = std::aligned_alloc(kRowAlignment, bytes);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    owner_.reset(memory, [](void* p) { std::free(p); });
    data_ = static_cast<std::uint8_t*>(memory);
}

Frame Frame::wrap(const FrameInfo& info,
                  std::uint8_t* data,
                  std::size_t stride,
                  std::shared_ptr<void> owner)
{
    validate(info);
    if (data == nullptr) {
        throw std::invalid_argument("Frame::wrap: data is null");
    }
    if (stride < info.rowBytes()) {
        throw std::invalid_argument("Frame::wrap: stride " + std::to_string(stride) +
                                    " is smaller than a row (" + std::to_string(info.rowBytes()) +
                                    " bytes)");
    }
    Frame frame;
    frame.info_ = info;
    frame.stride_ = stride;
    frame.data_ = data;
    frame.owner_ = std::move(owner);
    return frame;
}

Frame Frame::clone() const
{
    if (empty()) {
        return {};
    }
    Frame copy(info_);
    const std::size_t rowBytes = info_.rowBytes();
    for (int y = 0; y < info_.height; ++y) {
        std::memcpy(copy.row(y), row(y), rowBytes);
    }
    copy.meta = meta;
    return copy;
}

void Frame::fill(std::uint8_t value)
{
    const std::size_t rowBytes = info_.rowBytes();
    for (int y = 0; y < info_.height; ++y) {
        std::memset(row(y), value, rowBytes);
    }
}

}  // namespace scip

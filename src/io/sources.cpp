#include "scip/io.hpp"

#include "../common/parse.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace scip::io {

namespace {

class RepeatSource final : public FrameSource {
public:
    RepeatSource(Frame frame, std::uint64_t count) : frame_(std::move(frame)), count_(count)
    {
        if (frame_.empty()) {
            throw std::invalid_argument("RepeatSource: empty frame");
        }
    }

    FrameInfo info() const override { return frame_.info(); }

    bool read(Frame& out) override
    {
        if (count_ != 0 && produced_ >= count_) {
            return false;
        }
        out = frame_;  // shares the pixels, no copy
        out.meta.sequence = produced_++;
        out.meta.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        return true;
    }

private:
    Frame frame_;
    std::uint64_t count_;
    std::uint64_t produced_ = 0;
};

}  // namespace

Frame makeTestPattern(const FrameInfo& info)
{
    Frame frame(info);
    const int bpp = bytesPerPixel(info.format);
    for (int y = 0; y < info.height; ++y) {
        std::uint8_t* row = frame.row(y);
        for (int x = 0; x < info.width; ++x) {
            const bool checker = ((x / 32) + (y / 32)) % 2 == 0;
            const auto horizontal =
                static_cast<std::uint8_t>(x * 255 / std::max(1, info.width - 1));
            const auto vertical = static_cast<std::uint8_t>(y * 255 / std::max(1, info.height - 1));
            const std::uint8_t pixel[3] = {
                horizontal, vertical, static_cast<std::uint8_t>(checker ? 230 : 25)};
            for (int c = 0; c < bpp; ++c) {
                row[x * bpp + c] =
                    bpp == 1 ? static_cast<std::uint8_t>((pixel[0] + pixel[1] + pixel[2]) / 3)
                             : pixel[c];
            }
        }
    }
    return frame;
}

FrameSourcePtr makeRepeatSource(Frame frame, std::uint64_t count)
{
    return std::make_unique<RepeatSource>(std::move(frame), count);
}

FrameSourcePtr openSource(const std::string& uri, std::uint64_t count)
{
    constexpr std::string_view kPattern = "pattern:";
    if (uri.rfind(kPattern, 0) == 0) {
        const auto parts = detail::split(std::string_view(uri).substr(kPattern.size()), ':');
        const auto [width, height] = detail::parseSize(parts[0], "pattern size");
        PixelFormat format = PixelFormat::Bgr8;
        if (parts.size() == 2 && parts[1] == "gray") {
            format = PixelFormat::Gray8;
        } else if (parts.size() != 1) {
            throw std::invalid_argument("usage: pattern:WIDTHxHEIGHT[:gray]");
        }
        return makeRepeatSource(makeTestPattern(FrameInfo{width, height, format}), count);
    }
    return makeRepeatSource(readImage(uri), count);
}

}  // namespace scip::io

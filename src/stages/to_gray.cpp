#include "scip/stages.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace scip::stages {

namespace {

/// Plain C++ reference implementation. Later NEON / GPU versions are tested
/// against this one, so it favours clarity over speed.
class ToGray final : public Stage {
public:
    std::string name() const override { return "gray"; }

    FrameInfo configure(const FrameInfo& input) override
    {
        if (input.format != PixelFormat::Bgr8 && input.format != PixelFormat::Gray8) {
            throw std::invalid_argument("expects bgr8 or gray8");
        }
        return FrameInfo{input.width, input.height, PixelFormat::Gray8};
    }

    void process(const Frame& in, Frame& out) override
    {
        if (in.format() == PixelFormat::Gray8) {
            for (int y = 0; y < in.height(); ++y) {
                std::memcpy(out.row(y), in.row(y), in.info().rowBytes());
            }
            return;
        }

        // BT.601 luma Y = 0.114 B + 0.587 G + 0.299 R in 8.8 fixed point.
        // The weights sum to exactly 256, so a gray input (B = G = R) maps to itself,
        // and +128 rounds to nearest instead of truncating.
        constexpr unsigned kB = 29;
        constexpr unsigned kG = 150;
        constexpr unsigned kR = 77;
        static_assert(kB + kG + kR == 256, "weights must sum to 1.0 in 8.8 fixed point");

        const int width = in.width();
        for (int y = 0; y < in.height(); ++y) {
            const std::uint8_t* src = in.row(y);
            std::uint8_t* dst = out.row(y);
            for (int x = 0; x < width; ++x, src += 3) {
                const unsigned luma = kB * src[0] + kG * src[1] + kR * src[2] + 128u;
                dst[x] = static_cast<std::uint8_t>(luma >> 8);
            }
        }
    }
};

}  // namespace

StagePtr makeToGray()
{
    return std::make_unique<ToGray>();
}

}  // namespace scip::stages

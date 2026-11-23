#pragma once

#include "scip/frame.hpp"

#include <memory>

namespace scip {

/// Something that produces frames: an image file, a test pattern, a camera...
class FrameSource {
public:
    FrameSource() = default;
    virtual ~FrameSource() = default;
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    /// Format of the frames this source produces.
    virtual FrameInfo info() const = 0;

    /// Nominal frames per second, or 0 if unknown / not meaningful (still images).
    virtual double frameRate() const { return 0.0; }

    /// Get the next frame. `out` may be re-pointed at a buffer owned by the source
    /// (no copy), so treat it as read-only. Returns false at the end of the stream.
    virtual bool read(Frame& out) = 0;
};

using FrameSourcePtr = std::unique_ptr<FrameSource>;

}  // namespace scip

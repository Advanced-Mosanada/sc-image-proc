#pragma once

#include "scip/frame.hpp"

#include <memory>
#include <string>

namespace scip {

/// Something that consumes frames: a window, a video file, a network stream...
///
/// A sink is opened for one FrameInfo and receives only frames of that format.
/// write() must not keep a reference to the frame: the pipeline reuses its buffers,
/// so sinks that work asynchronously copy the pixels.
class FrameSink {
public:
    FrameSink() = default;
    virtual ~FrameSink() = default;
    FrameSink(const FrameSink&) = delete;
    FrameSink& operator=(const FrameSink&) = delete;

    /// Short description for logs, e.g. "display (kmssink)".
    virtual std::string name() const = 0;

    virtual void write(const Frame& frame) = 0;

    /// Flush and finish (e.g. finalise an MP4 file). Called before destruction;
    /// may throw. Destructors also close, but swallow errors.
    virtual void close() {}
};

using FrameSinkPtr = std::unique_ptr<FrameSink>;

}  // namespace scip

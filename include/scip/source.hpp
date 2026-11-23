#pragma once

#include "scip/frame.hpp"

#include <chrono>
#include <memory>

namespace scip {

/// Result of FrameSource::tryRead().
enum class ReadStatus {
    Frame,    ///< a frame was delivered
    Timeout,  ///< no frame within the timeout; the source may still deliver later
    End,      ///< the stream has ended
};

/// Something that produces frames: an image file, a test pattern, a camera, a
/// network stream...
class FrameSource {
public:
    FrameSource() = default;
    virtual ~FrameSource() = default;
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    /// Format of the frames this source produces. Live sources may only know it
    /// once the first frame has arrived; until then width and height are 0.
    /// It may also change during the stream (a network sender switching videos):
    /// check each frame's info().
    virtual FrameInfo info() const = 0;

    /// Nominal frames per second, or 0 if unknown / not meaningful (still images).
    virtual double frameRate() const { return 0.0; }

    /// Live sources (cameras, network streams) produce frames at their own pace
    /// and may pause without having ended.
    virtual bool isLive() const { return false; }

    /// Get the next frame, waiting as long as needed. `out` may be re-pointed at a
    /// buffer owned by the source (no copy), so treat it as read-only. Returns
    /// false at the end of the stream.
    virtual bool read(Frame& out) = 0;

    /// Like read(), but gives up after `timeout`. Lets a caller stay responsive
    /// (commands, Ctrl+C) while a live source is quiet. The default suits sources
    /// that never wait.
    virtual ReadStatus tryRead(Frame& out, std::chrono::milliseconds /*timeout*/)
    {
        return read(out) ? ReadStatus::Frame : ReadStatus::End;
    }
};

using FrameSourcePtr = std::unique_ptr<FrameSource>;

}  // namespace scip

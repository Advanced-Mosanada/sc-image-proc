#pragma once

#include "scip/frame.hpp"

#include <chrono>
#include <memory>
#include <optional>

namespace scip {

/// Result of FrameSource::tryRead().
enum class ReadStatus {
    Frame,    ///< a frame was delivered
    Timeout,  ///< no frame within the timeout; the source may still deliver later
    End,      ///< the stream has ended
};

/// Where a controllable source (a video file opened as a player) is.
struct PlaybackState {
    bool paused = true;
    bool ended = false;     ///< stopped at the end (no loop); play or seek to go on
    double position = 0.0;  ///< seconds: the last frame delivered
    double duration = 0.0;  ///< seconds, 0 when unknown
    double rate = 1.0;      ///< 1 = normal speed
    bool loop = false;      ///< the whole video, or the A-B range when both are set
    std::optional<double> a;
    std::optional<double> b;
};

/// Player controls of a source (see FrameSource::playback()). Called from the
/// thread that reads the source, between reads.
class Playback {
public:
    Playback() = default;
    virtual ~Playback() = default;
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    /// A paused source delivers no frames, except the one a seek lands on.
    virtual void setPaused(bool paused) = 0;
    /// Jump to `seconds` (the exact frame, clamped to the video / A-B range).
    virtual void seek(double seconds) = 0;
    /// Speed, > 0 (forward only).
    virtual void setRate(double rate) = 0;
    /// Loop the whole video, or between a and b seconds when both are given
    /// (the video then plays only that range).
    virtual void setLoop(bool loop, std::optional<double> a, std::optional<double> b) = 0;
    virtual PlaybackState state() const = 0;
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

    /// Player controls, for sources that have them (video files opened with
    /// SourceOptions::player); null otherwise.
    virtual Playback* playback() { return nullptr; }

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

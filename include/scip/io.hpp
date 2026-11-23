#pragma once

#include "scip/frame.hpp"
#include "scip/sink.hpp"
#include "scip/source.hpp"

#include <cstdint>
#include <string>

/// Frame input / output.
namespace scip::io {

/// Load an image file (PNG, JPEG, BMP...). Grayscale files give gray8,
/// everything else bgr8 (alpha is dropped).
Frame readImage(const std::string& path);

/// Write a frame to an image file; the format follows the extension.
void writeImage(const std::string& path, const Frame& frame);

/// Deterministic test image: gradients plus a checkerboard, so blur and
/// edge stages have structure to work on.
Frame makeTestPattern(const FrameInfo& info);

/// A source that hands out the same in-memory frame `count` times (0 = forever).
/// Loading once keeps disk / NFS I/O out of the timing.
FrameSourcePtr makeRepeatSource(Frame frame, std::uint64_t count);

/// Options for opening a source.
struct SourceOptions {
    std::uint64_t maxFrames = 0;  ///< stop after this many frames, 0 = until the source ends
    bool realtime = false;        ///< video files: deliver at the file's frame rate
    bool gray = false;            ///< video/camera: ask GStreamer for gray8 instead of bgr8
};

/// Open a source from a URI:
///   pattern:WIDTHxHEIGHT[:gray]   generated test pattern (repeats, needs maxFrames)
///   <image file>                  .png/.jpg/... loaded once and repeated
///   <video file>                  .mp4/.mkv/.mov/.avi/.webm/.h264, decoded by GStreamer
///   camera:/dev/videoN[:WxH]      V4L2 camera via GStreamer
///   gst:<launch line>             any GStreamer pipeline ending in `appsink name=scip_sink`
FrameSourcePtr openSource(const std::string& uri, const SourceOptions& options);

/// True for sources that repeat a still image (pattern:, image files): they never
/// end on their own, so a frame count must be given.
bool isStillSource(const std::string& uri);

/// Open a sink for frames of format `info`. `fps` (0 = unknown, 30 is assumed)
/// sets timestamps for encoded outputs.
///   display[:CONNECTOR]           window on the PC, the screen (KMS) on the board;
///                                 CONNECTOR picks the output (modetest -M imx-drm -c)
///   udp:HOST:PORT                 H.264 over RTP/UDP, e.g. to watch the board on the PC
///   <video file>                  .mp4 / .mkv, H.264 (hardware encoder when available)
///   <image file>                  .png/.jpg/...: the LAST frame is written on close
///   gst:<launch line>             any pipeline starting with `appsrc name=scip_src`
FrameSinkPtr openSink(const std::string& uri, const FrameInfo& info, double fps);

/// For a udp: sink, the gst-launch command that shows the stream on a PC. Empty otherwise.
std::string viewerHint(const std::string& sinkUri);

/// True when the build includes GStreamer (video files, camera, display, udp).
bool hasGStreamer();

}  // namespace scip::io

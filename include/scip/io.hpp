#pragma once

#include "scip/frame.hpp"
#include "scip/sink.hpp"
#include "scip/source.hpp"

#include <cstdint>
#include <string>
#include <vector>

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
    bool loop = false;            ///< video files: start again at the end
    /// Video files: open as a player (FrameSource::playback()): start paused on
    /// the first frame, and stay open at the end instead of ending.
    bool player = false;
};

/// Open a source from a URI:
///   pattern:WIDTHxHEIGHT[:gray]   generated test pattern (repeats, needs maxFrames)
///   <image file>                  .png/.jpg/... loaded once and repeated
///   <video file>                  .mp4/.mkv/.mov/.avi/.webm/.h264, decoded by GStreamer
///   camera:/dev/videoN[:WxH]      V4L2 camera via GStreamer (live)
///   rtp://[ADDRESS]:PORT          H.264 over RTP/UDP, e.g. rtp://:5000 (live)
///   tcp://[ADDRESS]:PORT          raw video in GDP over TCP, e.g. tcp://:5000 (live): the
///                                 host GUI's lossless mode; waits for the next sender
///                                 when one disconnects
///   rtsp://... http(s)://... file://...   anything uridecodebin plays (rtsp: live)
///   gst:<launch line>             any GStreamer pipeline ending in `appsink name=scip_sink`
/// Live sources may start before the sender: their format arrives with the
/// first frame (see FrameSource::info()).
FrameSourcePtr openSource(const std::string& uri, const SourceOptions& options);

/// Check an input URI without opening it (syntax, file / device exists).
/// Throws std::invalid_argument with a helpful message.
void validateSourceUri(const std::string& uri);

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

/// Check an output URI without opening it. Throws std::invalid_argument.
void validateSinkUri(const std::string& uri);

/// For a udp: sink, the gst-launch command that shows the stream on a PC. Empty otherwise.
std::string viewerHint(const std::string& sinkUri);

/// True when the build includes GStreamer (video files, camera, display, udp).
bool hasGStreamer();

/// GStreamer's runtime version, "1.18.0"; "" without GStreamer.
std::string gstreamerVersion();

/// True when GStreamer can create this element (e.g. "vpuenc_h264" on the board).
bool hasGstElement(const std::string& element);

/// A folder entry an input can open (see listMediaFiles()).
struct MediaEntry {
    std::string name;
    std::string path;        ///< absolute
    std::string kind;        ///< "folder", "video" or "image"
    std::uint64_t size = 0;  ///< bytes, 0 for folders
};

/// The sub-folders, videos and images in `folder` (hidden ones left out),
/// folders first, then by name. Throws std::invalid_argument if it is not a folder.
std::vector<MediaEntry> listMediaFiles(const std::string& folder);

/// A size a camera delivers uncompressed, at its best frame rate.
struct CameraMode {
    int width = 0;
    int height = 0;
    double fps = 0.0;    ///< highest rate at this size, 0 if the driver does not say
    std::string format;  ///< V4L2 fourcc, "YUYV"
};

/// A V4L2 device that can capture video (open it as "camera:" + path).
struct VideoDevice {
    std::string path;    ///< /dev/video0
    std::string name;    ///< "HD Pro Webcam C920"
    std::string driver;  ///< "uvcvideo"
    std::string bus;     ///< "usb-xhci-hcd.1.auto-1"
    /// Largest first. Open one as "camera:" + path + ":" + WxH. Without a size
    /// the camera's largest mode is used, which may be slow (C920: 2 fps).
    std::vector<CameraMode> modes;
};

/// Cameras on this machine. Codec and metadata nodes are left out.
std::vector<VideoDevice> listVideoDevices();

/// A display connector (DRM/KMS). Use it as output "display:" + connectorId.
struct DisplayOutput {
    std::string card;     ///< /dev/dri/card1
    int connectorId = 0;  ///< for display:ID (kmssink connector-id)
    std::string name;     ///< "HDMI-A-1", "LVDS-1"
    bool connected = false;
    int width = 0;  ///< current (or preferred) mode, 0 if none
    int height = 0;
    int refreshHz = 0;
};

/// Display connectors; empty when built without libdrm.
std::vector<DisplayOutput> listDisplays();

/// True when a window system (X11 / Wayland) is available: "display" then
/// opens a window instead of drawing on a connector.
bool hasWindowSystem();

}  // namespace scip::io

#pragma once

// GStreamer-backed sources and sinks. Internal header: the public entry points are
// io::openSource() / io::openSink(). Implemented in gst_*.cpp, or by
// no_gstreamer.cpp (throwing stubs) when the build has no GStreamer.

#include "scip/io.hpp"

#include <string>

namespace scip::io::gst {

/// False in builds without GStreamer (no_gstreamer.cpp).
bool available();

FrameSourcePtr openVideoFile(const std::string& path, const SourceOptions& options);
FrameSourcePtr
openCamera(const std::string& device, int width, int height, const SourceOptions& options);
FrameSourcePtr openRtp(const std::string& address, int port, const SourceOptions& options);
FrameSourcePtr openUri(const std::string& uri, const SourceOptions& options);
FrameSourcePtr openLaunch(const std::string& description, const SourceOptions& options);

FrameSinkPtr openDisplay(const FrameInfo& info, double fps, int connector);
FrameSinkPtr openUdp(const std::string& host, int port, const FrameInfo& info, double fps);
FrameSinkPtr openVideoFileSink(const std::string& path, const FrameInfo& info, double fps);
FrameSinkPtr openLaunchSink(const std::string& description, const FrameInfo& info, double fps);

std::string udpViewerCommand(int port);

}  // namespace scip::io::gst

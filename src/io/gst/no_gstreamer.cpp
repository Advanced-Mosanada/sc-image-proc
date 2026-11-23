// Stand-ins used when the project is configured with -DSCIP_WITH_GSTREAMER=OFF.

#include "gst_io.hpp"

#include <stdexcept>

namespace scip::io::gst {

namespace {

[[noreturn]] void unavailable(const std::string& what)
{
    throw std::runtime_error(what + " needs GStreamer, but this build has none "
                                    "(reconfigure with -DSCIP_WITH_GSTREAMER=ON)");
}

}  // namespace

bool available()
{
    return false;
}

std::string version()
{
    return {};
}

bool hasPlugin(const std::string& /*element*/)
{
    return false;
}

FrameSourcePtr openVideoFile(const std::string& path, const SourceOptions& /*options*/)
{
    unavailable("video file '" + path + "'");
}

FrameSourcePtr openCamera(const std::string& device, int, int, const SourceOptions&)
{
    unavailable("camera " + device);
}

FrameSourcePtr openRtp(const std::string& address, int port, const SourceOptions&)
{
    unavailable("rtp input " + address + ":" + std::to_string(port));
}

FrameSourcePtr openTcp(const std::string& address, int port, const SourceOptions&)
{
    unavailable("tcp input " + address + ":" + std::to_string(port));
}

FrameSourcePtr openUri(const std::string& uri, const SourceOptions&)
{
    unavailable("input '" + uri + "'");
}

FrameSourcePtr openLaunch(const std::string&, const SourceOptions&)
{
    unavailable("gst: source");
}

FrameSinkPtr openDisplay(const FrameInfo&, double, int)
{
    unavailable("display output");
}

FrameSinkPtr openUdp(const std::string&, int, const FrameInfo&, double)
{
    unavailable("udp output");
}

FrameSinkPtr openVideoFileSink(const std::string& path, const FrameInfo&, double)
{
    unavailable("video file '" + path + "'");
}

FrameSinkPtr openLaunchSink(const std::string&, const FrameInfo&, double)
{
    unavailable("gst: sink");
}

std::string udpViewerCommand(int /*port*/)
{
    return {};
}

}  // namespace scip::io::gst

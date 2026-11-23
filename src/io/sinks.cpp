#include "scip/io.hpp"

#include "../common/parse.hpp"
#include "gst/gst_io.hpp"
#include "uri.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace scip::io {

namespace {

/// Keeps the most recent frame and writes it as an image when closed.
/// (Writing a PNG per frame would dominate the timing.)
class ImageSink final : public FrameSink {
public:
    explicit ImageSink(std::string path) : path_(std::move(path)) {}

    ~ImageSink() override
    {
        try {
            close();
        } catch (...) {}
    }

    std::string name() const override { return "image " + path_ + " (last frame)"; }

    void write(const Frame& frame) override { last_ = frame.clone(); }

    void close() override
    {
        if (!last_.empty()) {
            writeImage(path_, last_);
            last_ = Frame{};
        }
    }

private:
    std::string path_;
    Frame last_;
};

}  // namespace

FrameSinkPtr openSink(const std::string& uri, const FrameInfo& info, double fps)
{
    constexpr std::string_view kUdp = "udp:";
    constexpr std::string_view kGst = "gst:";

    if (uri == "display") {
        return gst::openDisplay(info, fps, -1);
    }
    if (detail::startsWith(uri, "display:")) {
        // display:CONNECTOR - a DRM connector id (board), see `modetest -M imx-drm -c`
        return gst::openDisplay(info, fps, detail::parseInt(uri.substr(8), "display connector"));
    }
    if (detail::startsWith(uri, kUdp)) {
        // udp:HOST:PORT
        const auto parts = detail::split(std::string_view(uri).substr(kUdp.size()), ':');
        if (parts.size() != 2 || parts[0].empty()) {
            throw std::invalid_argument("usage: udp:HOST:PORT");
        }
        return gst::openUdp(
            std::string(parts[0]), detail::parseInt(parts[1], "udp port"), info, fps);
    }
    if (detail::startsWith(uri, kGst)) {
        return gst::openLaunchSink(uri.substr(kGst.size()), info, fps);
    }
    if (detail::isVideoPath(uri)) {
        return gst::openVideoFileSink(uri, info, fps);
    }
    if (detail::isImagePath(uri)) {
        return std::make_unique<ImageSink>(uri);
    }
    throw std::invalid_argument(
        "unknown output '" + uri +
        "' (use display, udp:HOST:PORT, a .mp4/.mkv/.png path, or gst:...)");
}

std::string viewerHint(const std::string& sinkUri)
{
    if (!detail::startsWith(sinkUri, "udp:")) {
        return {};
    }
    const auto parts = detail::split(std::string_view(sinkUri).substr(4), ':');
    return parts.size() == 2 ? gst::udpViewerCommand(detail::parseInt(parts[1], "udp port"))
                             : std::string{};
}

bool hasGStreamer()
{
    return gst::available();
}

}  // namespace scip::io

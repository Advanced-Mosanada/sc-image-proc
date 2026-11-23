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

namespace {

/// A sink URI, parsed and checked, but not opened.
struct SinkTarget {
    enum class Kind {
        Display,
        Udp,
        Launch,
        VideoFile,
        ImageFile
    };
    Kind kind = Kind::Display;
    std::string text;  ///< host, launch line or path
    int number = -1;   ///< display connector or udp port
};

SinkTarget parseSinkUri(const std::string& uri)
{
    constexpr std::string_view kUdp = "udp:";
    constexpr std::string_view kGst = "gst:";
    constexpr std::string_view kDisplay = "display:";
    SinkTarget target;

    if (uri == "display") {
        return target;
    }
    if (detail::startsWith(uri, kDisplay)) {
        // display:CONNECTOR - a DRM connector id, see `scip-cli --list-devices`
        target.number = detail::parseInt(uri.substr(kDisplay.size()), "display connector");
        return target;
    }
    if (detail::startsWith(uri, kUdp)) {
        const auto parts = detail::split(std::string_view(uri).substr(kUdp.size()), ':');
        if (parts.size() != 2 || parts[0].empty()) {
            throw std::invalid_argument("usage: udp:HOST:PORT");
        }
        target.kind = SinkTarget::Kind::Udp;
        target.text = std::string(parts[0]);
        target.number = detail::parseInt(parts[1], "udp port");
        if (target.number <= 0 || target.number > 65535) {
            throw std::invalid_argument("udp port must be 1..65535");
        }
        return target;
    }
    if (detail::startsWith(uri, kGst)) {
        target.kind = SinkTarget::Kind::Launch;
        target.text = uri.substr(kGst.size());
        if (target.text.find("scip_src") == std::string::npos) {
            throw std::invalid_argument("gst: outputs must start with 'appsrc name=scip_src'");
        }
        return target;
    }
    if (detail::isVideoPath(uri)) {
        const std::string ext = detail::lowerExtension(uri);
        if (ext != ".mp4" && ext != ".mkv") {
            throw std::invalid_argument("can record .mp4 or .mkv, not '" + ext + "'");
        }
        target.kind = SinkTarget::Kind::VideoFile;
        target.text = uri;
        return target;
    }
    if (detail::isImagePath(uri)) {
        target.kind = SinkTarget::Kind::ImageFile;
        target.text = uri;
        return target;
    }
    throw std::invalid_argument(
        "unknown output '" + uri +
        "' (use display[:ID], udp:HOST:PORT, a .mp4/.mkv/.png path, or gst:...)");
}

}  // namespace

void validateSinkUri(const std::string& uri)
{
    parseSinkUri(uri);
}

FrameSinkPtr openSink(const std::string& uri, const FrameInfo& info, double fps)
{
    const SinkTarget target = parseSinkUri(uri);
    switch (target.kind) {
        case SinkTarget::Kind::Display:
            return gst::openDisplay(info, fps, target.number);
        case SinkTarget::Kind::Udp:
            return gst::openUdp(target.text, target.number, info, fps);
        case SinkTarget::Kind::Launch:
            return gst::openLaunchSink(target.text, info, fps);
        case SinkTarget::Kind::VideoFile:
            return gst::openVideoFileSink(target.text, info, fps);
        case SinkTarget::Kind::ImageFile:
            return std::make_unique<ImageSink>(target.text);
    }
    throw std::logic_error("openSink: unhandled output kind");
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

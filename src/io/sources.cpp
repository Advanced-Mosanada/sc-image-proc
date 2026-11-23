#include "scip/io.hpp"

#include "../common/parse.hpp"
#include "gst/gst_io.hpp"
#include "uri.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace scip::io {

namespace {

class RepeatSource final : public FrameSource {
public:
    RepeatSource(Frame frame, std::uint64_t count) : frame_(std::move(frame)), count_(count)
    {
        if (frame_.empty()) {
            throw std::invalid_argument("RepeatSource: empty frame");
        }
    }

    FrameInfo info() const override { return frame_.info(); }

    bool read(Frame& out) override
    {
        if (count_ != 0 && produced_ >= count_) {
            return false;
        }
        out = frame_;  // shares the pixels, no copy
        out.meta.sequence = produced_++;
        out.meta.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        return true;
    }

private:
    Frame frame_;
    std::uint64_t count_;
    std::uint64_t produced_ = 0;
};

/// rtp://:5000, rtp://@:5000 (any address), rtp://239.1.2.3:5000 (multicast);
/// the same for tcp:// (an address of this machine to listen on).
std::pair<std::string, int> parseListenAddress(const std::string& uri, const std::string& scheme)
{
    const std::string usage = "usage: " + scheme + "://[ADDRESS]:PORT, e.g. " + scheme + "://:5000";
    std::string rest = uri.substr(scheme.size() + 3);
    if (!rest.empty() && rest.front() == '@') {
        rest.erase(0, 1);
    }
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument(usage);
    }
    const int port = detail::parseInt(std::string_view(rest).substr(colon + 1), scheme + " port");
    if (port <= 0 || port > 65535) {
        throw std::invalid_argument(scheme + " port must be 1..65535");
    }
    return {rest.substr(0, colon), port};
}

}  // namespace

Frame makeTestPattern(const FrameInfo& info)
{
    Frame frame(info);
    const int bpp = bytesPerPixel(info.format);
    for (int y = 0; y < info.height; ++y) {
        std::uint8_t* row = frame.row(y);
        for (int x = 0; x < info.width; ++x) {
            const bool checker = ((x / 32) + (y / 32)) % 2 == 0;
            const auto horizontal =
                static_cast<std::uint8_t>(x * 255 / std::max(1, info.width - 1));
            const auto vertical = static_cast<std::uint8_t>(y * 255 / std::max(1, info.height - 1));
            const std::uint8_t pixel[3] = {
                horizontal, vertical, static_cast<std::uint8_t>(checker ? 230 : 25)};
            for (int c = 0; c < bpp; ++c) {
                row[x * bpp + c] =
                    bpp == 1 ? static_cast<std::uint8_t>((pixel[0] + pixel[1] + pixel[2]) / 3)
                             : pixel[c];
            }
        }
    }
    return frame;
}

FrameSourcePtr makeRepeatSource(Frame frame, std::uint64_t count)
{
    return std::make_unique<RepeatSource>(std::move(frame), count);
}

std::vector<MediaEntry> listMediaFiles(const std::string& folder)
{
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        throw std::invalid_argument("no such folder: '" + folder + "'");
    }
    std::vector<MediaEntry> entries;
    for (const fs::directory_entry& item :
         fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error)) {
        const std::string name = item.path().filename().string();
        if (name.empty() || name.front() == '.') {
            continue;
        }
        MediaEntry entry{name, fs::absolute(item.path()).lexically_normal().string(), "", 0};
        if (item.is_directory(error)) {
            entry.kind = "folder";
        } else if (detail::isVideoPath(name)) {
            entry.kind = "video";
        } else if (detail::isImagePath(name)) {
            entry.kind = "image";
        } else {
            continue;
        }
        if (entry.kind != "folder") {
            entry.size = item.file_size(error);
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const MediaEntry& x, const MediaEntry& y) {
        return std::make_tuple(x.kind != "folder", x.name) <
               std::make_tuple(y.kind != "folder", y.name);
    });
    return entries;
}

bool isStillSource(const std::string& uri)
{
    if (detail::startsWith(uri, "pattern:")) {
        return true;
    }
    if (detail::startsWith(uri, "gst:") || detail::startsWith(uri, "camera:") ||
        uri.find("://") != std::string::npos) {
        return false;  // pipelines, cameras and network / URI streams
    }
    return !detail::isVideoPath(uri);
}

void validateSourceUri(const std::string& uri)
{
    if (uri.empty()) {
        throw std::invalid_argument("empty input");
    }
    if (detail::startsWith(uri, "pattern:")) {
        const auto parts = detail::split(std::string_view(uri).substr(8), ':');
        detail::parseSize(parts[0], "pattern size");
        if (parts.size() > 2 || (parts.size() == 2 && parts[1] != "gray")) {
            throw std::invalid_argument("usage: pattern:WIDTHxHEIGHT[:gray]");
        }
        return;
    }
    if (detail::startsWith(uri, "camera:")) {
        const auto parts = detail::split(std::string_view(uri).substr(7), ':');
        if (parts.empty() || !std::filesystem::exists(std::string(parts[0]))) {
            throw std::invalid_argument("no such camera: '" + uri.substr(7) + "'");
        }
        if (parts.size() == 2) {
            detail::parseSize(parts[1], "camera size");
        }
        return;
    }
    for (const char* scheme : {"rtp", "tcp"}) {
        const std::string prefix = std::string(scheme) + "://";
        if (detail::startsWith(uri, prefix)) {
            parseListenAddress(uri, scheme);
            return;
        }
    }
    if (detail::startsWith(uri, "gst:")) {
        if (uri.find("scip_sink") == std::string::npos) {
            throw std::invalid_argument("gst: inputs must end with 'appsink name=scip_sink'");
        }
        return;
    }
    if (uri.find("://") != std::string::npos) {
        return;  // network URIs are checked when opened
    }
    if (!std::filesystem::exists(uri)) {
        throw std::invalid_argument("no such file: '" + uri + "'");
    }
}

FrameSourcePtr openSource(const std::string& uri, const SourceOptions& options)
{
    constexpr std::string_view kPattern = "pattern:";
    constexpr std::string_view kGst = "gst:";
    constexpr std::string_view kCamera = "camera:";

    if (detail::startsWith(uri, kPattern)) {
        const auto parts = detail::split(std::string_view(uri).substr(kPattern.size()), ':');
        const auto [width, height] = detail::parseSize(parts[0], "pattern size");
        PixelFormat format = PixelFormat::Bgr8;
        if (parts.size() == 2 && parts[1] == "gray") {
            format = PixelFormat::Gray8;
        } else if (parts.size() != 1) {
            throw std::invalid_argument("usage: pattern:WIDTHxHEIGHT[:gray]");
        }
        return makeRepeatSource(makeTestPattern(FrameInfo{width, height, format}),
                                options.maxFrames);
    }
    if (detail::startsWith(uri, kGst)) {
        return gst::openLaunch(uri.substr(kGst.size()), options);
    }
    if (detail::startsWith(uri, kCamera)) {
        // camera:/dev/video0  or  camera:/dev/video0:1280x720
        const auto parts = detail::split(std::string_view(uri).substr(kCamera.size()), ':');
        int width = 0;
        int height = 0;
        if (parts.size() == 2) {
            std::tie(width, height) = detail::parseSize(parts[1], "camera size");
        } else if (parts.size() != 1 || parts[0].empty()) {
            throw std::invalid_argument("usage: camera:/dev/videoN[:WIDTHxHEIGHT]");
        }
        return gst::openCamera(std::string(parts[0]), width, height, options);
    }
    if (detail::startsWith(uri, "rtp://")) {
        const auto [address, port] = parseListenAddress(uri, "rtp");
        return gst::openRtp(address, port, options);
    }
    if (detail::startsWith(uri, "tcp://")) {
        const auto [address, port] = parseListenAddress(uri, "tcp");
        return gst::openTcp(address, port, options);
    }
    if (uri.find("://") != std::string::npos) {
        return gst::openUri(uri, options);
    }
    if (!std::filesystem::exists(uri)) {
        throw std::runtime_error("no such file: '" + uri + "'");
    }
    if (detail::isVideoPath(uri)) {
        return gst::openVideoFile(uri, options);
    }
    return makeRepeatSource(readImage(uri), options.maxFrames);
}

}  // namespace scip::io

// What a machine offers as video inputs (V4L2 cameras) and outputs (DRM/KMS
// connectors), for the control interface and the host GUI to choose from.

#include "scip/io.hpp"

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <unistd.h>

#ifdef SCIP_HAVE_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace scip::io {

namespace {

/// Closes a file descriptor on scope exit.
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

private:
    int fd_;
};

std::vector<std::string> devicesStartingWith(const std::string& directory,
                                             const std::string& prefix)
{
    std::vector<std::string> paths;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0) {
            paths.push_back(entry.path().string());
        }
    }
    // Natural order: video2 before video10.
    std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return paths;
}

std::string text(const __u8* bytes, std::size_t size)
{
    const auto* chars = reinterpret_cast<const char*>(bytes);
    return std::string(chars, strnlen(chars, size));
}

#ifdef SCIP_HAVE_DRM
std::string connectorTypeName(std::uint32_t type)
{
    switch (type) {
        case DRM_MODE_CONNECTOR_VGA:
            return "VGA";
        case DRM_MODE_CONNECTOR_DVII:
            return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID:
            return "DVI-D";
        case DRM_MODE_CONNECTOR_LVDS:
            return "LVDS";
        case DRM_MODE_CONNECTOR_DisplayPort:
            return "DP";
        case DRM_MODE_CONNECTOR_HDMIA:
            return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB:
            return "HDMI-B";
        case DRM_MODE_CONNECTOR_eDP:
            return "eDP";
        case DRM_MODE_CONNECTOR_DSI:
            return "DSI";
        case DRM_MODE_CONNECTOR_DPI:
            return "DPI";
        default:
            return "Unknown";
    }
}

/// The mode the connector currently shows, else its preferred one.
bool currentMode(int fd, const drmModeConnector& connector, drmModeModeInfo& mode)
{
    if (connector.encoder_id != 0) {
        if (drmModeEncoder* encoder = drmModeGetEncoder(fd, connector.encoder_id)) {
            const std::uint32_t crtcId = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
            if (drmModeCrtc* crtc = drmModeGetCrtc(fd, crtcId)) {
                const bool valid = crtc->mode_valid != 0;
                if (valid) {
                    mode = crtc->mode;
                }
                drmModeFreeCrtc(crtc);
                if (valid) {
                    return true;
                }
            }
        }
    }
    for (int i = 0; i < connector.count_modes; ++i) {
        if ((connector.modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0) {
            mode = connector.modes[i];
            return true;
        }
    }
    if (connector.count_modes > 0) {
        mode = connector.modes[0];
        return true;
    }
    return false;
}
#endif

}  // namespace

std::vector<VideoDevice> listVideoDevices()
{
    std::vector<VideoDevice> devices;
    for (const std::string& path : devicesStartingWith("/dev", "video")) {
        Fd fd(::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
        v4l2_capability capability{};
        if (!fd.valid() || ::ioctl(fd.get(), VIDIOC_QUERYCAP, &capability) != 0) {
            continue;
        }
        const __u32 caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                               ? capability.device_caps
                               : capability.capabilities;
        const bool captures =
            (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) != 0;
        const bool memoryToMemory = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0;
        if (!captures || memoryToMemory) {
            continue;  // metadata nodes, codecs and converters are not cameras
        }
        VideoDevice device;
        device.path = path;
        device.name = text(capability.card, sizeof(capability.card));
        device.driver = text(capability.driver, sizeof(capability.driver));
        device.bus = text(capability.bus_info, sizeof(capability.bus_info));
        devices.push_back(device);
    }
    return devices;
}

std::vector<DisplayOutput> listDisplays()
{
    std::vector<DisplayOutput> displays;
#ifdef SCIP_HAVE_DRM
    for (const std::string& path : devicesStartingWith("/dev/dri", "card")) {
        Fd fd(::open(path.c_str(), O_RDWR | O_CLOEXEC));
        if (!fd.valid()) {
            continue;
        }
        drmModeRes* resources = drmModeGetResources(fd.get());
        if (resources == nullptr) {
            continue;  // render-only device (GPU/NPU), no display
        }
        for (int i = 0; i < resources->count_connectors; ++i) {
            drmModeConnector* connector = drmModeGetConnector(fd.get(), resources->connectors[i]);
            if (connector == nullptr) {
                continue;
            }
            DisplayOutput display;
            display.card = path;
            display.connectorId = static_cast<int>(connector->connector_id);
            display.name = connectorTypeName(connector->connector_type) + "-" +
                           std::to_string(connector->connector_type_id);
            display.connected = connector->connection == DRM_MODE_CONNECTED;
            drmModeModeInfo mode{};
            if (display.connected && currentMode(fd.get(), *connector, mode)) {
                display.width = mode.hdisplay;
                display.height = mode.vdisplay;
                display.refreshHz = static_cast<int>(mode.vrefresh);
            }
            drmModeFreeConnector(connector);
            displays.push_back(display);
        }
        drmModeFreeResources(resources);
    }
#endif
    return displays;
}

bool hasWindowSystem()
{
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
}

}  // namespace scip::io

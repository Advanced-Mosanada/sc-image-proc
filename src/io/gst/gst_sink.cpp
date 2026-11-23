// Frames OUT of the pipeline: any GStreamer pipeline that starts with an appsrc.
//
// Unlike the source, the sink always COPIES each frame: our pipeline reuses its
// output buffer for the next frame while GStreamer may still be encoding or
// showing this one on another thread.
//
// Since we copy anyway, the copy can also convert. On the board, the display
// (kmssink) and the VPU encoder take 32-bit BGRx directly, so we hand them BGRx
// (OpenCV, NEON) instead of GStreamer's videoconvert, which is slow on the A53.

#include "scip/cv.hpp"

#include "gst_io.hpp"
#include "gst_util.hpp"

#include <gst/app/gstappsrc.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace scip::io::gst {

using namespace ::scip::gst;  // shared helpers: launch, Ptr, hasElement...

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr double kDefaultFps = 30.0;
constexpr seconds kCloseTimeout{10};

/// What the appsrc hands to GStreamer.
enum class PushFormat {
    Native,  ///< the frame's own format (BGR or GRAY8), plain copy
    Bgrx,    ///< 32-bit BGRx, converted while copying
};

class GstSink final : public FrameSink {
public:
    using Setup = std::function<void(GstElement* pipeline)>;

    GstSink(const std::string& description,
            const FrameInfo& info,
            double fps,
            std::string label,
            PushFormat pushFormat = PushFormat::Native,
            cv::Size outputSize = {},
            const Setup& setup = {})
        : label_(std::move(label)), info_(info), pushFormat_(pushFormat)
    {
        pipeline_ = launch(description);
        appsrc_ = childByName(pipeline_.get(), "scip_src");

        cv::Size size(info.width, info.height);
        if (!outputSize.empty() && outputSize != size) {
            if (pushFormat != PushFormat::Bgrx) {
                throw std::logic_error("GstSink: scaling needs PushFormat::Bgrx");
            }
            // Largest rectangle with the frame's aspect ratio, centred in the output.
            const double scale = std::min(static_cast<double>(outputSize.width) / info.width,
                                          static_cast<double>(outputSize.height) / info.height);
            const cv::Size fitted(static_cast<int>(info.width * scale),
                                  static_cast<int>(info.height * scale));
            fit_ = cv::Rect((outputSize.width - fitted.width) / 2,
                            (outputSize.height - fitted.height) / 2,
                            fitted.width,
                            fitted.height);
            size = outputSize;
        }

        const GstVideoFormat format =
            pushFormat == PushFormat::Bgrx ? GST_VIDEO_FORMAT_BGRx : toGstFormat(info.format);
        gst_video_info_set_format(
            &video_, format, static_cast<guint>(size.width), static_cast<guint>(size.height));
        // Whole frames per second: the i.MX VPU encoder rejects fractional rates such
        // as 60001/1000, which real recordings often report (60.001 fps -> 60).
        const int rate = std::max(1, static_cast<int>(std::lround(fps > 0.0 ? fps : kDefaultFps)));
        video_.fps_n = rate;
        video_.fps_d = 1;
        frameDuration_ = kNsPerSecond / static_cast<std::uint64_t>(rate);
        maxQueuedBytes_ = 3 * static_cast<std::uint64_t>(video_.size);

        GstCaps* caps = gst_video_info_to_caps(&video_);
        g_object_set(appsrc_.get(),
                     "caps",
                     caps,
                     "format",
                     GST_FORMAT_TIME,
                     "block",
                     FALSE,
                     "max-bytes",
                     static_cast<guint64>(maxQueuedBytes_),
                     nullptr);
        gst_caps_unref(caps);

        if (setup) {
            setup(pipeline_.get());
        }
        setState(pipeline_.get(), GST_STATE_PLAYING, label_);
    }

    ~GstSink() override
    {
        try {
            close();
        } catch (...) {  // destructors must not throw; close() explicitly to see errors
        }
    }

    std::string name() const override { return label_; }

    void write(const Frame& frame) override
    {
        if (closed_) {
            throw std::logic_error(label_ + ": write after close");
        }
        if (frame.info() != info_) {
            throw std::invalid_argument(label_ + ": frame format differs from the one opened");
        }
        try {
            push(frame);
        } catch (...) {
            failed_ = true;
            throw;
        }
    }

    void close() override
    {
        if (closed_) {
            return;
        }
        closed_ = true;
        if (!failed_) {
            // EOS lets muxers finish their files (an MP4 without it is unreadable).
            gst_app_src_end_of_stream(reinterpret_cast<GstAppSrc*>(appsrc_.get()));
            try {
                waitForEos(pipeline_.get(), label_, kCloseTimeout);
            } catch (...) {
                gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
                throw;
            }
        }
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
    }

private:
    void push(const Frame& frame)
    {
        auto* src = reinterpret_cast<GstAppSrc*>(appsrc_.get());

        // Back-pressure: if the downstream is slower than us (encoder, display), wait
        // until there is room. Poll the bus meanwhile, so a failed sink (window closed,
        // network error) is reported instead of making us wait forever.
        while (gst_app_src_get_current_level_bytes(src) >= maxQueuedBytes_) {
            throwIfError(pipeline_.get(), label_, milliseconds(5));
        }
        throwIfError(pipeline_.get(), label_);

        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, video_.size, nullptr);
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            throw std::runtime_error(label_ + ": cannot map output buffer");
        }
        copyInto(frame, map.data);
        gst_buffer_unmap(buffer, &map);

        buffer->pts = written_ * frameDuration_;
        buffer->duration = frameDuration_;
        ++written_;

        const GstFlowReturn flow = gst_app_src_push_buffer(src, buffer);  // takes ownership
        if (flow != GST_FLOW_OK) {
            throwIfError(pipeline_.get(), label_, milliseconds(100));
            throw std::runtime_error(label_ + ": stream stopped (" + gst_flow_get_name(flow) + ")");
        }
    }

    void copyInto(const Frame& frame, std::uint8_t* dst)
    {
        const auto dstStride = static_cast<std::size_t>(video_.stride[0]);
        if (pushFormat_ == PushFormat::Native) {
            const std::size_t rowBytes = info_.rowBytes();
            for (int y = 0; y < info_.height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * dstStride, frame.row(y), rowBytes);
            }
            return;
        }

        const int code =
            info_.format == PixelFormat::Gray8 ? cv::COLOR_GRAY2BGRA : cv::COLOR_BGR2BGRA;
        cv::Mat out(video_.height, video_.width, CV_8UC4, dst, dstStride);
        if (fit_.empty()) {
            cv::cvtColor(asMat(frame), out, code);
            return;
        }
        // Letterbox: black borders, frame scaled into fit_. Convert at the (small)
        // input size first, then resize the 4-channel image (NEON-friendly layout).
        out(cv::Rect(0, 0, out.cols, fit_.y)).setTo(cv::Scalar::all(0));
        out(cv::Rect(0, fit_.y + fit_.height, out.cols, out.rows - fit_.y - fit_.height))
            .setTo(cv::Scalar::all(0));
        out(cv::Rect(0, fit_.y, fit_.x, fit_.height)).setTo(cv::Scalar::all(0));
        out(cv::Rect(fit_.x + fit_.width, fit_.y, out.cols - fit_.x - fit_.width, fit_.height))
            .setTo(cv::Scalar::all(0));
        cv::cvtColor(asMat(frame), scratch_, code);
        cv::Mat target = out(fit_);
        cv::resize(scratch_, target, fit_.size(), 0.0, 0.0, cv::INTER_LINEAR);
    }

    std::string label_;
    FrameInfo info_;
    PushFormat pushFormat_;
    cv::Rect fit_;     // where the frame goes when scaling to a bigger output; empty = 1:1
    cv::Mat scratch_;  // BGRx copy of the frame before scaling, reused between frames
    GstVideoInfo video_{};
    GstClockTime frameDuration_ = 0;
    std::uint64_t maxQueuedBytes_ = 0;
    std::uint64_t written_ = 0;
    bool closed_ = false;
    bool failed_ = false;
    Ptr<GstElement> pipeline_;
    Ptr<GstElement> appsrc_;
};

struct EncoderChain {
    std::string elements;
    PushFormat push;
};

/// H.264 encoder chain: the VPU on the i.MX 8M Plus, x264 (software) on a PC.
///
/// The VPU takes BGRx and converts to YUV itself. For x264 the input is converted
/// to 8-bit 4:2:0 (I420): left to negotiate on its own, x264 would encode gray
/// frames as 4:0:0 and BGR as 4:4:4, which many decoders cannot play.
EncoderChain h264Encoder(bool lowLatency)
{
    if (hasElement("vpuenc_h264")) {
        return {"vpuenc_h264", PushFormat::Bgrx};
    }
    if (hasElement("x264enc")) {
        return {lowLatency ? "videoconvert ! video/x-raw,format=I420 ! x264enc tune=zerolatency "
                             "speed-preset=ultrafast key-int-max=30"
                           : "videoconvert ! video/x-raw,format=I420 ! x264enc speed-preset=fast",
                PushFormat::Native};
    }
    throw std::runtime_error("no H.264 encoder found (vpuenc_h264 or x264enc)");
}

/// Open the display once with a bare kmssink to learn the screen's resolution.
cv::Size kmsScreenSize(int connector)
{
    GstElement* element = gst_element_factory_make("kmssink", nullptr);
    if (element == nullptr) {
        throw std::runtime_error("display: kmssink not available");
    }
    Ptr<GstElement> kms(static_cast<GstElement*>(gst_object_ref_sink(element)));
    g_object_set(kms.get(), "force-modesetting", TRUE, nullptr);
    if (connector >= 0) {
        g_object_set(kms.get(), "connector-id", connector, nullptr);
    }
    const std::string where = connector >= 0 ? "connector " + std::to_string(connector)
                                             : std::string("default connector");
    if (gst_element_set_state(kms.get(), GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
        throw std::runtime_error("display: kmssink cannot open the screen (" + where +
                                 "); list connectors with: modetest -M imx-drm -c");
    }
    gint width = 0;
    gint height = 0;
    g_object_get(kms.get(), "display-width", &width, "display-height", &height, nullptr);
    gst_element_set_state(kms.get(), GST_STATE_NULL);
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("display: no active mode on " + where);
    }
    return {width, height};
}

}  // namespace

FrameSinkPtr openDisplay(const FrameInfo& info, double fps, int connector)
{
    // sync=false: show each frame as soon as it is ready. Use --realtime on the
    // source to play a file at its own speed.
    const bool desktop =
        std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
    if (desktop && hasElement("autovideosink")) {
        return std::make_unique<GstSink>("appsrc name=scip_src ! videoconvert ! autovideosink "
                                         "sync=false",
                                         info,
                                         fps,
                                         "display (window)");
    }
    if (hasElement("kmssink")) {
        // No window system (the board without Weston): draw directly with DRM/KMS.
        // On the i.MX 8M Plus the display planes cannot scale, and kmssink only works
        // when it sets the mode itself (force-modesetting) and gets frames of exactly
        // the screen's size. So: ask kmssink for the screen size, then letterbox.
        const cv::Size screen = kmsScreenSize(connector);
        std::string label = "display (kmssink " + std::to_string(screen.width) + "x" +
                            std::to_string(screen.height);
        label += connector >= 0 ? ", connector " + std::to_string(connector) + ")" : ")";
        return std::make_unique<GstSink>(
            "appsrc name=scip_src ! kmssink name=scip_kms force-modesetting=true sync=false",
            info,
            fps,
            label,
            PushFormat::Bgrx,
            screen,
            [connector](GstElement* pipeline) {
                if (connector >= 0) {
                    g_object_set(childByName(pipeline, "scip_kms").get(),
                                 "connector-id",
                                 connector,
                                 nullptr);
                }
            });
    }
    throw std::runtime_error("display: no window system and no kmssink available");
}

FrameSinkPtr openUdp(const std::string& host, int port, const FrameInfo& info, double fps)
{
    const EncoderChain encoder = h264Encoder(true);
    const std::string description = "appsrc name=scip_src ! " + encoder.elements +
                                    " ! h264parse ! rtph264pay config-interval=1 pt=96 ! "
                                    "udpsink name=scip_udp sync=false";
    return std::make_unique<GstSink>(description,
                                     info,
                                     fps,
                                     "udp " + host + ":" + std::to_string(port),
                                     encoder.push,
                                     cv::Size{},
                                     [&](GstElement* pipeline) {
                                         g_object_set(childByName(pipeline, "scip_udp").get(),
                                                      "host",
                                                      host.c_str(),
                                                      "port",
                                                      port,
                                                      nullptr);
                                     });
}

FrameSinkPtr openVideoFileSink(const std::string& path, const FrameInfo& info, double fps)
{
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string muxer;
    if (ext == ".mp4") {
        muxer = "mp4mux";
    } else if (ext == ".mkv") {
        muxer = "matroskamux";
    } else {
        throw std::invalid_argument("can record .mp4 or .mkv, not '" + ext + "'");
    }
    const EncoderChain encoder = h264Encoder(false);
    const std::string description = "appsrc name=scip_src ! " + encoder.elements +
                                    " ! h264parse ! " + muxer + " ! filesink name=scip_file";
    return std::make_unique<GstSink>(description,
                                     info,
                                     fps,
                                     "file " + path,
                                     encoder.push,
                                     cv::Size{},
                                     [&](GstElement* pipeline) {
                                         g_object_set(childByName(pipeline, "scip_file").get(),
                                                      "location",
                                                      path.c_str(),
                                                      nullptr);
                                     });
}

FrameSinkPtr openLaunchSink(const std::string& description, const FrameInfo& info, double fps)
{
    return std::make_unique<GstSink>(description, info, fps, "gst sink");
}

std::string udpViewerCommand(int port)
{
    return "gst-launch-1.0 udpsrc port=" + std::to_string(port) +
           " caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96\""
           " ! rtpjitterbuffer latency=100 ! rtph264depay ! avdec_h264 ! videoconvert"
           " ! autovideosink sync=false";
}

}  // namespace scip::io::gst

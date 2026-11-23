// Frames INTO the pipeline: any GStreamer pipeline that ends in an appsink.
//
// GStreamer runs decoding / capture on its own threads; appsink keeps a few decoded
// frames ready, so decode and our processing overlap.
//
// Accepted appsink formats, cheapest first:
//   BGR, GRAY8      used as they are, zero-copy (the Frame keeps the GstSample alive
//                   and the buffer returns to the decoder's pool when released)
//   NV12, I420      gray8 from the Y plane, zero-copy: the luma plane IS the gray image
//   BGRx, BGRA      converted to bgr8 on the CPU with OpenCV (NEON), one copy
//
// Why not let GStreamer's videoconvert produce BGR? On the Cortex-A53 it costs
// ~20 ms per 640x512 frame (GStreamer 1.18) - ten times the hardware decode.
// On the board we let the 2D GPU (imxvideoconvert_g2d) produce BGRx instead.

#include "scip/cv.hpp"

#include "gst_io.hpp"
#include "gst_util.hpp"

#include <gst/app/gstappsink.h>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace scip::io::gst {

using namespace ::scip::gst;  // shared helpers: launch, Ptr, hasElement...

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using Clock = std::chrono::steady_clock;

constexpr milliseconds kPollInterval{100};
constexpr seconds kStallTimeout{10};

/// Owns a sample and its CPU mapping for as long as a Frame points at the pixels.
struct MappedSample {
    GstSample* sample = nullptr;
    GstVideoFrame video{};
    bool mapped = false;

    MappedSample() = default;
    MappedSample(const MappedSample&) = delete;
    MappedSample& operator=(const MappedSample&) = delete;
    ~MappedSample()
    {
        if (mapped) {
            gst_video_frame_unmap(&video);
        }
        if (sample != nullptr) {
            gst_sample_unref(sample);
        }
    }
};

enum class Conversion {
    None,       ///< BGR / GRAY8 as delivered
    LumaPlane,  ///< NV12 / I420 -> gray8 from plane 0
    BgrxToBgr,  ///< 32-bit BGRx / BGRA -> 24-bit bgr8
};

struct StreamFormat {
    GstVideoInfo video;
    FrameInfo frame;
    Conversion conversion = Conversion::None;
    double fps = 0.0;
};

StreamFormat parseCaps(GstSample* sample, const std::string& what)
{
    StreamFormat format{};
    GstCaps* caps = gst_sample_get_caps(sample);
    if (caps == nullptr || !gst_video_info_from_caps(&format.video, caps)) {
        throw std::runtime_error(what + ": stream has no raw video caps");
    }

    PixelFormat pixelFormat{};
    const GstVideoFormat gstFormat = format.video.finfo->format;
    switch (gstFormat) {
        case GST_VIDEO_FORMAT_BGR:
            pixelFormat = PixelFormat::Bgr8;
            break;
        case GST_VIDEO_FORMAT_GRAY8:
            pixelFormat = PixelFormat::Gray8;
            break;
        case GST_VIDEO_FORMAT_NV12:
        case GST_VIDEO_FORMAT_NV21:
        case GST_VIDEO_FORMAT_I420:
        case GST_VIDEO_FORMAT_YV12:
            pixelFormat = PixelFormat::Gray8;
            format.conversion = Conversion::LumaPlane;
            break;
        case GST_VIDEO_FORMAT_BGRx:
        case GST_VIDEO_FORMAT_BGRA:
            pixelFormat = PixelFormat::Bgr8;
            format.conversion = Conversion::BgrxToBgr;
            break;
        default:
            throw std::runtime_error(what + ": unsupported pixel format " +
                                     gst_video_format_to_string(gstFormat) +
                                     " (end the pipeline with video/x-raw,format=BGR, GRAY8, "
                                     "NV12, I420 or BGRx)");
    }
    format.frame = FrameInfo{format.video.width, format.video.height, pixelFormat};
    if (format.video.fps_n > 0 && format.video.fps_d > 0) {
        format.fps = static_cast<double>(format.video.fps_n) / format.video.fps_d;
    }
    return format;
}

class GstSource final : public FrameSource {
public:
    using Setup = std::function<void(GstElement* pipeline, GstElement* appsink)>;

    struct Config {
        std::uint64_t maxFrames = 0;  ///< 0 = no limit
        bool live = false;            ///< camera / network: newest frame only, may pause
        bool loop = false;            ///< files: start again at the end
    };

    GstSource(const std::string& description,
              Config config,
              std::string label,
              const Setup& setup = {})
        : label_(std::move(label)), config_(config)
    {
        pipeline_ = launch(description);
        appsink_ = childByName(pipeline_.get(), "scip_sink");
        if (config_.live) {
            // Keep only the newest frame: if processing is slower than the source,
            // skip frames instead of falling further and further behind.
            g_object_set(appsink_.get(), "max-buffers", 1u, "drop", TRUE, nullptr);
        } else {
            // Up to 4 decoded frames may wait for us; the decoder blocks beyond that
            // (back-pressure) instead of dropping frames.
            g_object_set(appsink_.get(), "max-buffers", 4u, "drop", FALSE, nullptr);
        }
        if (setup) {
            setup(pipeline_.get(), appsink_.get());
        }
        setState(pipeline_.get(), GST_STATE_PLAYING, label_);

        if (config_.live) {
            // A live sender may start later; only catch immediate failures
            // (missing device, port in use). The format comes with the first frame.
            throwIfError(pipeline_.get(), label_, std::chrono::milliseconds(200));
            return;
        }
        // Files: wait for the first frame, so a bad file fails right here and the
        // format is known up front. It is kept for the first read().
        const auto deadline = Clock::now() + kStallTimeout;
        while (first_ == nullptr) {
            first_ = pullSample(kPollInterval);
            if (first_ == nullptr && gst_app_sink_is_eos(appSink())) {
                throw std::runtime_error(label_ + ": stream ended before the first frame");
            }
            if (first_ == nullptr && Clock::now() > deadline) {
                throw std::runtime_error(label_ + ": no first frame within " +
                                         std::to_string(kStallTimeout.count()) + " s");
            }
        }
        format_ = parseCaps(first_, label_);
    }

    ~GstSource() override
    {
        if (first_ != nullptr) {
            gst_sample_unref(first_);
        }
        // Release every buffer we hold before stopping. If we stopped early, the
        // appsink queue is full and the producer waits for a free buffer from its
        // pool; NXP's G2D converter waits for that inside the GPU driver, where the
        // shutdown flush cannot wake it, so a plain set_state(NULL) hangs forever.
        // Switching appsink to drop mode and emptying the queue lets it finish.
        g_object_set(appsink_.get(), "drop", TRUE, "max-buffers", 1u, nullptr);
        while (GstSample* sample = gst_app_sink_try_pull_sample(appSink(), 0)) {
            gst_sample_unref(sample);
        }
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
    }

    FrameInfo info() const override { return format_.frame; }
    double frameRate() const override { return format_.fps; }
    bool isLive() const override { return config_.live; }

    bool read(Frame& out) override
    {
        // A file that produces nothing for kStallTimeout is reported as stalled: a
        // stuck element (e.g. a GPU/VPU driver waiting forever) must not hang the
        // app. Live sources may legitimately pause, so they wait indefinitely.
        const auto deadline = Clock::now() + kStallTimeout;
        while (true) {
            switch (tryRead(out, kPollInterval)) {
                case ReadStatus::Frame:
                    return true;
                case ReadStatus::End:
                    return false;
                case ReadStatus::Timeout:
                    if (!config_.live && Clock::now() > deadline) {
                        throw std::runtime_error(label_ + ": stalled, no frame for " +
                                                 std::to_string(kStallTimeout.count()) + " s");
                    }
                    break;
            }
        }
    }

    ReadStatus tryRead(Frame& out, std::chrono::milliseconds timeout) override
    {
        if (config_.maxFrames != 0 && produced_ >= config_.maxFrames) {
            return ReadStatus::End;
        }
        GstSample* sample = std::exchange(first_, nullptr);
        if (sample == nullptr) {
            sample = pullSample(timeout);
        }
        if (sample == nullptr) {
            if (!gst_app_sink_is_eos(appSink())) {
                return ReadStatus::Timeout;
            }
            if (!config_.loop || produced_ == 0) {
                return ReadStatus::End;
            }
            restart();
            return ReadStatus::Timeout;
        }
        out = toFrame(sample);
        out.meta.sequence = produced_++;
        out.meta.timestampNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
                .count();
        return ReadStatus::Frame;
    }

private:
    GstAppSink* appSink() const { return reinterpret_cast<GstAppSink*>(appsink_.get()); }

    /// One sample, or nullptr after `timeout`. Pipeline errors are thrown, so a
    /// broken stream is reported instead of looking like a quiet one.
    GstSample* pullSample(std::chrono::milliseconds timeout)
    {
        GstSample* sample = gst_app_sink_try_pull_sample(appSink(), toClockTime(timeout));
        if (sample == nullptr) {
            throwIfError(pipeline_.get(), label_);
        }
        return sample;
    }

    /// Back to the start of a file (loop). A flushing seek also clears the EOS.
    void restart()
    {
        const auto flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT);
        if (!gst_element_seek_simple(pipeline_.get(), GST_FORMAT_TIME, flags, 0)) {
            throw std::runtime_error(label_ + ": cannot loop, the stream is not seekable");
        }
    }

    Frame toFrame(GstSample* sample)
    {
        auto holder = std::make_shared<MappedSample>();
        holder->sample = sample;  // owned from here on, also if we throw below

        // The format may change mid-stream (a network sender switching videos);
        // callers see it in each frame's info().
        const StreamFormat format = parseCaps(sample, label_);
        format_ = format;

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstVideoInfo video = format.video;
        if (buffer == nullptr ||
            !gst_video_frame_map(&holder->video, &video, buffer, GST_MAP_READ)) {
            throw std::runtime_error(label_ + ": cannot map video buffer");
        }
        holder->mapped = true;

        // gst_video_frame_map honours the producer's real strides (GstVideoMeta),
        // which may include padding, so always use them rather than width * bpp.
        auto* data = static_cast<std::uint8_t*>(holder->video.data[0]);
        const auto stride = static_cast<std::size_t>(holder->video.info.stride[0]);

        if (format.conversion != Conversion::BgrxToBgr) {
            return Frame::wrap(format.frame, data, stride, holder);  // zero-copy
        }
        const cv::Mat bgrx(format.frame.height, format.frame.width, CV_8UC4, data, stride);
        Frame bgr(format.frame);
        cv::Mat dst = asMat(bgr);
        cv::cvtColor(bgrx, dst, cv::COLOR_BGRA2BGR);
        return bgr;  // holder released here: the GStreamer buffer can be reused
    }

    std::string label_;
    Config config_;
    std::uint64_t produced_ = 0;
    Ptr<GstElement> pipeline_;
    Ptr<GstElement> appsink_;
    GstSample* first_ = nullptr;
    StreamFormat format_{};
};

/// Elements converting decoded / captured video into a format GstSource accepts,
/// choosing the cheapest path available on this machine.
std::string convertForScip(const SourceOptions& options)
{
    if (options.gray) {
        // Decoders output NV12/I420: videoconvert passes it through untouched and we
        // read the Y plane. GRAY8 first for sources that can produce it directly.
        return "videoconvert ! video/x-raw,format={GRAY8,NV12,I420}";
    }
    if (hasElement("imxvideoconvert_g2d")) {
        return "imxvideoconvert_g2d ! video/x-raw,format=BGRx";  // i.MX 2D GPU
    }
    return "videoconvert ! video/x-raw,format=BGR";  // fine on a PC
}

/// H.264 decoder for live streams, named explicitly rather than left to decodebin:
/// on a PC decodebin prefers VA-API (vah264dec), which drops most frames of a
/// live RTP stream; on the board vpudec is the VPU.
std::string h264Decoder()
{
    for (const char* decoder : {"vpudec", "avdec_h264", "openh264dec"}) {
        if (hasElement(decoder)) {
            return decoder;
        }
    }
    return "decodebin";
}

/// Only video streams are exposed: audio tracks would otherwise stay unlinked.
constexpr const char* kVideoOnly = "caps=\"video/x-raw(ANY)\" expose-all-streams=false";

}  // namespace

FrameSourcePtr openVideoFile(const std::string& path, const SourceOptions& options)
{
    // decodebin picks the best decoder: vpudec (hardware) on the board,
    // libav on the PC.
    const std::string description =
        std::string("filesrc name=scip_file ! decodebin ") + kVideoOnly + " ! " +
        convertForScip(options) +
        " ! appsink name=scip_sink sync=" + (options.realtime ? "true" : "false");
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.loop = options.loop;
    return std::make_unique<GstSource>(
        description, config, "video '" + path + "'", [&](GstElement* pipeline, GstElement*) {
            g_object_set(
                childByName(pipeline, "scip_file").get(), "location", path.c_str(), nullptr);
        });
}

FrameSourcePtr
openCamera(const std::string& device, int width, int height, const SourceOptions& options)
{
    std::string cameraCaps = "video/x-raw";
    if (width > 0 && height > 0) {
        cameraCaps += ",width=" + std::to_string(width) + ",height=" + std::to_string(height);
    }
    const std::string description = "v4l2src name=scip_cam ! " + cameraCaps + " ! " +
                                    convertForScip(options) +
                                    " ! appsink name=scip_sink sync=false";
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.live = true;
    return std::make_unique<GstSource>(
        description, config, "camera " + device, [&](GstElement* pipeline, GstElement*) {
            g_object_set(
                childByName(pipeline, "scip_cam").get(), "device", device.c_str(), nullptr);
        });
}

FrameSourcePtr openRtp(const std::string& address, int port, const SourceOptions& options)
{
    // H.264 over RTP/UDP, as sent by udp: outputs, the host GUI, or
    // gst-launch-1.0 ... ! x264enc ! rtph264pay ! udpsink. The jitter buffer
    // reorders packets and drops ones that arrive too late (50 ms).
    const std::string description =
        "udpsrc name=scip_udp "
        "caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96\" "
        "! rtpjitterbuffer latency=50 drop-on-latency=true ! rtph264depay ! h264parse ! " +
        h264Decoder() + " ! " + convertForScip(options) + " ! appsink name=scip_sink sync=false";
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.live = true;
    const std::string where =
        (address.empty() ? std::string("*") : address) + ":" + std::to_string(port);
    return std::make_unique<GstSource>(
        description, config, "rtp " + where, [&](GstElement* pipeline, GstElement*) {
            Ptr<GstElement> udp = childByName(pipeline, "scip_udp");
            g_object_set(udp.get(), "port", port, nullptr);
            if (!address.empty()) {
                g_object_set(udp.get(), "address", address.c_str(), nullptr);
            }
        });
}

FrameSourcePtr openUri(const std::string& uri, const SourceOptions& options)
{
    // uridecodebin handles the protocol (rtsp, http, file...) and the decoder.
    const bool live = uri.rfind("rtsp://", 0) == 0;
    const std::string description =
        std::string("uridecodebin name=scip_uri ") + kVideoOnly + " ! " + convertForScip(options) +
        " ! appsink name=scip_sink sync=" + (!live && options.realtime ? "true" : "false");
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.live = live;
    config.loop = !live && options.loop;
    return std::make_unique<GstSource>(
        description, config, uri, [&](GstElement* pipeline, GstElement*) {
            g_object_set(childByName(pipeline, "scip_uri").get(), "uri", uri.c_str(), nullptr);
        });
}

FrameSourcePtr openLaunch(const std::string& description, const SourceOptions& options)
{
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.loop = options.loop;
    return std::make_unique<GstSource>(description, config, "gst source");
}

bool available()
{
    return true;
}

}  // namespace scip::io::gst

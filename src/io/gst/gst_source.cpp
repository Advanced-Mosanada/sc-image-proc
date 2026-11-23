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

    GstSource(const std::string& description,
              std::uint64_t maxFrames,
              std::string label,
              const Setup& setup = {})
        : label_(std::move(label)), maxFrames_(maxFrames)
    {
        pipeline_ = launch(description);
        appsink_ = childByName(pipeline_.get(), "scip_sink");
        // Up to 4 decoded frames may wait for us; the decoder blocks beyond that
        // (back-pressure) instead of dropping frames.
        g_object_set(appsink_.get(), "max-buffers", 4u, "drop", FALSE, nullptr);
        if (setup) {
            setup(pipeline_.get(), appsink_.get());
        }
        setState(pipeline_.get(), GST_STATE_PLAYING, label_);

        // The first frame tells us the size and format; keep it for the first read().
        first_ = pullSample();
        if (first_ == nullptr) {
            throw std::runtime_error(label_ + ": stream ended before the first frame");
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
        auto* sink = reinterpret_cast<GstAppSink*>(appsink_.get());
        g_object_set(appsink_.get(), "drop", TRUE, "max-buffers", 1u, nullptr);
        while (GstSample* sample = gst_app_sink_try_pull_sample(sink, 0)) {
            gst_sample_unref(sample);
        }
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
    }

    FrameInfo info() const override { return format_.frame; }
    double frameRate() const override { return format_.fps; }

    bool read(Frame& out) override
    {
        if (maxFrames_ != 0 && produced_ >= maxFrames_) {
            return false;
        }
        GstSample* sample = first_ != nullptr ? std::exchange(first_, nullptr) : pullSample();
        if (sample == nullptr) {
            return false;  // end of stream
        }
        out = toFrame(sample);
        out.meta.sequence = produced_++;
        out.meta.timestampNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
                .count();
        return true;
    }

private:
    /// Wait for the next sample, checking the bus regularly so a pipeline error
    /// is reported instead of blocking forever. nullptr means end of stream.
    /// A stream that produces nothing for kStallTimeout is reported as stalled: a
    /// stuck element (e.g. a GPU/VPU driver waiting forever) must not hang the app.
    GstSample* pullSample()
    {
        auto* sink = reinterpret_cast<GstAppSink*>(appsink_.get());
        const auto deadline = Clock::now() + kStallTimeout;
        while (true) {
            GstSample* sample = gst_app_sink_try_pull_sample(sink, toClockTime(kPollInterval));
            if (sample != nullptr) {
                return sample;
            }
            throwIfError(pipeline_.get(), label_);
            if (gst_app_sink_is_eos(sink)) {
                return nullptr;
            }
            if (Clock::now() > deadline) {
                throw std::runtime_error(label_ + ": stalled, no frame for " +
                                         std::to_string(kStallTimeout.count()) + " s");
            }
        }
    }

    Frame toFrame(GstSample* sample)
    {
        auto holder = std::make_shared<MappedSample>();
        holder->sample = sample;  // owned from here on, also if we throw below

        const StreamFormat format = parseCaps(sample, label_);
        if (format.frame != format_.frame) {
            throw std::runtime_error(label_ + ": stream changed size or format mid-way");
        }
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
    std::uint64_t maxFrames_;
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

}  // namespace

FrameSourcePtr openVideoFile(const std::string& path, const SourceOptions& options)
{
    // decodebin picks the best decoder: vpudec (hardware) on the board,
    // libav on the PC.
    const std::string description =
        "filesrc name=scip_file ! decodebin ! " + convertForScip(options) +
        " ! appsink name=scip_sink sync=" + (options.realtime ? "true" : "false");
    return std::make_unique<GstSource>(description,
                                       options.maxFrames,
                                       "video '" + path + "'",
                                       [&](GstElement* pipeline, GstElement*) {
                                           g_object_set(childByName(pipeline, "scip_file").get(),
                                                        "location",
                                                        path.c_str(),
                                                        nullptr);
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
    return std::make_unique<GstSource>(
        description,
        options.maxFrames,
        "camera " + device,
        [&](GstElement* pipeline, GstElement* appsink) {
            g_object_set(
                childByName(pipeline, "scip_cam").get(), "device", device.c_str(), nullptr);
            // Live input: if we are too slow, drop old frames rather than fall behind.
            g_object_set(appsink, "max-buffers", 2u, "drop", TRUE, nullptr);
        });
}

FrameSourcePtr openLaunch(const std::string& description, const SourceOptions& options)
{
    return std::make_unique<GstSource>(description, options.maxFrames, "gst source");
}

}  // namespace scip::io::gst

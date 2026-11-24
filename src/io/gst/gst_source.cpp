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

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
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

class GstSource final
    : public FrameSource
    , public Playback {
public:
    using Setup = std::function<void(GstElement* pipeline, GstElement* appsink)>;

    struct Config {
        std::uint64_t maxFrames = 0;  ///< 0 = no limit
        bool live = false;            ///< camera / network: newest frame only, may pause
        bool loop = false;            ///< files: start again at the end
        bool relisten = false;        ///< live servers: wait for the next sender at the end
        bool player = false;          ///< files: controllable (Playback), start paused
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
        setState(pipeline_.get(), config_.player ? GST_STATE_PAUSED : GST_STATE_PLAYING, label_);

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
            // A player starts paused: its first frame is the preroll.
            first_ = config_.player ? pullPreroll(kPollInterval) : pullSample(kPollInterval);
            if (first_ == nullptr && gst_app_sink_is_eos(appSink())) {
                throw std::runtime_error(label_ + ": stream ended before the first frame");
            }
            if (first_ == nullptr && Clock::now() > deadline) {
                throw std::runtime_error(label_ + ": no first frame within " +
                                         std::to_string(kStallTimeout.count()) + " s");
            }
        }
        format_ = parseCaps(first_, label_);
        loop_ = config_.loop;
        skipRepeat_ = config_.player;  // PLAYING renders the preroll buffer again
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
    Playback* playback() override { return config_.player ? this : nullptr; }

    // --- Playback (files opened as a player) -----------------------------------

    void setPaused(bool paused) override
    {
        if (!paused) {
            if (ended_) {
                seekTo(range().first);  // play again from the start (of the A-B range)
            }
            setState(pipeline_.get(), GST_STATE_PLAYING, label_);
            needPreroll_ = false;  // a pending seek frame now arrives as a sample
        } else if (!paused_) {
            setState(pipeline_.get(), GST_STATE_PAUSED, label_);
        }
        paused_ = paused;
    }

    void seek(double position) override { seekTo(position); }

    void setRate(double rate) override
    {
        if (!(rate > 0.0)) {
            throw std::invalid_argument("rate must be > 0");
        }
        rate_ = rate;
        if (!ended_) {
            seekTo(position_);  // the rate is part of the seek
        }
    }

    void setLoop(bool loop, std::optional<double> a, std::optional<double> b) override
    {
        if (a.has_value() != b.has_value()) {
            throw std::invalid_argument("give both A and B, or neither");
        }
        if (a && b && *b <= *a) {
            throw std::invalid_argument("B must be after A");
        }
        loop_ = loop;
        a_ = a;
        b_ = b;
        if (!ended_) {
            seekTo(position_);  // the range is part of the seek
        }
    }

    PlaybackState state() const override
    {
        PlaybackState st;
        st.paused = paused_;
        st.ended = ended_;
        st.position = position_;
        st.duration = duration();
        st.rate = rate_;
        st.loop = loop_;
        st.a = a_;
        st.b = b_;
        return st;
    }

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
        if (config_.player) {
            return readPlayer(out, timeout);
        }
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
            if (config_.relisten) {
                relisten();
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

    /// tryRead() of a player: frames while playing; while paused only the one a
    /// seek lands on (the preroll; pulling a sample while paused would discard it).
    /// At the end it loops, or pauses there, but never ends.
    ReadStatus readPlayer(Frame& out, std::chrono::milliseconds timeout)
    {
        GstSample* sample = std::exchange(first_, nullptr);
        if (sample == nullptr && needPreroll_) {
            sample = pullPreroll(timeout);
            if (sample == nullptr) {
                return ReadStatus::Timeout;  // still seeking
            }
            needPreroll_ = false;
            skipRepeat_ = true;
        }
        if (sample == nullptr) {
            if (paused_) {
                throwIfError(pipeline_.get(), label_);
                std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(20)));
                return ReadStatus::Timeout;
            }
            sample = pullSample(timeout);
            if (sample == nullptr) {
                if (gst_app_sink_is_eos(appSink())) {
                    endReached();
                }
                return ReadStatus::Timeout;
            }
            const bool repeat = skipRepeat_ && bufferPts(sample) == lastPts_;
            skipRepeat_ = false;
            if (repeat) {
                gst_sample_unref(sample);  // the paused frame again, as PLAYING renders it
                return ReadStatus::Timeout;
            }
        }
        lastPts_ = bufferPts(sample);
        position_ = streamSeconds(sample);
        out = toFrame(sample);
        out.meta.sequence = produced_++;
        out.meta.timestampNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
                .count();
        return ReadStatus::Frame;
    }

    /// The end of the video, or of the A-B range.
    void endReached()
    {
        if (loop_ || (a_ && b_)) {
            seekTo(range().first);
            return;
        }
        setState(pipeline_.get(), GST_STATE_PAUSED, label_);
        paused_ = true;
        ended_ = true;
    }

    /// What plays: the A-B range when set, else the whole video.
    std::pair<double, std::optional<double>> range() const
    {
        if (a_ && b_) {
            return {*a_, b_};
        }
        return {0.0, std::nullopt};
    }

    void seekTo(double position)
    {
        const auto [start, end] = range();
        double target = std::max(position, start);
        const double last = end ? *end : duration();
        if (last > 0.0) {
            target = std::min(target, last);
        }
        const auto flags = static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);
        if (!gst_element_seek(pipeline_.get(),
                              rate_,
                              GST_FORMAT_TIME,
                              flags,
                              GST_SEEK_TYPE_SET,
                              toNanoseconds(target),
                              end ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
                              end ? toNanoseconds(*end) : GST_CLOCK_TIME_NONE)) {
            throw std::runtime_error(label_ + ": cannot seek in this video");
        }
        if (ended_) {
            ended_ = false;  // still paused: shows the frame it landed on
        }
        needPreroll_ = paused_;
        position_ = target;
    }

    double duration() const
    {
        if (duration_ <= 0.0) {
            gint64 nanoseconds = 0;
            if (gst_element_query_duration(pipeline_.get(), GST_FORMAT_TIME, &nanoseconds) &&
                nanoseconds > 0) {
                duration_ = static_cast<double>(nanoseconds) / 1e9;
            }
        }
        return duration_;
    }

    static gint64 toNanoseconds(double time) { return static_cast<gint64>(time * 1e9); }

    static GstClockTime bufferPts(GstSample* sample)
    {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        return buffer != nullptr ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    }

    /// The position of a frame in the video, in seconds.
    static double streamSeconds(GstSample* sample)
    {
        const GstClockTime pts = bufferPts(sample);
        const GstSegment* segment = gst_sample_get_segment(sample);
        if (!GST_CLOCK_TIME_IS_VALID(pts)) {
            return 0.0;
        }
        const guint64 time =
            segment != nullptr ? gst_segment_to_stream_time(segment, GST_FORMAT_TIME, pts) : pts;
        return GST_CLOCK_TIME_IS_VALID(time) ? static_cast<double>(time) / 1e9 : 0.0;
    }

    /// The preroll sample (the frame a paused pipeline shows), or nullptr after `timeout`.
    GstSample* pullPreroll(std::chrono::milliseconds timeout)
    {
        GstSample* sample = gst_app_sink_try_pull_preroll(appSink(), toClockTime(timeout));
        if (sample == nullptr) {
            throwIfError(pipeline_.get(), label_);
        }
        return sample;
    }

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

    /// A server source whose sender left (EOS): start again and wait for the next one.
    void relisten()
    {
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        setState(pipeline_.get(), config_.player ? GST_STATE_PAUSED : GST_STATE_PLAYING, label_);
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

    // Player state (config_.player).
    bool paused_ = true;
    bool ended_ = false;
    bool loop_ = false;
    double rate_ = 1.0;
    std::optional<double> a_;
    std::optional<double> b_;
    double position_ = 0.0;
    mutable double duration_ = 0.0;
    bool needPreroll_ = false;  ///< paused after a seek: deliver the frame it lands on
    bool skipRepeat_ = false;   ///< the next sample may repeat the preroll frame
    GstClockTime lastPts_ = GST_CLOCK_TIME_NONE;
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
    config.player = options.player;
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

FrameSourcePtr openRtpRaw(const std::string& address, int port, const SourceOptions& options)
{
    // Raw frames over RTP/UDP, as the host GUI sends in raw UDP mode:
    //   ... ! video/x-raw,format=I420 ! rtpgstpay config-interval=1 ! udpsink host=TARGET port=5000
    // rtpgstpay carries the caps in-band (resent every second, so a receiver that
    // starts late, or a sender with another size, is picked up) and splits each
    // frame over many packets; rtpgstdepay drops a frame that lost a packet, so
    // frames arrive whole or not at all. No jitter buffer: on a LAN packets do not
    // reorder, and at ~60k packets/s it would only cost time.
    // The kernel caps the socket buffer at net.core.rmem_max (208 KB on Debian):
    // raise it on the receiver for large frames, or frames are lost in bursts.
    const std::string description =
        "udpsrc name=scip_udp buffer-size=33554432 "
        "caps=\"application/x-rtp,media=application,clock-rate=90000,encoding-name=X-GST\" "
        "! rtpgstdepay ! " +
        convertForScip(options) + " ! appsink name=scip_sink sync=false";
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.live = true;
    const std::string where =
        (address.empty() ? std::string("*") : address) + ":" + std::to_string(port);
    return std::make_unique<GstSource>(
        description, config, "rtpraw " + where, [&](GstElement* pipeline, GstElement*) {
            Ptr<GstElement> udp = childByName(pipeline, "scip_udp");
            g_object_set(udp.get(), "port", port, nullptr);
            if (!address.empty()) {
                g_object_set(udp.get(), "address", address.c_str(), nullptr);
            }
        });
}

FrameSourcePtr openTcp(const std::string& address, int port, const SourceOptions& options)
{
    // Raw frames in GDP (GStreamer's own framing: caps travel with the data) over
    // TCP, as the host GUI sends in raw mode:
    //   ... ! video/x-raw,format=I420 ! gdppay ! tcpclientsink host=TARGET port=5000
    // Lossless, and the size may change with every sender: each frame carries its
    // format. When a sender disconnects we listen for the next one.
    // (No decodebin: its output pad would not be relinked after the restart.)
    const std::string description = "tcpserversrc name=scip_tcp ! gdpdepay ! " +
                                    convertForScip(options) +
                                    " ! appsink name=scip_sink sync=false";
    GstSource::Config config;
    config.maxFrames = options.maxFrames;
    config.live = true;
    config.relisten = true;
    const std::string host = address.empty() ? std::string("0.0.0.0") : address;
    return std::make_unique<GstSource>(description,
                                       config,
                                       "tcp " + host + ":" + std::to_string(port),
                                       [&](GstElement* pipeline, GstElement*) {
                                           // The default host is "localhost": a sender on the
                                           // network could not connect.
                                           g_object_set(childByName(pipeline, "scip_tcp").get(),
                                                        "host",
                                                        host.c_str(),
                                                        "port",
                                                        port,
                                                        nullptr);
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
    config.player = !live && options.player;
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

std::string version()
{
    guint major = 0;
    guint minor = 0;
    guint micro = 0;
    guint nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
}

bool hasPlugin(const std::string& element)
{
    return hasElement(element.c_str());
}

}  // namespace scip::io::gst

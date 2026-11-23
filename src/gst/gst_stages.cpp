// GStreamer video filters as pipeline functions (see scip/gst_stages.hpp).
//
// Each stage owns a small GStreamer pipeline and runs it synchronously: push one
// frame into appsrc, pull the result from appsink. That suits 1-in/1-out filters
// (flip, colour balance, gamma, crop, median...). Elements that hold frames back
// (temporal filters, rate changers) time out with an error.
//
// Cost: two frame copies plus videoconvert where the element does not accept
// BGR / GRAY8 directly. Fine for trying things out; a function that turns out to
// matter can later get a native implementation.

#include "scip/gst_stages.hpp"

#include "gst_util.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scip::gst {

namespace {

constexpr std::chrono::seconds kFrameTimeout{2};
constexpr GstClockTime kFrameDuration = kNsPerSecond / 30;

/// Properties every element has, or that make no sense to expose.
bool isHiddenProperty(const std::string& name)
{
    return name == "name" || name == "parent" || name == "qos";
}

class GstFilterStage final : public Stage {
public:
    using Setup = std::function<void(GstElement* pipeline)>;

    GstFilterStage(std::string fragment, Setup setup)
        : fragment_(std::move(fragment)), setup_(std::move(setup))
    {}

    ~GstFilterStage() override { stop(); }

    FrameInfo configure(const FrameInfo& input) override
    {
        stop();
        const std::string format = input.format == PixelFormat::Gray8 ? "GRAY8" : "BGR";
        pipeline_ = launch("appsrc name=scip_src format=time ! videoconvert ! " + fragment_ +
                           " ! videoconvert ! video/x-raw,format=" + format +
                           " ! appsink name=scip_sink sync=false");
        src_ = childByName(pipeline_.get(), "scip_src");
        sink_ = childByName(pipeline_.get(), "scip_sink");
        if (setup_) {
            setup_(pipeline_.get());
        }

        gst_video_info_set_format(&inVideo_,
                                  toGstFormat(input.format),
                                  static_cast<guint>(input.width),
                                  static_cast<guint>(input.height));
        inVideo_.fps_n = 30;
        inVideo_.fps_d = 1;
        GstCaps* caps = gst_video_info_to_caps(&inVideo_);
        g_object_set(src_.get(), "caps", caps, nullptr);
        gst_caps_unref(caps);
        input_ = input;
        setState(pipeline_.get(), GST_STATE_PLAYING, what());

        // Some filters change the size (rotate, crop): run one blank frame through
        // to learn the output format.
        Frame blank(input);
        blank.fill(0);
        push(blank);
        GstSample* sample = pull();
        GstVideoInfo outVideo;
        const bool ok = gst_video_info_from_caps(&outVideo, gst_sample_get_caps(sample));
        gst_sample_unref(sample);
        if (!ok) {
            throw std::runtime_error(what() + ": no video caps on the output");
        }
        output_ = FrameInfo{outVideo.width, outVideo.height, input.format};
        return output_;
    }

    void process(const Frame& in, Frame& out) override
    {
        push(in);
        GstSample* sample = pull();
        GstVideoInfo video;
        GstVideoFrame mapped;
        const bool ok =
            gst_video_info_from_caps(&video, gst_sample_get_caps(sample)) &&
            gst_video_frame_map(&mapped, &video, gst_sample_get_buffer(sample), GST_MAP_READ);
        if (!ok) {
            gst_sample_unref(sample);
            throw std::runtime_error(what() + ": cannot map the output frame");
        }
        if (video.width != output_.width || video.height != output_.height) {
            gst_video_frame_unmap(&mapped);
            gst_sample_unref(sample);
            throw std::runtime_error(what() + ": output size changed mid-stream");
        }
        const auto* src = static_cast<const std::uint8_t*>(mapped.data[0]);
        const auto srcStride = static_cast<std::size_t>(mapped.info.stride[0]);
        for (int y = 0; y < output_.height; ++y) {
            std::memcpy(
                out.row(y), src + static_cast<std::size_t>(y) * srcStride, output_.rowBytes());
        }
        gst_video_frame_unmap(&mapped);
        gst_sample_unref(sample);
    }

private:
    std::string what() const { return "GStreamer '" + fragment_ + "'"; }

    void push(const Frame& frame)
    {
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, inVideo_.size, nullptr);
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            throw std::runtime_error(what() + ": cannot map input buffer");
        }
        const auto dstStride = static_cast<std::size_t>(inVideo_.stride[0]);
        for (int y = 0; y < input_.height; ++y) {
            std::memcpy(map.data + static_cast<std::size_t>(y) * dstStride,
                        frame.row(y),
                        input_.rowBytes());
        }
        gst_buffer_unmap(buffer, &map);
        buffer->pts = frames_ * kFrameDuration;
        buffer->duration = kFrameDuration;
        ++frames_;
        if (gst_app_src_push_buffer(reinterpret_cast<GstAppSrc*>(src_.get()), buffer) !=
            GST_FLOW_OK) {
            throwIfError(pipeline_.get(), what(), std::chrono::milliseconds(100));
            throw std::runtime_error(what() + ": refused the frame");
        }
    }

    GstSample* pull()
    {
        GstSample* sample = gst_app_sink_try_pull_sample(reinterpret_cast<GstAppSink*>(sink_.get()),
                                                         toClockTime(kFrameTimeout));
        if (sample == nullptr) {
            throwIfError(pipeline_.get(), what());
            throw std::runtime_error(what() + ": no output within " +
                                     std::to_string(kFrameTimeout.count()) +
                                     " s (only 1-in/1-out filters are supported)");
        }
        return sample;
    }

    void stop()
    {
        if (pipeline_) {
            gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        }
        sink_.reset();
        src_.reset();
        pipeline_.reset();
    }

    std::string fragment_;
    Setup setup_;
    Ptr<GstElement> pipeline_;
    Ptr<GstElement> src_;
    Ptr<GstElement> sink_;
    GstVideoInfo inVideo_{};
    FrameInfo input_;
    FrameInfo output_;
    std::uint64_t frames_ = 0;
};

double clampToInt64(double value)
{
    constexpr double kMax = 9.0e18;  // representable, below INT64_MAX
    return std::clamp(value, -kMax, kMax);
}

/// ParamSpec from a GObject property, or nothing for types we do not expose.
std::optional<ParamSpec> describeProperty(GParamSpec* property)
{
    const std::string name = g_param_spec_get_name(property);
    const char* blurb = g_param_spec_get_blurb(property);
    const std::string description = blurb != nullptr ? blurb : "";
    const std::string type = g_type_name(g_type_fundamental(property->value_type));

    auto integer = [&](double min, double max, double def) {
        return param::integer(name,
                              description,
                              static_cast<std::int64_t>(clampToInt64(def)),
                              static_cast<std::int64_t>(clampToInt64(min)),
                              static_cast<std::int64_t>(clampToInt64(max)));
    };

    if (type == "gboolean") {
        return param::boolean(
            name, description, reinterpret_cast<GParamSpecBoolean*>(property)->default_value);
    }
    if (type == "gint") {
        auto* p = reinterpret_cast<GParamSpecInt*>(property);
        return integer(p->minimum, p->maximum, p->default_value);
    }
    if (type == "guint") {
        auto* p = reinterpret_cast<GParamSpecUInt*>(property);
        return integer(p->minimum, p->maximum, p->default_value);
    }
    if (type == "gint64") {
        auto* p = reinterpret_cast<GParamSpecInt64*>(property);
        return integer(static_cast<double>(p->minimum),
                       static_cast<double>(p->maximum),
                       static_cast<double>(p->default_value));
    }
    if (type == "guint64") {
        auto* p = reinterpret_cast<GParamSpecUInt64*>(property);
        return integer(static_cast<double>(p->minimum),
                       static_cast<double>(p->maximum),
                       static_cast<double>(p->default_value));
    }
    if (type == "gfloat") {
        auto* p = reinterpret_cast<GParamSpecFloat*>(property);
        return param::number(name,
                             description,
                             static_cast<double>(p->default_value),
                             static_cast<double>(p->minimum),
                             static_cast<double>(p->maximum));
    }
    if (type == "gdouble") {
        auto* p = reinterpret_cast<GParamSpecDouble*>(property);
        return param::number(name, description, p->default_value, p->minimum, p->maximum);
    }
    if (type == "GEnum") {
        auto* p = reinterpret_cast<GParamSpecEnum*>(property);
        std::vector<std::string> choices;
        for (guint i = 0; i < p->enum_class->n_values; ++i) {
            choices.emplace_back(p->enum_class->values[i].value_nick);
        }
        const GEnumValue* def = g_enum_get_value(p->enum_class, p->default_value);
        return param::choice(
            name, description, def != nullptr ? def->value_nick : choices.front(), choices);
    }
    if (type == "gchararray") {
        const char* def = reinterpret_cast<GParamSpecString*>(property)->default_value;
        return param::text(name, description, def != nullptr ? def : "");
    }
    return std::nullopt;  // flags, objects, boxed types...
}

std::vector<ParamSpec> describeElement(GstElementFactory* factory)
{
    auto* feature = gst_plugin_feature_load(reinterpret_cast<GstPluginFeature*>(factory));
    if (feature == nullptr) {
        return {};
    }
    const GType type =
        gst_element_factory_get_element_type(reinterpret_cast<GstElementFactory*>(feature));
    gpointer klass = g_type_class_ref(type);
    guint count = 0;
    GParamSpec** properties =
        g_object_class_list_properties(static_cast<GObjectClass*>(klass), &count);

    std::vector<ParamSpec> params;
    for (guint i = 0; i < count; ++i) {
        GParamSpec* property = properties[i];
        const bool writable = (property->flags & G_PARAM_WRITABLE) != 0;
        const bool constructOnly = (property->flags & G_PARAM_CONSTRUCT_ONLY) != 0;
        if (!writable || constructOnly || isHiddenProperty(g_param_spec_get_name(property))) {
            continue;
        }
        if (auto spec = describeProperty(property)) {
            params.push_back(std::move(*spec));
        }
    }
    g_free(static_cast<gpointer>(properties));
    g_type_class_unref(klass);
    gst_object_unref(feature);
    return params;
}

StageInfo makeElementInfo(const std::string& element, GstElementFactory* factory)
{
    StageInfo info;
    info.name = element;
    const char* longName = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_LONGNAME);
    const char* description =
        gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_DESCRIPTION);
    info.title = longName != nullptr ? longName : element;
    info.description = std::string(description != nullptr ? description : "") +
                       " (GStreamer element '" + element + "')";
    info.category = "gstreamer";
    info.backend = "gstreamer";
    info.params = describeElement(factory);

    const std::vector<ParamSpec> specs = info.params;
    info.create = [element, specs](const ParamMap& params) -> StagePtr {
        // Only parameters that differ from the element's defaults are set: some
        // elements have overlapping properties (videoflip: method / video-direction).
        std::vector<std::pair<std::string, std::string>> changed;
        for (const ParamSpec& spec : specs) {
            const ParamValue& value = params.at(spec.name);
            if (!(value == spec.defaultValue)) {
                changed.emplace_back(spec.name, formatValue(value));
            }
        }
        return std::make_unique<GstFilterStage>(
            element + " name=scip_filter", [changed](GstElement* pipeline) {
                Ptr<GstElement> filter = childByName(pipeline, "scip_filter");
                for (const auto& [name, value] : changed) {
                    gst_util_set_object_arg(
                        reinterpret_cast<GObject*>(filter.get()), name.c_str(), value.c_str());
                }
            });
    };
    return info;
}

StageInfo makeChainInfo()
{
    StageInfo info;
    info.name = "gst";
    info.title = "GStreamer filter chain";
    info.description = "Runs any GStreamer video filter chain, written as in gst-launch-1.0, e.g. "
                       "pipeline=\"videoflip method=clockwise ! gamma gamma=1.5\"";
    info.category = "gstreamer";
    info.backend = "gstreamer";
    info.params = {param::text("pipeline", "Filter chain in gst-launch syntax", "identity")};
    info.create = [](const ParamMap& params) -> StagePtr {
        const std::string chain = getString(params, "pipeline");
        if (chain.find_first_not_of(" \t") == std::string::npos) {
            throw std::invalid_argument("pipeline must not be empty");
        }
        return std::make_unique<GstFilterStage>(chain, nullptr);
    };
    return info;
}

}  // namespace

const std::vector<std::string>& defaultFilterElements()
{
    static const std::vector<std::string> elements = {
        "coloreffects",
        "edgetv",
        "gamma",
        "videobalance",
        "videocrop",
        "videoflip",
        "videomedian",
    };
    return elements;
}

int registerStages(StageRegistry& registry, const std::vector<std::string>& elements)
{
    ensureInit();
    int added = registry.add(makeChainInfo()) ? 1 : 0;
    for (const std::string& element : elements) {
        GstElementFactory* factory = gst_element_factory_find(element.c_str());
        if (factory == nullptr) {
            continue;  // not installed here
        }
        if (registry.add(makeElementInfo(element, factory))) {
            ++added;
        }
        gst_object_unref(factory);
    }
    return added;
}

}  // namespace scip::gst

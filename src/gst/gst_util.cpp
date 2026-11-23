#include "gst_util.hpp"

#include <mutex>
#include <stdexcept>

namespace scip::gst {

namespace {

std::string describeError(GstMessage* message)
{
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    gchar* source = gst_object_get_name(message->src);

    std::string text = std::string(source != nullptr ? source : "?") + ": " +
                       (error != nullptr ? error->message : "unknown error");
    if (debug != nullptr) {
        text += "\n    (" + std::string(debug) + ")";
        if (std::string(debug).find("not-negotiated") != std::string::npos) {
            text += "\n    hint: an element cannot handle this frame size or format; check its "
                    "limits with gst-inspect-1.0 <element> (the i.MX VPU encoder accepts "
                    "64x64 .. 1920x1088)";
        }
    }
    g_free(source);
    g_free(debug);
    if (error != nullptr) {
        g_error_free(error);
    }
    return text;
}

}  // namespace

void ensureInit()
{
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

bool hasElement(const char* name)
{
    ensureInit();
    GstElementFactory* factory = gst_element_factory_find(name);
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

Ptr<GstElement> launch(const std::string& description)
{
    ensureInit();
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    if (error != nullptr) {
        const std::string message = error->message;
        g_error_free(error);
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
        }
        throw std::runtime_error("GStreamer cannot build '" + description + "': " + message);
    }
    return Ptr<GstElement>(pipeline);
}

Ptr<GstElement> childByName(GstElement* pipeline, const char* name)
{
    GstElement* child = gst_bin_get_by_name(reinterpret_cast<GstBin*>(pipeline), name);
    if (child == nullptr) {
        throw std::invalid_argument(std::string("GStreamer pipeline needs an element named '") +
                                    name + "'");
    }
    return Ptr<GstElement>(child);
}

void setState(GstElement* pipeline, GstState state, const std::string& what)
{
    if (gst_element_set_state(pipeline, state) == GST_STATE_CHANGE_FAILURE) {
        throwIfError(pipeline, what, std::chrono::milliseconds(100));
        throw std::runtime_error(what + ": GStreamer pipeline failed to start");
    }
}

void throwIfError(GstElement* pipeline, const std::string& what, std::chrono::nanoseconds wait)
{
    Ptr<GstBus> bus(gst_element_get_bus(pipeline));
    GstMessage* message =
        gst_bus_timed_pop_filtered(bus.get(), toClockTime(wait), GST_MESSAGE_ERROR);
    if (message == nullptr) {
        return;
    }
    const std::string text = describeError(message);
    gst_message_unref(message);
    throw std::runtime_error(what + ": " + text);
}

void waitForEos(GstElement* pipeline, const std::string& what, std::chrono::nanoseconds timeout)
{
    Ptr<GstBus> bus(gst_element_get_bus(pipeline));
    const auto types = static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    GstMessage* message = gst_bus_timed_pop_filtered(bus.get(), toClockTime(timeout), types);
    if (message == nullptr) {
        throw std::runtime_error(what + ": timed out waiting for end of stream");
    }
    if (message->type == GST_MESSAGE_ERROR) {
        const std::string text = describeError(message);
        gst_message_unref(message);
        throw std::runtime_error(what + ": " + text);
    }
    gst_message_unref(message);
}

std::optional<PixelFormat> fromGstFormat(GstVideoFormat format)
{
    switch (format) {
        case GST_VIDEO_FORMAT_GRAY8:
            return PixelFormat::Gray8;
        case GST_VIDEO_FORMAT_BGR:
            return PixelFormat::Bgr8;
        default:
            return std::nullopt;
    }
}

GstVideoFormat toGstFormat(PixelFormat format)
{
    switch (format) {
        case PixelFormat::Gray8:
            return GST_VIDEO_FORMAT_GRAY8;
        case PixelFormat::Bgr8:
            return GST_VIDEO_FORMAT_BGR;
    }
    return GST_VIDEO_FORMAT_UNKNOWN;
}

}  // namespace scip::gst

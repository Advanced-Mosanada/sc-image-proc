#pragma once

// Thin C++ helpers over the GStreamer C API: RAII, error reporting, formats.
// Internal to scip (library scip_gst), shared by the I/O code and the GStreamer stages.
//
// Note: GStreamer's casting macros (GST_BIN(), G_OBJECT(), GST_SECOND...) expand to
// C-style casts, which our -Wold-style-cast forbids. This code uses the functions
// and struct fields directly instead.

#include "scip/frame.hpp"

#include <gst/gst.h>
#include <gst/video/video.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace scip::gst {

/// unique_ptr deleter for anything derived from GstObject (elements, buses, pads).
struct ObjectUnref {
    void operator()(void* object) const
    {
        if (object != nullptr) {
            gst_object_unref(object);
        }
    }
};

template <class T>
using Ptr = std::unique_ptr<T, ObjectUnref>;

constexpr std::uint64_t kNsPerSecond = 1'000'000'000ULL;

inline GstClockTime toClockTime(std::chrono::nanoseconds duration)
{
    return static_cast<GstClockTime>(duration.count());
}

/// gst_init() exactly once per process.
void ensureInit();

/// True if an element factory of this name is installed.
bool hasElement(const char* name);

/// Build a pipeline from gst-launch syntax. Throws with GStreamer's message on failure.
Ptr<GstElement> launch(const std::string& description);

/// Find a named element inside a pipeline. Throws if it does not exist.
Ptr<GstElement> childByName(GstElement* pipeline, const char* name);

void setState(GstElement* pipeline, GstState state, const std::string& what);

/// Throw if an error message is (or, within `wait`, becomes) available on the bus.
void throwIfError(GstElement* pipeline,
                  const std::string& what,
                  std::chrono::nanoseconds wait = std::chrono::nanoseconds::zero());

/// Wait for end-of-stream after an EOS was sent. Throws on error or timeout.
void waitForEos(GstElement* pipeline, const std::string& what, std::chrono::nanoseconds timeout);

std::optional<PixelFormat> fromGstFormat(GstVideoFormat format);
GstVideoFormat toGstFormat(PixelFormat format);

}  // namespace scip::gst

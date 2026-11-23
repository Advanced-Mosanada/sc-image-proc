#pragma once

// GStreamer video filters as pipeline functions.
//
// Any GStreamer element that filters raw video can run as a stage: frames go
// through `appsrc ! videoconvert ! <element> ! videoconvert ! appsink`. Its
// parameters are not written by hand but read from the element's GObject
// properties (types, ranges, enum choices, defaults), so they show up in
// `--list-stages` and the control interface like native functions do.

#include "scip/registry.hpp"

#include <string>
#include <vector>

namespace scip::gst {

/// Standard GStreamer video filters exposed by default, when installed.
const std::vector<std::string>& defaultFilterElements();

/// Register GStreamer-backed functions: "gst" (any filter chain given as text)
/// and one function per element in `elements` that is installed on this
/// machine. Returns how many were added; 0 in builds without GStreamer.
int registerStages(StageRegistry& registry,
                   const std::vector<std::string>& elements = defaultFilterElements());

}  // namespace scip::gst

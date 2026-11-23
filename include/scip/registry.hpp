#pragma once

// The catalogue of processing functions.
//
// Each function registers a StageInfo: its name, a description, its parameters
// and a factory. Applications, the control interface and the GUI only talk to the
// registry, so adding a function never touches them:
//
//   // src/stages/my_filter.cpp
//   SCIP_REGISTER_STAGE(makeInfo());   // see src/stages/README.md
//
// Pipelines are written like GStreamer launch lines:
//   gray ! blur ksize=5 sigma=1.5 ! resize width=640 height=480

#include "scip/params.hpp"
#include "scip/stage.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace scip {

/// Description and factory of one processing function.
struct StageInfo {
    std::string name;   ///< identifier used in pipelines, e.g. "blur"
    std::string title;  ///< human readable, e.g. "Gaussian blur"
    std::string description;
    std::string category;            ///< "color", "filter", "geometry"...
    std::string backend = "native";  ///< "native" (C++/OpenCV) or "gstreamer"
    std::vector<ParamSpec> params;
    /// Creates the stage from fully resolved parameters (defaults filled in,
    /// values validated against `params`). May throw std::invalid_argument.
    std::function<StagePtr(const ParamMap& params)> create;
};

/// One element of a pipeline: function name and (possibly partial) parameters.
struct StageSpec {
    std::string name;
    ParamMap params;
};

/// "blur ksize=5"; values with spaces or special characters are quoted.
std::string formatSpec(const StageSpec& spec);
/// Stages joined with " ! ".
std::string formatChain(const std::vector<StageSpec>& chain);

class StageRegistry {
public:
    /// The process-wide registry that SCIP_REGISTER_STAGE fills.
    static StageRegistry& instance();

    /// Register a function. Returns false (and keeps the existing one) if the
    /// name is taken.
    bool add(StageInfo info);

    bool contains(const std::string& name) const;
    /// Throws std::invalid_argument listing the known names if `name` is unknown.
    StageInfo info(const std::string& name) const;
    /// All functions, sorted by name.
    std::vector<StageInfo> list() const;

    /// Defaults filled in and every value validated, without creating anything.
    ParamMap resolve(const StageSpec& spec) const;
    /// Create a stage. Its name() is the spec's text form, e.g. "blur ksize=5".
    StagePtr create(const StageSpec& spec) const;

    /// Parse "gray ! blur ksize=5"; values are typed using the parameter specs.
    std::vector<StageSpec> parseChain(std::string_view text) const;
    std::vector<StagePtr> createChain(std::string_view text) const;

    /// One block per function: name, description, parameters with ranges.
    std::string help() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, StageInfo> stages_;
};

/// Registers a function during static initialisation; use SCIP_REGISTER_STAGE.
struct StageRegistrar {
    explicit StageRegistrar(StageInfo info);
};

#define SCIP_CONCAT_INNER(a, b) a##b
#define SCIP_CONCAT(a, b)       SCIP_CONCAT_INNER(a, b)

/// Register a stage from its own .cpp file: SCIP_REGISTER_STAGE(makeInfo());
/// The file must be compiled into an OBJECT library (see src/stages/CMakeLists.txt):
/// a static library would let the linker drop the unreferenced registration.
#define SCIP_REGISTER_STAGE(infoExpression) \
    static const ::scip::StageRegistrar SCIP_CONCAT(scipStageRegistrar_, __LINE__)(infoExpression)

}  // namespace scip

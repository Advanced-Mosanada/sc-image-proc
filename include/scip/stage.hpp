#pragma once

#include "scip/frame.hpp"

#include <memory>
#include <string>

namespace scip {

/// One processing step of a pipeline (colour conversion, blur, resize...).
///
/// Life cycle:
///   1. configure() once, with the format of the frames that will arrive.
///      The stage validates it and reports the format it will produce.
///   2. process() for every frame. The pipeline passes an output frame that is
///      already allocated with the format returned by configure(), so stages do
///      not allocate in the per-frame path.
class Stage {
public:
    Stage() = default;
    virtual ~Stage() = default;
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    /// Short identifier including parameters, e.g. "blur:5".
    virtual std::string name() const = 0;

    /// Validate `input` and return the output format.
    /// Throws std::invalid_argument for inputs this stage cannot handle.
    virtual FrameInfo configure(const FrameInfo& input) = 0;

    /// Process one frame from `in` into the pre-allocated `out`.
    virtual void process(const Frame& in, Frame& out) = 0;
};

using StagePtr = std::unique_ptr<Stage>;

}  // namespace scip

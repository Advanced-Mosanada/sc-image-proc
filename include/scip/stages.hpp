#pragma once

#include "scip/stage.hpp"

#include <string>
#include <string_view>
#include <vector>

/// The built-in processing stages and a small factory to build them from text.
namespace scip::stages {

/// BGR -> gray (BT.601 luma). Hand-written CPU reference implementation.
StagePtr makeToGray();

/// Gaussian blur with an odd `kernelSize` >= 3 (OpenCV). sigma 0 = derive from size.
StagePtr makeGaussianBlur(int kernelSize, double sigma = 0.0);

/// Bilinear resize to width x height (OpenCV).
StagePtr makeResize(int width, int height);

/// Create a stage from a spec such as "gray", "blur:5", "blur:7:1.5", "resize:640x480".
/// Throws std::invalid_argument with a helpful message on bad specs.
StagePtr create(std::string_view spec);

/// Create every stage of a comma-separated chain, e.g. "gray,blur:5".
std::vector<StagePtr> createChain(std::string_view chain);

/// Human-readable list of available stages and their spec syntax.
std::string help();

}  // namespace scip::stages

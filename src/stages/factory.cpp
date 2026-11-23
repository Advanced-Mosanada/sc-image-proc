#include "scip/stages.hpp"

#include "../common/parse.hpp"

#include <stdexcept>
#include <string>

namespace scip::stages {

namespace {

void expectArgs(const std::vector<std::string_view>& parts,
                std::size_t min,
                std::size_t max,
                std::string_view usage)
{
    const std::size_t args = parts.size() - 1;
    if (args < min || args > max) {
        throw std::invalid_argument("bad stage spec, usage: " + std::string(usage));
    }
}

}  // namespace

std::string help()
{
    return "  gray                  BGR -> gray (BT.601), hand-written C++\n"
           "  blur:K[:SIGMA]        Gaussian blur, odd kernel K >= 3 (OpenCV)\n"
           "  resize:WxH            bilinear resize (OpenCV)\n";
}

StagePtr create(std::string_view spec)
{
    const auto parts = detail::split(detail::trim(spec), ':');
    const std::string_view name = parts.front();

    if (name == "gray") {
        expectArgs(parts, 0, 0, "gray");
        return makeToGray();
    }
    if (name == "blur") {
        expectArgs(parts, 1, 2, "blur:K[:SIGMA]");
        const int kernel = detail::parseInt(parts[1], "blur kernel");
        const double sigma = parts.size() > 2 ? detail::parseDouble(parts[2], "blur sigma") : 0.0;
        return makeGaussianBlur(kernel, sigma);
    }
    if (name == "resize") {
        expectArgs(parts, 1, 1, "resize:WxH");
        const auto [width, height] = detail::parseSize(parts[1], "resize");
        return makeResize(width, height);
    }
    throw std::invalid_argument("unknown stage '" + std::string(name) + "'. Available:\n" + help());
}

std::vector<StagePtr> createChain(std::string_view chain)
{
    std::vector<StagePtr> result;
    for (const std::string_view spec : detail::split(chain, ',')) {
        if (spec.empty()) {
            throw std::invalid_argument("empty stage in pipeline '" + std::string(chain) + "'");
        }
        result.push_back(create(spec));
    }
    return result;
}

}  // namespace scip::stages

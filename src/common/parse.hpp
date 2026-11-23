#pragma once

// Small text-parsing helpers shared by the stage factory and source URIs.
// Internal header: not part of the public include/ API.

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scip::detail {

inline std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

inline std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(separator, start);
        parts.push_back(trim(text.substr(start, end - start)));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

inline int parseInt(std::string_view text, std::string_view what)
{
    int value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (text.empty() || ec != std::errc() || ptr != end) {
        throw std::invalid_argument(std::string(what) + ": '" + std::string(text) +
                                    "' is not an integer");
    }
    return value;
}

inline double parseDouble(std::string_view text, std::string_view what)
{
    // std::from_chars for double needs GCC 11; the board SDK has GCC 10.
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size()) {
        throw std::invalid_argument(std::string(what) + ": '" + copy + "' is not a number");
    }
    return value;
}

/// Parse "640x480".
inline std::pair<int, int> parseSize(std::string_view text, std::string_view what)
{
    const auto parts = split(text, 'x');
    if (parts.size() != 2) {
        throw std::invalid_argument(std::string(what) + ": expected WIDTHxHEIGHT, got '" +
                                    std::string(text) + "'");
    }
    return {parseInt(parts[0], what), parseInt(parts[1], what)};
}

}  // namespace scip::detail

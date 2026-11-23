#pragma once

// URI / file-name classification shared by sources and sinks. Internal header.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace scip::detail {

inline bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

inline std::string lowerExtension(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

inline bool isVideoPath(const std::string& path)
{
    const std::string ext = lowerExtension(path);
    for (const char* known : {".mp4", ".mkv", ".mov", ".avi", ".webm", ".h264", ".264", ".ts"}) {
        if (ext == known) {
            return true;
        }
    }
    return false;
}

inline bool isImagePath(const std::string& path)
{
    const std::string ext = lowerExtension(path);
    for (const char* known :
         {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".pgm", ".ppm", ".pnm", ".webp"}) {
        if (ext == known) {
            return true;
        }
    }
    return false;
}

}  // namespace scip::detail

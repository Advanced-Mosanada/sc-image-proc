// What the target is: board, OS, build, libraries, hardware it can use.
// Everything is read from standard Linux places, so it works the same on the
// PC and on the board, and an unknown field is "" rather than an error.

#include "scip/build_info.hpp"

#include "json.hpp"

#include <opencv2/core/version.hpp>

#include <sys/utsname.h>

#include <chrono>
#include <climits>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace scip::aipc {

namespace {

std::string trim(std::string text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && (isSpace(text.back()) || text.back() == '\0')) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && isSpace(text[start])) {
        ++start;
    }
    return text.substr(start);
}

std::string readFile(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return trim(text.str());
}

/// Lines "key<sep>value" -> map, e.g. /proc/cpuinfo (':') or os-release ('=').
std::multimap<std::string, std::string> readKeyValues(const std::string& path, char separator)
{
    std::multimap<std::string, std::string> values;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const auto at = line.find(separator);
        if (at == std::string::npos) {
            continue;
        }
        std::string value = trim(line.substr(at + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values.emplace(trim(line.substr(0, at)), value);
    }
    return values;
}

std::string first(const std::multimap<std::string, std::string>& values, const std::string& key)
{
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

/// Device tree model on boards ("MYIR i.MX8MP ..."), DMI vendor + product on PCs.
std::string boardName()
{
    std::string model = readFile("/proc/device-tree/model");
    if (!model.empty()) {
        return model;
    }
    const std::string vendor = readFile("/sys/class/dmi/id/sys_vendor");
    const std::string product = readFile("/sys/class/dmi/id/product_name");
    return trim(vendor + " " + product);
}

/// x86 names its CPU; ARM gives a part number, decoded for common cores.
std::string cpuModel()
{
    const auto info = readKeyValues("/proc/cpuinfo", ':');
    std::string model = first(info, "model name");
    if (!model.empty()) {
        return model;
    }
    static const std::map<std::string, std::string> kArmParts = {
        {"0xd03", "Cortex-A53"},
        {"0xd04", "Cortex-A35"},
        {"0xd05", "Cortex-A55"},
        {"0xd07", "Cortex-A57"},
        {"0xd08", "Cortex-A72"},
        {"0xd09", "Cortex-A73"},
        {"0xd0a", "Cortex-A75"},
        {"0xd0b", "Cortex-A76"},
        {"0xd0d", "Cortex-A77"},
        {"0xd41", "Cortex-A78"},
    };
    const std::string part = first(info, "CPU part");
    const auto it = kArmParts.find(part);
    if (it != kArmParts.end()) {
        return it->second;
    }
    return part.empty() ? std::string() : "ARM part " + part;
}

std::int64_t memoryMb()
{
    // "MemTotal:  3024660 kB"
    std::istringstream text(first(readKeyValues("/proc/meminfo", ':'), "MemTotal"));
    std::int64_t kb = 0;
    text >> kb;
    return kb / 1024;
}

Json inputSchemes()
{
    Json schemes = {"pattern", "image"};
    if (io::hasGStreamer()) {
        for (const char* scheme : {"video", "camera", "rtp", "uri", "gst"}) {
            schemes.push_back(scheme);
        }
    }
    return schemes;
}

Json outputSchemes()
{
    Json schemes = Json::array();
    if (io::hasGStreamer()) {
        for (const char* scheme : {"display", "udp", "video"}) {
            schemes.push_back(scheme);
        }
    }
    schemes.push_back("image");
    if (io::hasGStreamer()) {
        schemes.push_back("gst");
    }
    return schemes;
}

}  // namespace

std::string hostName()
{
    char name[HOST_NAME_MAX + 1] = {};
    if (::gethostname(name, sizeof(name) - 1) != 0) {
        return {};
    }
    return name;
}

Json systemInfo(const Identity& identity)
{
    static const double started =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const build::Info& build = build::info();

    utsname uts{};
    ::uname(&uts);
    const std::string host = hostName();

    return {
        {"protocol", {{"name", kProtocolName}, {"version", kProtocolVersion}}},
        {"server",
         {
             {"name", "scipd"},
             {"version", build.version},
             {"commit", build.commit},
             {"describe", build.describe},
             {"branch", build.branch},
             {"commit_date", build.commitDate},
             {"dirty", build.dirty},
             {"build_type", build.buildType},
             {"compiler", build.compiler},
             {"target_cpu", build.targetCpu},
             {"pid", static_cast<std::int64_t>(::getpid())},
             {"started", started},
         }},
        {"device",
         {
             {"name", identity.name.empty() ? host : identity.name},
             {"hostname", host},
             {"board", boardName()},
             {"machine", uts.machine},
             {"cpu", {{"model", cpuModel()}, {"cores", std::thread::hardware_concurrency()}}},
             {"memory_mb", memoryMb()},
         }},
        {"os",
         {
             {"name", first(readKeyValues("/etc/os-release", '='), "PRETTY_NAME")},
             {"kernel", uts.release},
             {"kernel_build", uts.version},
         }},
        {"libraries", {{"gstreamer", io::gstreamerVersion()}, {"opencv", CV_VERSION}}},
        {"capabilities",
         {
             {"gstreamer", io::hasGStreamer()},
             {"window_system", io::hasWindowSystem()},
             {"hardware",
              {
                  {"vpu_decoder", io::hasGstElement("vpudec")},
                  {"vpu_encoder", io::hasGstElement("vpuenc_h264")},
                  {"g2d", io::hasGstElement("imxvideoconvert_g2d")},
              }},
             {"inputs", inputSchemes()},
             {"outputs", outputSchemes()},
         }},
    };
}

}  // namespace scip::aipc

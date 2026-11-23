// JSON forms of the project's types (docs/aipc-protocol.md, section 4).

#include "json.hpp"

#include <cmath>

namespace scip::aipc {

namespace {

/// Integers stay integers on the wire: 5, not 5.0.
Json number(double value, bool integral)
{
    if (integral && std::trunc(value) == value) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

/// Durations and rates are rounded so status messages stay short.
double rounded(double value, double unit = 0.001)
{
    return std::round(value / unit) * unit;
}

std::string typeName(const Json& value)
{
    return value.type_name();
}

RpcError wrongType(const std::string& name, const char* expected, const Json& value)
{
    return RpcError(kInvalidParams,
                    "'" + name + "' must be " + expected + ", not " + typeName(value));
}

}  // namespace

// --- to JSON -----------------------------------------------------------------

Json toJson(const ParamValue& value)
{
    return std::visit([](const auto& v) { return Json(v); }, value);
}

Json toJson(const ParamSpec& spec)
{
    const bool integral = spec.type == ParamType::Int;
    Json j = {
        {"name", spec.name},
        {"type", std::string(toString(spec.type))},
        {"description", spec.description},
        {"default", toJson(spec.defaultValue)},
    };
    if (spec.hasRange()) {
        j["min"] = number(spec.min, integral);
        j["max"] = number(spec.max, integral);
    }
    if (spec.step > 0.0) {
        j["step"] = number(spec.step, integral);
    }
    if (spec.type == ParamType::Enum) {
        j["choices"] = spec.choices;
    }
    if (!spec.unit.empty()) {
        j["unit"] = spec.unit;
    }
    return j;
}

Json toJson(const StageInfo& info)
{
    Json params = Json::array();
    for (const ParamSpec& spec : info.params) {
        params.push_back(toJson(spec));
    }
    return {
        {"name", info.name},
        {"title", info.title},
        {"description", info.description},
        {"category", info.category},
        {"backend", info.backend},
        {"params", params},
    };
}

Json toJson(const StageEntry& entry)
{
    Json params = Json::object();
    for (const auto& [name, value] : entry.spec.params) {
        params[name] = toJson(value);
    }
    return {
        {"id", entry.id},
        {"function", entry.spec.name},
        {"params", params},
        {"enabled", entry.enabled},
        {"text", formatSpec(entry.spec)},
    };
}

Json pipelineJson(const std::vector<StageEntry>& stages)
{
    Json list = Json::array();
    std::vector<StageSpec> running;
    for (const StageEntry& entry : stages) {
        list.push_back(toJson(entry));
        if (entry.enabled) {
            running.push_back(entry.spec);
        }
    }
    return {{"stages", list}, {"text", formatChain(running)}};
}

Json toJson(const FrameInfo& info)
{
    return {
        {"width", info.width},
        {"height", info.height},
        {"format", std::string(toString(info.format))},
    };
}

Json toJson(const InputConfig& input)
{
    return {
        {"uri", input.uri},
        {"loop", input.options.loop},
        {"realtime", input.options.realtime},
        {"gray", input.options.gray},
        {"max_fps", input.maxFps},
        {"max_frames", input.options.maxFrames},
    };
}

Json toJson(const EngineStatus& status)
{
    Json stages = Json::array();
    for (const StageStatus& s : status.stages) {
        stages.push_back({
            {"id", s.id},
            {"text", s.name},
            {"enabled", s.enabled},
            {"active", s.active},
            {"error", s.error},
            {"mean_ms", rounded(s.meanMs)},
            {"p95_ms", rounded(s.p95Ms)},
        });
    }
    Json outputs = Json::array();
    for (const OutputStatus& o : status.outputs) {
        outputs.push_back({
            {"uri", o.uri},
            {"description", o.description},
            {"active", o.active},
            {"error", o.error},
        });
    }
    return {
        {"state", std::string(toString(status.state))},
        {"message", status.message},
        {"input",
         {
             {"uri", status.inputUri},
             {"live", status.inputLive},
             {"fps", rounded(status.inputFps, 0.01)},
             {"info", toJson(status.inputInfo)},
         }},
        {"output_info", toJson(status.outputInfo)},
        {"fps", rounded(status.measuredFps, 0.1)},
        {"frames", status.frames},
        {"timing",
         {
             {"read_ms", rounded(status.readMs)},
             {"process_ms", rounded(status.processMs)},
             {"process_p95_ms", rounded(status.processP95Ms)},
             {"write_ms", rounded(status.writeMs)},
         }},
        {"stages", stages},
        {"outputs", outputs},
    };
}

Json toJson(const io::VideoDevice& device)
{
    Json modes = Json::array();
    for (const io::CameraMode& mode : device.modes) {
        const std::string size = std::to_string(mode.width) + "x" + std::to_string(mode.height);
        modes.push_back({
            {"width", mode.width},
            {"height", mode.height},
            {"fps", rounded(mode.fps, 0.01)},
            {"format", mode.format},
            {"uri", "camera:" + device.path + ":" + size},
        });
    }
    return {
        {"path", device.path},
        {"name", device.name},
        {"driver", device.driver},
        {"bus", device.bus},
        {"uri", "camera:" + device.path},
        {"modes", modes},
    };
}

Json toJson(const io::DisplayOutput& display)
{
    return {
        {"card", display.card},
        {"connector", display.connectorId},
        {"name", display.name},
        {"connected", display.connected},
        {"width", display.width},
        {"height", display.height},
        {"refresh_hz", display.refreshHz},
        {"uri", "display:" + std::to_string(display.connectorId)},
    };
}

// --- from JSON ---------------------------------------------------------------

ParamValue paramValueFromJson(const Json& value, const std::string& name)
{
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    throw wrongType(name, "a boolean, number or string", value);
}

ParamMap paramsFromJson(const Json& params)
{
    if (!params.is_object()) {
        throw wrongType("params", "an object", params);
    }
    ParamMap map;
    for (const auto& [name, value] : params.items()) {
        map[name] = paramValueFromJson(value, name);
    }
    return map;
}

const Json& requireField(const Json& params, const char* name)
{
    const auto it = params.find(name);
    if (it == params.end()) {
        throw RpcError(kInvalidParams, std::string("missing '") + name + "'");
    }
    return *it;
}

std::string getString(const Json& params, const char* name)
{
    const Json& value = requireField(params, name);
    if (!value.is_string()) {
        throw wrongType(name, "a string", value);
    }
    return value.get<std::string>();
}

std::string getString(const Json& params, const char* name, const std::string& fallback)
{
    return params.contains(name) ? getString(params, name) : fallback;
}

std::int64_t getInt(const Json& params, const char* name)
{
    const Json& value = requireField(params, name);
    if (!value.is_number_integer()) {
        throw wrongType(name, "an integer", value);
    }
    return value.get<std::int64_t>();
}

std::int64_t getInt(const Json& params, const char* name, std::int64_t fallback)
{
    return params.contains(name) ? getInt(params, name) : fallback;
}

double getNumber(const Json& params, const char* name, double fallback)
{
    if (!params.contains(name)) {
        return fallback;
    }
    const Json& value = params.at(name);
    if (!value.is_number()) {
        throw wrongType(name, "a number", value);
    }
    return value.get<double>();
}

bool getBool(const Json& params, const char* name, bool fallback)
{
    if (!params.contains(name)) {
        return fallback;
    }
    const Json& value = params.at(name);
    if (!value.is_boolean()) {
        throw wrongType(name, "a boolean", value);
    }
    return value.get<bool>();
}

}  // namespace scip::aipc

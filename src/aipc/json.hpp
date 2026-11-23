#pragma once

// Internal to scip_aipc: the JSON forms of the project's types, as specified
// in docs/aipc-protocol.md section 4. Keeping all of them here keeps the wire
// format in one place.

#include "scip/aipc.hpp"
#include "scip/engine.hpp"
#include "scip/frame.hpp"
#include "scip/io.hpp"
#include "scip/params.hpp"
#include "scip/registry.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace scip::aipc {

using Json = nlohmann::json;

/// Thrown by method handlers; becomes a JSON-RPC error response.
class RpcError : public std::runtime_error {
public:
    RpcError(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
    int code() const { return code_; }

private:
    int code_;
};

// --- to JSON -----------------------------------------------------------------
Json toJson(const ParamValue& value);
Json toJson(const ParamSpec& spec);
Json toJson(const StageInfo& info);
Json toJson(const StageEntry& entry);
Json toJson(const FrameInfo& info);
Json toJson(const InputConfig& input);
Json toJson(const EngineStatus& status);
Json toJson(const io::VideoDevice& device);
Json toJson(const io::DisplayOutput& display);
Json pipelineJson(const std::vector<StageEntry>& stages);

// --- from JSON ---------------------------------------------------------------
// These throw RpcError(kInvalidParams) for a wrong JSON type.
ParamValue paramValueFromJson(const Json& value, const std::string& name);
ParamMap paramsFromJson(const Json& params);

/// Required / optional fields of a params object, with type checks.
const Json& requireField(const Json& params, const char* name);
std::string getString(const Json& params, const char* name);
std::string getString(const Json& params, const char* name, const std::string& fallback);
std::int64_t getInt(const Json& params, const char* name);
std::int64_t getInt(const Json& params, const char* name, std::int64_t fallback);
double getNumber(const Json& params, const char* name, double fallback);
bool getBool(const Json& params, const char* name, bool fallback);

// --- the target --------------------------------------------------------------
/// The `system.info` result (without the "control" part, added by the Service).
Json systemInfo(const Identity& identity);

/// The host name of this machine.
std::string hostName();

}  // namespace scip::aipc

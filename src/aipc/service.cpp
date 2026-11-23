// The AIPC methods (docs/aipc-protocol.md, section 5) on top of the Engine.
//
// Each method is a small function from a params object to a result object,
// registered by name in one table. Adding a method = adding one entry; the
// framing, error mapping and system.methods come for free.

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>

namespace scip::aipc {

namespace {

using Clock = std::chrono::steady_clock;
using Handler = std::function<Json(const Json& params, Session& session)>;

const std::set<std::string> kTopics = {"config", "state", "stats"};
constexpr std::int64_t kMinStatsIntervalMs = 100;

std::string dumpLine(const Json& message)
{
    // replace: a GStreamer error text with invalid UTF-8 must not break the reply.
    return message.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json notification(const std::string& method, Json params)
{
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

Json errorResponse(const Json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

int stageId(const Json& params)
{
    const std::int64_t id = getInt(params, "id");
    if (id <= 0 || id > INT32_MAX) {
        throw RpcError(kNotFound, "no stage with id " + std::to_string(id));
    }
    return static_cast<int>(id);
}

StageSpec specFromJson(const Json& item)
{
    if (!item.is_object()) {
        throw RpcError(kInvalidParams, "each stage must be an object");
    }
    StageSpec spec;
    spec.name = getString(item, "function");
    if (item.contains("params")) {
        spec.params = paramsFromJson(item.at("params"));
    }
    return spec;
}

Json outputsJson(const std::vector<std::string>& outputs)
{
    return {{"outputs", outputs}};
}

}  // namespace

struct Service::Impl {
    Impl(Engine& e, const StageRegistry& r, Identity i)
        : engine(e), registry(r), identity(std::move(i)), started(Clock::now())
    {
        addMethods();
    }

    void addMethods();
    void add(const std::string& name, Handler handler) { methods[name] = std::move(handler); }

    std::string handle(std::string_view line, Session& session);
    Json call(const std::string& method, const Json& params, Session& session);

    void broadcast(const std::string& topic, const std::string& method, const Json& params)
    {
        const std::string line = dumpLine(notification(method, params));
        for (Session* s : sessions) {
            if (s->topics.count(topic) != 0) {
                s->outbox.push_back(line);
            }
        }
    }
    void configChanged(const char* what) { broadcast("config", "event.config", {{"what", what}}); }

    /// Throws kNotFound unless the pipeline has this stage.
    void requireStage(int id) const
    {
        const auto stages = engine.pipeline();
        if (std::none_of(
                stages.begin(), stages.end(), [id](const StageEntry& e) { return e.id == id; })) {
            throw RpcError(kNotFound, "no stage with id " + std::to_string(id));
        }
    }

    Json pipelineWith(const StageEntry& stage) const
    {
        Json result = pipelineJson(engine.pipeline());
        result["stage"] = toJson(stage);
        return result;
    }

    Engine& engine;
    const StageRegistry& registry;
    Identity identity;
    Clock::time_point started;
    std::map<std::string, Handler> methods;
    std::vector<Session*> sessions;

    std::optional<std::pair<EngineState, std::string>> lastState;
};

void Service::Impl::addMethods()
{
    // --- system --------------------------------------------------------------
    add("system.ping", [this](const Json&, Session&) {
        const double now =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        const double uptime = std::chrono::duration<double>(Clock::now() - started).count();
        return Json{{"time", now}, {"uptime", uptime}};
    });
    add("system.info", [this](const Json&, Session&) {
        Json info = systemInfo(identity);
        info["control"] = {
            {"port", identity.port},
            {"discovery_port", identity.discoveryPort},
            {"clients", sessions.size()},
        };
        return info;
    });
    add("system.methods", [this](const Json&, Session&) {
        Json names = Json::array();
        for (const auto& [name, handler] : methods) {
            names.push_back(name);
        }
        return Json{{"methods", names}};
    });

    // --- functions -----------------------------------------------------------
    add("functions.list", [this](const Json&, Session&) {
        Json list = Json::array();
        for (const StageInfo& info : registry.list()) {
            list.push_back(toJson(info));
        }
        return Json{{"functions", list}};
    });
    add("functions.describe", [this](const Json& params, Session&) {
        return toJson(registry.info(getString(params, "name")));
    });

    // --- pipeline ------------------------------------------------------------
    add("pipeline.get", [this](const Json&, Session&) { return pipelineJson(engine.pipeline()); });
    add("pipeline.set", [this](const Json& params, Session&) {
        std::vector<StageSpec> specs;
        std::vector<bool> enabled;
        if (params.contains("text")) {
            specs = registry.parseChain(getString(params, "text"));
            enabled.assign(specs.size(), true);
        } else if (params.contains("stages")) {
            const Json& stages = params.at("stages");
            if (!stages.is_array()) {
                throw RpcError(kInvalidParams, "'stages' must be an array");
            }
            for (const Json& item : stages) {
                specs.push_back(specFromJson(item));
                enabled.push_back(getBool(item, "enabled", true));
            }
        } else {
            throw RpcError(kInvalidParams, "give 'text' or 'stages'");
        }
        const auto entries = engine.setPipeline(specs);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!enabled[i]) {
                engine.updateStage(entries[i].id, std::nullopt, false);
            }
        }
        configChanged("pipeline");
        return pipelineJson(engine.pipeline());
    });
    add("pipeline.add", [this](const Json& params, Session&) {
        StageSpec spec = specFromJson(params);
        const auto index = getInt(params, "index", -1);
        const StageEntry entry =
            engine.addStage(spec,
                            static_cast<int>(std::clamp<std::int64_t>(index, -1, INT32_MAX)),
                            getBool(params, "enabled", true));
        configChanged("pipeline");
        return pipelineWith(entry);
    });
    add("pipeline.update", [this](const Json& params, Session&) {
        const int id = stageId(params);
        requireStage(id);
        std::optional<ParamMap> values;
        if (params.contains("params")) {
            values = paramsFromJson(params.at("params"));
        }
        std::optional<bool> enabled;
        if (params.contains("enabled")) {
            enabled = getBool(params, "enabled", true);
        }
        const StageEntry entry = engine.updateStage(id, values, enabled);
        configChanged("pipeline");
        return pipelineWith(entry);
    });
    add("pipeline.remove", [this](const Json& params, Session&) {
        const int id = stageId(params);
        requireStage(id);
        engine.removeStage(id);
        configChanged("pipeline");
        return pipelineJson(engine.pipeline());
    });
    add("pipeline.move", [this](const Json& params, Session&) {
        const int id = stageId(params);
        requireStage(id);
        const auto index = getInt(params, "index");
        engine.moveStage(id, static_cast<int>(std::clamp<std::int64_t>(index, 0, INT32_MAX)));
        configChanged("pipeline");
        return pipelineJson(engine.pipeline());
    });
    add("pipeline.clear", [this](const Json&, Session&) {
        engine.clearPipeline();
        configChanged("pipeline");
        return pipelineJson(engine.pipeline());
    });

    // --- input ---------------------------------------------------------------
    add("input.get", [this](const Json&, Session&) { return toJson(engine.input()); });
    add("input.set", [this](const Json& params, Session&) {
        InputConfig input;
        input.uri = getString(params, "uri");
        input.options.loop = getBool(params, "loop", false);
        input.options.realtime = getBool(params, "realtime", false);
        input.options.gray = getBool(params, "gray", false);
        input.maxFps = getNumber(params, "max_fps", 0.0);
        const std::int64_t maxFrames = getInt(params, "max_frames", 0);
        if (maxFrames < 0) {
            throw RpcError(kRejected, "max_frames must be >= 0");
        }
        input.options.maxFrames = static_cast<std::uint64_t>(maxFrames);
        engine.setInput(input);
        configChanged("input");
        return toJson(engine.input());
    });
    add("input.devices", [](const Json&, Session&) {
        Json cameras = Json::array();
        for (const auto& device : io::listVideoDevices()) {
            cameras.push_back(toJson(device));
        }
        return Json{{"cameras", cameras}};
    });

    // --- output --------------------------------------------------------------
    add("output.get", [this](const Json&, Session&) { return outputsJson(engine.outputs()); });
    add("output.set", [this](const Json& params, Session&) {
        const Json& list = requireField(params, "outputs");
        if (!list.is_array() ||
            !std::all_of(list.begin(), list.end(), [](const Json& j) { return j.is_string(); })) {
            throw RpcError(kInvalidParams, "'outputs' must be an array of strings");
        }
        engine.setOutputs(list.get<std::vector<std::string>>());
        configChanged("outputs");
        return outputsJson(engine.outputs());
    });
    add("output.add", [this](const Json& params, Session&) {
        const std::string uri = getString(params, "uri");
        auto outputs = engine.outputs();
        if (std::find(outputs.begin(), outputs.end(), uri) != outputs.end()) {
            throw RpcError(kRejected, "output '" + uri + "' is already there");
        }
        outputs.push_back(uri);
        engine.setOutputs(outputs);
        configChanged("outputs");
        return outputsJson(engine.outputs());
    });
    add("output.remove", [this](const Json& params, Session&) {
        const std::string uri = getString(params, "uri");
        auto outputs = engine.outputs();
        const auto it = std::find(outputs.begin(), outputs.end(), uri);
        if (it == outputs.end()) {
            throw RpcError(kNotFound, "no output '" + uri + "'");
        }
        outputs.erase(it);
        engine.setOutputs(outputs);
        configChanged("outputs");
        return outputsJson(engine.outputs());
    });
    add("output.displays", [](const Json&, Session&) {
        Json displays = Json::array();
        for (const auto& display : io::listDisplays()) {
            displays.push_back(toJson(display));
        }
        return Json{{"window_system", io::hasWindowSystem()}, {"displays", displays}};
    });

    // --- stats and events ----------------------------------------------------
    add("stats.get", [this](const Json&, Session&) { return toJson(engine.status()); });
    add("events.subscribe", [](const Json& params, Session& session) {
        const Json& topics = requireField(params, "topics");
        if (!topics.is_array()) {
            throw RpcError(kInvalidParams, "'topics' must be an array");
        }
        for (const Json& topic : topics) {
            if (!topic.is_string() || kTopics.count(topic.get<std::string>()) == 0) {
                throw RpcError(kRejected,
                               "unknown topic " + topic.dump() + " (use config, state, stats)");
            }
        }
        const std::int64_t interval = getInt(params, "interval_ms", session.statsInterval.count());
        if (interval < kMinStatsIntervalMs) {
            throw RpcError(kRejected, "interval_ms must be >= 100");
        }
        for (const Json& topic : topics) {
            session.topics.insert(topic.get<std::string>());
        }
        session.statsInterval = std::chrono::milliseconds(interval);
        session.nextStats = Clock::now();
        return Json{{"topics", session.topics}, {"interval_ms", interval}};
    });
    add("events.unsubscribe", [](const Json& params, Session& session) {
        if (!params.contains("topics")) {
            session.topics.clear();
        } else {
            const Json& topics = params.at("topics");
            if (!topics.is_array()) {
                throw RpcError(kInvalidParams, "'topics' must be an array");
            }
            for (const Json& topic : topics) {
                if (topic.is_string()) {
                    session.topics.erase(topic.get<std::string>());
                }
            }
        }
        return Json{{"topics", session.topics}};
    });
}

std::string Service::Impl::handle(std::string_view line, Session& session)
{
    Json request;
    try {
        request = Json::parse(line);
    } catch (const Json::parse_error& e) {
        return dumpLine(
            errorResponse(nullptr, kParseError, std::string("parse error: ") + e.what()));
    }

    // Validate the envelope (JSON-RPC 2.0, section 4).
    if (!request.is_object()) {
        return dumpLine(errorResponse(nullptr,
                                      kInvalidRequest,
                                      request.is_array() ? "batches are not supported"
                                                         : "a request must be an object"));
    }
    const auto idIt = request.find("id");
    const bool hasId = idIt != request.end();
    const Json id = hasId ? *idIt : Json(nullptr);
    if (hasId && !id.is_string() && !id.is_number() && !id.is_null()) {
        return dumpLine(errorResponse(nullptr, kInvalidRequest, "'id' must be a number or string"));
    }
    const auto version = request.find("jsonrpc");
    const auto method = request.find("method");
    if (version == request.end() || *version != "2.0") {
        return dumpLine(errorResponse(id, kInvalidRequest, "'jsonrpc' must be \"2.0\""));
    }
    if (method == request.end() || !method->is_string()) {
        return dumpLine(errorResponse(id, kInvalidRequest, "'method' must be a string"));
    }
    Json params = Json::object();
    if (const auto p = request.find("params"); p != request.end()) {
        if (!p->is_object()) {
            return dumpLine(errorResponse(id, kInvalidParams, "'params' must be an object"));
        }
        params = *p;
    }

    Json response;
    try {
        const Json result = call(method->get<std::string>(), params, session);
        response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    } catch (const RpcError& e) {
        response = errorResponse(id, e.code(), e.what());
    } catch (const std::invalid_argument& e) {
        response = errorResponse(id, kRejected, e.what());  // validation in engine / registry
    } catch (const Json::exception& e) {
        response = errorResponse(id, kInvalidParams, e.what());
    } catch (const std::exception& e) {
        response = errorResponse(id, kInternalError, e.what());
    }
    return hasId ? dumpLine(response) : std::string();
}

Json Service::Impl::call(const std::string& method, const Json& params, Session& session)
{
    const auto it = methods.find(method);
    if (it == methods.end()) {
        throw RpcError(kMethodNotFound, "unknown method '" + method + "' (see system.methods)");
    }
    return it->second(params, session);
}

// ---------------------------------------------------------------------------

Service::Service(Engine& engine, const StageRegistry& registry, Identity identity)
    : impl_(std::make_unique<Impl>(engine, registry, std::move(identity)))
{}

Service::~Service() = default;

void Service::attach(Session& session)
{
    impl_->sessions.push_back(&session);
}

void Service::detach(Session& session)
{
    auto& sessions = impl_->sessions;
    sessions.erase(std::remove(sessions.begin(), sessions.end(), &session), sessions.end());
}

std::string Service::handle(std::string_view line, Session& session)
{
    return impl_->handle(line, session);
}

void Service::tick()
{
    const EngineStatus status = impl_->engine.status();
    const auto state = std::make_pair(status.state, status.message);
    if (impl_->lastState != state) {
        impl_->lastState = state;
        impl_->broadcast(
            "state",
            "event.state",
            {{"state", std::string(toString(status.state))}, {"message", status.message}});
    }

    const auto now = Clock::now();
    std::optional<std::string> statsLine;  // built once, only if someone wants it
    for (Session* session : impl_->sessions) {
        if (session->topics.count("stats") == 0 || now < session->nextStats) {
            continue;
        }
        if (!statsLine) {
            statsLine = dumpLine(notification("event.stats", toJson(status)));
        }
        session->outbox.push_back(*statsLine);
        session->nextStats = now + session->statsInterval;
    }
}

std::string Service::discoveryReply(std::string_view datagram) const
{
    const Json request = Json::parse(datagram, nullptr, /*allow_exceptions=*/false);
    if (!request.is_object() || request.value("aipc", "") != "discover") {
        return {};
    }
    const Json info = systemInfo(impl_->identity);
    return dumpLine({
        {"aipc", "announce"},
        {"protocol", kProtocolVersion},
        {"name", info["device"]["name"]},
        {"hostname", info["device"]["hostname"]},
        {"board", info["device"]["board"]},
        {"version", info["server"]["version"]},
        {"commit", info["server"]["commit"]},
        {"port", impl_->identity.port},
    });
}

void Service::setIdentity(Identity identity)
{
    impl_->identity = std::move(identity);
}

const Identity& Service::identity() const
{
    return impl_->identity;
}

}  // namespace scip::aipc

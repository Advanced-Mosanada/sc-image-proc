// AIPC: the protocol through Service (no sockets), then once over TCP and UDP.

#include "scip/aipc.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>

using Json = nlohmann::json;
using scip::aipc::Service;
using scip::aipc::Session;

namespace {

/// A service over an idle engine, and a session to talk through.
struct Fixture {
    scip::Engine engine;
    Service service{engine, scip::StageRegistry::instance(), scip::aipc::Identity{"test-target"}};
    Session session;

    Fixture() { service.attach(session); }
    ~Fixture() { service.detach(session); }

    /// The whole response object.
    Json
    request(const std::string& method, const Json& params = Json::object(), Session* from = nullptr)
    {
        const Json message = {
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
        return Json::parse(service.handle(message.dump(), from ? *from : session));
    }

    /// The result; fails the test on an error response.
    Json call(const std::string& method, const Json& params = Json::object())
    {
        const Json response = request(method, params);
        EXPECT_TRUE(response.contains("result")) << method << ": " << response.dump();
        return response.value("result", Json());
    }

    /// The error code (0 if it succeeded).
    int errorCode(const std::string& method, const Json& params = Json::object())
    {
        const Json response = request(method, params);
        return response.contains("error") ? response["error"]["code"].get<int>() : 0;
    }

    int rawErrorCode(const std::string& line)
    {
        return Json::parse(service.handle(line, session))["error"]["code"].get<int>();
    }
};

const Json* findByName(const Json& list, const std::string& name)
{
    for (const Json& item : list) {
        if (item.value("name", "") == name) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace

TEST(Aipc, ChecksTheEnvelope)
{
    Fixture f;
    EXPECT_EQ(f.rawErrorCode("{not json"), scip::aipc::kParseError);
    EXPECT_EQ(f.rawErrorCode("[]"), scip::aipc::kInvalidRequest);
    EXPECT_EQ(f.rawErrorCode(R"({"id":1,"method":"system.ping"})"), scip::aipc::kInvalidRequest);
    EXPECT_EQ(f.rawErrorCode(R"({"jsonrpc":"2.0","id":1,"method":"system.ping","params":[1]})"),
              scip::aipc::kInvalidParams);
    EXPECT_EQ(f.errorCode("no.such.method"), scip::aipc::kMethodNotFound);

    // Responses echo the id; notifications get no response.
    const Json reply = Json::parse(
        f.service.handle(R"({"jsonrpc":"2.0","id":"a7","method":"system.ping"})", f.session));
    EXPECT_EQ(reply["id"], "a7");
    EXPECT_EQ(f.service.handle(R"({"jsonrpc":"2.0","method":"system.ping"})", f.session), "");
}

TEST(Aipc, DescribesTheTarget)
{
    Fixture f;
    const Json info = f.call("system.info");
    EXPECT_EQ(info["protocol"]["name"], "AIPC");
    EXPECT_EQ(info["protocol"]["version"], scip::aipc::kProtocolVersion);
    EXPECT_FALSE(info["server"]["commit"].get<std::string>().empty());
    EXPECT_TRUE(info["server"].contains("branch"));
    EXPECT_EQ(info["device"]["name"], "test-target");
    EXPECT_FALSE(info["os"]["kernel"].get<std::string>().empty());
    EXPECT_EQ(info["control"]["clients"], 1);

    const Json methods = f.call("system.methods")["methods"];
    EXPECT_NE(std::find(methods.begin(), methods.end(), "pipeline.add"), methods.end());
}

TEST(Aipc, ListsFunctionsWithTheirParameters)
{
    Fixture f;
    const Json functions = f.call("functions.list")["functions"];
    const Json* blur = findByName(functions, "blur");
    ASSERT_NE(blur, nullptr);
    EXPECT_EQ((*blur)["backend"], "native");
    const Json* ksize = findByName((*blur)["params"], "ksize");
    ASSERT_NE(ksize, nullptr);
    EXPECT_EQ((*ksize)["type"], "int");
    EXPECT_EQ((*ksize)["min"], 3);
    EXPECT_EQ((*ksize)["step"], 2);
    EXPECT_TRUE((*ksize)["default"].is_number_integer());

    EXPECT_EQ(f.call("functions.describe", {{"name", "threshold"}})["name"], "threshold");
    EXPECT_EQ(f.errorCode("functions.describe", {{"name", "nope"}}), scip::aipc::kRejected);
    EXPECT_EQ(f.errorCode("functions.describe"), scip::aipc::kInvalidParams);
}

TEST(Aipc, EditsThePipeline)
{
    Fixture f;
    Json p = f.call("pipeline.set", {{"text", "gray ! blur ksize=5"}});
    ASSERT_EQ(p["stages"].size(), 2u);
    const int blur = p["stages"][1]["id"];

    // Partial update: ksize stays, sigma is set and stored as a float.
    p = f.call("pipeline.update", {{"id", blur}, {"params", {{"sigma", 2}}}});
    EXPECT_EQ(p["stage"]["params"]["ksize"], 5);
    EXPECT_TRUE(p["stage"]["params"]["sigma"].is_number_float());

    // Rejected changes leave it alone.
    EXPECT_EQ(f.errorCode("pipeline.update", {{"id", blur}, {"params", {{"ksize", 4}}}}),
              scip::aipc::kRejected);
    EXPECT_EQ(f.errorCode("pipeline.update", {{"id", 999}, {"enabled", false}}),
              scip::aipc::kNotFound);
    EXPECT_EQ(f.errorCode("pipeline.add", {{"function", "nope"}}), scip::aipc::kRejected);

    p = f.call("pipeline.add", {{"function", "invert"}, {"index", 0}, {"enabled", false}});
    EXPECT_EQ(p["stages"][0]["function"], "invert");
    EXPECT_EQ(p["text"], "gray ! blur ksize=5 sigma=2");  // disabled stages do not run

    const int invert = p["stage"]["id"];
    p = f.call("pipeline.move", {{"id", invert}, {"index", 99}});
    EXPECT_EQ(p["stages"][2]["id"], invert);
    p = f.call("pipeline.remove", {{"id", invert}});
    EXPECT_EQ(p["stages"].size(), 2u);

    p = f.call("pipeline.set",
               {{"stages",
                 Json::array({{{"function", "resize"}, {"params", {{"width", 32}}}},
                              {{"function", "gray"}, {"enabled", false}}})}});
    EXPECT_EQ(p["text"], "resize width=32");
    EXPECT_EQ(f.call("pipeline.clear")["stages"].size(), 0u);
}

TEST(Aipc, SetsInputAndOutputs)
{
    Fixture f;
    EXPECT_EQ(f.errorCode("input.set", {{"uri", "/no/such.mp4"}}), scip::aipc::kRejected);
    EXPECT_EQ(f.errorCode("input.set", {{"uri", 5}}), scip::aipc::kInvalidParams);
    const Json input = f.call("input.set", {{"uri", "pattern:64x48"}, {"max_fps", 10}});
    EXPECT_EQ(input["uri"], "pattern:64x48");
    EXPECT_EQ(f.call("input.get")["max_fps"], 10.0);

    EXPECT_EQ(f.call("output.set", {{"outputs", {"a.png"}}})["outputs"], Json({"a.png"}));
    EXPECT_EQ(f.call("output.add", {{"uri", "b.png"}})["outputs"].size(), 2u);
    EXPECT_EQ(f.errorCode("output.add", {{"uri", "b.png"}}), scip::aipc::kRejected);
    EXPECT_EQ(f.errorCode("output.add", {{"uri", "b.avi"}}), scip::aipc::kRejected);
    EXPECT_EQ(f.errorCode("output.remove", {{"uri", "c.png"}}), scip::aipc::kNotFound);
    EXPECT_EQ(f.call("output.remove", {{"uri", "a.png"}})["outputs"], Json({"b.png"}));

    EXPECT_TRUE(f.call("input.devices")["cameras"].is_array());
    EXPECT_TRUE(f.call("output.displays")["displays"].is_array());
}

TEST(Aipc, SendsEventsToSubscribers)
{
    Fixture f;
    Session other;
    f.service.attach(other);
    EXPECT_EQ(f.errorCode("events.subscribe", {{"topics", {"weather"}}}), scip::aipc::kRejected);
    EXPECT_EQ(f.errorCode("events.subscribe", {{"topics", {"stats"}}, {"interval_ms", 5}}),
              scip::aipc::kRejected);
    f.call("events.subscribe", {{"topics", {"config", "state", "stats"}}, {"interval_ms", 100}});

    // A change made by another client is announced.
    f.request("pipeline.add", {{"function", "gray"}}, &other);
    ASSERT_EQ(f.session.outbox.size(), 1u);
    EXPECT_EQ(Json::parse(f.session.outbox.front())["params"]["what"], "pipeline");
    f.session.outbox.clear();

    f.service.tick();  // first tick: current state, and stats are due
    bool state = false;
    bool stats = false;
    for (const std::string& line : f.session.outbox) {
        const Json event = Json::parse(line);
        state |= event["method"] == "event.state";
        stats |= event["method"] == "event.stats" && event["params"].contains("fps");
    }
    EXPECT_TRUE(state);
    EXPECT_TRUE(stats);
    EXPECT_TRUE(other.outbox.empty());  // not subscribed

    f.session.outbox.clear();
    f.service.tick();  // nothing changed, next stats not due yet
    EXPECT_TRUE(f.session.outbox.empty());
    f.service.detach(other);
}

TEST(Aipc, AnswersDiscovery)
{
    Fixture f;
    const Json reply = Json::parse(f.service.discoveryReply(R"({"aipc":"discover"})"));
    EXPECT_EQ(reply["aipc"], "announce");
    EXPECT_EQ(reply["name"], "test-target");
    EXPECT_EQ(f.service.discoveryReply("hello"), "");
}

namespace {

/// Read one line from a socket (test helper, blocking with a timeout).
std::string readLine(int fd, std::string& buffer)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (buffer.find('\n') == std::string::npos) {
        pollfd p{fd, POLLIN, 0};
        if (std::chrono::steady_clock::now() > deadline || ::poll(&p, 1, 100) < 0) {
            return {};
        }
        if ((p.revents & POLLIN) != 0) {
            char chunk[4096];
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                return {};
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }
    const auto end = buffer.find('\n');
    std::string line = buffer.substr(0, end);
    buffer.erase(0, end + 1);
    return line;
}

sockaddr_in loopback(int port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

}  // namespace

TEST(AipcServer, ServesTcpAndDiscovery)
{
    scip::Engine engine;
    scip::aipc::ServerConfig config;
    config.bind = "127.0.0.1";
    config.port = 0;
    config.discoveryPort = 0;
    config.name = "loopback";
    scip::aipc::Server server(engine, scip::StageRegistry::instance(), config);
    server.start();
    ASSERT_GT(server.port(), 0);

    const int tcp = ::socket(AF_INET, SOCK_STREAM, 0);
    const sockaddr_in to = loopback(server.port());
    ASSERT_EQ(::connect(tcp, reinterpret_cast<const sockaddr*>(&to), sizeof(to)), 0);

    // Two requests in one packet, the second split across two sends.
    const std::string first = R"({"jsonrpc":"2.0","id":1,"method":"system.ping"})"
                              "\n"
                              R"({"jsonrpc":"2.0","id":2,"meth)";
    const std::string second = R"(od":"system.info"})"
                               "\r\n";
    ASSERT_GT(::send(tcp, first.data(), first.size(), 0), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_GT(::send(tcp, second.data(), second.size(), 0), 0);
    std::string buffer;
    EXPECT_EQ(Json::parse(readLine(tcp, buffer))["id"], 1);
    const Json info = Json::parse(readLine(tcp, buffer));
    EXPECT_EQ(info["result"]["control"]["port"], server.port());
    ::close(tcp);

    const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    const sockaddr_in discovery = loopback(server.discoveryPort());
    const std::string ask = R"({"aipc":"discover"})";
    ::sendto(udp,
             ask.data(),
             ask.size(),
             0,
             reinterpret_cast<const sockaddr*>(&discovery),
             sizeof(discovery));
    pollfd ready{udp, POLLIN, 0};
    ASSERT_EQ(::poll(&ready, 1, 2000), 1);
    char datagram[2048];
    const ssize_t n = ::recv(udp, datagram, sizeof(datagram), 0);
    ASSERT_GT(n, 0);
    const Json announce = Json::parse(std::string(datagram, static_cast<std::size_t>(n)));
    EXPECT_EQ(announce["name"], "loopback");
    EXPECT_EQ(announce["port"], server.port());
    ::close(udp);
    server.stop();
}

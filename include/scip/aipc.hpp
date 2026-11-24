#pragma once

// AIPC, AMC Image Proc Control: the network control interface of the target.
// The protocol is specified in docs/aipc-protocol.md.
//
// Two layers, so the protocol can be tested without sockets:
//   Service  turns request lines into response lines and queues events, for
//            any number of Sessions (one per client).
//   Server   owns the sockets: TCP clients, UDP discovery, and a thread that
//            drives the Service.

#include "scip/engine.hpp"
#include "scip/registry.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace scip::aipc {

inline constexpr char kProtocolName[] = "AIPC";
inline constexpr char kProtocolVersion[] = "1.2";
inline constexpr int kDefaultPort = 5600;
inline constexpr int kDefaultDiscoveryPort = 5601;
inline constexpr std::size_t kMaxMessageBytes = 1024 * 1024;

/// JSON-RPC error codes (docs/aipc-protocol.md, section 3).
enum ErrorCode : int {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams = -32602,
    kInternalError = -32603,
    kRejected = -32000,
    kNotFound = -32001,
    kUnavailable = -32002,
};

/// One connected client: its subscriptions and the events waiting to be sent.
struct Session {
    std::set<std::string> topics;
    std::chrono::milliseconds statsInterval{1000};
    std::chrono::steady_clock::time_point nextStats{};
    std::deque<std::string> outbox;  ///< notification lines, without '\n'
};

/// What the target says about itself beyond what it can find out.
struct Identity {
    std::string name;  ///< shown in GUIs; empty = host name
    int port = kDefaultPort;
    int discoveryPort = kDefaultDiscoveryPort;
};

/// The protocol logic. Not thread-safe: one thread (the server's) calls it.
class Service {
public:
    Service(Engine& engine, const StageRegistry& registry, Identity identity);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    /// Sessions receive events (config changes, subscriptions) while attached.
    void attach(Session& session);
    void detach(Session& session);

    /// Handle one request line. Returns the response line, or "" for a
    /// notification (a request without id).
    std::string handle(std::string_view line, Session& session);

    /// Queue due events (state changes, periodic stats). Call regularly.
    void tick();

    /// The answer to a discovery datagram, or "" if it is not one.
    std::string discoveryReply(std::string_view datagram) const;

    void setIdentity(Identity identity);
    const Identity& identity() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ServerConfig {
    std::string bind = "0.0.0.0";               ///< address to listen on
    int port = kDefaultPort;                    ///< 0 = any free port (tests)
    int discoveryPort = kDefaultDiscoveryPort;  ///< -1 = no discovery, 0 = any free port
    std::string name;                           ///< see Identity::name
    int maxClients = 8;
};

/// Serves AIPC on TCP and answers discovery on UDP, on its own thread.
class Server {
public:
    Server(Engine& engine, const StageRegistry& registry, ServerConfig config);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Bind the sockets (throws std::runtime_error on failure) and start serving.
    void start();
    void stop();

    /// The ports actually bound, after start().
    int port() const;
    int discoveryPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scip::aipc

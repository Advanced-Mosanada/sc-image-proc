// Sockets for AIPC: a TCP listener with one Session per client, a UDP socket
// answering discovery, all served by one thread with poll().
//
// One thread is enough: requests are small and the engine applies them on its
// own thread, so nothing here blocks for long. It also means the Service needs
// no locking.

#include "scip/aipc.hpp"

#include <sys/socket.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace scip::aipc {

namespace {

constexpr int kPollMs = 50;                                  // also the resolution of event timing
constexpr std::size_t kMaxPendingOutput = 16 * 1024 * 1024;  // a client that stopped reading

[[noreturn]] void fail(const std::string& what)
{
    throw std::runtime_error("aipc: " + what + ": " + std::strerror(errno));
}

void closeFd(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fail("fcntl");
    }
}

sockaddr_in address(const std::string& host, int port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("aipc: '" + host + "' is not an IPv4 address");
    }
    return addr;
}

int boundPort(int fd)
{
    sockaddr_in addr{};
    socklen_t size = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size) != 0) {
        fail("getsockname");
    }
    return ntohs(addr.sin_port);
}

int openSocket(int type, const std::string& host, int port)
{
    const int fd = ::socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fail("socket");
    }
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (type == SOCK_DGRAM) {
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }
    const sockaddr_in addr = address(host, port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int error = errno;
        ::close(fd);
        errno = error;
        fail("bind " + host + ":" + std::to_string(port));
    }
    setNonBlocking(fd);
    return fd;
}

std::string errorLine(int code, const std::string& message)
{
    return R"({"jsonrpc":"2.0","id":null,"error":{"code":)" + std::to_string(code) +
           R"(,"message":")" + message + "\"}}\n";
}

}  // namespace

struct Server::Impl {
    struct Client {
        int fd = -1;
        Session session;
        std::string in;
        std::string out;
        bool closeAfterWrite = false;
    };

    Impl(Engine& engine, const StageRegistry& registry, ServerConfig cfg)
        : config(std::move(cfg)), service(engine, registry, Identity{config.name, 0, 0})
    {}

    ~Impl() { closeAll(); }

    void run();
    void accept();
    void receive(Client& client);
    void send(Client& client);
    void answerDiscovery();
    void closeAll();

    ServerConfig config;
    Service service;
    int listenFd = -1;
    int udpFd = -1;
    int wake[2] = {-1, -1};  // a pipe: stop() writes, poll() wakes
    int port = 0;
    int discoveryPort = -1;
    std::vector<std::unique_ptr<Client>> clients;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

void Server::Impl::run()
{
    std::vector<pollfd> fds;
    while (!stopping) {
        fds.clear();
        fds.push_back({wake[0], POLLIN, 0});
        fds.push_back({listenFd, POLLIN, 0});
        fds.push_back({udpFd, static_cast<short>(udpFd >= 0 ? POLLIN : 0), 0});
        for (const auto& c : clients) {
            const short events = static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT));
            fds.push_back({c->fd, events, 0});
        }
        if (::poll(fds.data(), fds.size(), kPollMs) < 0 && errno != EINTR) {
            fail("poll");
        }
        if ((fds[1].revents & POLLIN) != 0) {
            accept();
        }
        if ((fds[2].revents & POLLIN) != 0) {
            answerDiscovery();
        }
        // Clients accepted in this round are not in fds yet: only look at the
        // first ones.
        const std::size_t polled = fds.size() - 3;
        for (std::size_t i = 0; i < polled; ++i) {
            const short revents = fds[i + 3].revents;
            Client& client = *clients[i];
            if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                receive(client);
            }
        }

        service.tick();
        for (auto& c : clients) {
            while (!c->session.outbox.empty()) {
                c->out += c->session.outbox.front();
                c->out += '\n';
                c->session.outbox.pop_front();
            }
            if (!c->out.empty()) {
                send(*c);
            }
        }

        // Drop closed clients.
        for (auto it = clients.begin(); it != clients.end();) {
            Client& c = **it;
            const bool done = c.fd < 0 || (c.closeAfterWrite && c.out.empty()) ||
                              c.out.size() > kMaxPendingOutput;
            if (done) {
                service.detach(c.session);
                closeFd(c.fd);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Server::Impl::accept()
{
    while (true) {
        const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;  // EAGAIN: no more waiting
        }
        const int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));  // small messages
        auto client = std::make_unique<Client>();
        client->fd = fd;
        if (static_cast<int>(clients.size()) >= config.maxClients) {
            client->out = errorLine(kUnavailable, "too many clients");
            client->closeAfterWrite = true;
        } else {
            service.attach(client->session);
        }
        clients.push_back(std::move(client));
    }
}

void Server::Impl::receive(Client& client)
{
    char buffer[64 * 1024];
    while (client.fd >= 0) {
        const ssize_t n = ::recv(client.fd, buffer, sizeof(buffer), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            closeFd(client.fd);  // peer closed or failed
            return;
        }
        if (n < 0) {
            break;
        }
        if (client.closeAfterWrite) {
            continue;  // rejected client: ignore what it sends
        }
        client.in.append(buffer, static_cast<std::size_t>(n));
    }

    std::size_t start = 0;
    while (true) {
        const std::size_t end = client.in.find('\n', start);
        if (end == std::string::npos) {
            break;
        }
        std::string_view line(client.in.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);  // tolerate CRLF (telnet, some terminals)
        }
        if (line.find_first_not_of(" \t") != std::string_view::npos) {
            const std::string response = service.handle(line, client.session);
            if (!response.empty()) {
                client.out += response;
                client.out += '\n';
            }
        }
        start = end + 1;
    }
    client.in.erase(0, start);
    if (client.in.size() > kMaxMessageBytes) {
        client.in.clear();
        client.out += errorLine(kInvalidRequest, "message longer than 1 MiB");
        client.closeAfterWrite = true;
    }
}

void Server::Impl::send(Client& client)
{
    while (client.fd >= 0 && !client.out.empty()) {
        const ssize_t n = ::send(client.fd, client.out.data(), client.out.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                closeFd(client.fd);
            }
            return;
        }
        client.out.erase(0, static_cast<std::size_t>(n));
    }
}

void Server::Impl::answerDiscovery()
{
    char buffer[2048];
    while (true) {
        sockaddr_in from{};
        socklen_t size = sizeof(from);
        const ssize_t n =
            ::recvfrom(udpFd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &size);
        if (n < 0) {
            return;
        }
        const std::string reply =
            service.discoveryReply(std::string_view(buffer, static_cast<std::size_t>(n)));
        if (!reply.empty()) {
            ::sendto(
                udpFd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), size);
        }
    }
}

void Server::Impl::closeAll()
{
    for (auto& c : clients) {
        service.detach(c->session);
        closeFd(c->fd);
    }
    clients.clear();
    closeFd(listenFd);
    closeFd(udpFd);
    closeFd(wake[0]);
    closeFd(wake[1]);
}

// ---------------------------------------------------------------------------

Server::Server(Engine& engine, const StageRegistry& registry, ServerConfig config)
    : impl_(std::make_unique<Impl>(engine, registry, std::move(config)))
{}

Server::~Server()
{
    stop();
}

void Server::start()
{
    Impl& s = *impl_;
    if (s.thread.joinable()) {
        return;
    }
    try {
        s.listenFd = openSocket(SOCK_STREAM, s.config.bind, s.config.port);
        if (::listen(s.listenFd, 16) != 0) {
            fail("listen");
        }
        s.port = boundPort(s.listenFd);
        if (s.config.discoveryPort >= 0) {
            // Broadcasts only reach sockets bound to the wildcard address.
            s.udpFd = openSocket(SOCK_DGRAM, "0.0.0.0", s.config.discoveryPort);
            s.discoveryPort = boundPort(s.udpFd);
        }
        if (::pipe2(s.wake, O_CLOEXEC | O_NONBLOCK) != 0) {
            fail("pipe");
        }
    } catch (...) {
        s.closeAll();
        throw;
    }
    s.service.setIdentity(Identity{s.config.name, s.port, s.discoveryPort});
    s.stopping = false;
    s.thread = std::thread([&s] {
        try {
            s.run();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());  // poll() failed: nothing left to serve
        }
    });
}

void Server::stop()
{
    Impl& s = *impl_;
    if (!s.thread.joinable()) {
        return;
    }
    s.stopping = true;
    const char byte = 0;
    [[maybe_unused]] const ssize_t written = ::write(s.wake[1], &byte, 1);
    s.thread.join();
    s.closeAll();
}

int Server::port() const
{
    return impl_->port;
}

int Server::discoveryPort() const
{
    return impl_->discoveryPort;
}

}  // namespace scip::aipc

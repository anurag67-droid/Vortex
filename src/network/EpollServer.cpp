#include "network/EpollServer.hpp"
#include "network/Connection.hpp"
#include "utils/Logger.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// epoll hands back data.ptr for each event. Client sockets store their
// Connection*; these two addresses tag the listening socket and the wake-up fd.
char listen_tag;
char wake_tag;

// Accept at most this many connections per wake-up so a connection flood can't
// starve the rest of the loop. The listener is level-triggered, so leftovers
// are reported again straight away.
constexpr int MAX_ACCEPTS_PER_WAKEUP = 256;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

} // namespace

EpollServer::EpollServer(const ServerConfig& config) : config_(config), files_(config.root) {
    // Writing to a socket the client already closed raises SIGPIPE, which kills
    // the process by default. Ignore it and handle the EPIPE error instead.
    std::signal(SIGPIPE, SIG_IGN);

    listen_fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listen_fd_.valid()) fail("socket");

    // Allow port reuse so a restart doesn't fail with "Address already in use".
    int opt = 1;
    if (::setsockopt(listen_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fail("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        fail("bind to port " + std::to_string(config.port));
    }
    if (::listen(listen_fd_.get(), SOMAXCONN) < 0) fail("listen");

    socklen_t len = sizeof(address);
    if (::getsockname(listen_fd_.get(), reinterpret_cast<sockaddr*>(&address), &len) < 0) fail("getsockname");
    port_ = ntohs(address.sin_port);

    epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd_.valid()) fail("epoll_create1");

    wake_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!wake_fd_.valid()) fail("eventfd");

    // Not fatal if this fails; we just lose the graceful EMFILE handling.
    reserve_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));

    // The listening socket is level-triggered: if accept() stops early for any
    // reason, epoll reports it again instead of going quiet until the next client.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &listen_tag;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd_.get(), &ev) < 0) fail("epoll_ctl(listen)");
    ev.data.ptr = &wake_tag;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, wake_fd_.get(), &ev) < 0) fail("epoll_ctl(wake)");

    size_t threads = config.threads;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    pool_ = std::make_unique<ThreadPool>(threads);

    Logger::info("Serving " + files_.root() + " on port " + std::to_string(port_) + " with " +
                 std::to_string(threads) + " worker threads" +
                 (config.use_sendfile ? "" : " (sendfile disabled)"));
}

// Members are destroyed in reverse order: the pool joins its workers first,
// then open connections are closed, then the epoll and listening sockets.
EpollServer::~EpollServer() = default;

void EpollServer::start() {
    epoll_event events[MAX_EVENTS];

    while (running_.load()) {
        int num_events = ::epoll_wait(epoll_fd_.get(), events, MAX_EVENTS, -1);
        if (num_events < 0) {
            if (errno == EINTR) continue;  // a signal arrived; re-check running_
            Logger::error(std::string("epoll_wait failed: ") + std::strerror(errno));
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            void* tag = events[i].data.ptr;
            if (tag == &listen_tag) {
                accept_connections();
            } else if (tag == &wake_tag) {
                uint64_t value;
                ssize_t ignored = ::read(wake_fd_.get(), &value, sizeof(value));
                (void)ignored;
            } else {
                // EPOLLONESHOT has disarmed this socket, so no other worker can
                // pick it up until handle() re-arms it.
                auto* conn = static_cast<Connection*>(tag);
                pool_->enqueue([this, conn] { handle(conn); });
            }
        }
    }

    Logger::info("Shutting down: finishing in-flight work...");
    pool_.reset();  // runs queued tasks and joins every worker
    size_t open_connections;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        open_connections = conns_.size();
        conns_.clear();
    }
    Logger::info("Stopped (closed " + std::to_string(open_connections) + " idle connections)");
}

void EpollServer::stop() {
    // Only an atomic store and write(): both are async-signal-safe.
    running_.store(false);
    uint64_t one = 1;
    ssize_t ignored = ::write(wake_fd_.get(), &one, sizeof(one));
    (void)ignored;
}

void EpollServer::accept_connections() {
    for (int accepted = 0; accepted < MAX_ACCEPTS_PER_WAKEUP; ++accepted) {
        int fd = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EMFILE || errno == ENFILE) {
                // Out of descriptors. The client would sit in the backlog and
                // epoll would keep waking us, so free the spare descriptor,
                // accept the client, close it at once, and take the spare back.
                Logger::warn("Out of file descriptors; dropping an incoming connection");
                if (reserve_fd_.valid()) {
                    reserve_fd_.reset();
                    int dropped = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
                    if (dropped >= 0) ::close(dropped);
                    reserve_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                }
                return;
            }
            Logger::error(std::string("accept failed: ") + std::strerror(errno));
            return;
        }

        // Responses are written in one go, so there is nothing for Nagle's
        // algorithm to batch; it would only add latency on keep-alive connections.
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        auto conn = std::make_unique<Connection>(fd, files_, config_.use_sendfile);
        Connection* raw = conn.get();
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            conns_.emplace(raw, std::move(conn));
        }

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.ptr = raw;
        if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
            Logger::error(std::string("epoll_ctl(ADD) failed: ") + std::strerror(errno));
            close_connection(raw);
            continue;
        }
        Logger::debug("Accepted connection (fd " + std::to_string(fd) + ")");
    }
}

// Runs on a worker thread. This worker is the only one touching `conn` until
// rearm() hands it back to epoll; after that it must not touch it again.
void EpollServer::handle(Connection* conn) {
    Connection::Next next;
    try {
        next = conn->on_ready();
    } catch (const std::exception& e) {
        Logger::error(std::string("Error while handling a connection: ") + e.what());
        next = Connection::Next::Close;
    }

    switch (next) {
        case Connection::Next::WantRead:  rearm(conn, EPOLLIN); break;
        case Connection::Next::WantWrite: rearm(conn, EPOLLOUT); break;
        case Connection::Next::Close:     close_connection(conn); break;
    }
}

void EpollServer::rearm(Connection* conn, uint32_t events) {
    epoll_event ev{};
    ev.events = events | EPOLLONESHOT;
    ev.data.ptr = conn;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, conn->fd(), &ev) < 0) {
        close_connection(conn);
    }
}

void EpollServer::close_connection(Connection* conn) {
    ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, conn->fd(), nullptr);
    std::unique_ptr<Connection> owned;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        auto it = conns_.find(conn);
        if (it == conns_.end()) return;
        owned = std::move(it->second);
        conns_.erase(it);
    }
    // `owned` is destroyed here, outside the lock, which closes the socket.
}

#pragma once

#include "concurrency/ThreadPool.hpp"
#include "http/StaticFiles.hpp"
#include "utils/UniqueFd.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class Connection;

struct ServerConfig {
    int port = 8080;               // 0 picks a free port (see EpollServer::port())
    std::string root = "public";   // document root
    size_t threads = 0;            // 0 = one worker per CPU core
    bool use_sendfile = true;      // false copies through user space (baseline for benchmarks)
};

// Reactor plus worker pool: the main thread waits in epoll_wait() and hands
// ready connections to the ThreadPool. Client sockets use EPOLLONESHOT, so a
// connection is disarmed while a worker owns it and re-armed (for read or
// write) when the worker is done with it.
class EpollServer {
public:
    // Throws std::runtime_error if the socket, epoll or document root can't be set up.
    explicit EpollServer(const ServerConfig& config);
    ~EpollServer();

    EpollServer(const EpollServer&) = delete;
    EpollServer& operator=(const EpollServer&) = delete;

    // Runs the event loop until stop() is called, then shuts down cleanly.
    void start();
    // Safe to call from any thread or from a signal handler.
    void stop();

    int port() const { return port_; }

private:
    static constexpr int MAX_EVENTS = 256;

    void accept_connections();
    void handle(Connection* conn);
    void rearm(Connection* conn, uint32_t events);
    void close_connection(Connection* conn);

    ServerConfig config_;
    StaticFiles files_;
    UniqueFd listen_fd_;
    UniqueFd epoll_fd_;
    UniqueFd wake_fd_;     // eventfd that stop() writes to, to interrupt epoll_wait()
    UniqueFd reserve_fd_;  // spare descriptor for shedding load when we run out (EMFILE)
    int port_ = 0;
    std::atomic<bool> running_{true};

    std::mutex conns_mutex_;
    std::unordered_map<Connection*, std::unique_ptr<Connection>> conns_;

    std::unique_ptr<ThreadPool> pool_;
};

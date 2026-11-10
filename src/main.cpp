#include "network/EpollServer.hpp"
#include "utils/Logger.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <string>
#include <sys/resource.h>

namespace {

std::atomic<EpollServer*> g_server{nullptr};

void on_signal(int) {
    int saved_errno = errno;
    if (EpollServer* server = g_server.load()) server->stop();
    errno = saved_errno;
}

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "  -p, --port N        port to listen on (default 8080, 0 = any free port)\n"
              << "  -r, --root DIR      directory to serve (default ./public)\n"
              << "  -t, --threads N     worker threads (default: one per CPU core)\n"
              << "      --no-sendfile   copy files through user space instead of sendfile()\n"
              << "  -v, --verbose       log every request\n"
              << "  -h, --help          show this help\n";
}

// The default soft limit is often 1024 descriptors, which a benchmark with
// 1,000 concurrent connections hits almost immediately.
void raise_fd_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        setrlimit(RLIMIT_NOFILE, &limit);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    ServerConfig config;
    bool verbose = false;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
                return argv[++i];
            };

            if (arg == "-p" || arg == "--port") {
                config.port = std::stoi(value());
                if (config.port < 0 || config.port > 65535) throw std::invalid_argument("port out of range");
            } else if (arg == "-r" || arg == "--root") {
                config.root = value();
            } else if (arg == "-t" || arg == "--threads") {
                config.threads = std::stoul(value());
            } else if (arg == "--no-sendfile") {
                config.use_sendfile = false;
            } else if (arg == "-v" || arg == "--verbose") {
                verbose = true;
            } else if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else {
                throw std::invalid_argument("unknown option " + arg);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    Logger::init();
    if (verbose) Logger::set_level(LogLevel::DEBUG);
    raise_fd_limit();

    try {
        EpollServer server(config);
        g_server.store(&server);

        struct sigaction action{};
        action.sa_handler = on_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);

        Logger::info("Press Ctrl+C to stop");
        server.start();
        g_server.store(nullptr);
    } catch (const std::exception& e) {
        Logger::error(std::string("Startup failed: ") + e.what());
        return 1;
    }
    return 0;
}

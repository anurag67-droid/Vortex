// End-to-end tests: start a real EpollServer on a free port and talk to it
// over TCP sockets, the way a browser or curl would.
#include "check.hpp"
#include "network/EpollServer.hpp"
#include "temp_dir.hpp"
#include "utils/Logger.hpp"
#include "utils/UniqueFd.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr size_t BIG_FILE_SIZE = 16 * 1024 * 1024;

std::string make_big_file() {
    std::string data(BIG_FILE_SIZE, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>('a' + (i * 7 + i / 4096) % 26);
    return data;
}

struct Response {
    bool ok = false;  // false if the connection closed or timed out before a full response arrived
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
};

// A small blocking HTTP client with a read buffer, so pipelined responses
// that arrive in one packet are split correctly.
class Client {
    UniqueFd fd_;
    std::string buf_;

    bool read_more() {
        char chunk[64 * 1024];
        ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buf_.append(chunk, static_cast<size_t>(n));
        return true;
    }

public:
    explicit Client(int port, int recv_buffer = 0) {
        fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (recv_buffer > 0) {
            ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &recv_buffer, sizeof(recv_buffer));
        }
        timeval timeout{5, 0};
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("connect failed");
        }
    }

    void send(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(fd_.get(), data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("send failed");
            sent += static_cast<size_t>(n);
        }
    }

    Response read_response(bool head_request = false) {
        Response r;
        size_t head_end;
        while ((head_end = buf_.find("\r\n\r\n")) == std::string::npos) {
            if (!read_more()) return r;
        }
        std::string head = buf_.substr(0, head_end);
        size_t body_start = head_end + 4;

        size_t line_end = head.find("\r\n");
        std::string status_line = head.substr(0, line_end);
        if (status_line.rfind("HTTP/1.1 ", 0) != 0) return r;
        r.status = std::stoi(status_line.substr(9, 3));

        size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
        while (pos < head.size()) {
            size_t end = head.find("\r\n", pos);
            if (end == std::string::npos) end = head.size();
            std::string line = head.substr(pos, end - pos);
            size_t colon = line.find(':');
            std::string name = line.substr(0, colon);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            size_t value_start = line.find_first_not_of(' ', colon + 1);
            r.headers[name] = value_start == std::string::npos ? "" : line.substr(value_start);
            pos = end + 2;
        }

        size_t length = head_request ? 0 : std::stoul(r.headers["content-length"]);
        while (buf_.size() < body_start + length) {
            if (!read_more()) return r;
        }
        r.body = buf_.substr(body_start, length);
        buf_.erase(0, body_start + length);
        r.ok = true;
        return r;
    }

    // True once the server has closed its end and nothing else is left to read.
    bool closed_by_server() {
        if (!buf_.empty()) return false;
        char c;
        return ::recv(fd_.get(), &c, 1, 0) == 0;
    }
};

std::string get(const std::string& path, const std::string& extra_headers = "") {
    return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extra_headers + "\r\n";
}

class RunningServer {
    EpollServer server_;
    std::thread thread_;

public:
    explicit RunningServer(const ServerConfig& config) : server_(config), thread_([this] { server_.start(); }) {}
    ~RunningServer() { stop(); }
    void stop() {
        if (thread_.joinable()) {
            server_.stop();
            thread_.join();
        }
    }
    int port() const { return server_.port(); }
};

struct Site {
    TempDir tmp;
    std::string root;
    std::string index = "<!DOCTYPE html><title>home</title>hello from vortex\n";
    std::string big = make_big_file();

    Site() {
        root = tmp.mkdir("public");
        tmp.write("public/index.html", index);
        tmp.write("public/app.js", "console.log('hi');\n");
        tmp.write("public/data.json", "{\"ok\":true}\n");
        tmp.write("public/big.bin", big);
        tmp.write("public/.env", "API_KEY=do-not-serve");
        tmp.write("secret.txt", "TOP SECRET outside the document root");
        tmp.symlink(tmp.path() + "/secret.txt", "public/escape.txt");
    }
};

void test_basic_get(int port, const Site& site) {
    std::cout << "  GET / serves index.html with correct headers\n";
    Client c(port);
    c.send(get("/"));
    Response r = c.read_response();
    CHECK(r.ok);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, site.index);
    CHECK_EQ(r.headers["content-type"], "text/html; charset=utf-8");
    CHECK_EQ(r.headers["content-length"], std::to_string(site.index.size()));
    CHECK_EQ(r.headers["connection"], "keep-alive");
    CHECK(!r.headers["date"].empty());

    c.send(get("/data.json?cache=1"));
    r = c.read_response();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers["content-type"], "application/json");
}

void test_path_traversal(int port) {
    std::cout << "  path traversal, encoded traversal, symlink escape and dotfiles are refused\n";
    for (const char* path : {"/../secret.txt", "/%2e%2e/secret.txt", "/%2E%2E%2Fsecret.txt",
                             "/./../secret.txt", "/escape.txt", "/.env"}) {
        Client c(port);
        c.send(get(path));
        Response r = c.read_response();
        CHECK(r.ok);
        CHECK_EQ(r.status, 403);
        CHECK(r.body.find("SECRET") == std::string::npos);
        CHECK(r.body.find("API_KEY") == std::string::npos);
    }
}

void test_keep_alive(int port, const Site& site) {
    std::cout << "  keep-alive: several requests (including a 404) on one connection\n";
    Client c(port);
    for (int i = 0; i < 3; ++i) {
        c.send(get("/"));
        Response r = c.read_response();
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, site.index);
    }
    c.send(get("/missing.html"));
    Response r = c.read_response();
    CHECK_EQ(r.status, 404);
    CHECK_EQ(r.headers["connection"], "keep-alive");
    c.send(get("/app.js"));
    r = c.read_response();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.headers["content-type"], "text/javascript; charset=utf-8");
}

void test_pipelining_and_head(int port, const Site& site) {
    std::cout << "  pipelined requests in one packet, including HEAD\n";
    Client c(port);
    c.send(get("/") + "HEAD /big.bin HTTP/1.1\r\nHost: x\r\n\r\n" + get("/app.js"));
    Response a = c.read_response();
    Response b = c.read_response(/*head_request=*/true);
    Response d = c.read_response();
    CHECK_EQ(a.body, site.index);
    CHECK_EQ(b.status, 200);
    CHECK_EQ(b.headers["content-length"], std::to_string(BIG_FILE_SIZE));
    CHECK_EQ(d.status, 200);
    CHECK_EQ(d.body, "console.log('hi');\n");  // proves HEAD sent no body
}

void test_partial_request(int port, const Site& site) {
    std::cout << "  a request split across packets is reassembled\n";
    Client c(port);
    c.send("GET / HT");
    std::this_thread::sleep_for(50ms);
    c.send("TP/1.1\r\nHo");
    std::this_thread::sleep_for(50ms);
    c.send("st: x\r\n\r\n");
    Response r = c.read_response();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, site.index);
}

void test_connection_close(int port) {
    std::cout << "  Connection: close and HTTP/1.0 close after the response\n";
    {
        Client c(port);
        c.send(get("/", "Connection: close\r\n"));
        Response r = c.read_response();
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.headers["connection"], "close");
        CHECK(c.closed_by_server());
    }
    {
        Client c(port);
        c.send("GET / HTTP/1.0\r\n\r\n");
        Response r = c.read_response();
        CHECK_EQ(r.status, 200);
        CHECK(c.closed_by_server());
    }
}

void test_error_responses(int port) {
    std::cout << "  malformed, oversized and unsupported requests get proper errors\n";
    {
        Client c(port);
        c.send("GARBAGE\r\n\r\n");
        Response r = c.read_response();
        CHECK_EQ(r.status, 400);
        CHECK(c.closed_by_server());
    }
    {
        Client c(port);
        c.send(get("/", "X-Big: " + std::string(10000, 'a') + "\r\n"));
        Response r = c.read_response();
        CHECK_EQ(r.status, 431);
        CHECK(c.closed_by_server());
    }
    {
        Client c(port);
        c.send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
        Response r = c.read_response();
        CHECK_EQ(r.status, 405);
        CHECK_EQ(r.headers["allow"], "GET, HEAD");
        CHECK(c.closed_by_server());
    }
    {
        Client c(port);
        c.send("GET / HTTP/2.0\r\n\r\n");
        CHECK_EQ(c.read_response().status, 505);
    }
}

void test_large_file(int port, const Site& site) {
    std::cout << "  a 16 MiB file arrives intact\n";
    Client c(port);
    c.send(get("/big.bin"));
    Response r = c.read_response();
    CHECK(r.ok);
    CHECK_EQ(r.body.size(), BIG_FILE_SIZE);
    CHECK(r.body == site.big);
}

// The old code slept in a loop whenever a socket buffer was full, so a few
// clients that read slowly could tie up every worker and freeze the server.
void test_slow_clients_dont_block(int port, const Site& site, size_t workers) {
    std::cout << "  slow downloads on every worker don't block other clients\n";
    std::vector<std::unique_ptr<Client>> slow;
    for (size_t i = 0; i < workers * 2; ++i) {
        slow.push_back(std::make_unique<Client>(port, /*recv_buffer=*/4096));
        slow.back()->send(get("/big.bin"));  // ...and never read the reply
    }
    std::this_thread::sleep_for(200ms);  // let their socket buffers fill up

    auto start = std::chrono::steady_clock::now();
    Client fast(port);
    fast.send(get("/"));
    Response r = fast.read_response();
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK_EQ(r.status, 200);
    CHECK(elapsed < 1s);

    // The stalled downloads still complete once their clients start reading.
    Response big = slow.front()->read_response();
    CHECK(big.ok);
    CHECK(big.body == site.big);
}

void test_many_concurrent_clients(int port, const Site& site) {
    std::cout << "  200 concurrent connections all get answers\n";
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 200; ++i) {
        clients.push_back(std::make_unique<Client>(port));
        clients.back()->send(get("/"));
    }
    int ok = 0;
    for (auto& c : clients) {
        Response r = c->read_response();
        if (r.status == 200 && r.body == site.index) ++ok;
    }
    CHECK_EQ(ok, 200);
}

void run_suite(const Site& site, bool use_sendfile) {
    std::cout << "[server] " << (use_sendfile ? "sendfile" : "copy (--no-sendfile)") << " mode\n";
    ServerConfig config;
    config.port = 0;
    config.root = site.root;
    config.threads = 2;
    config.use_sendfile = use_sendfile;
    RunningServer server(config);
    int port = server.port();

    test_basic_get(port, site);
    test_path_traversal(port);
    test_keep_alive(port, site);
    test_pipelining_and_head(port, site);
    test_partial_request(port, site);
    test_connection_close(port);
    test_error_responses(port);
    test_large_file(port, site);
    test_slow_clients_dont_block(port, site, config.threads);
    test_many_concurrent_clients(port, site);
}

void test_shutdown_with_open_connections(const Site& site) {
    std::cout << "[server] stop() returns promptly with idle keep-alive connections open\n";
    ServerConfig config;
    config.port = 0;
    config.root = site.root;
    config.threads = 2;
    RunningServer server(config);

    Client idle(server.port());
    idle.send(get("/"));
    CHECK_EQ(idle.read_response().status, 200);

    auto start = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(idle.closed_by_server());
}

} // namespace

int main() {
    Logger::set_level(LogLevel::WARNING);
    Site site;
    run_suite(site, /*use_sendfile=*/true);
    run_suite(site, /*use_sendfile=*/false);
    test_shutdown_with_open_connections(site);
    return finish_tests("test_server");
}

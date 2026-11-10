#include "network/Connection.hpp"
#include "http/Request.hpp"
#include "http/Response.hpp"
#include "http/StaticFiles.hpp"
#include "utils/Logger.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr size_t READ_CHUNK = 16 * 1024;
constexpr size_t COPY_CHUNK = 64 * 1024;  // only used with --no-sendfile

int status_for(HttpRequest::Status status) {
    switch (status) {
        case HttpRequest::Status::UriTooLong:          return 414;
        case HttpRequest::Status::VersionNotSupported: return 505;
        default:                                       return 400;
    }
}
} // namespace

Connection::Connection(int fd, const StaticFiles& files, bool use_sendfile)
    : socket_(fd), files_(files), use_sendfile_(use_sendfile) {}

Connection::Next Connection::on_ready() {
    if (state_ == State::Reading && !fill_input()) return Next::Close;
    return advance();
}

// Reads whatever the socket has right now. Returns false if the socket failed.
bool Connection::fill_input() {
    char buf[READ_CHUNK];
    while (!peer_closed_ && in_.size() < MAX_BUFFERED_INPUT) {
        ssize_t n = ::recv(socket_.get(), buf, sizeof(buf), 0);
        if (n > 0) {
            in_.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            peer_closed_ = true;  // client finished sending; we may still owe it responses
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return false;  // ECONNRESET and friends
        }
    }
    return true;
}

// Runs the state machine until it has to wait for the socket.
Connection::Next Connection::advance() {
    for (;;) {
        if (state_ == State::Writing) {
            switch (flush()) {
                case Flush::Blocked: return Next::WantWrite;
                case Flush::Failed:  return Next::Close;
                case Flush::Done:    break;
            }
            finish_response();
            if (!keep_alive_) return Next::Close;
            state_ = State::Reading;
        }

        // Clients may send stray CRLFs between requests (RFC 9112 section 2.2).
        size_t skip = 0;
        while (in_.compare(skip, 2, "\r\n") == 0) skip += 2;
        if (skip) in_.erase(0, skip);

        // The request head ends at the first blank line. Until we see it the
        // request is incomplete, so keep waiting rather than parsing a fragment.
        size_t end = in_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (in_.size() > MAX_HEADER_BYTES) {
                queue_error(431, false);
                continue;
            }
            if (peer_closed_) return Next::Close;
            return Next::WantRead;
        }
        if (end + 4 > MAX_HEADER_BYTES) {
            queue_error(431, false);
            continue;
        }

        handle_request(std::string_view(in_).substr(0, end));
        in_.erase(0, end + 4);  // anything left over is the next pipelined request
    }
}

void Connection::handle_request(std::string_view head) {
    HttpRequest req;
    HttpRequest::Status parsed = req.parse(head);
    if (parsed != HttpRequest::Status::Ok) {
        queue_error(status_for(parsed), false);
        Logger::debug("Rejected malformed request on fd " + std::to_string(fd()));
        return;
    }

    const std::string& method = req.get_method();
    bool head_only = method == "HEAD";
    bool keep_alive = req.keep_alive();

    // This server never reads request bodies, so if one follows we can't tell
    // where the next request starts. Answer, then close the connection.
    if (req.has_body()) keep_alive = false;

    int status = 200;
    if (method != "GET" && !head_only) {
        status = 405;
        queue_error(status, keep_alive, false, "Allow: GET, HEAD\r\n");
    } else if (req.has_body()) {
        status = 413;
        queue_error(status, keep_alive, head_only);
    } else {
        StaticFiles::Lookup lookup = files_.resolve(req.get_path());
        UniqueFd file;
        struct stat st{};
        if (lookup.result == StaticFiles::Result::Found) {
            file.reset(::open(lookup.fs_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        }

        if (lookup.result == StaticFiles::Result::Forbidden) {
            status = 403;
            queue_error(status, keep_alive, head_only);
        } else if (!file.valid() || ::fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
            status = 404;
            queue_error(status, keep_alive, head_only);
        } else {
            size_t size = static_cast<size_t>(st.st_size);
            keep_alive_ = keep_alive;
            out_ = HttpResponse::head(200, StaticFiles::mime_type(lookup.fs_path), size, keep_alive);
            out_pos_ = 0;
            if (!head_only && size > 0) {
                file_ = std::move(file);
                file_offset_ = 0;
                file_remaining_ = size;
            }
            state_ = State::Writing;
        }
    }

    if (Logger::enabled(LogLevel::DEBUG)) {
        Logger::debug(method + " " + req.get_target() + " -> " + std::to_string(status) +
                      " (fd " + std::to_string(fd()) + ")");
    }
}

void Connection::queue_error(int status, bool keep_alive, bool head_only, std::string_view extra_headers) {
    keep_alive_ = keep_alive;
    out_ = HttpResponse::error(status, keep_alive, !head_only, extra_headers);
    out_pos_ = 0;
    file_.reset();
    file_remaining_ = 0;
    state_ = State::Writing;
}

// Sends the queued head and then the file body. Stops when the socket buffer is
// full (Blocked) or after WRITE_BUDGET bytes, so one large download can't keep a
// worker to itself while other clients wait.
Connection::Flush Connection::flush() {
    size_t budget = WRITE_BUDGET;

    for (;;) {
        if (out_pos_ < out_.size()) {
            // MSG_MORE lets the kernel put the response head and the start of the
            // body in the same packet instead of sending a tiny head on its own.
            int flags = MSG_NOSIGNAL | (file_remaining_ > 0 ? MSG_MORE : 0);
            ssize_t n = ::send(socket_.get(), out_.data() + out_pos_, out_.size() - out_pos_, flags);
            if (n > 0) {
                out_pos_ += static_cast<size_t>(n);
                budget -= std::min(budget, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Flush::Blocked;
            return Flush::Failed;
        }

        out_.clear();
        out_pos_ = 0;
        if (file_remaining_ == 0) return Flush::Done;
        if (budget == 0) return Flush::Blocked;  // yield; epoll reports us writable again right away

        size_t chunk = std::min(file_remaining_, budget);

        if (use_sendfile_) {
            // The kernel copies straight from the page cache to the socket.
            ssize_t n = ::sendfile(socket_.get(), file_.get(), &file_offset_, chunk);
            if (n > 0) {
                file_remaining_ -= static_cast<size_t>(n);
                budget -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Flush::Blocked;
            return Flush::Failed;  // n == 0 means the file shrank after we promised Content-Length
        }

        // Baseline path for benchmarking: read into user space, then send.
        chunk = std::min(chunk, COPY_CHUNK);
        out_.resize(chunk);
        ssize_t n = ::pread(file_.get(), &out_[0], chunk, file_offset_);
        if (n < 0 && errno == EINTR) {
            out_.clear();
            continue;
        }
        if (n <= 0) return Flush::Failed;
        out_.resize(static_cast<size_t>(n));
        file_offset_ += n;
        file_remaining_ -= static_cast<size_t>(n);
    }
}

void Connection::finish_response() {
    file_.reset();
    file_offset_ = 0;
    file_remaining_ = 0;
    out_.clear();
    out_pos_ = 0;
}

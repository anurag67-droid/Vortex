#pragma once

#include "utils/UniqueFd.hpp"
#include <string>
#include <string_view>
#include <sys/types.h>

class StaticFiles;

// One client connection, driven as a non-blocking state machine.
//
// The server registers each socket with EPOLLONESHOT, so exactly one worker
// thread owns a Connection at a time. That worker calls on_ready(), which reads
// and writes as much as it can without blocking, then says what to wait for
// next. Nothing in here ever sleeps or spins on a socket.
class Connection {
public:
    enum class Next {
        WantRead,   // re-arm for EPOLLIN
        WantWrite,  // re-arm for EPOLLOUT (socket buffer full, or yielding to other clients)
        Close       // done with this connection
    };

    static constexpr size_t MAX_HEADER_BYTES = 8 * 1024;   // larger heads get 431
    static constexpr size_t MAX_BUFFERED_INPUT = 64 * 1024; // caps pipelined input held in memory
    static constexpr size_t WRITE_BUDGET = 1024 * 1024;     // bytes per turn before yielding the worker

    Connection(int fd, const StaticFiles& files, bool use_sendfile);

    Next on_ready();
    int fd() const { return socket_.get(); }

private:
    enum class State { Reading, Writing };
    enum class Flush { Done, Blocked, Failed };

    bool fill_input();
    Next advance();
    void handle_request(std::string_view head);
    void queue_error(int status, bool keep_alive, bool head_only = false, std::string_view extra_headers = {});
    Flush flush();
    void finish_response();

    UniqueFd socket_;
    const StaticFiles& files_;
    bool use_sendfile_;

    State state_ = State::Reading;
    bool keep_alive_ = true;
    bool peer_closed_ = false;

    std::string in_;     // received bytes not yet consumed by a request
    std::string out_;    // bytes queued for the socket: response head, error page, or a copied file chunk
    size_t out_pos_ = 0;

    UniqueFd file_;      // file being sent, if any
    off_t file_offset_ = 0;
    size_t file_remaining_ = 0;
};

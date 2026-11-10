#include "check.hpp"
#include "http/Request.hpp"
#include <string>

using Status = HttpRequest::Status;

namespace {

Status parse(HttpRequest& req, const std::string& head) {
    return req.parse(head);
}

void test_standard_request() {
    std::cout << "[parser] standard GET\n";
    HttpRequest req;
    CHECK(parse(req, "GET /style.css HTTP/1.1\r\nHost: localhost:8080") == Status::Ok);
    CHECK_EQ(req.get_method(), "GET");
    CHECK_EQ(req.get_target(), "/style.css");
    CHECK_EQ(req.get_path(), "/style.css");
    CHECK_EQ(req.get_version(), "HTTP/1.1");
    CHECK_EQ(req.header("host"), "localhost:8080");
    CHECK_EQ(req.header("HOST"), "localhost:8080");  // case-insensitive
}

void test_query_and_decoding() {
    std::cout << "[parser] query string and percent-decoding\n";
    HttpRequest req;
    CHECK(parse(req, "GET /my%20file.html?v=2&x=%41 HTTP/1.1\r\nHost: a") == Status::Ok);
    CHECK_EQ(req.get_path(), "/my file.html");
    CHECK_EQ(req.get_query(), "v=2&x=%41");  // the query is left as-is

    // Encoded dots decode to "..", which the static file layer must still refuse.
    CHECK(parse(req, "GET /%2e%2e/etc/passwd HTTP/1.1\r\nHost: a") == Status::Ok);
    CHECK_EQ(req.get_path(), "/../etc/passwd");

    CHECK(parse(req, "GET /bad%zz HTTP/1.1\r\nHost: a") == Status::BadRequest);
    CHECK(parse(req, "GET /bad% HTTP/1.1\r\nHost: a") == Status::BadRequest);
    CHECK(parse(req, "GET /nul%00byte HTTP/1.1\r\nHost: a") == Status::BadRequest);
}

void test_malformed_request_lines() {
    std::cout << "[parser] malformed request lines\n";
    HttpRequest req;
    CHECK(parse(req, "") == Status::BadRequest);
    CHECK(parse(req, "GARBAGE") == Status::BadRequest);
    CHECK(parse(req, "GET /\r\nHost: a") == Status::BadRequest);
    CHECK(parse(req, "GET  / HTTP/1.1\r\nHost: a") == Status::BadRequest);       // double space
    CHECK(parse(req, "GET / HTTP/1.1 extra\r\nHost: a") == Status::BadRequest);
    CHECK(parse(req, "GET http://evil.com/ HTTP/1.1\r\nHost: a") == Status::BadRequest);
    CHECK(parse(req, "G(T / HTTP/1.1\r\nHost: a") == Status::BadRequest);        // not a token
    CHECK(parse(req, "GET / HTTP/2.0\r\nHost: a") == Status::VersionNotSupported);
    CHECK(parse(req, "GET / FOO/1.1\r\nHost: a") == Status::BadRequest);
    CHECK(parse(req, "GET /" + std::string(HttpRequest::MAX_URI_LENGTH, 'a') + " HTTP/1.1\r\nHost: a") ==
          Status::UriTooLong);
}

void test_malformed_headers() {
    std::cout << "[parser] malformed headers\n";
    HttpRequest req;
    CHECK(parse(req, "GET / HTTP/1.1") == Status::BadRequest);                         // HTTP/1.1 needs Host
    CHECK(parse(req, "GET / HTTP/1.1\r\nHost a") == Status::BadRequest);               // no colon
    CHECK(parse(req, "GET / HTTP/1.1\r\nHost : a") == Status::BadRequest);             // space before colon
    CHECK(parse(req, "GET / HTTP/1.1\r\nHost: a\r\n folded") == Status::BadRequest);   // line folding
    CHECK(parse(req, "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5x") == Status::BadRequest);
    CHECK(parse(req, "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6") ==
          Status::BadRequest);
    CHECK(parse(req, "GET / HTTP/1.0") == Status::Ok);  // HTTP/1.0 doesn't require Host
}

void test_keep_alive_rules() {
    std::cout << "[parser] keep-alive rules\n";
    HttpRequest req;
    parse(req, "GET / HTTP/1.1\r\nHost: a");
    CHECK(req.keep_alive());
    parse(req, "GET / HTTP/1.1\r\nHost: a\r\nConnection: close");
    CHECK(!req.keep_alive());
    parse(req, "GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, CLOSE");
    CHECK(!req.keep_alive());
    parse(req, "GET / HTTP/1.0");
    CHECK(!req.keep_alive());
    parse(req, "GET / HTTP/1.0\r\nConnection: Keep-Alive");
    CHECK(req.keep_alive());
}

void test_body_detection() {
    std::cout << "[parser] request body detection\n";
    HttpRequest req;
    parse(req, "GET / HTTP/1.1\r\nHost: a");
    CHECK(!req.has_body());
    parse(req, "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0");
    CHECK(!req.has_body());
    parse(req, "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 12");
    CHECK(req.has_body());
    parse(req, "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked");
    CHECK(req.has_body());
}

} // namespace

int main() {
    test_standard_request();
    test_query_and_decoding();
    test_malformed_request_lines();
    test_malformed_headers();
    test_keep_alive_rules();
    test_body_detection();
    return finish_tests("test_parser");
}

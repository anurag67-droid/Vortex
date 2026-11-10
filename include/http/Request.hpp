#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

// Parses the head of an HTTP/1.x request: the request line and headers,
// without the blank line that ends them.
class HttpRequest {
public:
    enum class Status {
        Ok,
        BadRequest,          // 400
        UriTooLong,          // 414
        VersionNotSupported  // 505
    };

    static constexpr size_t MAX_URI_LENGTH = 2048;

    Status parse(std::string_view head);

    const std::string& get_method() const { return method; }
    const std::string& get_target() const { return target; }    // raw request-target, e.g. /a%20b.html?x=1
    const std::string& get_path() const { return path; }        // percent-decoded, without the query string
    const std::string& get_query() const { return query; }
    const std::string& get_version() const { return version; }

    // Header lookup is case-insensitive. Returns "" when the header is absent.
    std::string header(std::string_view name) const;
    bool has_header(std::string_view name) const;

    // Whether the client wants the connection kept open after this response.
    bool keep_alive() const;
    // Whether the request says a body follows the head.
    bool has_body() const;

private:
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::unordered_map<std::string, std::string> headers;  // keys are lower-case
};

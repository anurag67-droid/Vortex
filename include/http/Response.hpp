#pragma once
#include <string>
#include <string_view>

// Builds response heads and small canned error responses. The file body itself
// is streamed separately by Connection.
class HttpResponse {
public:
    static const char* reason_phrase(int status);

    // Status line and headers, ending with the blank line. `extra_headers` must
    // be complete lines, each ending in "\r\n".
    static std::string head(int status, std::string_view content_type, size_t content_length,
                            bool keep_alive, std::string_view extra_headers = {});

    // A complete response with a short HTML body (omitted for HEAD requests).
    static std::string error(int status, bool keep_alive, bool include_body = true,
                             std::string_view extra_headers = {});
};

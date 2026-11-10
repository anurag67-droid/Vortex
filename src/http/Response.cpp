#include "http/Response.hpp"
#include <ctime>

namespace {

// The Date header only changes once a second, so each thread caches it.
const std::string& http_date() {
    thread_local std::time_t cached_time = 0;
    thread_local std::string cached;
    std::time_t now = std::time(nullptr);
    if (now != cached_time) {
        std::tm utc{};
        gmtime_r(&now, &utc);
        char buf[64];
        size_t n = std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &utc);
        cached.assign(buf, n);
        cached_time = now;
    }
    return cached;
}

} // namespace

const char* HttpResponse::reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

std::string HttpResponse::head(int status, std::string_view content_type, size_t content_length,
                               bool keep_alive, std::string_view extra_headers) {
    std::string out;
    out.reserve(256);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason_phrase(status);
    out += "\r\nServer: Vortex\r\nDate: ";
    out += http_date();
    out += "\r\nContent-Type: ";
    out += content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(content_length);
    out += keep_alive ? "\r\nConnection: keep-alive\r\n" : "\r\nConnection: close\r\n";
    out += extra_headers;
    out += "\r\n";
    return out;
}

std::string HttpResponse::error(int status, bool keep_alive, bool include_body, std::string_view extra_headers) {
    std::string title = std::to_string(status) + " " + reason_phrase(status);
    std::string body = "<!DOCTYPE html>\n<html><head><title>" + title + "</title></head>"
                       "<body><h1>" + title + "</h1></body></html>\n";
    std::string out = head(status, "text/html; charset=utf-8", body.size(), keep_alive, extra_headers);
    if (include_body) out += body;
    return out;
}

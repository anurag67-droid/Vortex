#include "http/Request.hpp"
#include <algorithm>
#include <cctype>

namespace {

constexpr auto npos = std::string_view::npos;

// RFC 9110 "tchar": the characters allowed in methods and header names.
bool is_tchar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ||
           std::string_view("!#$%&'*+-.^_`|~").find(c) != npos;
}

bool is_token(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_tchar);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes %XX escapes. Fails on a malformed escape or an encoded NUL byte,
// which could otherwise truncate the path when it reaches the C file APIs.
bool percent_decode(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        int hi = hex_value(in[i + 1]);
        int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        char decoded = static_cast<char>(hi * 16 + lo);
        if (decoded == '\0') return false;
        out.push_back(decoded);
        i += 2;
    }
    return true;
}

// True if a comma-separated header value (like Connection) contains `token`.
bool has_token(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        size_t comma = list.find(',');
        std::string_view item = trim(list.substr(0, comma));
        if (to_lower(item) == token) return true;
        if (comma == npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

bool is_control(unsigned char c) {
    return c < 0x20 || c == 0x7f;
}

} // namespace

HttpRequest::Status HttpRequest::parse(std::string_view head) {
    *this = HttpRequest{};

    size_t line_end = head.find("\r\n");
    std::string_view request_line = head.substr(0, line_end);
    std::string_view rest = line_end == npos ? std::string_view{} : head.substr(line_end + 2);

    // Request line: METHOD SP request-target SP HTTP-version
    size_t sp1 = request_line.find(' ');
    size_t sp2 = sp1 == npos ? npos : request_line.find(' ', sp1 + 1);
    if (sp2 == npos || request_line.find(' ', sp2 + 1) != npos) return Status::BadRequest;

    std::string_view m = request_line.substr(0, sp1);
    std::string_view t = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view v = request_line.substr(sp2 + 1);

    if (!is_token(m)) return Status::BadRequest;
    if (t.size() > MAX_URI_LENGTH) return Status::UriTooLong;
    // Only origin-form targets ("/path?query") make sense for a static file server.
    if (t.empty() || t.front() != '/') return Status::BadRequest;
    for (char c : t) {
        if (is_control(static_cast<unsigned char>(c)) || c == ' ') return Status::BadRequest;
    }
    if (v != "HTTP/1.1" && v != "HTTP/1.0") {
        return v.rfind("HTTP/", 0) == 0 ? Status::VersionNotSupported : Status::BadRequest;
    }

    method = std::string(m);
    target = std::string(t);
    version = std::string(v);

    size_t q = t.find('?');
    if (q != npos) query = std::string(t.substr(q + 1));
    if (!percent_decode(t.substr(0, q), path)) return Status::BadRequest;

    // Header fields: name ":" OWS value OWS
    while (!rest.empty()) {
        size_t end = rest.find("\r\n");
        std::string_view line = rest.substr(0, end);
        rest = end == npos ? std::string_view{} : rest.substr(end + 2);

        if (line.empty()) continue;
        // Obsolete line folding is a known request-smuggling vector; reject it.
        if (line.front() == ' ' || line.front() == '\t') return Status::BadRequest;

        size_t colon = line.find(':');
        if (colon == npos) return Status::BadRequest;
        std::string_view name = line.substr(0, colon);
        if (!is_token(name)) return Status::BadRequest;

        std::string_view value = trim(line.substr(colon + 1));
        for (char c : value) {
            if (c != '\t' && is_control(static_cast<unsigned char>(c))) return Status::BadRequest;
        }

        auto [it, inserted] = headers.emplace(to_lower(name), std::string(value));
        if (!inserted) {
            it->second += ", ";
            it->second += value;
        }
    }

    if (version == "HTTP/1.1" && !has_header("host")) return Status::BadRequest;

    // Content-Length must be a single plain number. Conflicting duplicates end up
    // joined with ", " above and are rejected here.
    if (has_header("content-length")) {
        std::string cl = header("content-length");
        if (cl.empty() || !std::all_of(cl.begin(), cl.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return Status::BadRequest;
        }
    }

    return Status::Ok;
}

std::string HttpRequest::header(std::string_view name) const {
    auto it = headers.find(to_lower(name));
    return it == headers.end() ? std::string{} : it->second;
}

bool HttpRequest::has_header(std::string_view name) const {
    return headers.count(to_lower(name)) != 0;
}

bool HttpRequest::keep_alive() const {
    std::string connection = header("connection");
    if (version == "HTTP/1.1") return !has_token(connection, "close");
    return has_token(connection, "keep-alive");
}

bool HttpRequest::has_body() const {
    if (has_header("transfer-encoding")) return true;
    std::string cl = header("content-length");
    return !cl.empty() && cl.find_first_not_of('0') != std::string::npos;
}

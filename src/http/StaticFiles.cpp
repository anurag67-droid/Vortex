#include "http/StaticFiles.hpp"
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <sys/stat.h>

namespace {

constexpr auto npos = std::string_view::npos;

// realpath() resolves "..", "." and every symlink. Returns "" on failure.
std::string canonicalize(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) return {};
    std::string out(resolved);
    std::free(resolved);
    return out;
}

// True if any '/'-separated segment starts with '.', e.g. "/.git/config".
bool has_hidden_segment(std::string_view path) {
    size_t pos = 0;
    while (pos < path.size()) {
        size_t slash = path.find('/', pos);
        std::string_view seg = path.substr(pos, slash == npos ? npos : slash - pos);
        if (!seg.empty() && seg.front() == '.') return true;
        if (slash == npos) break;
        pos = slash + 1;
    }
    return false;
}

} // namespace

StaticFiles::StaticFiles(const std::string& root) : root_(canonicalize(root)) {
    struct stat st{};
    if (root_.empty() || ::stat(root_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        throw std::runtime_error("document root '" + root + "' does not exist or is not a directory");
    }
}

bool StaticFiles::inside_root(const std::string& canonical) const {
    if (root_ == "/") return true;
    return canonical.size() > root_.size() &&
           canonical.compare(0, root_.size(), root_) == 0 &&
           canonical[root_.size()] == '/';
}

StaticFiles::Lookup StaticFiles::resolve(std::string_view url_path) const {
    if (url_path.empty() || url_path.front() != '/') return {Result::NotFound, {}};

    // Rebuild the path segment by segment, refusing anything that walks upward
    // or names a hidden file. Checking segments (instead of searching for "..")
    // means names like "notes..txt" still work.
    std::string relative;
    size_t pos = 1;
    while (pos <= url_path.size()) {
        size_t slash = url_path.find('/', pos);
        std::string_view seg = url_path.substr(pos, slash == npos ? npos : slash - pos);
        pos = slash == npos ? url_path.size() + 1 : slash + 1;

        if (seg.empty() || seg == ".") continue;
        if (seg.front() == '.') return {Result::Forbidden, {}};  // "..", ".git", ".env", ...
        relative += '/';
        relative += seg;
    }

    std::string real = canonicalize(root_ + relative);
    if (real.empty()) return {Result::NotFound, {}};

    // A symlink inside the root could still point anywhere on disk, so check
    // where the path really ends up.
    if (real != root_ && (!inside_root(real) || has_hidden_segment(real.substr(root_.size())))) {
        return {Result::Forbidden, {}};
    }

    struct stat st{};
    if (::stat(real.c_str(), &st) != 0) return {Result::NotFound, {}};

    if (S_ISDIR(st.st_mode)) {
        real = canonicalize(real + "/index.html");
        if (real.empty()) return {Result::NotFound, {}};
        if (!inside_root(real) || has_hidden_segment(real.substr(root_.size()))) return {Result::Forbidden, {}};
        if (::stat(real.c_str(), &st) != 0) return {Result::NotFound, {}};
    }

    if (!S_ISREG(st.st_mode)) return {Result::NotFound, {}};
    return {Result::Found, real};
}

std::string StaticFiles::mime_type(std::string_view fs_path) {
    size_t slash = fs_path.rfind('/');
    std::string_view name = slash == npos ? fs_path : fs_path.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot == npos || dot == 0) return "application/octet-stream";

    std::string ext(name.substr(dot + 1));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")   return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
    if (ext == "json")  return "application/json";
    if (ext == "txt")   return "text/plain; charset=utf-8";
    if (ext == "xml")   return "application/xml";
    if (ext == "svg")   return "image/svg+xml";
    if (ext == "png")   return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")   return "image/gif";
    if (ext == "webp")  return "image/webp";
    if (ext == "ico")   return "image/x-icon";
    if (ext == "woff")  return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "pdf")   return "application/pdf";
    if (ext == "wasm")  return "application/wasm";
    return "application/octet-stream";
}

#pragma once

#include <string>
#include <string_view>

// Maps URL paths to files under a document root, refusing anything that
// would escape it.
class StaticFiles {
public:
    enum class Result { Found, NotFound, Forbidden };

    struct Lookup {
        Result result;
        std::string fs_path;  // canonical path on disk, set when result == Found
    };

    // Throws std::runtime_error if `root` doesn't exist or isn't a directory.
    explicit StaticFiles(const std::string& root);

    // `url_path` is the percent-decoded path from the request, e.g. "/css/site.css".
    // Rejects ".." segments, dotfiles (like /.git) and symlinks that point outside
    // the root. A path naming a directory resolves to its index.html.
    Lookup resolve(std::string_view url_path) const;

    static std::string mime_type(std::string_view fs_path);

    const std::string& root() const { return root_; }

private:
    std::string root_;  // canonical absolute path, no trailing slash

    bool inside_root(const std::string& canonical) const;
};

#include "check.hpp"
#include "http/StaticFiles.hpp"
#include "temp_dir.hpp"
#include <stdexcept>

using Result = StaticFiles::Result;

namespace {

struct Fixture {
    TempDir tmp;
    std::string root;

    Fixture() {
        root = tmp.mkdir("public");
        tmp.write("public/index.html", "home");
        tmp.mkdir("public/docs");
        tmp.write("public/docs/index.html", "docs home");
        tmp.write("public/docs/guide.html", "guide");
        tmp.mkdir("public/empty");
        tmp.write("public/notes..txt", "two dots in a name are fine");
        tmp.write("public/.env", "SECRET=1");
        tmp.mkdir("public/.git");
        tmp.write("public/.git/config", "secret");
        tmp.write("secret.txt", "outside the root");
        tmp.symlink(tmp.path() + "/secret.txt", "public/escape.txt");   // points outside
        tmp.symlink(root + "/docs/guide.html", "public/alias.html");     // points inside
        tmp.symlink(root + "/.env", "public/env.txt");                   // points at a dotfile
    }
};

void test_found(const StaticFiles& files, const std::string& root) {
    std::cout << "[static] normal files and directory indexes\n";
    auto r = files.resolve("/");
    CHECK(r.result == Result::Found);
    CHECK_EQ(r.fs_path, root + "/index.html");

    r = files.resolve("/docs/guide.html");
    CHECK(r.result == Result::Found);
    CHECK_EQ(r.fs_path, root + "/docs/guide.html");

    CHECK_EQ(files.resolve("/docs").fs_path, root + "/docs/index.html");
    CHECK_EQ(files.resolve("/docs/").fs_path, root + "/docs/index.html");
    CHECK_EQ(files.resolve("//docs/./guide.html").fs_path, root + "/docs/guide.html");
    CHECK(files.resolve("/notes..txt").result == Result::Found);
    CHECK(files.resolve("/alias.html").result == Result::Found);  // symlink that stays inside is fine
}

void test_not_found(const StaticFiles& files) {
    std::cout << "[static] missing files\n";
    CHECK(files.resolve("/missing.html").result == Result::NotFound);
    CHECK(files.resolve("/empty").result == Result::NotFound);  // directory without index.html
    CHECK(files.resolve("/index.html/x").result == Result::NotFound);
    CHECK(files.resolve("relative").result == Result::NotFound);
}

void test_traversal(const StaticFiles& files) {
    std::cout << "[static] path traversal and hidden files\n";
    CHECK(files.resolve("/../secret.txt").result == Result::Forbidden);
    CHECK(files.resolve("/docs/../../secret.txt").result == Result::Forbidden);
    CHECK(files.resolve("/docs/../index.html").result == Result::Forbidden);  // any ".." is refused
    CHECK(files.resolve("/..").result == Result::Forbidden);
    CHECK(files.resolve("/../../../../etc/passwd").result == Result::Forbidden);
    CHECK(files.resolve("/escape.txt").result == Result::Forbidden);  // symlink out of the root
    CHECK(files.resolve("/env.txt").result == Result::Forbidden);     // symlink to a dotfile
    CHECK(files.resolve("/.env").result == Result::Forbidden);
    CHECK(files.resolve("/.git/config").result == Result::Forbidden);
}

void test_mime_types() {
    std::cout << "[static] MIME types\n";
    CHECK_EQ(StaticFiles::mime_type("/a/index.html"), "text/html; charset=utf-8");
    CHECK_EQ(StaticFiles::mime_type("/a/STYLE.CSS"), "text/css; charset=utf-8");
    CHECK_EQ(StaticFiles::mime_type("/a/app.js"), "text/javascript; charset=utf-8");
    CHECK_EQ(StaticFiles::mime_type("/a/data.json"), "application/json");  // was served as JavaScript before
    CHECK_EQ(StaticFiles::mime_type("/a/logo.svg"), "image/svg+xml");
    CHECK_EQ(StaticFiles::mime_type("/a/photo.jpeg"), "image/jpeg");
    CHECK_EQ(StaticFiles::mime_type("/a.html/readme"), "application/octet-stream");
    CHECK_EQ(StaticFiles::mime_type("/a/file.html.bak"), "application/octet-stream");
}

void test_bad_root() {
    std::cout << "[static] invalid document root\n";
    bool threw = false;
    try {
        StaticFiles files("/definitely/not/a/real/dir");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main() {
    Fixture fx;
    StaticFiles files(fx.root);
    test_found(files, files.root());
    test_not_found(files);
    test_traversal(files);
    test_mime_types();
    test_bad_root();
    return finish_tests("test_static_files");
}

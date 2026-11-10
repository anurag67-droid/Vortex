#pragma once
// A scratch directory under /tmp that is deleted when the object goes away.
#include <cstdlib>
#include <fstream>
#include <ftw.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class TempDir {
    std::string path_;

    static int remove_entry(const char* path, const struct stat*, int, struct FTW*) {
        return ::remove(path);
    }

public:
    TempDir() {
        char tmpl[] = "/tmp/vortex-test-XXXXXX";
        if (!::mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
        path_ = tmpl;
    }
    ~TempDir() { ::nftw(path_.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }

    std::string mkdir(const std::string& rel) const {
        std::string full = path_ + "/" + rel;
        ::mkdir(full.c_str(), 0755);
        return full;
    }

    std::string write(const std::string& rel, const std::string& content) const {
        std::string full = path_ + "/" + rel;
        std::ofstream(full, std::ios::binary) << content;
        return full;
    }

    void symlink(const std::string& target, const std::string& rel) const {
        std::string full = path_ + "/" + rel;
        if (::symlink(target.c_str(), full.c_str()) != 0) throw std::runtime_error("symlink failed");
    }
};

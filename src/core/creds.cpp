#include "creds.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "store.h"

namespace {

std::string creds_path(const std::string &name) {
    std::string dir = data_dir() + "/creds";
    if (mkdir(dir.c_str(), 0700) != 0) {
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) throw std::runtime_error("cannot create " + dir);
    }
    return dir + "/" + name;
}

}  // namespace

std::map<std::string, std::string> creds_load(const std::string &name) {
    std::ifstream f(creds_path(name));
    std::map<std::string, std::string> kv;
    for (std::string l; std::getline(f, l);) {
        size_t eq = l.find('=');
        if (eq == std::string::npos || !eq) continue;
        kv[l.substr(0, eq)] = l.substr(eq + 1);
    }
    return kv;
}

void creds_save(const std::string &name, const std::map<std::string, std::string> &kv) {
    std::string path = creds_path(name);
    std::string tmp = path + ".tmp";
    std::string out;
    for (const auto &[k, v] : kv) {
        if (k.find('=') != std::string::npos || k.find('\n') != std::string::npos ||
            v.find('\n') != std::string::npos)
            throw std::runtime_error("creds: unrepresentable key/value in " + name);
        out += k + "=" + v + "\n";
    }

    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("creds: cannot write " + tmp);
    ssize_t n = write(fd, out.data(), out.size());
    if (fsync(fd) != 0 || close(fd) != 0 || n != (ssize_t)out.size())
        throw std::runtime_error("creds: write failed " + tmp);
    if (rename(tmp.c_str(), path.c_str()) != 0)
        throw std::runtime_error("creds: cannot replace " + path);
}

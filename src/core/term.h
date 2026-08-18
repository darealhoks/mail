#pragma once
#include <string>

#include <unistd.h>
#include <cstdlib>

inline bool color_on(int fd = 1) {
    static bool no = getenv("NO_COLOR") != nullptr;
    return !no && isatty(fd);
}

inline std::string c(const char *sgr, const std::string &s, int fd = 1) {
    return color_on(fd) ? "\033[" + std::string(sgr) + "m" + s + "\033[0m" : s;
}

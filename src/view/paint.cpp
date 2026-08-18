#include "paint.h"

#include <cstdio>
#include <ctime>

#include <sys/ioctl.h>
#include <unistd.h>

#include "term.h"

namespace paint {

int term_cols() {
    struct winsize w {};
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 20) return w.ws_col < 100 ? w.ws_col : 100;
    return 80;
}

size_t utf8_len(const std::string &s) {
    size_t n = 0;
    for (unsigned char ch : s)
        if ((ch & 0xC0) != 0x80) n++;
    return n;
}

// greedy wrap on spaces, counting codepoints; long words are left to overflow
std::vector<std::string> wrap(const std::string &s, size_t width) {
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < s.size()) {
        size_t sp = s.find_first_of(" \n", i);
        std::string word = s.substr(i, sp == std::string::npos ? sp : sp - i);
        bool brk = sp != std::string::npos && s[sp] == '\n';
        if (!line.empty() && utf8_len(line) + 1 + utf8_len(word) > width) {
            out.push_back(line);
            line.clear();
        }
        if (!word.empty()) line += (line.empty() ? "" : " ") + word;
        if (brk) {
            out.push_back(line);
            line.clear();
        }
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

const char *kind_color(const std::string &k) {
    if (k == "test") return "1;35";
    if (k == "task") return "1;33";
    return "1;36";
}

std::string when(long long due) {
    struct tm tm {};
    time_t tt = (time_t)due;
    localtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof b, "%a %d %b %H:%M", &tm);
    long long d = due - (long long)time(nullptr);
    std::string rel;
    long long a = d < 0 ? -d : d;
    if (a < 3600) rel = std::to_string(a / 60) + "m";
    else if (a < 172800) rel = std::to_string(a / 3600) + "h";
    else rel = std::to_string(a / 86400) + "d";
    return std::string(b) + (d < 0 ? "  (" + rel + " ago)" : "  (in " + rel + ")");
}

const char *due_color(long long due) {
    long long d = due - (long long)time(nullptr);
    if (d < 0) return "1;31";
    if (d < 86400) return "0;31";
    if (d < 3 * 86400) return "0;33";
    return "0;32";
}

// urls survive into the body text; make them stand out without breaking the wrap width
std::string link_up(const std::string &l) {
    if (!color_on()) return l;
    std::string o;
    for (size_t i = 0; i < l.size();) {
        if (!l.compare(i, 8, "https://") || !l.compare(i, 7, "http://")) {
            size_t e = l.find(' ', i);
            if (e == std::string::npos) e = l.size();
            o += "\033[4;34m" + l.substr(i, e - i) + "\033[0m\033[37m";
            i = e;
            continue;
        }
        o += l[i++];
    }
    return o;
}


}  // namespace paint

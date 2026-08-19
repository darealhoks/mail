#include "paint.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <sys/ioctl.h>
#include <unistd.h>

#include "config.h"
#include "term.h"
#include "view.h"

namespace paint {

int term_cols(bool cap) {
    struct winsize w {};
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 20) return !cap || w.ws_col < 100 ? w.ws_col : 100;
    return 80;
}

size_t utf8_len(const std::string &s) {
    size_t n = 0;
    for (unsigned char ch : s)
        if ((ch & 0xC0) != 0x80) n++;
    return n;
}

// cut to w codepoints and pad to exactly w; the input carries no sgr
std::string plain_cut(const std::string &s, size_t w) {
    std::string out;
    size_t vis = 0;
    for (size_t i = 0; i < s.size() && vis < w; i++) {
        out += s[i];
        while (i + 1 < s.size() && (unsigned char)s[i + 1] >> 6 == 2) out += s[++i];
        vis++;
    }
    out.append(w - vis, ' ');
    return out;
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

std::string accent_sgr(std::string v) {
    for (char &ch : v) ch = (char)tolower((unsigned char)ch);
    static const char *NAMES[] = {"black", "red",     "green", "yellow",
                                  "blue",  "magenta", "cyan",  "white"};
    for (int i = 0; i < 8; i++) {
        if (v == NAMES[i]) return std::to_string(30 + i);
        if (v == std::string("bright") + NAMES[i]) return std::to_string(90 + i);
    }
    if (v.size() == 7 && v[0] == '#') {
        long h = strtol(v.c_str() + 1, nullptr, 16);
        return "38;2;" + std::to_string((h >> 16) & 255) + ";" +
               std::to_string((h >> 8) & 255) + ";" + std::to_string(h & 255);
    }
    if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
        int n = atoi(v.c_str());
        if (n < 8) return std::to_string(30 + n);
        if (n < 16) return std::to_string(82 + n);
        if (n < 256) return "38;5;" + std::to_string(n);
    }
    return "34";
}

// resolve general.accent once; unknown values fall back to the terminal's blue
static const std::string &acc() {
    static const std::string sgr = accent_sgr(config().str("general.accent"));
    return sgr;
}

const char *accent() { return acc().c_str(); }

const char *accent_bg() {
    static const std::string sgr = "7;" + acc();
    return sgr.c_str();
}

std::string bg_sgr(const std::string &fg) {
    if (fg.compare(0, 2, "38") == 0) return "4" + fg.substr(1);  // 38;5;n / 38;2;r;g;b
    return std::to_string(atoi(fg.c_str()) + 10);                // 30-37 -> 40-47, 90-97 -> 100-107
}

const char *bar_bg() {
    static const std::string sgr = [] {
        std::string v = config().str("general.bar");
        for (char &ch : v) ch = (char)tolower((unsigned char)ch);
        if (v.empty() || v == "none" || v == "transparent") return std::string();
        return bg_sgr(accent_sgr(v));
    }();
    return sgr.c_str();
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

const char *mark_color(const std::string &t) {
    static const char *SGR[] = {"1;32", "0;32", "0;33", "0;31", "1;31"};
    int m = 0;
    if (t.find('/') != std::string::npos) m = view::points_of(t);
    else if (!t.empty() && t[0] >= mark_scale().first && t[0] <= mark_scale().second)
        m = t[0] - mark_scale().first + 1;
    return m >= 1 && m <= 5 ? SGR[m - 1] : "39";
}

std::string avg_str(double a) {
    char b[24];
    snprintf(b, sizeof b, "%.2f (%d)", a, avg_mark(a));
    for (char &ch : b)
        if (ch == '.') ch = ',';
    return b;
}

const char *avg_color(double a) {
    return mark_color(std::string(1, (char)(mark_scale().first + avg_mark(a) - 1)));
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
            o += "\033[4;" + acc() + "m" + l.substr(i, e - i) + "\033[0m\033[39m";
            i = e;
            continue;
        }
        o += l[i++];
    }
    return o;
}

// "YYYY-MM-DD" through general.date; the year is only worth screen space when it is not this one
std::string date_short(const std::string &ymd) {
    struct tm tm {};
    if (!strptime(ymd.c_str(), "%Y-%m-%d", &tm)) return ymd;
    time_t now = time(nullptr);
    struct tm cur {};
    localtime_r(&now, &cur);
    std::string f = config().str("general.date");
    if (f.empty()) f = "%-d. %-m.";
    if (tm.tm_year != cur.tm_year) f += " %Y";
    char buf[64];
    size_t n = strftime(buf, sizeof buf, f.c_str(), &tm);
    return n ? std::string(buf, n) : ymd;
}

namespace {
// "8:00" -> "00": the minute half is enough to tell hours apart when the column is tight
std::string mins(const std::string &t) {
    size_t p = t.find(':');
    return p == std::string::npos ? t : t.substr(p + 1);
}
}  // namespace

TableLayout table_layout(const view::Timetable &tt, size_t need, size_t cols, size_t gut) {
    TableLayout L;
    L.room = config().flag("table.room");
    L.teacher = config().flag("table.teacher");
    size_t nh = tt.hours.size();
    if (!nh) return L;
    std::vector<std::string> full, comp, beg, end;
    size_t wfull = 0, wcomp = 0, wedge = 0;
    bool have = false;
    for (size_t h = 0; h < nh; h++) {
        need = std::max(need, utf8_len(tt.hours[h]));
        std::string b = tt.edge(h, false), e = tt.edge(h, true);
        have |= !b.empty();
        full.push_back(tt.span(h));
        comp.push_back(b.empty() ? "" : mins(b) + "-" + mins(e));
        beg.push_back(b);
        end.push_back(e);
        wfull = std::max(wfull, utf8_len(full.back()));
        wcomp = std::max(wcomp, utf8_len(comp.back()));
        wedge = std::max({wedge, utf8_len(b), utf8_len(e)});
    }
    // one column costs cw + its separator, and one more separator closes the grid
    size_t room = cols > gut + 1 + nh ? (cols - gut - 1) / nh - 1 : 1;
    size_t cw = std::min(need, room);
    if (have && config().flag("table.time")) {
        auto pick = [&](size_t w, int rows, std::vector<std::string> &a, std::vector<std::string> &b) {
            cw = std::max(need, w);
            L.time_rows = rows;
            L.t0 = a;
            if (rows == 2) L.t1 = b;
        };
        // widest form that still fits; a one-row span beats splitting the times over two rows
        if (std::max(need, wfull) <= room) pick(wfull, 1, full, full);
        else if (std::max(need, wcomp) <= room) pick(wcomp, 1, comp, comp);
        else if (std::max(need, wedge) <= room) pick(wedge, 2, beg, end);
        else if (wcomp <= room) {
            pick(wcomp, 1, comp, comp);
            cw = room;  // content has to truncate either way; keep the times readable
        }
    }
    L.cw = std::max<size_t>(2, std::min(cw, room));
    return L;
}

}  // namespace paint

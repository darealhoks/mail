#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "paint.h"
#include "registry.h"
#include "store.h"
#include "term.h"
#include "view.h"

#define TUI_NAME APP_NAME "t"

namespace {

using namespace paint;

enum Mode { M_FEED, M_MARKS, M_TABLE, M_ABSENCE, M_N };
const char *MODE_NAME[M_N] = {"feed", "marks", "table", "absence"};

volatile sig_atomic_t resized = 1;
void on_winch(int) { resized = 1; }

termios saved{};
void leave() {
    tcsetattr(0, TCSANOW, &saved);
    fputs("\033[?1006l\033[?1000l\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}
void die(int sig) {
    leave();
    _exit(128 + sig);
}

void enter() {
    tcgetattr(0, &saved);
    termios raw = saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    // 1000 = button press/release + wheel, 1006 = sgr coords so columns past 223 still work
    fputs("\033[?1049h\033[?25l\033[?1000h\033[?1006h", stdout);
}

// changes when maild writes: wal carries the data until a checkpoint, so both files count
long long db_stamp() {
    long long v = 0;
    for (const char *suf : {"", "-wal"}) {
        struct stat st {};
        if (stat((data_dir() + "/" APP_NAME ".db" + suf).c_str(), &st) == 0)
            v += (long long)st.st_mtime * 1000000 + st.st_size;
    }
    return v;
}

long long now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// [key] section: "<char> = feed|marks|table|absence"; unset falls back to f/m/t/b
std::map<char, Mode> keymap() {
    std::map<char, Mode> k{{'f', M_FEED}, {'m', M_MARKS}, {'t', M_TABLE}, {'b', M_ABSENCE}};
    for (const auto &[key, val] : config().v) {
        if (key.compare(0, 4, "key.") || key.size() != 5) continue;
        for (int m = 0; m < M_N; m++)
            if (val == MODE_NAME[m] || (m == M_TABLE && val == "timetable")) k[key[4]] = (Mode)m;
    }
    return k;
}

// mode chip per tab, hotkey underlined except on the active one; hits gets each tab's columns
std::string tabs(Mode mode, const std::map<char, Mode> &keys, size_t &width_used,
                 std::vector<std::pair<size_t, size_t>> &hits, bool compact) {
    std::string fill = bar_bg(), sep = fill.empty() ? "" : ";" + fill;
    std::string out;
    hits.assign(M_N, {0, 0});
    width_used = 0;
    for (int m = 0; m < M_N; m++) {
        std::string name = MODE_NAME[m], base;
        base = m == mode ? "1;" + std::string(accent_bg()) : "90" + sep;
        char hot = 0;
        for (const auto &[ch, md] : keys)
            if (md == m) hot = ch;
        if (compact) name = std::string(1, hot ? hot : name[0]);
        size_t p = m == mode || !hot ? std::string::npos : name.find(hot);
        hits[m] = {width_used + 1, width_used + name.size() + 2};
        if (p == std::string::npos)
            out += c(base.c_str(), " " + name + " ");
        else
            out += c(base.c_str(), " " + name.substr(0, p)) +
                   c((base + ";4").c_str(), name.substr(p, 1)) +
                   c(base.c_str(), name.substr(p + 1) + " ");
        width_used += name.size() + 2;
    }
    return out;
}

// the health chip: a dead session first, then a stale source, then plain data age.
// a failing fetch is a gripe, and gripes live in the statusline
std::string age_chip(Store &s, bool &red, long long &best) {
    best = 0;
    red = false;
    std::string unsigned_names, stale_names;
    for (const view::SourceStatus &st : view::status(s)) {
        if (st.fetched_at > best) best = st.fetched_at;
        if (st.stale && !st.offline) {
            red = true;
            stale_names += (stale_names.empty() ? "" : ", ") + std::string(st.pretty);
        }
        if (!st.signed_in || !st.error.empty())
            unsigned_names += (unsigned_names.empty() ? "" : ", ") + std::string(st.pretty);
    }
    if (!unsigned_names.empty()) {
        red = true;
        return unsigned_names + " not signed, press a to sign in";
    }
    if (!stale_names.empty()) return stale_names + " stale";
    if (!best) return "no data yet";
    long long d = (long long)time(nullptr) - best;
    return "data " +
           (d < 60       ? std::string("<1 min")
            : d < 5400   ? std::to_string(d / 60) + " min"
            : d < 172800 ? std::to_string(d / 3600) + " hr"
                         : std::to_string(d / 86400) + " day") +
           " old";
}

// drop out of the tui for the interactive sign-ins, one source after the other
void auth_all() {
    leave();
    for (const Source &src : sources()) {
        while (!src.have_session() || !src.session_error().empty()) {
            fputs("\033[H\033[2J", stdout);
            try {
                if (src.login() != 0) break;  // empty input backs out of this source
            } catch (const std::exception &e) {
                fprintf(stderr, "%s\n", c("1;31", e.what()).c_str());
                printf("%s ", c("90", "enter to retry, q to skip:").c_str());
                fflush(stdout);
                char b[16];
                if (!fgets(b, sizeof b, stdin) || b[0] == 'q') break;
            }
        }
    }
    enter();
}

std::string refresh() {
    return system(APP_NAME "d >/dev/null 2>&1 &") == 0 ? "fetching" : "refresh failed";
}

// "data 5 min old" -> "5 min old" -> "5 min" -> "5m"; false once nothing is left to shed
bool shorten_age(std::string &a) {
    if (a.compare(0, 5, "data ") == 0) return a.erase(0, 5), true;
    if (a.size() > 4 && a.compare(a.size() - 4, 4, " old") == 0) return a.erase(a.size() - 4), true;
    size_t sp = a.rfind(' ');
    if (sp == std::string::npos) return false;
    std::string unit = a.substr(sp + 1);
    if (unit != "min" && unit != "hr" && unit != "day") return false;  // not an age, do not mangle
    a = a.substr(0, sp) + unit[0];
    return true;
}

// the whole ui keeps this line; tab strip left, transient action in the middle, position right
std::string status(Mode mode, const std::map<char, Mode> &keys, const std::string &msg,
                   const char *msg_col, const std::vector<std::string> &filters,
                   const std::string &age_in, bool age_red, const std::string &pos, size_t width,
                   std::vector<std::pair<size_t, size_t>> &hits,
                   std::vector<std::pair<size_t, size_t>> &chips) {
    std::string m = msg, age = age_in;
    size_t tw = 0;
    std::string strip;
    auto build = [&](bool compact) {
        strip = tabs(mode, keys, tw, hits, compact);
        chips.assign(filters.size(), {0, 0});
        for (size_t i = 0; i < filters.size(); i++) {  // click one to drop it
            chips[i] = {tw + 1, tw + utf8_len(filters[i]) + 2};
            strip += c("1;7;33", " " + filters[i] + " ");
            tw += utf8_len(filters[i]) + 2;
        }
    };
    build(false);
    // narrow terminal: shed the message, then the tab names, then the age chip, in that order
    size_t used;
    for (bool compact = false;;) {
        // +1 for the space before the gap, +1 for the pad that keeps the chip off the last column
        used = tw + (m.empty() ? 0 : utf8_len(m) + 1) + 1 + utf8_len(age) + 1 + utf8_len(pos) + 2 + 1;
        if (used <= width) break;
        if (!m.empty()) m.clear();
        else if (!compact) build(compact = true);
        else if (!shorten_age(age)) break;
    }
    std::string gap(used < width ? width - used : 0, ' ');
    // every segment repaints the fill: c() resets at each segment end
    std::string fill = bar_bg();
    std::string sep = fill.empty() ? "" : ";" + fill;
    std::string msg_sgr = m.empty() ? "90" : msg_col;
    return fit(strip + c((msg_sgr + sep).c_str(), (m.empty() ? "" : " " + m) + " " + gap) +
                   c(((age_red ? "1;31" : "90") + sep).c_str(), age + " ") +
                   c(("1;" + std::string(accent()) + sep).c_str(), " " + pos + " ") +
                   c(("90" + sep).c_str(), " "),
               width);
}

// under this the panes stop making sense: say so instead of painting a sheared grid
const int MIN_COLS = 24, MIN_ROWS = 8;

std::string too_small(int rows, int cols) {
    std::string t = (size_t)cols >= 16 ? "Window too small" : (size_t)cols >= 9 ? "too small" : "!";
    std::string out = "\033[2J";
    for (int r = 1; r < (rows + 1) / 2; r++) out += "\r\n";
    return out + std::string(((size_t)cols - utf8_len(t)) / 2, ' ') +
           c("1;33", plain_cut(t, std::min((size_t)cols, utf8_len(t)))) + "\033[J";
}

// marks: subject list, or one subject's marks. rows are 1:1 with lines, so sel is a line index
struct MarksView {
    std::vector<std::string> lines;
    std::vector<std::string> subjects;  // list level only: line -> subject to open
};

MarksView build_marks(const view::Marks &snap, const std::string &subject, size_t cols) {
    MarksView v;
    if (!subject.empty()) {
        view::Marks one;
        for (const auto &r : snap.rows)
            if (r.klass == subject) one.rows.push_back(r);
        for (const auto &a : snap.averages)
            if (std::get<0>(a) == subject) one.averages.push_back(a);
        v.lines = paint::mark_lines(one, cols);
        if (v.lines.empty()) v.lines.push_back(c("90", "no marks"));
        return v;
    }
    std::vector<std::string> order;
    std::map<std::string, std::pair<int, int>> tally;  // subject -> {marks, new}
    for (const auto &r : snap.rows) {
        if (!tally.count(r.klass)) order.push_back(r.klass);
        tally[r.klass].first++;
        tally[r.klass].second += r.is_new;
    }
    std::map<std::string, double> newest;  // averages run oldest period first, so the last wins
    for (const auto &[k, p, a] : snap.averages) newest[k] = a;
    size_t wc = 3;
    for (const auto &k : order) wc = std::max(wc, utf8_len(k));
    for (const auto &k : order) {
        auto it = newest.find(k);
        // a subject whose marks are none of them gradeable averages to N, not to a blank
        std::string a = it == newest.end() ? "N" : paint::avg_str(it->second);
        char n[32];
        snprintf(n, sizeof n, "%d mark%s", tally[k].first, tally[k].first == 1 ? "" : "s");
        v.lines.push_back(c((std::string("1;") + accent()).c_str(),
                            k + std::string(wc - utf8_len(k), ' ')) +
                          "  " +
                          c(it == newest.end() ? "39" : paint::avg_color(it->second), a) + "  " +
                          c("90", n) + (tally[k].second ? "  " + c(NEW_CHIP, " NEW ") : ""));
        v.subjects.push_back(k);
    }
    if (order.empty()) v.lines.push_back(c("90", "no marks"));
    return v;
}

std::string plain(const std::string &l);

// a box over the frame; drawn with absolute cursor moves so nothing has to be spliced. body
// lines may carry their own sgr, so they are padded by visible width and never re-coloured
std::string popup(const std::string &name, const std::vector<std::string> &body, int rows,
                  int cols, size_t minw = 0, size_t at_row = 0, size_t at_col = 0,
                  size_t *row0 = nullptr, size_t *col0 = nullptr, size_t *wide = nullptr) {
    size_t w = std::max(utf8_len(name), minw);
    for (const auto &b : body) w = std::max(w, utf8_len(plain(b)));
    if (w > (size_t)cols - 6) w = (size_t)cols - 6;
    size_t h = body.size() + 4;
    // an anchored box hangs off a screen cell, and is pulled back in when it would run off
    size_t r0 = at_row ? at_row : (size_t)rows > h ? ((size_t)rows - h) / 2 + 1 : 1;
    size_t c0 = at_col ? at_col : (size_t)cols > w + 4 ? ((size_t)cols - w - 4) / 2 + 1 : 1;
    if (r0 + h > (size_t)rows + 1) r0 = (size_t)rows > h ? (size_t)rows - h + 1 : 1;
    if (c0 + w + 4 > (size_t)cols + 1) c0 = (size_t)cols > w + 4 ? (size_t)cols - w - 3 : 1;
    std::string ac = accent();
    if (row0) *row0 = r0;
    if (col0) *col0 = c0;
    if (wide) *wide = w + 4;
    auto at = [&](size_t r) { return "\033[" + std::to_string(r0 + r) + ";" +
                                     std::to_string(c0) + "H"; };
    auto pad = [&](const std::string &l) {
        size_t n = utf8_len(plain(l));
        return n <= w ? l + "\033[0m" + std::string(w - n, ' ') : plain_cut(plain(l), w);
    };
    std::string out = at(0) + c(ac.c_str(), "┌");
    for (size_t i = 0; i < w + 2; i++) out += c(ac.c_str(), "─");
    out += c(ac.c_str(), "┐");
    out += at(1) + c(ac.c_str(), "│") + " " + c("1", plain_cut(name, w)) + " " +
           c(ac.c_str(), "│");
    for (size_t i = 0; i < body.size(); i++)
        out += at(2 + i) + c(ac.c_str(), "│") + " " + pad(body[i]) + " " + c(ac.c_str(), "│");
    out += at(2 + body.size()) + c(ac.c_str(), "│") + " " + std::string(w, ' ') + " " +
           c(ac.c_str(), "│");
    out += at(3 + body.size()) + c(ac.c_str(), "└");
    for (size_t i = 0; i < w + 2; i++) out += c(ac.c_str(), "─");
    out += c(ac.c_str(), "┘");
    return out;
}

std::string lesson_popup(const Lesson &l, int rows, int cols) {
    std::vector<std::string> body;
    auto hhmm = [](const std::string &t) { return t.size() == 4 ? "0" + t : t; };
    if (!l.begins.empty()) body.push_back(hhmm(l.begins) + " - " + hhmm(l.ends));
    if (!l.room.empty()) body.push_back(l.room);
    const std::string &who = l.teacher_name.empty() ? l.teacher : l.teacher_name;
    if (!who.empty()) body.push_back(who);
    // a changed lesson says what the change is; the grid keeps showing what it should have been
    if (!l.state.empty()) {
        std::string what = l.change;
        if (what.empty()) what = l.state == "x" ? "cancelled" : "changed";
        body.push_back("");
        body.push_back(c(l.state == "x" ? "1;32" : "1;31", "*") + " " + what);
    }
    return popup(l.subject_name.empty() ? l.subject : l.subject_name, body, rows, cols, 34);
}

// the week menu: two weeks back, four ahead, then the permanent grid. `cur` is the week on
// screen, `sel` the cursor
std::vector<std::pair<std::string, std::string>> weeks(const std::string &cur) {
    std::string base = view::wanted_monday();
    std::vector<std::pair<std::string, std::string>> out;
    for (int n = -2; n <= 4; n++) {
        std::string m = view::ymd_plus(base, 7 * n);
        out.push_back({m, paint::date_short(m)});
    }
    if (std::find_if(out.begin(), out.end(), [&](const auto &w) { return w.first == cur; }) ==
        out.end() && cur != PERM_MONDAY)
        out.insert(out.begin(), {cur, paint::date_short(cur)});  // stepped past the menu range
    out.push_back({PERM_MONDAY, "permanent"});
    return out;
}

// one menu row: label centred in a uniform block, this week in the accent, the cursor inverse
std::string week_row(const std::string &label, size_t w, bool now, bool sel) {
    size_t n = std::min(w, utf8_len(label)), lead = (w - n) / 2;
    std::string t = std::string(lead, ' ') + plain_cut(label, w - lead);
    std::string sgr = sel ? "7" : now ? "1;" + std::string(accent()) : "39";
    return c(sgr.c_str(), t);
}

std::vector<std::string> help_body(const std::map<char, Mode> &keys) {
    std::string modes;
    for (int m = 0; m < M_N; m++)
        for (const auto &[ch, md] : keys)
            if (md == m) modes += std::string(1, ch) + " " + MODE_NAME[m] + "   ";
    return {modes,
            "tab / shift-tab  next / previous tab",
            "j k g G arrows   move",
            "enter            open link / open subject",
            "/                filter, empty clears",
            "x                dismiss the item",
            "[ ] w            table: week back, forward, menu",
            "a r q            sign in, refresh, quit"};
}

// a painted line with its sgr escapes dropped, for locating chips by column
std::string plain(const std::string &l) {
    std::string o;
    for (size_t i = 0; i < l.size(); i++) {
        if (l[i] == 27) {
            size_t e = l.find('m', i);
            if (e == std::string::npos) break;
            i = e;
        } else o += l[i];
    }
    return o;
}

// "/ang info teams" -> three folded words, ANDed by view::feed_rows
std::vector<std::string> filter_words(const std::string &line) {
    std::vector<std::string> out;
    for (size_t i = 0; i < line.size();) {
        size_t e = line.find(' ', i);
        if (e == std::string::npos) e = line.size();
        if (e > i) out.push_back(view::fold(line.substr(i, e - i)));
        i = e + 1;
    }
    return out;
}

// paint::term_cols() caps at 100 for pipes; the tui owns the whole screen so it uses the real size
void term_size(int &rows, int &cols) {
    struct winsize w {};
    if (ioctl(1, TIOCGWINSZ, &w) != 0) w = {24, 80, 0, 0};
    rows = w.ws_row ? w.ws_row : 24;
    cols = w.ws_col ? w.ws_col : 80;
}

// keymap parsing, cell fitting and the grid geometry; run by --selfcheck
int selfcheck() {
    {  // date_short: czech day. month., year only when it is not the current one
        time_t n = time(nullptr);
        struct tm cur {};
        localtime_r(&n, &cur);
        char y[8];
        strftime(y, sizeof y, "%Y", &cur);
        if (paint::date_short(std::string(y) + "-08-17") != "17. 8." ||
            paint::date_short("1999-01-05") != "5. 1. 1999") {
            fprintf(stderr, "selfcheck failed: date_short\n");
            return 1;
        }
    }
    if (plain_cut("abcdef", 3) != "abc" || plain_cut("ab", 4) != "ab  " ||
        plain_cut("čeština", 3) != "češ") {
        fputs("selfcheck failed: plain_cut\n", stderr);
        return 1;
    }
    config().set("key.x", "timetable");
    config().set("key.z", "nonsense");
    std::map<char, Mode> k = keymap();
    if (k['f'] != M_FEED || k['m'] != M_MARKS || k['x'] != M_TABLE || k['b'] != M_ABSENCE ||
        k.count('z')) {
        fputs("selfcheck failed: keymap\n", stderr);
        return 1;
    }
    size_t w = 0;
    std::vector<std::pair<size_t, size_t>> hits;
    tabs(M_MARKS, k, w, hits, false);
    if (w != 6 + 7 + 7 + 9 || hits[0].first != 1 || hits[1].first != 7 ||
        hits[2].second != 20 || hits[3].first != 21) {
        fputs("selfcheck failed: tab hit columns\n", stderr);
        return 1;
    }
    tabs(M_MARKS, k, w, hits, true);  // compact: one letter a tab
    if (w != 4 * 3) {
        fputs("selfcheck failed: compact tabs\n", stderr);
        return 1;
    }

    view::Timetable tt;
    tt.monday = "2026-08-17";
    tt.days = {"2026-08-17", "2026-08-18"};
    tt.hours = {"1", "2"};
    tt.rows = {{"bakalari", "2026-08-17", "1", "CJL", "Cestina", "214", "Novak", "", "8:00", "8:45", "Jan Novak", ""}};
    tt.grid = {&tt.rows[0], nullptr, nullptr, nullptr};
    {  // widest time form that fits: full span, minutes-only, start over end, then nothing
        paint::TableLayout wide = paint::table_layout(tt, 3, 200, 3);
        paint::TableLayout m = paint::table_layout(tt, 3, 20, 3);
        paint::TableLayout e = paint::table_layout(tt, 3, 14, 3);
        paint::TableLayout n = paint::table_layout(tt, 3, 11, 3);
        if (wide.t0[0] != "8:00-8:45" || wide.cw != 9 || m.t0[0] != "00-45" || m.time_rows != 1 ||
            e.time_rows != 2 || e.t1[0] != "8:45" || n.time_rows) {
            fputs("selfcheck failed: table time forms\n", stderr);
            return 1;
        }
    }
    {  // an hour with no lesson all week is dropped, and so is a day with none
        view::Timetable e = tt;
        e.hours = {"0", "1", "2"};
        e.days = {"2026-08-17", "2026-08-18"};
        e.grid = {nullptr, &e.rows[0], nullptr, nullptr, nullptr, nullptr};
        view::compact(e);
        if (e.hours != std::vector<std::string>{"1"} || e.days.size() != 1 || e.grid.size() != 1) {
            fputs("selfcheck failed: table compact\n", stderr);
            return 1;
        }
    }
    {  // the lesson box states the change, not the hour or the date; the menu offers the grid
        Lesson x = tt.rows[0];
        x.state = "x";
        x.change = "Odpadá";
        std::string b = lesson_popup(x, 40, 100);  // raw: plain() cannot skip the cursor moves
        if (b.find("08:00 - 08:45") == std::string::npos || b.find("Jan Novak") == std::string::npos ||
            b.find("Odpadá") == std::string::npos || b.find("hour") != std::string::npos ||
            weeks("2026-08-17").back().first != std::string(PERM_MONDAY)) {
            fputs("selfcheck failed: lesson box\n", stderr);
            return 1;
        }
    }
    Geom big{}, small{};
    std::vector<std::string> b = grid_lines(tt, 0, 0, 40, 200, big);
    std::vector<std::string> s2 = grid_lines(tt, 0, 0, 5, 24, small);
    // wide terminal: three lines a cell plus a spacer row; cramped: one line, no spacer
    if (big.blk != 4 || small.blk != 2 || b.size() != big.top + 2 * 4 || s2.size() != small.top + 2 * 2) {
        fputs("selfcheck failed: table tiers\n", stderr);
        return 1;
    }
    // a grid smaller than the pane sits in the middle of it, not in the top left corner
    if (!big.left || b[0] != "" || b[big.top - 1].compare(0, big.left, std::string(big.left, ' '))) {
        fputs("selfcheck failed: table centring\n", stderr);
        return 1;
    }
    {  // the week control lands on the header line, shifted along with the block
        Geom flat{};
        grid_lines(tt, 0, 0, 0, 200, flat);  // the cli path draws no arrows to click
        std::string hdr = plain(b[big.hdr]);
        if (flat.prev || big.prev != big.left + 1 || big.lbl0 != big.left + 3 ||
            big.next != big.lbl1 + 2 || utf8_len(hdr) != big.next ||
            hdr.compare(big.left, 3, "◂") != 0) {
            fputs("selfcheck failed: week control columns\n", stderr);
            return 1;
        }
    }
    if (small.gut + tt.hours.size() * (small.cw + 1) + 1 > 24) {
        fputs("selfcheck failed: table overflows the terminal\n", stderr);
        return 1;
    }
    // this grid fits any width in the sweep: a cut row means the geometry overran the terminal
    for (int cx = 20; cx <= 200; cx++) {
        Geom g{};
        for (const auto &line : grid_lines(tt, 0, 0, 24, cx, g))
            if (fit(line, (size_t)cx) != line) {
                fprintf(stderr, "selfcheck failed: table row overflows %d columns\n", cx);
                return 1;
            }
    }
    {  // chip columns come off the painted title line, sgr and all
        std::string hdr = c("1", "title") + "  " + c("1;36", "<ANG>") + " " +
                          c("33", "<hw>") + " " + c("90", "<teams>");
        std::string p = plain(hdr);
        if (p != "title  <ANG> <hw> <teams>" || p.rfind("<hw>") != 13) {
            fputs("selfcheck failed: chip columns\n", stderr);
            return 1;
        }
    }
    {  // a bar narrower than its own chips must still return, not trim forever
        std::vector<std::pair<size_t, size_t>> sh, sc;
        std::string a = "data 12 min old";
        if (!shorten_age(a) || a != "12 min old" || !shorten_age(a) || a != "12 min" ||
            !shorten_age(a) || a != "12m" || shorten_age(a)) {
            fputs("selfcheck failed: age ladder\n", stderr);
            return 1;
        }
        for (size_t wd = 4; wd <= 40; wd++) {
            std::string bar = status(M_FEED, {{'f', M_FEED}}, "hi", "90", {}, "data 12 min old",
                                     false, "1/9", wd, sh, sc);
            if (fit(bar, wd) != bar) {
                fprintf(stderr, "selfcheck failed: status bar overflows %zu columns\n", wd);
                return 1;
            }
        }
    }
    if (filter_words("  ANG   info teams ") != std::vector<std::string>{"ang", "info", "teams"} ||
        !filter_words("   ").empty()) {
        fputs("selfcheck failed: filter_words\n", stderr);
        return 1;
    }
    fputs(TUI_NAME ": selfcheck ok\n", stdout);
    return 0;
}

}  // namespace

namespace {

// one keystroke: a plain byte, a csi with a final byte, or an sgr mouse report
struct Ev {
    char ch = 0;      // plain byte, 0 for the others
    char fin = 0;     // csi final byte
    std::string seq;  // csi parameters
    int btn = 0, mx = 0, my = 0;
    bool mouse = false;
};

// false = the buffer holds an incomplete sequence, wait for more input. esc_ok says the caller
// already waited out the escape timeout, so a lone esc is the key and not the head of a sequence
bool next_ev(std::string &b, Ev &e, bool esc_ok) {
    e = Ev{};
    if (b.empty()) return false;
    if (b[0] != 27) {
        e.ch = b[0];
        b.erase(0, 1);
        return true;
    }
    if (b.size() == 1) {
        if (!esc_ok) return false;
        e.ch = 27;
        b.erase(0, 1);
        return true;
    }
    if (b[1] != '[') {
        b.erase(0, 2);
        return true;  // swallowed, no event
    }
    size_t end = b.find_first_of("mM~ABCDHF", 2);
    if (end == std::string::npos) return false;
    e.seq = b.substr(2, end - 2);
    e.fin = b[end];
    b.erase(0, end + 1);
    if (!e.seq.empty() && e.seq[0] == '<') {
        if (sscanf(e.seq.c_str(), "<%d;%d;%d", &e.btn, &e.mx, &e.my) != 3) return true;
        e.mouse = true;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--selfcheck")) return selfcheck();
    if (argc > 1) {
        fprintf(stderr, "usage: %s [--selfcheck]\n", TUI_NAME);
        return 2;
    }
    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "%s: not a terminal\n", TUI_NAME);
        return 2;
    }
    try {
        Store store;
        std::map<char, Mode> keys = keymap();
        Mode mode = M_FEED;
        int rows = 24, cols = 80;
        bool relayout = true, first = true;
        std::string msg;
        long long msg_at = 0, click_at = 0;
        long long refresh_since = 0, refresh_base = 0, fetched_best = 0;
        std::vector<std::pair<size_t, size_t>> tab_hits(M_N, {0, 0}), chip_hits;

        std::vector<std::string> filters;
        std::string fbuf;
        bool fmode = false;

        view::Feed fsnap;
        std::vector<Post> posts;
        std::vector<size_t> owner;  // flat line -> post index
        std::vector<size_t> start;  // post index -> first flat line
        std::vector<std::string> flat;
        size_t sel = 0, top = 0, click_post = (size_t)-1;

        view::Marks msnap;
        bool msnap_ok = false, marks_seen = false;
        std::string subject;
        MarksView mv;
        size_t msel = 0, mtop = 0;

        std::vector<std::string> alines;
        bool abs_ok = false;
        size_t atop = 0;

        view::Timetable tt;
        bool tt_ok = false, tt_first = true;
        std::string tt_mon;  // empty = the week view::timetable picks on its own
        bool menu = false;
        size_t wsel = 0, menu_row = 0, menu_col = 0, menu_w = 0;
        size_t cd = 0, chr = 0;
        bool pop = false, help_pop = false;
        Geom geom;
        std::vector<std::string> tlines;

        enter();
        signal(SIGINT, die);
        signal(SIGTERM, die);
        // no SA_RESTART: sigwinch must break the blocking read so a resize repaints on its own
        struct sigaction wa {};
        wa.sa_handler = on_winch;
        sigaction(SIGWINCH, &wa, nullptr);
        atexit(leave);

        std::string pending;
        // the db, its wal and its shm all live here; maild renames/creates as well as writes
        int ifd = -1;  // non-linux: stays -1, the db_stamp() stat poll below covers it
#ifdef __linux__
        ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ifd >= 0 &&
            inotify_add_watch(ifd, data_dir().c_str(),
                              IN_CLOSE_WRITE | IN_MODIFY | IN_CREATE | IN_MOVED_TO) < 0) {
            close(ifd);
            ifd = -1;
        }
#endif
        long long stamp = db_stamp();  // ifd < 0 only: stat fallback
        for (;;) {
            if (resized) {
                resized = 0;
                term_size(rows, cols);
                rows -= 1;
                relayout = true;
            }
            if (relayout) {
                relayout = false;
                if (mode == M_FEED) {
                    fsnap = view::feed_rows(store, filters,
                                            (size_t)config().num("general.limit"));
                    posts = feed_posts(fsnap, (size_t)cols - 2, false);
                    flat.clear();
                    owner.clear();
                    start.clear();
                    for (size_t p = 0; p < posts.size(); p++) {
                        start.push_back(flat.size());
                        for (const auto &l : posts[p].lines) {
                            flat.push_back(fit(l, (size_t)cols - 2));
                            owner.push_back(p);
                        }
                    }
                    if (first || sel >= posts.size()) sel = posts.empty() ? 0 : posts.size() - 1;
                    first = false;
                } else if (mode == M_MARKS) {
                    if (!msnap_ok) {
                        // never consumed here: a live reload would wipe the chips off rows the
                        // user has not looked at yet. the watermark moves on the way out
                        msnap = view::marks_rows(store, filters, 0, false);
                        msnap_ok = true;
                        marks_seen = true;
                    }
                    mv = build_marks(msnap, subject, (size_t)cols - 2);
                    if (msel >= mv.lines.size()) msel = mv.lines.empty() ? 0 : mv.lines.size() - 1;
                } else if (mode == M_ABSENCE) {
                    if (!abs_ok) {
                        alines = paint::absence_lines(view::absence_rows(store, filters));
                        abs_ok = true;
                    }
                } else {
                    if (!tt_ok) {
                        tt = view::timetable(store, tt_mon);
                        tt_ok = true;
                    }
                    if (cd >= tt.days.size()) cd = 0;
                    if (chr >= tt.hours.size()) chr = 0;
                    if (tt_first) {
                        tt_first = false;
                        long long nw = (long long)time(nullptr);
                        for (size_t d = 0; d < tt.days.size(); d++)
                            for (size_t h = 0; h < tt.hours.size(); h++) {
                                const Lesson *l = tt.at(d, h);
                                if (l && !l->begins.empty() &&
                                    view::local_at(l->date, l->begins) <= nw &&
                                    nw < view::local_at(l->date, l->ends))
                                    cd = d, chr = h;
                            }
                    }
                }
            }
            if (msg_at && now_ms() - msg_at > 4000) {
                msg.clear();
                msg_at = 0;
            }

            std::string out = "\033[H", pos;
            bool tiny = cols < MIN_COLS || rows + 1 < MIN_ROWS;
            if (tiny) {
                out += too_small(rows + 1, cols);
            } else if (mode == M_FEED) {
                pos = std::to_string(posts.empty() ? 0 : sel + 1) + "/" +
                      std::to_string(posts.size());
                if (flat.empty()) {
                    out += "\033[90mnothing to show\033[0m\033[K\033[J\033[" +
                           std::to_string(rows + 1) + ";1H";
                } else {
                    // keep the selected post visible: its top, or its tail if it is taller than
                    // the screen
                    size_t s0 = start[sel], s1 = s0 + posts[sel].lines.size();
                    if (s0 < top) top = s0;
                    else if (s1 > top + (size_t)rows)
                        top = s1 - (size_t)rows < s0 ? s1 - (size_t)rows : s0;
                    size_t max_top = flat.size() > (size_t)rows ? flat.size() - (size_t)rows : 0;
                    if (top > max_top) top = max_top;
                    for (int r = 0; r < rows; r++) {
                        size_t li = top + (size_t)r;
                        if (li < flat.size())
                            out += (owner[li] == sel
                                        ? (std::string("\033[") + accent() + "m▎\033[0m ")
                                        : "  ") +
                                   flat[li];
                        out += "\033[K";
                        if (r < rows - 1) out += "\r\n";
                    }
                    out += "\033[J\r\n";
                }
            } else if (mode == M_MARKS) {
                pos = subject.empty() ? std::to_string(mv.subjects.size()) + " subjects" : subject;
                bool list = subject.empty() && !mv.subjects.empty();
                if (list) {
                    if (msel < mtop) mtop = msel;
                    if (msel >= mtop + (size_t)rows) mtop = msel - (size_t)rows + 1;
                }
                size_t max_top = mv.lines.size() > (size_t)rows ? mv.lines.size() - (size_t)rows : 0;
                if (mtop > max_top) mtop = max_top;
                for (int r = 0; r < rows; r++) {
                    size_t li = mtop + (size_t)r;
                    if (li < mv.lines.size())
                        out += (list ? (li == msel ? std::string("\033[") + accent() + "m▎\033[0m "
                                                   : "  ")
                                     : "  ") +
                               fit(mv.lines[li], (size_t)cols - 2);
                    out += "\033[K";
                    if (r < rows - 1) out += "\r\n";
                }
                out += "\033[J\r\n";
            } else if (mode == M_ABSENCE) {
                pos = std::to_string(alines.size()) + " subjects";
                size_t max_top = alines.size() > (size_t)rows ? alines.size() - (size_t)rows : 0;
                if (atop > max_top) atop = max_top;
                for (int r = 0; r < rows; r++) {
                    size_t li = atop + (size_t)r;
                    if (li < alines.size()) out += "  " + fit(alines[li], (size_t)cols - 2);
                    else if (!li) out += c("90", "no absence");
                    out += "\033[K";
                    if (r < rows - 1) out += "\r\n";
                }
                out += "\033[J\r\n";
            } else {
                tlines = grid_lines(tt, cd, chr, rows, cols, geom);
                pos = paint::date_short(tt.days.empty() ? tt.monday : tt.days[cd]);
                for (int r = 0; r < rows; r++) {
                    if ((size_t)r < tlines.size()) out += tlines[(size_t)r];
                    out += "\033[K";
                    if (r < rows - 1) out += "\r\n";
                }
                out += "\033[J\r\n";
            }
            if (!tiny) {
            bool age_red = false;
            std::string age = age_chip(store, age_red, fetched_best);
            if (refresh_since) {  // give up on the spinner if no fetch lands
                if (fetched_best > refresh_base || now_ms() - refresh_since > 120000) {
                    refresh_since = 0;
                    msg = fetched_best > refresh_base ? "refreshed" : "refresh timed out";
                    msg_at = now_ms();
                } else {
                    msg = "refreshing" + std::string(1 + (size_t)((now_ms() - refresh_since) / 400) % 3, '.');
                    msg_at = 0;
                }
            }
            // a gripe is a condition, not a toast: it holds until it clears, but any transient
            // message wins the line while it lives
            std::string line = msg;
            const char *msg_col = "90";
            if (line.empty() && !fsnap.bad_filter.empty()) {
                line = "no such filter '" + fsnap.bad_filter + "'";
                msg_col = "1;31";
            }
            if (line.empty())
                for (const view::Gripe &g : view::gripes(store)) {
                    line = g.text;
                    msg_col = g.error ? "1;31" : "1;33";
                    if (g.error) break;
                }
            if (fmode)
                out += c((std::string("1;") + accent()).c_str(), "/") + fbuf + "\033[7m \033[0m\033[K";
            else
                out += status(mode, keys, line, msg_col, filters, age, age_red, pos, (size_t)cols,
                              tab_hits, chip_hits);
            if (mode == M_TABLE && pop && !tt.grid.empty() && tt.at(cd, chr))
                out += lesson_popup(*tt.at(cd, chr), rows, cols);
            if (mode == M_TABLE && menu) {
                std::vector<std::string> body;
                auto ws = weeks(tt.monday);
                if (wsel >= ws.size()) wsel = 0;
                std::string now = view::wanted_monday();
                size_t w = 0;
                for (const auto &x : ws) w = std::max(w, utf8_len(x.second));
                w += 4;
                for (size_t i = 0; i < ws.size(); i++)
                    body.push_back(week_row(ws[i].second, w, ws[i].first == now, i == wsel));
                // the menu hangs under the week label it belongs to, never over the header
                out += popup("week", body, rows, cols, w, geom.hdr + 2,
                             geom.lbl0 > 2 ? geom.lbl0 - 2 : 1, &menu_row, &menu_col, &menu_w);
            }
            if (help_pop) out += popup("keys", help_body(keys), rows, cols);
            }
            fwrite(out.data(), 1, out.size(), stdout);
            fflush(stdout);

            char buf[64];
            {  // wake on the message deadline and once a minute so the age chip stays honest
                long long left = refresh_since ? 400 : ifd >= 0 ? 30000 : 1000;
                if (msg_at) left = std::min(left, 4000 - (now_ms() - msg_at));
                pollfd pf[2] = {{0, POLLIN, 0}, {ifd, POLLIN, 0}};
                int nf = poll(pf, ifd >= 0 ? 2 : 1, (int)(left > 0 ? left : 0));
                bool wrote = false;
#ifdef __linux__
                if (nf > 0 && ifd >= 0 && (pf[1].revents & POLLIN)) {
                    char eb[4096];
                    for (ssize_t n; (n = read(ifd, eb, sizeof eb)) > 0;)
                        for (char *q = eb; q < eb + n;) {
                            auto *ev = (inotify_event *)q;
                            if (ev->len && strncmp(ev->name, APP_NAME ".db", sizeof(APP_NAME ".db") - 1) == 0) wrote = true;
                            q += sizeof(inotify_event) + ev->len;
                        }
                }
#endif
                if (ifd < 0 && nf <= 0) {
                    long long now = db_stamp();
                    wrote = now != stamp;
                    stamp = now;
                }
                if (wrote) {  // maild wrote: drop every cached view and re-read
                    msnap_ok = tt_ok = abs_ok = false;
                    relayout = true;
                }
                if (!(pf[0].revents & POLLIN)) continue;  // repaint; the age chip ticks
            }
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                break;
            }
            pending.append(buf, (size_t)n);

            bool quit = false;
            Ev e;
            for (;;) {
                bool esc_ok = false;
                if (pending == "\x1b") {  // esc, or the first byte of a sequence still in flight
                    pollfd pf{0, POLLIN, 0};
                    if (poll(&pf, 1, 25) > 0) break;
                    esc_ok = true;
                }
                if (!next_ev(pending, e, esc_ok)) break;
                auto set_week = [&](const std::string &m) {
                    tt_mon = m;
                    tt_ok = false;
                    pop = menu = false;
                    cd = chr = 0;
                    relayout = true;
                };
                // stepping off the permanent grid lands on the real weeks, not on 1970
                auto step = [&](int d) {
                    return view::ymd_plus(
                        tt.monday == PERM_MONDAY ? view::wanted_monday() : tt.monday, d);
                };
                auto refilter = [&] {
                    fsnap.bad_filter.clear();  // marks mode never rebuilds fsnap
                    msnap_ok = abs_ok = false;
                    relayout = true;
                };
                if (fmode) {
                    if (e.ch == 27) fmode = false;
                    else if (e.ch == '\r' || e.ch == '\n') {
                        fmode = false;
                        filters = filter_words(fbuf);
                        refilter();
                    } else if (e.ch == 127 || e.ch == 8) {
                        while (!fbuf.empty() && ((unsigned char)fbuf.back() & 0xc0) == 0x80)
                            fbuf.pop_back();
                        if (!fbuf.empty()) fbuf.pop_back();
                    } else if (e.ch == 21) fbuf.clear();
                    else if (e.ch == 23) {
                        while (!fbuf.empty() && fbuf.back() == ' ') fbuf.pop_back();
                        while (!fbuf.empty() && fbuf.back() != ' ') fbuf.pop_back();
                    } else if ((unsigned char)e.ch >= 32) fbuf += e.ch;
                    continue;
                }
                auto go = [&](Mode m) {
                    if (m == mode) return;
                    mode = m;
                    pop = false;
                    relayout = true;
                };
                if (e.ch == 'q' || e.ch == 3) {
                    quit = true;
                    break;
                }
                if (menu) {  // the week menu owns every key while it is open
                    auto ws = weeks(tt.monday);
                    long hit = e.mouse && e.btn == 0 && e.fin == 'M'
                                   ? e.my - (long)menu_row - 2
                                   : -1;
                    bool inside = hit >= 0 && (size_t)hit < ws.size() &&
                                  (size_t)e.mx >= menu_col && (size_t)e.mx < menu_col + menu_w;
                    if (e.ch == 'j' || e.fin == 'B') wsel += wsel + 1 < ws.size();
                    else if (e.ch == 'k' || e.fin == 'A') wsel -= wsel > 0;
                    else if (e.ch == '\r' || e.ch == '\n' || e.ch == ' ') set_week(ws[wsel].first);
                    else if (inside) set_week(ws[(size_t)hit].first);
                    else if (!e.mouse || e.fin == 'M') menu = false;  // outside click, or any key
                    continue;
                }
                if (pop || help_pop) {  // any key dismisses a popup, and does nothing else
                    pop = help_pop = false;
                    pending.clear();
                    break;
                }
                if (e.ch == '/') {
                    fmode = true;
                    fbuf.clear();
                    continue;
                }
                if (e.ch == '?') {
                    help_pop = true;
                    continue;
                }
                if (e.ch && keys.count(e.ch)) {
                    go(keys[e.ch]);
                    continue;
                }
                if (e.ch == 'a') {
                    auth_all();
                    resized = 1;
                    relayout = true;
                    continue;
                }
                if (e.ch == 'r') {
                    msg = refresh();
                    msg_at = now_ms();
                    if (msg == "refresh failed") continue;  // leave it as a plain message
                    refresh_since = now_ms();
                    refresh_base = fetched_best;
                    msg.clear();
                    msg_at = 0;
                    relayout = true;
                    continue;
                }
                if (e.ch == '\t') {
                    go((Mode)((mode + 1) % M_N));
                    continue;
                }
                if (e.fin == 'Z') {
                    go((Mode)((mode + M_N - 1) % M_N));
                    continue;
                }
                // a click on the tab strip switches mode wherever you are
                if (e.mouse && e.btn == 0 && e.fin == 'M' && e.my == rows + 1) {
                    for (size_t i = 0; i < chip_hits.size(); i++)
                        if ((size_t)e.mx >= chip_hits[i].first &&
                            (size_t)e.mx <= chip_hits[i].second) {
                            filters.erase(filters.begin() + (long)i);
                            refilter();
                            break;
                        }
                    for (int m = 0; m < M_N; m++)
                        if ((size_t)e.mx >= tab_hits[m].first && (size_t)e.mx <= tab_hits[m].second)
                            go((Mode)m);
                    continue;
                }

                if (mode == M_FEED) {
                    auto open_sel = [&] {
                        if (posts.empty()) return;
                        msg = posts[sel].url.empty()          ? "no link"
                              : open_url(posts[sel].url) == 0 ? "opened in browser"
                                                              : "open failed";
                        msg_at = now_ms();
                    };
                    auto move = [&](long d) {
                        if (posts.empty()) return;
                        long v = (long)sel + d;
                        sel = (size_t)(v < 0                     ? 0
                                       : v >= (long)posts.size() ? (long)posts.size() - 1
                                                                 : v);
                    };
                    if (e.ch == '\r' || e.ch == '\n') open_sel();
                    else if (e.ch == 'x' && !posts.empty()) {
                        store.dismiss({fsnap.items[fsnap.rows[sel].n - 1].id});
                        msg = "dismissed";
                        msg_at = now_ms();
                        msnap_ok = false;
                        relayout = true;
                    }
                    else if (e.ch == 'j' || e.fin == 'B') move(1);
                    else if (e.ch == 'k' || e.fin == 'A') move(-1);
                    else if (e.ch == 'g' || e.fin == 'H') sel = 0;
                    else if (e.ch == 'G' || e.fin == 'F') move((long)posts.size());
                    else if (e.fin == '~' && e.seq == "5") move(-5);
                    else if (e.fin == '~' && e.seq == "6") move(5);
                    else if (e.mouse && e.btn == 64) move(-1);
                    else if (e.mouse && e.btn == 65) move(1);
                    else if (e.mouse && e.btn == 0 && e.fin == 'M') {
                        size_t li = top + (size_t)(e.my - 1);
                        if (li >= flat.size()) continue;
                        sel = owner[li];
                        // the title line carries the <class> chip: click it to filter by class
                        size_t hdr = start[sel] + (sel && fsnap.rows[sel].bucket ==
                                                              fsnap.rows[sel - 1].bucket ? 0 : 1);
                        if (li == hdr) {
                            const Item &it = fsnap.items[fsnap.rows[sel].n - 1];
                            std::string p = plain(flat[li]);
                            size_t col = (size_t)e.mx - 3;  // mx is 1-based, past the 2-col gutter
                            std::string k = view::fold(fsnap.rows[sel].klass);
                            for (const std::string &chip : {fsnap.rows[sel].klass, it.kind,
                                                            it.source}) {
                                size_t at = p.rfind("<" + chip + ">");
                                if (at == std::string::npos) continue;
                                size_t c0 = utf8_len(p.substr(0, at));
                                if (col >= c0 && col < c0 + utf8_len(chip) + 2) k = view::fold(chip);
                            }
                            if (std::find(filters.begin(), filters.end(), k) == filters.end()) {
                                filters.push_back(k);
                                refilter();
                            }
                            continue;
                        }
                        long long t = now_ms();
                        if (click_post == sel && t - click_at < 400) {
                            open_sel();
                            click_at = 0;
                        } else {
                            click_at = t;
                            click_post = sel;
                        }
                    }
                } else if (mode == M_MARKS) {
                    bool list = subject.empty() && !mv.subjects.empty();
                    auto move = [&](long d) {
                        if (list) {
                            long v = (long)msel + d, hi = (long)mv.subjects.size() - 1;
                            msel = (size_t)(v < 0 ? 0 : v > hi ? hi : v);
                        } else {
                            long v = (long)mtop + d;
                            mtop = (size_t)(v < 0 ? 0 : v);
                        }
                    };
                    auto back = [&] {
                        if (subject.empty()) return;
                        subject.clear();
                        mtop = 0;
                        relayout = true;
                    };
                    if ((e.ch == '\r' || e.ch == '\n' || e.ch == 'l' || e.fin == 'C') && list) {
                        subject = mv.subjects[msel];
                        mtop = 0;
                        relayout = true;
                    } else if (e.ch == 27 || e.ch == 'h' || e.fin == 'D') back();
                    else if (e.ch == 'j' || e.fin == 'B') move(1);
                    else if (e.ch == 'k' || e.fin == 'A') move(-1);
                    else if (e.ch == 'g' || e.fin == 'H') (list ? msel : mtop) = 0;
                    else if (e.ch == 'G' || e.fin == 'F') move(1 << 20);
                    else if (e.fin == '~' && e.seq == "5") move(-5);
                    else if (e.fin == '~' && e.seq == "6") move(5);
                    else if (e.mouse && e.btn == 64) move(-1);
                    else if (e.mouse && e.btn == 65) move(1);
                    else if (e.mouse && e.btn == 0 && e.fin == 'M' && list) {
                        size_t li = mtop + (size_t)(e.my - 1);
                        if (li >= mv.subjects.size()) continue;
                        long long t = now_ms();
                        bool again = msel == li && t - click_at < 400;
                        msel = li;
                        click_at = again ? 0 : t;
                        if (again) {
                            subject = mv.subjects[msel];
                            mtop = 0;
                            relayout = true;
                        }
                    }
                } else if (mode == M_ABSENCE) {
                    auto move = [&](long d) {
                        long v = (long)atop + d;
                        atop = (size_t)(v < 0 ? 0 : v);
                    };
                    if (e.ch == 'j' || e.fin == 'B') move(1);
                    else if (e.ch == 'k' || e.fin == 'A') move(-1);
                    else if (e.ch == 'g' || e.fin == 'H') atop = 0;
                    else if (e.ch == 'G' || e.fin == 'F') move(1 << 20);
                    else if (e.fin == '~' && e.seq == "5") move(-5);
                    else if (e.fin == '~' && e.seq == "6") move(5);
                    else if (e.mouse && e.btn == 64) move(-1);
                    else if (e.mouse && e.btn == 65) move(1);
                } else {
                    size_t nd = tt.days.size(), nh = tt.hours.size();
                    auto mvd = [&](long dd, long dh) {
                        if (!nd || !nh) return;
                        long v = (long)cd + dd;
                        cd = (size_t)(v < 0 ? 0 : v >= (long)nd ? (long)nd - 1 : v);
                        long w = (long)chr + dh;
                        chr = (size_t)(w < 0 ? 0 : w >= (long)nh ? (long)nh - 1 : w);
                    };
                    if (e.ch == '\r' || e.ch == '\n' || e.ch == ' ')
                        pop = nd && nh && tt.at(cd, chr);
                    else if (e.ch == 'j' || e.fin == 'B') mvd(1, 0);
                    else if (e.ch == 'k' || e.fin == 'A') mvd(-1, 0);
                    else if (e.ch == 'l' || e.fin == 'C') mvd(0, 1);
                    else if (e.ch == 'h' || e.fin == 'D') mvd(0, -1);
                    else if (e.ch == 'g' || e.fin == 'H') cd = chr = 0;
                    else if (e.ch == '[') set_week(step(-7));
                    else if (e.ch == ']') set_week(step(7));
                    else if (e.ch == 'w') {
                        auto ws = weeks(tt.monday);
                        wsel = 0;
                        for (size_t i = 0; i < ws.size(); i++)
                            if (ws[i].first == tt.monday) wsel = i;
                        menu = true;
                    }
                    // the header line is the week control: arrows step, the label opens the menu
                    else if (e.mouse && e.btn == 0 && e.fin == 'M' &&
                             e.my == (long)geom.hdr + 1 && geom.prev) {
                        if ((size_t)e.mx == geom.prev) set_week(step(-7));
                        else if ((size_t)e.mx == geom.next) set_week(step(7));
                        else if ((size_t)e.mx >= geom.lbl0 && (size_t)e.mx <= geom.lbl1) {
                            auto ws = weeks(tt.monday);
                            wsel = 0;
                            for (size_t i = 0; i < ws.size(); i++)
                                if (ws[i].first == tt.monday) wsel = i;
                            menu = true;
                        }
                    }
                    else if (e.mouse && e.btn == 0 && e.fin == 'M' && nd && nh) {
                        long r = e.my - 1 - (long)geom.top, cx = e.mx - 1 - (long)(geom.gut + geom.left);
                        if (r < 0 || cx < 0) continue;
                        size_t d = (size_t)r / geom.blk, h = (size_t)cx / (geom.cw + 1);
                        if (d >= nd || h >= nh) continue;
                        long long t = now_ms();
                        bool again = d == cd && h == chr && t - click_at < 400;
                        cd = d;
                        chr = h;
                        click_at = again ? 0 : t;
                        if (again) pop = tt.at(cd, chr) != nullptr;
                    }
                }
            }
            if (quit) break;
        }
        // the marks tab was open at some point: only now do they count as seen
        if (marks_seen) view::marks_rows(store, {}, 0);
        return 0;
    } catch (const std::exception &e) {
        leave();
        fprintf(stderr, TUI_NAME ": %s\n", e.what());
        return 1;
    }
}

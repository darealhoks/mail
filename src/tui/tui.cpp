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

enum Mode { M_FEED, M_MARKS, M_TABLE, M_N };
const char *MODE_NAME[M_N] = {"feed", "marks", "table"};

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

// [key] section: "<char> = feed|marks|table"; unset falls back to f/m/t
std::map<char, Mode> keymap() {
    std::map<char, Mode> k{{'f', M_FEED}, {'m', M_MARKS}, {'t', M_TABLE}};
    for (const auto &[key, val] : config().v) {
        if (key.compare(0, 4, "key.") || key.size() != 5) continue;
        for (int m = 0; m < M_N; m++)
            if (val == MODE_NAME[m] || (m == M_TABLE && val == "timetable")) k[key[4]] = (Mode)m;
    }
    return k;
}

// mode chip per tab, hotkey underlined except on the active one; hits gets each tab's columns
std::string tabs(Mode mode, const std::map<char, Mode> &keys, size_t &width_used,
                 std::vector<std::pair<size_t, size_t>> &hits) {
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

// the health chip: a dead session first, then a failing fetch, then plain data age
std::string age_chip(Store &s, bool &red, long long &best) {
    best = 0;
    red = false;
    std::string unsigned_names;
    for (const view::SourceStatus &st : view::status(s)) {
        if (st.fetched_at > best) best = st.fetched_at;
        if (st.stale && !st.offline) red = true;
        if (!st.signed_in || !st.error.empty())
            unsigned_names += (unsigned_names.empty() ? "" : ", ") + std::string(st.pretty);
    }
    if (!unsigned_names.empty()) {
        red = true;
        return unsigned_names + " not signed, press a to sign in";
    }
    for (const view::Gripe &g : view::gripes(s))
        if (g.error) {
            red = true;
            return g.text.size() > 60 ? g.text.substr(0, 59) + "…" : g.text;
        }
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

// the whole ui keeps this line; tab strip left, transient action in the middle, position right
std::string status(Mode mode, const std::map<char, Mode> &keys, const std::string &msg,
                   const std::string &age_in, bool age_red, const std::string &pos, size_t width,
                   std::vector<std::pair<size_t, size_t>> &hits) {
    std::string m = msg, age = age_in;
    size_t tw = 0;
    std::string strip = tabs(mode, keys, tw, hits);
    // narrow terminal: drop the transient message rather than let the bar wrap onto the feed
    for (;;) {
        // +1 for the space before the gap, +1 for the pad that keeps the chip off the last column
        size_t used = tw + (m.empty() ? 0 : utf8_len(m) + 1) + 1 + utf8_len(age) + 1 +
                      utf8_len(pos) + 2 + 1;
        if (used > width && m.empty() && utf8_len(age) > 4) {  // then trim the chip itself
            age = fit(age, utf8_len(age) - 1);
            continue;
        }
        if (used <= width || m.empty()) {
            std::string gap(used < width ? width - used : 0, ' ');
            // every segment repaints the fill: c() resets at each segment end
            std::string fill = bar_bg();
            std::string sep = fill.empty() ? "" : ";" + fill;
            return strip + c(("90" + sep).c_str(), (m.empty() ? "" : " " + m) + " " + gap) +
                   c(((age_red ? "1;31" : "90") + sep).c_str(), age + " ") +
                   c(("1;" + std::string(accent()) + sep).c_str(), " " + pos + " ") +
                   c(("90" + sep).c_str(), " ");
        }
        m.clear();
    }
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

// centred box over the frame; drawn with absolute cursor moves so nothing has to be spliced
std::string popup(const Lesson &l, int rows, int cols) {
    std::vector<std::string> body;
    std::string name = l.subject_name.empty() ? l.subject : l.subject_name;
    if (!l.begins.empty()) body.push_back(l.begins + "–" + l.ends);
    if (!l.room.empty()) body.push_back("room " + l.room);
    if (!l.teacher.empty()) body.push_back(l.teacher);
    body.push_back(paint::date_short(l.date) + "  hour " + l.hour);
    if (l.state == "x") body.push_back("cancelled");
    if (l.state == "!") body.push_back("changed");
    size_t w = utf8_len(name);
    for (const auto &b : body) w = std::max(w, utf8_len(b));
    if (w > (size_t)cols - 6) w = (size_t)cols - 6;
    size_t h = body.size() + 4;
    size_t r0 = (size_t)rows > h ? ((size_t)rows - h) / 2 + 1 : 1;
    size_t c0 = (size_t)cols > w + 4 ? ((size_t)cols - w - 4) / 2 + 1 : 1;
    std::string ac = accent();
    auto at = [&](size_t r) { return "\033[" + std::to_string(r0 + r) + ";" +
                                     std::to_string(c0) + "H"; };
    std::string out = at(0) + c(ac.c_str(), "┌");
    for (size_t i = 0; i < w + 2; i++) out += c(ac.c_str(), "─");
    out += c(ac.c_str(), "┐");
    out += at(1) + c(ac.c_str(), "│") + " " + c("1", plain_cut(name, w)) + " " +
           c(ac.c_str(), "│");
    for (size_t i = 0; i < body.size(); i++)
        out += at(2 + i) + c(ac.c_str(), "│") + " " + c("39", plain_cut(body[i], w)) + " " +
               c(ac.c_str(), "│");
    out += at(2 + body.size()) + c(ac.c_str(), "│") + " " + std::string(w, ' ') + " " +
           c(ac.c_str(), "│");
    out += at(3 + body.size()) + c(ac.c_str(), "└");
    for (size_t i = 0; i < w + 2; i++) out += c(ac.c_str(), "─");
    out += c(ac.c_str(), "┘");
    return out;
}

// paint::term_cols() caps at 100 for pipes; the tui owns the whole screen so it uses the real size
void term_size(int &rows, int &cols) {
    struct winsize w {};
    if (ioctl(1, TIOCGWINSZ, &w) != 0) w = {24, 80, 0, 0};
    rows = w.ws_row > 2 ? w.ws_row : 3;
    cols = w.ws_col > 20 ? w.ws_col : 20;
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
    if (k['f'] != M_FEED || k['m'] != M_MARKS || k['x'] != M_TABLE || k.count('z')) {
        fputs("selfcheck failed: keymap\n", stderr);
        return 1;
    }
    size_t w = 0;
    std::vector<std::pair<size_t, size_t>> hits;
    tabs(M_MARKS, k, w, hits);
    if (w != 6 + 7 + 7 || hits[0].first != 1 || hits[1].first != 7 || hits[2].second != 20) {
        fputs("selfcheck failed: tab hit columns\n", stderr);
        return 1;
    }

    view::Timetable tt;
    tt.monday = "2026-08-17";
    tt.days = {"2026-08-17", "2026-08-18"};
    tt.hours = {"1", "2"};
    tt.rows = {{"bakalari", "2026-08-17", "1", "CJL", "Cestina", "214", "Novak", "", "8:00", "8:45"}};
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
        std::vector<std::pair<size_t, size_t>> tab_hits(M_N, {0, 0});

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

        view::Timetable tt;
        bool tt_ok = false, tt_first = true;
        size_t cd = 0, chr = 0;
        bool pop = false;
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
                    posts = feed_posts(view::feed_rows(store, {},
                                                      (size_t)config().num("general.limit")),
                                       (size_t)cols - 2, false);
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
                        msnap = view::marks_rows(store, {}, 0, false);
                        msnap_ok = true;
                        marks_seen = true;
                    }
                    mv = build_marks(msnap, subject, (size_t)cols - 2);
                    if (msel >= mv.lines.size()) msel = mv.lines.empty() ? 0 : mv.lines.size() - 1;
                } else {
                    if (!tt_ok) {
                        tt = view::timetable(store);
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
            if (mode == M_FEED) {
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
            out += status(mode, keys, msg, age, age_red, pos, (size_t)cols, tab_hits);
            if (mode == M_TABLE && pop && !tt.grid.empty() && tt.at(cd, chr))
                out += popup(*tt.at(cd, chr), rows, cols);
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
                    msnap_ok = tt_ok = false;
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
                if (pop) {  // any key dismisses the lesson popup, and does nothing else
                    pop = false;
                    pending.clear();
                    break;
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

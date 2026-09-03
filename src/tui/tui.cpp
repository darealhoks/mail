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
const char *MODE_NAME[M_N] = {"feed", "marks", "table", "absence"};  // matches config.cpp key_modes()

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

std::map<char, Mode> keymap() {
    std::map<char, Mode> k;
    for (const auto &[ch, name] : key_modes())
        for (int m = 0; m < M_N; m++)
            if (name == MODE_NAME[m]) k[ch] = (Mode)m;
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
        // no colour: brackets are the only mark of the active tab. same width as the pad spaces
        std::string lp = " ", rp = " ";
        if (m == mode && !color_on()) lp = "[", rp = "]";
        if (p == std::string::npos)
            out += c(base.c_str(), lp + name + rp);
        else
            out += c(base.c_str(), lp + name.substr(0, p)) +
                   c((base + ";4").c_str(), name.substr(p, 1)) +
                   c(base.c_str(), name.substr(p + 1) + rp);
        width_used += name.size() + 2;
    }
    return out;
}

// the health chip: a dead session first, then a stale source, then plain data age.
// a failing fetch is a gripe, and gripes live in the statusline
std::string age_chip(const view::Health &h, bool &red, long long &best) {
    best = 0;
    red = false;
    std::string stale_names;
    for (const view::SourceStatus &st : h.sources) {
        if (st.fetched_at > best) best = st.fetched_at;
        if (st.stale && !h.offline)
            stale_names += (stale_names.empty() ? "" : ", ") + st.pretty;
    }
    if (!h.unsigned_names.empty()) {
        red = true;
        std::string names;
        for (const std::string &n : h.unsigned_names) names += (names.empty() ? "" : ", ") + n;
        return names + " not signed, a to sign in";
    }
    if (!stale_names.empty()) return stale_names + " stale";
    if (!best) return "no data yet";
    return view::rel_span((long long)time(nullptr) - best) + " old";
}

bool refresh() {
    // the trailing & makes system() succeed whatever the daemon does, so look for it first
    if (system("command -v " APP_NAME "d >/dev/null 2>&1") != 0) return false;
    return system(APP_NAME "d >/dev/null 2>&1 &") == 0;
}

// the whole ui keeps this line; tab strip left, transient action in the middle, position right
std::string status(Mode mode, const std::map<char, Mode> &keys, const std::string &msg,
                   const std::string &brief, const char *msg_col,
                   const std::vector<std::string> &filters, const std::string &age_in,
                   bool age_red, const std::string &pos, size_t width,
                   std::vector<std::pair<size_t, size_t>> &hits,
                   std::vector<std::pair<size_t, size_t>> &chips) {
    std::string m = msg, age = age_in;
    size_t tw = 0;
    std::string strip, fill = bar_bg(), sep = fill.empty() ? "" : ";" + fill;
    auto build = [&](bool compact) {
        strip = tabs(mode, keys, tw, hits, compact);
        chips.assign(filters.size(), {0, 0});
        if (filters.empty()) return;
        strip += c(("90" + sep).c_str(), "/");
        tw += 1;
        for (size_t i = 0; i < filters.size(); i++) {  // click one word to drop it
            std::string w = (i ? " " : "") + filters[i];
            chips[i] = {tw + 1, tw + utf8_len(w)};
            strip += c(("39" + sep).c_str(), w);
            tw += utf8_len(w);
        }
    };
    build(false);
    // narrow terminal: fall back to the short form of the message, then shed the tab names, then
    // drop the message. the message shortens first so a long gripe does not cost the tab names.
    // the age chip outlives all of it: staleness is never hidden
    size_t used;
    // each step runs at most once: clearing m must not fall back into re-setting it from brief
    for (bool briefed = false, compact = false;;) {
        // +1 for the space before the gap, +1 for the pad that keeps the chip off the last column
        used = tw + (m.empty() ? 0 : utf8_len(m) + 1) + 1 + utf8_len(age) + 1 + utf8_len(pos) + 3;
        if (used <= width) break;
        if (!briefed) briefed = true, m = brief.empty() ? m : brief;
        else if (!compact) build(compact = true);
        else if (!m.empty()) m.clear();
        else break;
    }
    std::string gap(used < width ? width - used : 0, ' ');
    // every segment repaints the fill: c() resets at each segment end
    std::string msg_sgr = m.empty() ? "90" : msg_col;
    return fit(strip + c((msg_sgr + sep).c_str(), (m.empty() ? "" : " " + m) + " " + gap) +
                   c(((age_red ? "1;31" : "90") + sep).c_str(), age + " ") +
                   c((std::string(accent()) + sep).c_str(), " " + pos + " ") +
                   c(("90" + sep).c_str(), " "),
               width);
}

// a screen of its own, not a box over the frame: title, two colour-separated columns, and a
// footer that blocks until a key. no state, no dismiss branch — it is a call, not a mode
using Row = std::pair<std::string, std::string>;

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

// a page opened by a double click gets that click's release next: mouse reports are not keys
void wait_key() {
    std::string b;
    Ev e;
    for (char buf[64];;) {
        ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) return;
        b.append(buf, (size_t)n);
        while (next_ev(b, e, true))
            if (!e.mouse) return;
    }
}

void show_page(const std::string &title, const std::vector<Row> &rows, int trows, int tcols) {
    size_t kw = 0;
    for (const auto &[k, v] : rows) kw = std::max(kw, utf8_len(k));
    std::string out = "\033[H\033[2J  " + c("90", "# ") + c("1", title) + "\r\n\r\n";
    int left = trows - 3;  // title, its blank line, and the footer
    for (const auto &[k, v] : rows) {
        if (left-- <= 0) break;
        if (k.empty() && v.empty()) {
            out += "\r\n";
            continue;
        }
        out += fit("  " + c("90", plain_cut(k, kw)) + "  " + c("39", v), (size_t)tcols) +
               "\r\n";
    }
    out += "\033[" + std::to_string(trows) + ";1H" + c("90", "  press any key to return…");
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
    wait_key();
}

// a screen of its own like show_page, but a pick: `permanent` on top, then the weeks around
// now, dimmed when nothing is stored for them (the grid falls back to permanent there). returns
// the monday chosen, empty on esc
std::string week_page(Store &store, const std::string &cur_mon, int trows, int tcols) {
    std::string base = view::wanted_monday();
    std::vector<std::string> mons{PERM_MONDAY}, labs{"permanent"};
    std::vector<bool> stored{true};
    for (int w = -6; w <= 10; w++) {
        std::string m = view::ymd_plus(base, 7 * w), su = view::ymd_plus(m, 6);
        mons.push_back(m);
        labs.push_back(date_short(m) + " – " + date_short(su) + (m == base ? "  now" : ""));
        stored.push_back(!store.lessons(m, su).empty());
    }
    size_t sel = 0, shown = std::min(mons.size(), (size_t)std::max(trows - 3, 0));
    for (size_t i = 0; i < mons.size(); i++)
        if (mons[i] == cur_mon) sel = i;
    std::string b;
    Ev e;
    for (;;) {
        std::string out = "\033[H\033[2J  " + c("90", "# ") + c("1", "week") + "\r\n\r\n";
        for (size_t i = 0; i < shown; i++)
            out += fit((i == sel ? std::string("\033[") + accent() + "m▎\033[0m " : "  ") +
                           c(stored[i] ? "39" : "90", labs[i]),
                       (size_t)tcols) +
                   "\r\n";
        out += "\033[" + std::to_string(trows) + ";1H" + c("90", "  enter picks, esc returns");
        fwrite(out.data(), 1, out.size(), stdout);
        fflush(stdout);
        for (char buf[64]; !next_ev(b, e, true);) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) return "";
            b.append(buf, (size_t)n);
        }
        if (e.ch == 27 || e.ch == 'q') return "";
        if (e.ch == '\r' || e.ch == '\n' || e.ch == ' ') return mons[sel];
        if (e.ch == 'j' || e.fin == 'B' || (e.mouse && e.btn == 65)) sel = std::min(sel + 1, shown - 1);
        else if (e.ch == 'k' || e.fin == 'A' || (e.mouse && e.btn == 64)) sel = sel ? sel - 1 : 0;
        else if (e.ch == 'g') sel = 0;
        else if (e.ch == 'G') sel = shown - 1;
        else if (e.mouse && e.btn == 0 && e.fin == 'M' && e.my >= 3 && (size_t)(e.my - 3) < shown)
            return mons[(size_t)(e.my - 3)];
    }
}

std::vector<Row> key_rows(const std::map<char, Mode> &keys) {
    std::string modes;
    for (int m = 0; m < M_N; m++)
        for (const auto &[ch, md] : keys)
            if (md == m)
                modes += (modes.empty() ? "" : "  ") + std::string(1, ch) + " " + MODE_NAME[m];
    return {{"tabs", modes},
            {"tab shift-tab", "next / previous tab"},
            {"", ""},
            {"j k \u2193 \u2191", "next post, or next paragraph of one taller than the screen"},
            {"space ^d ^u", "half a screen down / up"},
            {"pgdn pgup", "a screen down / up"},
            {"g G home end", "first / last"},
            {"", ""},
            {"feed", "J K next / previous post, enter opens the link, X dismisses"},
            {"marks", "enter l \u2192 open the subject, h \u2190 esc back"},
            {"timetable", "h j k l move, enter the lesson in full"},
            {"", "[ ] week back / forward, p the permanent grid and back"},
            {"", "w (or a click on the week) picks a week from a list"},
            {"", ""},
            {"/ esc", "filter, then clear the filters"},
            {"? a r q", "these keys, sign in, fetch now, quit"}};
}

std::vector<Row> lesson_rows(const Lesson &l) {
    std::vector<Row> r;
    if (!l.begins.empty()) r.push_back({"time", hhmm(l.begins) + " – " + hhmm(l.ends)});
    if (!l.hour.empty()) r.push_back({"hour", l.hour});
    r.push_back({"date", date_short(l.date)});
    if (!l.subject.empty()) r.push_back({"subject", l.subject});
    if (!l.room.empty()) r.push_back({"room", l.room});
    const std::string &who = l.teacher_name.empty() ? l.teacher : l.teacher_name;
    if (!who.empty()) r.push_back({"teacher", who});
    // a changed lesson says what the change is; the grid keeps showing what it should have been
    if (!l.state.empty()) {
        std::string what = l.change;
        if (what.empty()) what = l.state == "x" ? "cancelled" : "changed";
        r.push_back({"", ""});
        r.push_back({l.state == "x" ? "cancelled" : "changed", what});
    }
    return r;
}

// the interactive sign-ins want a real terminal, so this one drops out of the alt screen
void auth_page(const std::vector<std::string> &want) {
    leave();
    fputs("\033[H\033[2J", stdout);
    printf("%s\n\n", (c("90", "# ") + c("1", "sign in")).c_str());
    bool any = false;
    for (const Source &src : sources()) {
        if (std::find(want.begin(), want.end(), src.name) == want.end()) continue;
        any = true;
        try {
            src.login();
        } catch (const std::exception &e) {
            fprintf(stderr, "%s\n", c("1;31", e.what(), 2).c_str());
        }
        putchar('\n');
    }
    if (!any) printf("%s\n\n", c("90", "every source is already signed in").c_str());
    printf("%s", c("90", "press enter to return…").c_str());
    fflush(stdout);
    char b[16];
    if (!fgets(b, sizeof b, stdin)) {}
    enter();
}

// `lo`..`hi` is the cursor's line span. the feed puts its stop on the top line, so it must be
// allowed to scroll past the end or the last posts (or any post at all, when the feed fits the
// screen) are unreachable
void pane(std::string &out, const std::vector<std::string> &lines, size_t &top, size_t lo,
          size_t hi, bool gutter, bool overscroll, int rows, int cols, int pad = 0) {
    size_t max_top = lines.size() > (size_t)rows ? lines.size() - (size_t)rows : 0;
    if (overscroll) max_top = lines.empty() ? 0 : lines.size() - 1;
    if (top > max_top) top = max_top;
    for (int r = 0; r < rows; r++) {
        size_t li = top + (size_t)r;
        if (li < lines.size()) {
            out.append((size_t)pad, ' ');
            if (gutter)
                out += li >= lo && li < hi ? std::string("\033[") + accent() + "m▎\033[0m " : "  ";
            out += fit(lines[li], (size_t)(cols - pad) - (gutter ? 2 : 0));
        }
        out += "\033[K";
        if (r < rows - 1) out += "\r\n";
    }
    out += "\033[J\r\n";
}

// j/k/space/^d/^u/pgup/pgdn/wheel, identical in every pane. g/G and enter stay with the caller
bool scroll_key(const Ev &e, long half, long page, const std::function<void(long)> &move) {
    if (e.ch == 'j' || e.fin == 'B') move(1);
    else if (e.ch == 'k' || e.fin == 'A') move(-1);
    else if (e.ch == ' ' || e.ch == 4) move(half);
    else if (e.ch == 21) move(-half);
    else if (e.fin == '~' && e.seq == "5") move(-page);
    else if (e.fin == '~' && e.seq == "6") move(page);
    else if (e.mouse && e.btn == 64) move(-1);
    else if (e.mouse && e.btn == 65) move(1);
    else return false;
    return true;
}

size_t step(size_t cur, long d, size_t hi) {
    long v = (long)cur + d;
    return (size_t)(v < 0 ? 0 : v > (long)hi ? (long)hi : v);
}

}  // namespace

int main(int argc, char **) {
    if (argc > 1) {
        fprintf(stderr, "usage: %s\n", TUI_NAME);
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
        int rows = 24, cols = 80, pw = 68, lead = 0;
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
        // where j/k land: a post that fits the screen is one stop (and paints centred); a longer
        // one stops at each paragraph, and every `rows` lines inside a paragraph taller than that
        std::vector<size_t> stops;
        size_t cur = 0, top = 0;  // flat line of the current stop; line last painted at row 0

        view::Marks msnap;
        bool msnap_ok = false, marks_seen = false;
        std::string subject;
        std::vector<std::string> mlines, msubjects;
        size_t msel = 0, mtop = 0;

        view::Absences absnap;
        std::vector<std::string> alines;
        bool abs_ok = false;
        size_t atop = 0;

        view::Timetable tt;
        bool tt_ok = false, tt_first = true;
        std::string tt_mon;  // empty = the week view::timetable picks on its own
        size_t cd = 0, chr = 0;
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
                pw = std::min(cols - 2, 68);  // 68 = the prose cap in paint::term_cols
                lead = (cols - pw - 2) / 2;   // 2 = pane's gutter; the block centres like the grid
                relayout = true;
            }
            if (relayout) {
                relayout = false;
                if (mode == M_FEED) {
                    fsnap = view::feed_rows(store, filters,
                                            (size_t)config().num("general.limit"));
                    posts = feed_posts(fsnap, (size_t)pw);
                    flat.clear();
                    owner.clear();
                    start.clear();
                    stops.clear();
                    for (size_t p = 0; p < posts.size(); p++) {
                        start.push_back(flat.size());
                        const auto &pl = posts[p].lines;
                        bool fits = pl.size() <= (size_t)rows;
                        size_t last = flat.size();
                        for (size_t i = 0; i < pl.size(); i++) {
                            bool para = i == 0 || (i + 1 < pl.size() && pl[i - 1].empty());
                            if (i == 0 || (!fits && i + 1 < pl.size() &&
                                           (para || flat.size() - last >= (size_t)rows))) {
                                stops.push_back(flat.size());
                                last = flat.size();
                            }
                            flat.push_back(fit(pl[i], (size_t)pw));
                            owner.push_back(p);
                        }
                    }
                    // the feed ends with the most urgent items, so it opens at the bottom
                    if (first) {
                        first = false;
                        cur = stops.empty() ? 0 : stops.back();
                    }
                    auto s = std::upper_bound(stops.begin(), stops.end(), cur);
                    cur = s == stops.begin() ? 0 : *(s - 1);
                } else if (mode == M_MARKS) {
                    if (!msnap_ok) {
                        // never consumed here: a live reload would wipe the chips off rows the
                        // user has not looked at yet. the watermark moves on the way out
                        msnap = view::marks_rows(store, filters, 0, false);
                        msnap_ok = true;
                        marks_seen = true;
                    }
                    msubjects.clear();
                    if (subject.empty()) {
                        mlines = mark_subject_lines(msnap, msubjects);
                    } else {
                        view::Marks one;
                        for (const auto &r : msnap.rows)
                            if (r.klass == subject) one.rows.push_back(r);
                        for (const auto &a : msnap.averages)
                            if (std::get<0>(a) == subject) one.averages.push_back(a);
                        mlines = mark_lines(one, (size_t)pw);
                    }
                    if (mlines.empty()) mlines.push_back(c("90", "no marks"));
                    if (msel >= msubjects.size()) msel = msubjects.empty() ? 0 : msubjects.size() - 1;
                } else if (mode == M_ABSENCE) {
                    if (!abs_ok) {
                        absnap = view::absence_rows(store, filters);
                        alines = absence_lines(absnap);
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

            size_t sel = flat.empty() || cur >= owner.size() ? 0 : owner[cur];
            std::string out = "\033[H", pos;
            if (mode == M_FEED) {
                pos = std::to_string(posts.empty() ? 0 : sel + 1) + "/" +
                      std::to_string(posts.size());
                if (flat.empty())
                    out += c("90", "nothing to show") + "\033[K\033[J\033[" +
                           std::to_string(rows + 1) + ";1H";
                else {
                    size_t len = posts[sel].lines.size();
                    top = cur;
                    if (len <= (size_t)rows) {  // fits: centre the whole post
                        size_t pad = ((size_t)rows - len) / 2;
                        top = start[sel] > pad ? start[sel] - pad : 0;
                    }
                    pane(out, flat, top, start[sel], start[sel] + len, true, true, rows, cols, lead);
                }
            } else if (mode == M_MARKS) {
                bool list = subject.empty() && !msubjects.empty();
                pos = subject.empty() ? std::to_string(msubjects.size()) + " subjects" : subject;
                if (list) {
                    if (msel < mtop) mtop = msel;
                    if (msel >= mtop + (size_t)rows) mtop = msel - (size_t)rows + 1;
                }
                pane(out, mlines, mtop, list ? msel : 0, list ? msel + 1 : 0, true, false, rows,
                     cols, lead);
            } else if (mode == M_ABSENCE) {
                pos = std::to_string(absnap.rows.size()) + " subjects";
                pane(out, alines, atop, 0, 0, true, false, rows, cols, lead);
            } else {
                tlines = grid_lines(tt, cd, chr, rows, cols, geom);
                pos = tt.monday == PERM_MONDAY
                          ? "permanent"
                          : date_short(tt.days.empty() ? tt.monday : tt.days[cd]);
                size_t zero = 0;
                pane(out, tlines, zero, 0, 0, false, false, rows, cols);
            }

            bool age_red = false;
            view::Health health = view::health(store);
            std::string age = age_chip(health, age_red, fetched_best);
            if (refresh_since) {  // give up on the spinner if no fetch lands
                if (fetched_best > refresh_base || now_ms() - refresh_since > 120000) {
                    refresh_since = 0;
                    msg = fetched_best > refresh_base ? "refreshed" : "refresh timed out";
                    msg_at = now_ms();
                } else {
                    msg = "refreshing" +
                          std::string(1 + (size_t)((now_ms() - refresh_since) / 400) % 3, '.');
                    msg_at = 0;
                }
            }
            // a gripe is a condition, not a toast: it holds until it clears, but any transient
            // message wins the line while it lives
            std::string line = msg, brief = msg;
            const char *msg_col = "90";
            if (line.empty() && !fsnap.bad_filter.empty()) {
                line = brief = "no such filter '" + fsnap.bad_filter + "'";
                msg_col = "1;31";
            }
            if (line.empty())
                for (const view::Gripe &g : health.gripes) {
                    line = g.text;
                    brief = g.brief;
                    msg_col = g.error ? "1;31" : "1;33";
                    if (g.error) break;
                }
            if (fmode)
                out += c(accent_bold(), "/") + fbuf + "\033[7m \033[0m\033[K";
            else
                // the grid is unfiltered, so no chips there: view::timetable never sees filters
                out += status(mode, keys, line, brief, msg_col,
                              mode == M_TABLE ? std::vector<std::string>{} : filters, age, age_red,
                              pos, (size_t)cols, tab_hits, chip_hits);
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
                            if (ev->len &&
                                strncmp(ev->name, APP_NAME ".db", sizeof(APP_NAME ".db") - 1) == 0)
                                wrote = true;
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
                    cd = chr = 0;
                    relayout = true;
                };
                // stepping off the permanent grid lands on the real weeks, not on 1970
                auto week_step = [&](int d) {
                    return view::ymd_plus(
                        tt.monday == PERM_MONDAY ? view::wanted_monday() : tt.monday, d);
                };
                auto refilter = [&] {
                    fsnap.bad_filter.clear();  // marks mode never rebuilds fsnap
                    msnap_ok = abs_ok = false;
                    relayout = true;
                };
                auto after_page = [&] {
                    resized = 1;  // the page owned the screen: repaint all of it
                    pending.clear();
                };
                if (fmode) {
                    if (e.ch == 27) fmode = false;
                    else if (e.ch == '\r' || e.ch == '\n') {
                        fmode = false;
                        std::vector<std::string> words;
                        for (size_t i = 0; i < fbuf.size();) {
                            size_t sp = fbuf.find(' ', i);
                            if (sp == std::string::npos) sp = fbuf.size();
                            if (sp > i) words.push_back(fbuf.substr(i, sp - i));
                            i = sp + 1;
                        }
                        filters = view::fold_all(words);
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
                    relayout = true;
                };
                if (e.ch == 3 || e.ch == 'q') {
                    quit = true;
                    break;
                }
                if (e.ch == 27 && !filters.empty()) {
                    filters.clear();
                    refilter();
                    continue;
                }
                if (e.ch == '/') {
                    fmode = true;
                    fbuf.clear();
                    for (const std::string &f : filters) fbuf += (fbuf.empty() ? "" : " ") + f;
                    continue;
                }
                if (e.ch == '?') {
                    show_page("keys", key_rows(keys), rows + 1, cols);
                    after_page();
                    break;
                }
                if (e.ch && keys.count(e.ch)) {
                    go(keys[e.ch]);
                    continue;
                }
                if (e.ch == 'a') {
                    auth_page(health.unsigned_names);
                    after_page();
                    break;
                }
                if (e.ch == 'r') {
                    if (!refresh()) {
                        msg = "no " APP_NAME "d on PATH";
                        msg_at = now_ms();
                        continue;
                    }
                    refresh_since = now_ms();
                    refresh_base = fetched_best;
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
                auto pick_week = [&] {
                    std::string m = week_page(store, tt.monday, rows + 1, cols);
                    if (!m.empty()) set_week(m);
                    after_page();
                };
                // a click on the tab strip switches mode wherever you are; on the week label, a
                // week picker
                if (e.mouse && e.btn == 0 && e.fin == 'M' && e.my == rows + 1) {
                    if (mode == M_TABLE && (size_t)e.mx + utf8_len(pos) + 2 >= (size_t)cols) {
                        pick_week();
                        break;
                    }
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

                long half = rows > 1 ? rows / 2 : 1, page = rows > 1 ? rows : 1;
                if (mode == M_FEED) {
                    // d lines forward (back), then on to the next (previous) stop
                    auto move = [&](long d) {
                        if (stops.empty()) return;
                        long want = (long)cur + d;
                        if (d > 0) {
                            auto s = std::lower_bound(stops.begin(), stops.end(), (size_t)want);
                            while (s != stops.end() && *s <= cur) ++s;
                            cur = s == stops.end() ? stops.back() : *s;
                        } else {
                            auto s = std::upper_bound(stops.begin(), stops.end(),
                                                      (size_t)(want < 0 ? 0 : want));
                            while (s != stops.begin() && *(s - 1) >= cur) --s;
                            cur = s == stops.begin() ? stops.front() : *(s - 1);
                        }
                    };
                    auto open_sel = [&] {
                        if (posts.empty()) return;
                        msg = posts[sel].url.empty()          ? "no link"
                              : open_url(posts[sel].url) == 0 ? "opened in browser"
                                                              : "open failed";
                        msg_at = now_ms();
                    };
                    if (e.ch == '\r' || e.ch == '\n') open_sel();
                    else if (e.ch == 'X' && !posts.empty()) {
                        store.dismiss({fsnap.items[fsnap.rows[sel].n - 1].id});
                        msg = "dismissed";
                        msg_at = now_ms();
                        relayout = true;
                    }
                    // whole posts, for a feed of long mails that would take a page of j each
                    else if (e.ch == 'J' && sel + 1 < posts.size()) cur = start[sel + 1];
                    else if (e.ch == 'K') cur = sel ? start[sel - 1] : 0;
                    else if (e.ch == 'g' || e.fin == 'H') cur = 0;
                    else if (e.ch == 'G' || e.fin == 'F') cur = stops.empty() ? 0 : stops.back();
                    else if (scroll_key(e, half, page, move)) {}
                    else if (e.mouse && e.btn == 0 && e.fin == 'M') {
                        size_t li = top + (size_t)(e.my - 1);
                        if (li >= flat.size()) continue;
                        // feed_posts puts every url on a line of its own: click it, open it
                        std::string u = trim(strip_sgr(flat[li]));
                        if (u.compare(0, 4, "http") == 0 && u.find(' ') == std::string::npos) {
                            msg = open_url(u) == 0 ? "opened in browser" : "open failed";
                            msg_at = now_ms();
                            continue;
                        }
                        cur = start[owner[li]];
                        long long t = now_ms();
                        if (click_at && t - click_at < 400) {
                            open_sel();
                            click_at = 0;
                        } else click_at = t;
                    }
                } else if (mode == M_MARKS) {
                    bool list = subject.empty() && !msubjects.empty();
                    auto move = [&](long d) {
                        if (list) msel = step(msel, d, msubjects.size() - 1);
                        else mtop = step(mtop, d, mlines.size());
                    };
                    if ((e.ch == '\r' || e.ch == '\n' || e.ch == 'l' || e.fin == 'C') && list) {
                        subject = msubjects[msel];
                        mtop = 0;
                        relayout = true;
                    } else if (e.ch == 27 || e.ch == 'h' || e.fin == 'D') {
                        if (subject.empty()) continue;
                        subject.clear();
                        mtop = 0;
                        relayout = true;
                    } else if (e.ch == 'g' || e.fin == 'H') (list ? msel : mtop) = 0;
                    else if (e.ch == 'G' || e.fin == 'F') move(1 << 20);
                    else if (scroll_key(e, half, page, move)) {}
                    else if (e.mouse && e.btn == 0 && e.fin == 'M' && list) {
                        size_t li = mtop + (size_t)(e.my - 1);
                        if (li >= msubjects.size()) continue;
                        long long t = now_ms();
                        bool again = msel == li && t - click_at < 400;
                        msel = li;
                        click_at = again ? 0 : t;
                        if (again) {
                            subject = msubjects[msel];
                            mtop = 0;
                            relayout = true;
                        }
                    }
                } else if (mode == M_ABSENCE) {
                    auto move = [&](long d) { atop = step(atop, d, alines.size()); };
                    if (e.ch == 'g' || e.fin == 'H') atop = 0;
                    else if (e.ch == 'G' || e.fin == 'F') move(1 << 20);
                    else scroll_key(e, half, page, move);
                } else {
                    size_t nd = tt.days.size(), nh = tt.hours.size();
                    auto mvd = [&](long dd, long dh) {
                        if (!nd || !nh) return;
                        cd = step(cd, dd, nd - 1);
                        chr = step(chr, dh, nh - 1);
                    };
                    auto detail = [&] {
                        if (nd && nh && tt.at(cd, chr)) {
                            const Lesson &l = *tt.at(cd, chr);
                            show_page(l.subject_name.empty() ? l.subject : l.subject_name,
                                      lesson_rows(l), rows + 1, cols);
                            after_page();
                        }
                    };
                    if (e.ch == '\r' || e.ch == '\n' || e.ch == ' ') {
                        detail();
                        break;
                    }
                    else if (e.ch == 'j' || e.fin == 'B') mvd(1, 0);
                    else if (e.ch == 'k' || e.fin == 'A') mvd(-1, 0);
                    else if (e.ch == 'l' || e.fin == 'C') mvd(0, 1);
                    else if (e.ch == 'h' || e.fin == 'D') mvd(0, -1);
                    else if (e.ch == 'g' || e.fin == 'H') cd = chr = 0;
                    else if (e.ch == 'G' || e.fin == 'F') mvd((long)nd, 0);
                    else if (e.fin == '~' && e.seq == "5")
                        mvd(-(geom.blk ? std::max<long>(1, rows / (long)geom.blk) : 1), 0);
                    else if (e.fin == '~' && e.seq == "6")
                        mvd(geom.blk ? std::max<long>(1, rows / (long)geom.blk) : 1, 0);
                    else if (e.ch == '[') set_week(week_step(-7));
                    else if (e.ch == ']') set_week(week_step(7));
                    else if (e.ch == 'p')
                        set_week(tt.monday == PERM_MONDAY ? view::wanted_monday() : PERM_MONDAY);
                    else if (e.ch == 'w') {
                        pick_week();
                        break;
                    }
                    else if (e.mouse && e.btn == 0 && e.fin == 'M' &&
                             (size_t)(e.my - 1) == geom.hdr && e.mx > (long)geom.left) {
                        size_t cx = (size_t)e.mx - 1 - geom.left;  // "< label >"
                        if (cx < 2) set_week(week_step(-7));
                        else if (cx < 2 + geom.lab) {
                            pick_week();
                            break;
                        } else if (cx <= 3 + geom.lab) set_week(week_step(7));
                    } else if (e.mouse && e.btn == 0 && e.fin == 'M' && nd && nh) {
                        long r = e.my - 1 - (long)geom.top,
                             cx = e.mx - 1 - (long)(geom.gut + geom.left);
                        if (r < 0 || cx < 0) continue;
                        size_t d = (size_t)r / geom.blk, h = (size_t)cx / (geom.cw + 1);
                        if (d >= nd || h >= nh) continue;
                        long long t = now_ms();
                        bool again = d == cd && h == chr && t - click_at < 400;
                        cd = d;
                        chr = h;
                        click_at = again ? 0 : t;
                        if (again) {
                            detail();
                            break;
                        }
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

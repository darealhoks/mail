#include "paint.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/ioctl.h>
#include <unistd.h>

#include "classify.h"
#include "config.h"
#include "teams.h"
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
    std::string rel = view::rel_span(d < 0 ? -d : d);
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

std::string fit(const std::string &l, size_t width) {
    size_t vis = 0, i = 0, cut = 0;
    bool over = false;
    while (i < l.size()) {
        if (l[i] == 27) {  // csi: esc [ ... final byte 0x40-0x7e
            size_t j = i + 1;
            if (j < l.size() && l[j] == '[') {
                while (++j < l.size() && (l[j] < 0x40 || l[j] > 0x7e)) {}
            }
            i = j + 1;
            continue;
        }
        if ((unsigned char)l[i] >> 6 != 2) {
            if (vis == width) { over = true; break; }
            if (vis + 1 == width) cut = i;
            vis++;
        }
        i++;
    }
    if (!over) return l;
    return l.substr(0, cut) + "…\033[0m";
}

std::string hhmm(const std::string &t) {
    return t.size() > 1 && t[0] == '0' ? t.substr(1) : t;
}

int open_url(const std::string &url) {
    if (url.empty() || url.find('\'') != std::string::npos) return 1;
    std::string opener = config().str("general.browser");
    if (opener.empty()) opener = "xdg-open";
    return system((opener + " '" + url + "' >/dev/null 2>&1 &").c_str()) == 0 ? 0 : 1;
}

std::vector<Post> feed_posts(const view::Feed &f, size_t width, bool numbered) {
    std::string ac = std::string("1;") + accent();
    std::vector<Post> out;
    int bucket = -1;
    for (const view::FeedRow &r : f.rows) {
        const Item &i = f.items[r.n - 1];
        Post p;
        if (r.bucket != bucket) {
            bucket = r.bucket;
            p.lines.push_back(c("1;90", bucket == 0   ? "— no deadline —"
                                        : bucket == 1 ? "— upcoming —"
                                                      : "— overdue —"));
        }
        size_t chips = utf8_len(r.klass) + i.kind.size() + i.source.size() + 10;
        std::vector<std::string> title = wrap(i.title, width > chips + 30 ? width - chips : 30);
        p.lines.push_back((numbered ? c("90", std::to_string(r.n)) + " " : "") +
                          (r.is_new ? c(NEW_CHIP, " NEW ") + " " : "") +
                          c("1", title.empty() ? "" : title[0] + (title.size() > 1 ? "…" : "")) +
                          "  " + c(ac.c_str(), "<" + r.klass + ">") + " " +
                          c(kind_color(i.kind), "<" + i.kind + ">") + " " +
                          c("90", "<" + i.source + ">"));
        // teams posts have no subject line, so their title is the first slice of the body
        std::string rest = i.body.compare(0, i.title.size(), i.title) == 0
                               ? i.body.substr(i.title.size())
                               : i.body;
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '|' || rest[0] == '\n'))
            rest.erase(0, 1);
        // urls get their own rows so they survive un-elided and copy clean
        for (size_t at = rest.find("http"); at != std::string::npos; at = rest.find("http", at)) {
            size_t end = rest.find_first_of(" \n\t", at);
            if (end == std::string::npos) end = rest.size();
            while (end > at && strchr(",.;:!?)]\"", rest[end - 1])) end--;
            rest.insert(end, "\n");
            if (at) rest.insert(at, "\n"), end++;
            at = end + 1;
        }
        std::string link = std::string("4;") + accent();
        for (const auto &l : wrap(rest, width)) {
            if (l.compare(0, 4, "http") == 0 && l.find(' ') == std::string::npos)
                p.lines.push_back(c(link.c_str(), l));
            else p.lines.push_back(l == teams::TASK_NOTE  // set by teams.cpp, not a body line
                                       ? c("1;33", "<" + l + ">")
                                       : link_up(l));
        }
        if (!i.url.empty() && config().flag("general.links"))
            p.lines.push_back(c(link.c_str(), i.url));
        if (i.due_at) p.lines.push_back(c(due_color(i.due_at), when(i.due_at)));
        p.url = i.url;
        p.lines.push_back("");
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<std::string> mark_lines(const view::Marks &m, size_t width) {
    std::vector<std::string> out;
    size_t wc = 0, wm = 0, dw = 0;
    bool multi = false;
    for (const auto &r : m.rows) {
        multi |= r.klass != m.rows[0].klass;
        wc = std::max(wc, utf8_len(r.klass));
        wm = std::max(wm, utf8_len(r.mark));
        if (r.event_at) dw = std::max(dw, utf8_len(date_short(view::ymd_local(r.event_at))));
    }
    if (!multi) wc = 0;  // one subject: the column would repeat itself every row
    // the NEW chip, the gaps and the gutter the frontends keep to the left of every row
    size_t used = dw + wm + (wc ? wc + 2 : 0) + 12;
    std::string ac = std::string("1;") + accent();
    std::string period;
    for (const auto &r : m.rows) {
        if (r.period != period) {
            if (!period.empty()) out.push_back("");
            period = r.period;
            out.push_back(c("1;90", "— " + period + " —"));
        }
        std::string d = r.event_at ? date_short(view::ymd_local(r.event_at)) : "";
        d.append(dw - std::min(dw, utf8_len(d)), ' ');
        std::string note = plain_cut(r.note, width > used + 8 ? width - used : 8);
        while (!note.empty() && note.back() == ' ') note.pop_back();
        out.push_back((r.is_new ? c(NEW_CHIP, " NEW ") + " " : "") + c("90", d) + "  " +
                      (wc ? c(ac.c_str(), r.klass + std::string(wc - utf8_len(r.klass), ' ')) + "  "
                          : "") +
                      c(mark_color(r.mark), r.mark + std::string(wm - utf8_len(r.mark), ' ')) +
                      "  " + c("39", note));
    }
    for (const auto &[k, p, a] : m.averages) {
        out.push_back("");
        out.push_back(c("1;90", "average " + (multi ? k + " " : "") + p) + "  " +
                      c(avg_color(a), avg_str(a)));
    }
    return out;
}

std::vector<std::string> grid_lines(const view::Timetable &tt, size_t cd, size_t ch, int rows,
                                    int cols, Geom &g) {
    static const char *DAYNAME[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    std::string ac = std::string("1;") + accent();
    std::vector<std::string> out;
    g.nd = tt.days.size();
    g.nh = tt.hours.size();
    if (!g.nd || !g.nh) {
        out.push_back(c("90", "week ") + c(ac.c_str(), date_short(tt.monday)) +
                      c("90", " — no lessons"));
        return out;
    }
    // the widest cell content decides the width; vertical room decides how many lines a cell gets
    size_t need = 3;
    for (const Lesson *l : tt.grid) {
        if (!l) continue;
        need = std::max(need, utf8_len(l->subject));
        if (config().flag("table.room")) need = std::max(need, utf8_len(l->room));
        if (config().flag("table.teacher")) need = std::max(need, utf8_len(l->teacher));
    }
    TableLayout L = table_layout(tt, need + 2, (size_t)cols, g.gut);
    size_t cw = L.cw;
    size_t head = 2 + L.time_rows;
    bool fills = rows > 0;  // the tui owns a pane; the cli just prints the block and moves on
    size_t cell_rows = 1;
    if (fills) {
        size_t avail = (size_t)rows > head ? (size_t)rows - head : 1;
        size_t per_day = avail / g.nd;
        if (L.room && per_day >= 3) cell_rows = 2;
        if (L.teacher && cell_rows == 2 && per_day >= 4) cell_rows = 3;
    }
    int tier = (int)cell_rows - 1;
    g.cw = cw;
    g.blk = cell_rows + 1;  // the rule under each day belongs to its block

    auto centre = [&](const std::string &t) {
        size_t n = std::min(cw, utf8_len(t)), l = (cw - n) / 2;
        return std::string(l, ' ') + plain_cut(t, cw - l);
    };
    std::string bar = "│";
    std::string rule;
    for (size_t i = 0; i < g.gut; i++) rule += "─";
    for (size_t h = 0; h < g.nh; h++) {
        rule += "┼";
        for (size_t i = 0; i < cw; i++) rule += "─";
    }
    rule += "┼";

    std::vector<std::string> hd(head - 1, std::string(g.gut, ' '));
    for (size_t h = 0; h < g.nh; h++) {
        hd[0] += c("90", bar) + c("1", centre(tt.hours[h]));
        for (size_t r = 1; r < hd.size(); r++)
            hd[r] += c("90", bar) + c("90", centre(r == 1 ? L.t0[h] : L.t1[h]));
    }
    out.push_back(c("90", "week ") + c(ac.c_str(), date_short(tt.monday)));
    for (const auto &h : hd) out.push_back(h + c("90", bar));
    out.push_back(c("90", rule));
    g.top = out.size();

    std::string today = view::ymd_local((long long)time(nullptr));
    for (size_t d = 0; d < g.nd; d++) {
        struct tm tm {};
        time_t t = (time_t)classify::epoch(tt.days[d]);
        gmtime_r(&t, &tm);
        std::vector<std::string> block(cell_rows);
        for (size_t r = 0; r < cell_rows; r++)
            block[r] = (r == 0 ? c(tt.days[d] == today ? ac.c_str() : "90", DAYNAME[tm.tm_wday])
                               : std::string(2, ' ')) +
                       " ";
        for (size_t h = 0; h < g.nh; h++) {
            const Lesson *l = tt.at(d, h);
            std::vector<std::string> txt(cell_rows);
            if (l) {
                txt[0] = l->subject;
                if (cell_rows > 1) txt[1] = l->room;
                if (cell_rows > 2) txt[2] = l->teacher;
            }
            bool cur = d == cd && h == ch;
            std::string st = !l                ? "90"
                             : l->state == "x" ? "42;30"
                             : l->state == "!" ? "41;30"
                                               : "39";
            for (size_t r = 0; r < cell_rows; r++) {
                // the cursor is a bold cell with a trailing star, so it survives a colour-blind
                // terminal and still shows the state background
                bool star = cur && r == 0 && cw > 1;
                size_t avail = star ? cw - 1 : cw;  // the star only limits the text, never shifts it
                std::string sgr = (cur ? "1;" : "") + (r == 0 || !l ? st : std::string("90"));
                std::string body = txt[r], room;
                // one-line cells still carry the room, kept grey next to the subject
                if (r == 0 && tier == 0 && l && st == "39" && L.room &&
                    utf8_len(body) + 1 + utf8_len(l->room) <= avail)
                    room = l->room;
                size_t n = utf8_len(body) + (room.empty() ? 0 : 1 + utf8_len(room));
                if (n > avail) {
                    body = plain_cut(body, avail);
                    room.clear();
                    n = avail;
                }
                size_t lead = (cw - n) / 2, trail = cw - n - lead - (star ? 1 : 0);
                block[r] += c("90", bar) + c(sgr.c_str(), std::string(lead, ' ') + body) +
                            (room.empty() ? "" : c("90", " " + room)) +
                            c(sgr.c_str(), std::string(trail, ' ') + (star ? "*" : ""));
            }
        }
        for (auto &b : block) out.push_back(b + c("90", bar));
        out.push_back(c("90", rule));
    }
    // more columns than the terminal can hold: cut, never wrap — a wrap shears every row below
    for (auto &line : out) line = fit(line, (size_t)cols);
    if (!fills) return out;
    // centre the block in the pane: the leftover columns split evenly, the leftover rows too
    size_t used = g.gut + g.nh * (cw + 1) + 1;
    g.left = (size_t)cols > used ? ((size_t)cols - used) / 2 : 0;
    size_t pad = (size_t)rows > out.size() ? ((size_t)rows - out.size()) / 2 : 0;
    if (g.left)
        for (auto &line : out) line = std::string(g.left, ' ') + line;
    out.insert(out.begin(), pad, std::string());
    g.top += pad;
    return out;
}

}  // namespace paint

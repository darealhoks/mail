#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

#include "classify.h"
#include "config.h"
#include "registry.h"
#include "store.h"
#include "teams.h"
#include "teams_auth.h"
#include "term.h"
#include "paint.h"
#include "view.h"

#define CLI_NAME APP_NAME "c"
#define NEXT_FMT_SIMPLE "%t %s %r"

namespace {

using view::abbrev;
using view::ago;
using view::fold;
using view::local_at;
using view::matches;
using view::monday_of;
using view::watermark;
using view::ymd_local;
using view::ymd_plus;

void row(const char *cmd, const char *desc) {
    printf("  %s%s%s\n", cmd, std::string(strlen(cmd) < 21 ? 21 - strlen(cmd) : 1, ' ').c_str(),
           c("90", desc).c_str());
}

int help() {
    printf("usage: %s [filter...]        %s\n", CLI_NAME,
           c("90", "show the feed, newest deadlines first").c_str());
    printf("       %s <command> [args]\n\n", CLI_NAME);
    printf("%s\n", c("1", "commands").c_str());
    row("dismiss, d <n>...", "hide items by their feed number");
    row("open, o <n>", "open item <n> in a browser");
    row("marks, m [subj...]", "marks, newest first; subject abbrevs filter");
    row("timetable, r", "this week's timetable (next week on the weekend)");
    row("next", "the upcoming lesson; nothing (exit 1) if none");
    row("new, n", "one-line summary of what you haven't seen");
    row("auth, a [source]", "sign-in state; a source name to sign in");
    row("help, h", "this");
    printf("\n%s %s\n", c("1", "flags").c_str(),
           c("90", "one-shot overrides of " + config_path()).c_str());
    row("-n, --limit <n>", "cap the listing (0 = all)");
    row("--links, --no-links", "show or hide the item urls");
    row("-b, --blacklist a,b", "replace the class blacklist");
    row("-B, --no-blacklist", "ignore the blacklist this once");
    row("-a, --all", "uncapped, unfiltered: -B with --limit 0");
    row("-s, --simple", "next: alias for -f \"" NEXT_FMT_SIMPLE "\"");
    row("-f, --format <fmt>", "next: printf-ish line, see " CLI_NAME " next --help");
    std::string names;
    for (const Source &s : sources()) names += (names.empty() ? "" : ", ") + std::string(s.name);
    printf("\n%s %s\n", c("1", "filters").c_str(),
           c("90", "bare words, ANDed: a kind (info, task, test, mark, change), "
                   "a class abbrev, a source (" + names + ")")
               .c_str());
    return 0;
}

int next_help() {
    printf("usage: %s next [-s | -f <fmt>]\n\n%s\n", CLI_NAME, c("1", "format verbs").c_str());
    row("%t %e", "begin, end time (8:00)");
    row("%s %S", "subject abbrev, full name");
    row("%r %u", "room, teacher");
    row("%h %d", "hour number, date");
    row("%m", "\"in 12m\" until it starts");
    row("%! %%", "\"!\" if the lesson changed, literal %");
    return 0;
}

void usage() {
    fprintf(stderr, "usage: %s [filter...] | %s <dismiss|open|marks|new|auth|help>\n", CLI_NAME,
            CLI_NAME);
    fprintf(stderr, "       %s\n", c("90", CLI_NAME " help  for the full list", 2).c_str());
}

using paint::due_color;
using paint::kind_color;
using paint::link_up;
using paint::NEW_CHIP;
using paint::term_cols;
using paint::utf8_len;
using paint::when;
using paint::wrap;

int show(const std::vector<std::string> &argf) {
    std::vector<std::string> filters;
    for (const auto &a : argf) filters.push_back(fold(a));

    Store s;
    view::Feed f = view::feed_rows(s, filters, (size_t)config().num("general.limit"));
    if (!f.bad_filter.empty()) {
        fprintf(stderr, "%s %s\n", c("1;31", CLI_NAME ":", 2).c_str(),
                c("1", "no such filter '" + f.bad_filter + "'", 2).c_str());
        const std::pair<const char *, std::vector<std::string> *> groups[] = {
            {"kinds", &f.kinds}, {"classes", &f.classes}, {"sources", &f.sources}};
        for (const auto &g : groups) {
            std::string list;
            for (const auto &t : *g.second) list += (list.empty() ? "" : ", ") + t;
            std::string label = std::string(g.first) + ":";
            label.resize(9, ' ');
            fprintf(stderr, "  %s%s\n", c("1;33", label, 2).c_str(), c("37", list, 2).c_str());
        }
        fprintf(stderr, "%s\n", c("90", "  " CLI_NAME " help  for more", 2).c_str());
        return 2;
    }
    // numbers are positions in the whole feed, never in the filtered view, so `mail d 4` means
    // the same item whether or not a filter was on; freeze that numbering for the next command
    std::string idx;
    for (const auto &i : f.items) idx += (idx.empty() ? "" : ",") + std::to_string(i.id);
    s.set_state("cli_index", idx);

    if (f.rows.empty()) {
        puts(c("90", filters.empty() ? "nothing to show" : "nothing matches that filter").c_str());
        return 0;
    }

    size_t width = (size_t)term_cols();
    int bucket = -1;
    for (const view::FeedRow &r : f.rows) {
        const Item &i = f.items[r.n - 1];
        if (r.bucket != bucket) {
            bucket = r.bucket;
            printf("%s\n", c("1;90", bucket == 0 ? "— no deadline —"
                                     : bucket == 1 ? "— upcoming —"
                                                   : "— overdue —")
                                 .c_str());
        }
        size_t chips = utf8_len(r.klass) + i.kind.size() + i.source.size() + 10;
        std::vector<std::string> title = wrap(i.title, width > chips + 30 ? width - chips : 30);
        printf("%s %s%s  %s %s %s\n", c("90", std::to_string(r.n)).c_str(),
               r.is_new ? (c(NEW_CHIP, " NEW ") + " ").c_str() : "",
               c("1", title.empty() ? "" : title[0] + (title.size() > 1 ? "…" : "")).c_str(),
               c("1;36", "<" + r.klass + ">").c_str(),
               c(kind_color(i.kind), "<" + i.kind + ">").c_str(),
               c("90", "<" + i.source + ">").c_str());
        // teams posts have no subject line, so their title is the first slice of the body
        std::string rest = i.body.compare(0, i.title.size(), i.title) == 0
                               ? i.body.substr(i.title.size())
                               : i.body;
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '|' || rest[0] == '\n')) rest.erase(0, 1);
        if (!rest.empty())
            for (const auto &l : wrap(rest, width))
                printf("%s\n", l == teams::TASK_NOTE  // set by teams.cpp, not a real body line
                                   ? c("1;33", "<" + l + ">").c_str()
                                   : link_up(c("37", l)).c_str());
        if (!i.url.empty() && config().flag("general.links")) printf("%s\n", c("4;34", i.url).c_str());
        if (i.due_at) printf("%s\n", c(due_color(i.due_at), when(i.due_at)).c_str());
        putchar('\n');
    }
    return 0;
}

// mark 1..5 for a "<got>/<max>" text via school.points, 0 when it is not a fraction
const char *mark_color(const std::string &t) {
    static const char *SGR[] = {"1;32", "0;32", "0;33", "0;31", "1;31"};
    int m = 0;
    if (t.find('/') != std::string::npos) m = view::points_of(t);
    else if (!t.empty() && t[0] >= mark_scale().first && t[0] <= mark_scale().second)
        m = t[0] - mark_scale().first + 1;
    return m >= 1 && m <= 5 ? SGR[m - 1] : "37";
}

int marks(const std::vector<std::string> &argf) {
    std::vector<std::string> filters;
    for (const auto &a : argf) filters.push_back(fold(a));

    Store s;
    view::Marks m = view::marks_rows(s, filters, (size_t)config().num("general.limit"));

    size_t wc = 0, wm_col = 0;
    for (const auto &r : m.rows) {
        wc = std::max(wc, utf8_len(r.klass));
        wm_col = std::max(wm_col, utf8_len(r.mark));
    }

    std::string period;
    for (const auto &r : m.rows) {
        if (r.period != period) {
            if (!period.empty()) putchar('\n');
            period = r.period;
            printf("%s\n\n", c("1;90", "— " + period + " —").c_str());
        }
        char d[16] = "??? ?? ";
        if (r.event_at) {
            struct tm tm {};
            time_t tt = (time_t)r.event_at;
            localtime_r(&tt, &tm);
            strftime(d, sizeof d, "%d %b", &tm);
        }
        std::string cls = r.klass;
        cls.append(wc - utf8_len(cls), ' ');
        printf("%s%s  %s  %s  %s\n", r.is_new ? (c(NEW_CHIP, " NEW ") + " ").c_str() : "",
               c("90", d).c_str(), c("1;36", cls).c_str(),
               c(mark_color(r.mark), r.mark + std::string(wm_col - utf8_len(r.mark), ' ')).c_str(),
               c("37", r.note).c_str());
    }
    if (m.rows.empty())
        puts(c("90", filters.empty() ? "no marks" : "no marks for that subject").c_str());
    for (const auto &[p, a] : m.averages) {
        char v[16];
        snprintf(v, sizeof v, "%.2f", a);
        for (char &ch : v)
            if (ch == '.') ch = ',';
        printf("\n%s  %s", c("1;90", "average " + p).c_str(), c(mark_color(v), v).c_str());
    }
    if (!m.averages.empty()) putchar('\n');
    return 0;
}

// the numbering `mail` last printed; ids, in order
std::vector<long long> last_index(Store &s) {
    std::vector<long long> all;
    std::string idx = s.get_state("cli_index"), tok;
    for (char ch : idx + ",")
        if (ch == ',') {
            if (!tok.empty()) all.push_back(atoll(tok.c_str()));
            tok.clear();
        } else tok += ch;
    return all;
}

void no_such(const char *n) {
    fprintf(stderr, "%s %s\n", c("1;31", CLI_NAME ":", 2).c_str(),
            c("1", "no item " + std::string(n) + " in the last listing", 2).c_str());
    fprintf(stderr, "%s\n", c("90", "  run " CLI_NAME " first to number them", 2).c_str());
}

int open_item(int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr, "usage: %s open <n>\n", CLI_NAME);
        return 2;
    }
    Store s;
    std::vector<long long> all = last_index(s);
    long n = strtol(argv[0], nullptr, 10);
    if (n < 1 || (size_t)n > all.size()) {
        no_such(argv[0]);
        return 1;
    }
    std::string url;
    for (const auto &i : s.feed())
        if (i.id == all[n - 1]) url = i.url;
    if (url.empty()) {
        fprintf(stderr, "%s %s\n", c("1;31", CLI_NAME ":", 2).c_str(),
                c("1", "item " + std::string(argv[0]) + " has no link", 2).c_str());
        return 1;
    }
    if (url.find('\'') != std::string::npos) return 1;
    std::string opener = config().str("general.browser");
    if (opener.empty()) opener = "xdg-open";
    return system((opener + " '" + url + "' >/dev/null 2>&1 &").c_str()) == 0 ? 0 : 1;
}

int dismiss(int argc, char **argv) {
    Store s;
    std::vector<long long> all = last_index(s);
    std::vector<long long> ids;
    for (int a = 0; a < argc; a++) {
        long n = strtol(argv[a], nullptr, 10);
        if (n < 1 || (size_t)n > all.size()) {
            no_such(argv[a]);
            return 1;
        }
        ids.push_back(all[n - 1]);
    }
    if (ids.empty()) {
        fprintf(stderr, "usage: %s dismiss <n> [n...]\n", CLI_NAME);
        return 2;
    }
    s.dismiss(ids);
    printf("%s %zu\n", c("1;32", "dismissed").c_str(), ids.size());
    return 0;
}

int timetable() {
    Store s;
    view::Timetable tt = view::timetable(s);
    if (tt.rows.empty()) {
        printf("%s %s\n", c("1", "week " + tt.monday).c_str(), c("90", "— no lessons").c_str());
        return 0;
    }
    size_t w = 3;
    for (const Lesson &l : tt.rows) w = std::max(w, utf8_len(l.label()));

    static const char *DAYNAME[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    auto pad = [&](const std::string &t) { return t + std::string(w - utf8_len(t), ' '); };

    std::string head;
    for (const auto &h : tt.hours) head += " " + pad(h);
    printf("%s\n%s%s\n", c("1", "week " + tt.monday).c_str(), "  ", c("90", head).c_str());
    for (size_t di = 0; di < tt.days.size(); di++) {
        struct tm tm {};
        time_t t = (time_t)classify::epoch(tt.days[di]);
        gmtime_r(&t, &tm);
        printf("%s", c("1;36", DAYNAME[tm.tm_wday]).c_str());
        for (size_t hi = 0; hi < tt.hours.size(); hi++) {
            const Lesson *l = tt.at(di, hi);
            std::string st = l ? l->state : "";
            std::string cell = pad(l ? l->label() : "");
            printf(" %s", st == "x"   ? c("42;30", cell).c_str()
                          : st == "!" ? c("41;37", cell).c_str()
                                      : cell.c_str());
        }
        putchar('\n');
    }
    return 0;
}

std::string hhmm(const std::string &t) {
    return t.size() > 1 && t[0] == '0' ? t.substr(1) : t;
}

std::string in_mins(long long d) {
    if (d < 60) return "now";
    if (d < 5400) return "in " + std::to_string(d / 60) + "m";
    if (d < 172800) return "in " + std::to_string(d / 3600) + "h";
    return "in " + std::to_string(d / 86400) + "d";
}

std::string fmt_lesson(const std::string &f, const Lesson &l, long long start) {
    std::string o;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%' || i + 1 >= f.size()) {
            o += f[i];
            continue;
        }
        switch (f[++i]) {
        case 't': o += hhmm(l.begins); break;
        case 'e': o += hhmm(l.ends); break;
        case 's': o += l.subject; break;
        case 'S': o += l.subject_name.empty() ? l.subject : l.subject_name; break;
        case 'r': o += l.room; break;
        case 'u': o += l.teacher; break;
        case 'h': o += l.hour; break;
        case 'd': o += l.date; break;
        case 'm': o += in_mins(start - (long long)time(nullptr)); break;
        case '!': o += l.state == "!" ? "!" : ""; break;
        case '%': o += '%'; break;
        default:
            throw std::runtime_error(std::string("unknown format verb %") + f[i] +
                                     " (see " CLI_NAME " next --help)");
        }
    }
    return o;
}

std::string next_fmt;  // -f, or -s expanded

int next_lesson() {
    Store s;
    view::Next nx = view::next_lesson(s);
    if (nx.state == view::Next::NoTimes) {
        fprintf(stderr, "%s\n",
                c("1;31", "timetable stored without lesson times; re-run " APP_NAME "d", 2).c_str());
        return 2;
    }
    if (nx.state != view::Next::Ok) return 1;  // holidays, or nothing left: print nothing
    const Lesson &bl = nx.lesson;
    long long best = nx.start;

    if (!next_fmt.empty()) {
        try {
            printf("%s\n", fmt_lesson(next_fmt, bl, best).c_str());
        } catch (const std::exception &e) {
            fprintf(stderr, "%s: %s\n", CLI_NAME, e.what());
            return 2;  // 1 means "nothing next", so a bad format must not look like it
        }
        return 0;
    }

    static const char *DAYNAME[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    struct tm tm {};
    time_t tt = (time_t)best;
    localtime_r(&tt, &tm);
    long long now = (long long)time(nullptr);
    std::string day = bl.date == ymd_local(now) ? "" : std::string(DAYNAME[tm.tm_wday]) + " ";
    printf("%s%s  %s  %s\n", c("90", day).c_str(),
           c("1;36", hhmm(bl.begins) + "-" + hhmm(bl.ends)).c_str(),
           c("1", fmt_lesson(bl.subject_name.empty() ? "%s" : "%S (%s)", bl, best)).c_str(),
           c("90", trim(bl.room + (bl.teacher.empty() ? "" : "  " + bl.teacher) +
                        (bl.state == "!" ? "  changed" : "") + "  " + in_mins(best - now)))
               .c_str());
    return 0;
}

std::vector<char *> take_flags(int argc, char **argv, int &rc);
std::vector<std::string> expand_bind(int argc, char **argv);

int selfcheck() {
    struct {
        const char *in, *want;
    } cases[] = {
        {"https://login.microsoft.com/device", "HTTPS://LOGIN.MICROSOFT.COM/DEVICE"},
        {"https://microsoft.com/devicelogin", "HTTPS://MICROSOFT.COM/DEVICELOGIN"},
        {"https://login.microsoft.com/device?otc=ABC", "https://login.microsoft.com/device?otc=ABC"},
        {"https://ha.mr/a_b", "https://ha.mr/a_b"},
    };
    for (const auto &c : cases) {
        if (teams::qr_upper(c.in) != c.want) {
            fprintf(stderr, "selfcheck failed: qr_upper(%s)\n", c.in);
            return 1;
        }
    }
    struct {
        const char *in, *want;
    } abbrevs[] = {
        {"3A_GRS_SK1_25/26", "GRS"}, {"3 MAT", "MAT"},   {"3A_CJL_CELÁ", "CJL"},
        {"WBA", "WBA"},              {"OSE1", "OSE1"},   {"Jan Novák", "Jan Novák"},
    };
    for (const auto &a : abbrevs) {
        if (abbrev(a.in) != a.want) {
            fprintf(stderr, "selfcheck failed: abbrev(%s) = %s\n", a.in, abbrev(a.in).c_str());
            return 1;
        }
    }
    Lesson l;
    l.date = "2026-08-17";
    l.hour = "2";
    l.subject = "MAT";
    l.subject_name = "Matematika";
    l.room = "309";
    l.teacher = "NOV";
    l.begins = "09:55";
    l.ends = "10:40";
    Lesson gone = l;
    gone.hour = "1";
    gone.subject = "CJL";
    gone.room = "";
    gone.state = "x";
    {
        std::string p = "/tmp/" CLI_NAME "-selfcheck-" + std::to_string(getpid()) + ".db";
        unlink(p.c_str());
        Store t(p);
        t.put_lessons("bakalari", "2026-08-17", "2026-08-23", {l, gone});
        std::vector<Lesson> got = t.lessons("2026-08-17", "2026-08-23");
        // re-fetch of the same week without CJL: the stale row must not survive
        t.put_lessons("bakalari", "2026-08-17", "2026-08-23", {l});
        std::vector<Lesson> after = t.lessons("2026-08-17", "2026-08-23");
        unlink(p.c_str());
        unlink((p + "-wal").c_str());
        unlink((p + "-shm").c_str());
        if (got.size() != 2 || got[0].hour != "1" || got[0].state != "x" ||
            got[0].label() != "CJL" || got[1].label() != "MAT 309" || got[1].teacher != "NOV" ||
            got[1].subject_name != "Matematika" || got[1].begins != "09:55" ||
            got[1].ends != "10:40" || got[1].source != "bakalari" || after.size() != 1 ||
            after[0].hour != "2" || !t.lessons("2026-08-24", "2026-08-30").empty()) {
            fputs("selfcheck failed: lessons round-trip\n", stderr);
            return 1;
        }
    }

    long long start = local_at(l.date, l.begins);
    if (fmt_lesson(NEXT_FMT_SIMPLE, l, start) != "9:55 MAT 309" ||
        fmt_lesson("%S/%u/%e/%h/%!/%%", l, start) != "Matematika/NOV/10:40/2//%" ||
        !start || local_at("2026-08-17", "") || ymd_local(start) != "2026-08-17" ||
        ymd_plus("2026-08-17", 6) != "2026-08-23" || ymd_plus("2026-08-17", 13) != "2026-08-30" ||
        monday_of(start) != "2026-08-17" || monday_of(start + 6 * 86400) != "2026-08-17") {
        fputs("selfcheck failed: fmt_lesson/local_at\n", stderr);
        return 1;
    }

    if (fold("Čeština") != "cestina" || fold("MAT") != "mat") {
        fputs("selfcheck failed: fold\n", stderr);
        return 1;
    }

    Item it;
    it.kind = "test";
    it.klass = "3A_MAT_SK1_25/26";
    it.source = "teams";
    if (!matches(it, {"mat"}) || !matches(it, {"mat", "test"}) || matches(it, {"mat", "task"}) ||
        !matches(it, {}) || matches(it, {"othersrc"})) {
        fputs("selfcheck failed: matches\n", stderr);
        return 1;
    }
    if (wrap("ab cd ef", 5) != std::vector<std::string>{"ab cd", "ef"} ||
        wrap("příliš žluťoučký", 8) != std::vector<std::string>{"příliš", "žluťoučký"}) {
        fputs("selfcheck failed: wrap\n", stderr);
        return 1;
    }
    if (strcmp(mark_color("1"), mark_color("1-")) || !strcmp(mark_color("2"), mark_color("5")) ||
        strcmp(mark_color("N"), "37") || strcmp(mark_color("45/50"), "1;32") ||
        strcmp(mark_color("0/25"), "1;31") || strcmp(mark_color("5/0"), "37")) {
        fputs("selfcheck failed: mark_color\n", stderr);
        return 1;
    }
    auto at = [](const char *iso) { return classify::epoch(iso); };
    if (school_year(at("2026-08-16")) != "26/27" || school_year(at("2026-07-31")) != "25/26" ||
        school_year(at("2026-01-05")) != "25/26") {
        fputs("selfcheck failed: school_year\n", stderr);
        return 1;
    }
    if (period_label(at("2025-09-10")) != "25/26 · H1 · Q1" ||
        period_label(at("2025-12-01")) != "25/26 · H1 · Q2" ||
        period_label(at("2026-01-20")) != "25/26 · H1 · Q2" ||
        period_label(at("2026-03-01")) != "25/26 · H2 · Q3" ||
        period_label(at("2026-05-01")) != "25/26 · H2 · Q4") {
        fputs("selfcheck failed: period_label\n", stderr);
        return 1;
    }
    if (school_year(school_year_start("26/27")) != "26/27") {
        fputs("selfcheck failed: school_year_start\n", stderr);
        return 1;
    }
    char *av[] = {(char *)CLI_NAME, (char *)"-s", (char *)"-n", (char *)"3", (char *)"marks"};
    int frc = 0;
    std::vector<char *> rest = take_flags(5, av, frc);
    if (frc || next_fmt != NEXT_FMT_SIMPLE || config().num("general.limit") != 3 ||
        rest.size() != 1 || strcmp(rest[0], "marks")) {
        fputs("selfcheck failed: take_flags\n", stderr);
        return 1;
    }

    next_fmt.clear();
    config().set("bind.t", "next -s");
    config().set("bind.marks", "timetable");  // a bind must beat the builtin verb of that name
    char *bv[] = {(char *)CLI_NAME, (char *)"-b", (char *)"t", (char *)"t", (char *)"x"};
    char *mv[] = {(char *)CLI_NAME, (char *)"marks"};
    std::vector<std::string> ex = expand_bind(5, bv);
    std::vector<char *> bound{(char *)CLI_NAME};
    for (auto &w : ex) bound.push_back(&w[0]);
    rest = take_flags((int)bound.size(), bound.data(), frc);
    if (frc || ex != std::vector<std::string>{"-b", "t", "next", "-s", "x"} ||
        next_fmt != NEXT_FMT_SIMPLE || rest.size() != 2 || strcmp(rest[0], "next") ||
        expand_bind(2, mv) != std::vector<std::string>{"timetable"}) {
        fputs("selfcheck failed: expand_bind\n", stderr);
        return 1;
    }

    config().set("general.blacklist", "anj");
    if (!blacklisted("ANJ", "ANJ") || !blacklisted("3A_ANJ_SK1_25/26", "ANJ") ||
        blacklisted("3A_MAT_SK1_25/26", "MAT")) {
        fputs("selfcheck failed: blacklisted\n", stderr);
        return 1;
    }
    puts(CLI_NAME ": selfcheck ok");
    return 0;
}

void line(const char *label, const char *sgr, const std::string &state, const std::string &note) {
    std::string l = std::string(label) + ":";
    l.resize(11, ' ');
    printf("  %s%s%s\n", c("1", l).c_str(), c(sgr, state).c_str(),
           note.empty() ? "" : c("90", "  " + note).c_str());
}

void source_status(const view::SourceStatus &st) {
    printf("%s\n", c("1;33", st.name).c_str());
    if (!st.signed_in)
        line("session", "1;31", "NOT SIGNED IN", "run: " CLI_NAME " auth " + st.name);
    else if (!st.error.empty())
        line("session", "1;31", "EXPIRED", st.error + "; run: " CLI_NAME " auth " + st.name);
    else {
        line("session", "1;32", "ok", "");
        line("refreshed", "90", ago(st.refreshed_at), "");
    }
    line("fetched", st.stale ? "1;31" : "0;32", ago(st.fetched_at),
         st.stale && st.fetched_at ? "stale" : "");
}

// anything that reads the feed offers the sign-in first: a dead session means the store
// stopped growing, so the listing is quietly incomplete. never prompts off a terminal.
void ensure_signed() {
    for (const Source &src : sources()) {
        if (src.session_error().empty()) continue;
        if (!isatty(0) || !isatty(1)) {
            fprintf(stderr, "%s\n", c("1;31", std::string(src.pretty) + " unsigned", 2).c_str());
            continue;
        }
        printf("%s ", c("1;31", std::string(src.pretty) + " is unsigned. Sign in? [Y/n]").c_str());
        fflush(stdout);
        char l[8] = "";
        if (!fgets(l, sizeof l, stdin) || l[0] == 'n' || l[0] == 'N') continue;
        try {
            src.login();
        } catch (const std::exception &e) {
            fprintf(stderr, "%s %s\n", c("1;31", CLI_NAME ":", 2).c_str(), e.what());
        }
        putchar('\n');
    }
}

int new_summary() {
    Store s;
    view::NewCounts n = view::new_counts(s);
    auto count = [](int k, const char *one, const char *many, const char *sgr) {
        if (k) printf("%s %s\n", c(sgr, std::to_string(k)).c_str(),
                      c("1", k == 1 ? one : many).c_str());
    };
    count(n.msgs, "new message", "new messages", "1;34");
    count(n.work, "new task", "new tasks", "1;33");
    count(n.marks, "new mark", "new marks", "1;35");
    std::string unsigned_line;
    for (size_t i = 0; i < n.unsigned_pretty.size(); i++)
        unsigned_line += (i ? (i + 1 == n.unsigned_pretty.size() ? " and " : ", ") : "") +
                         n.unsigned_pretty[i];

    std::vector<view::Gripe> bad = view::gripes(s);
    if (!n.msgs && !n.work && !n.marks && bad.empty() && unsigned_line.empty())
        puts(c("90", "nothing new").c_str());
    if (!unsigned_line.empty()) printf("%s\n", c("1;31", unsigned_line + " unsigned").c_str());
    for (const auto &b : bad)
        printf("%s %s\n", c(b.error ? "1;31" : "1;33", APP_NAME "d:").c_str(),
               c(b.error ? "1;31" : "1;33", b.text).c_str());
    return 0;
}

int status() {
    Store s;
    std::string names;
    for (const view::SourceStatus &st : view::status(s)) {
        source_status(st);
        putchar('\n');
        names += (names.empty() ? "" : "|") + st.name;
    }
    printf("%s %s\n", c("90", "sign in with").c_str(),
           c("1;36", CLI_NAME " auth " + names).c_str());
    return 0;
}

// pulls flags out of argv in place; everything left is the command and its words
std::vector<char *> take_flags(int argc, char **argv, int &rc) {
    std::vector<char *> rest;
    auto val = [&](int &a, const char *name) -> const char * {
        if (a + 1 >= argc) {
            fprintf(stderr, "%s: %s needs a value\n", CLI_NAME, name);
            rc = 2;
            return nullptr;
        }
        return argv[++a];
    };
    for (int a = 1; a < argc && rc == 0; a++) {
        std::string f = argv[a];
        if (f == "-n" || f == "--limit") {
            if (const char *v = val(a, "--limit")) config().set("general.limit", v);
        } else if (f == "--links") config().set("general.links", "yes");
        else if (f == "--no-links") config().set("general.links", "no");
        else if (f == "-b" || f == "--blacklist") {
            const char *v = val(a, "--blacklist");
            if (!v) continue;
            std::string out, tok;
            // folded here, not in config: the store's class words are compared folded too
            for (char ch : std::string(v) + ",")
                if (ch == ',') {
                    if (!trim(tok).empty()) out += (out.empty() ? "" : ",") + fold(trim(tok));
                    tok.clear();
                } else tok += ch;
            config().set("general.blacklist", out);
        } else if (f == "-s" || f == "--simple") next_fmt = NEXT_FMT_SIMPLE;
        else if (f == "-f" || f == "--format") {
            if (const char *v = val(a, "--format")) next_fmt = v;
        } else if (f == "-B" || f == "--no-blacklist") config().set("general.blacklist", "");
        else if (f == "-a" || f == "--all") {
            config().set("general.blacklist", "");
            config().set("general.limit", "0");
        } else rest.push_back(argv[a]);
    }
    return rest;
}

// one level of substitution, no recursion, no shell: the first non-flag word is looked up in
// [bind] and replaced by its value split on spaces. runs before take_flags so flags inside a
// bind value are still parsed, and before the verb table so a bind wins over a builtin verb
std::vector<std::string> expand_bind(int argc, char **argv) {
    static const char *TAKES_VAL[] = {"-n", "--limit", "-b", "--blacklist", "-f", "--format"};
    std::vector<std::string> out;
    bool tried = false;
    for (int a = 1; a < argc; a++) {
        std::string t = argv[a];
        bool val = false;
        for (const char *f : TAKES_VAL) val |= t == f;
        if (val) {
            out.push_back(t);
            if (a + 1 < argc) out.push_back(argv[++a]);
            continue;
        }
        if (!tried && !t.empty() && t[0] != '-') {
            tried = true;
            std::istringstream in(config().str("bind." + fold(t)));
            std::string w;
            if (in >> w) {
                do out.push_back(w);
                while (in >> w);
                continue;
            }
        }
        out.push_back(t);
    }
    return out;
}

bool verb(const char *a, const char *full, const char *alias) {
    return !strcmp(a, full) || !strcmp(a, alias);
}

int auth(int argc, char **argv) {
    if (!argc) return status();
    if (const Source *src = source(argv[0])) return src->login();
    std::string names;
    for (const Source &src : sources()) names += (names.empty() ? "" : ", ") + std::string(src.name);
    fprintf(stderr, "%s %s\n", c("1;31", CLI_NAME ":", 2).c_str(),
            c("1", "no source '" + std::string(argv[0]) + "'", 2).c_str());
    fprintf(stderr, "  %s%s\n", c("1;33", "sources:  ", 2).c_str(), c("37", names, 2).c_str());
    fprintf(stderr, "%s\n", c("90", "  " CLI_NAME " auth  for the sign-in state", 2).c_str());
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        if (argc > 1 && !strcmp(argv[1], "--selfcheck")) return selfcheck();  // not a verb
        std::vector<std::string> ex = expand_bind(argc, argv);
        std::vector<char *> bound{argv[0]};
        for (auto &w : ex) bound.push_back(&w[0]);
        if (bound.size() > 1 &&
            (!strcmp(bound[1], "--help") || verb(bound[1], "help", "h"))) return help();
        int rc = 0;
        std::vector<char *> a = take_flags((int)bound.size(), bound.data(), rc);
        if (rc) return rc;
        int n = (int)a.size();
        char **v = a.data();
        if (!n) {
            ensure_signed();
            return show({});
        }
        if (verb(v[0], "auth", "a")) return auth(n - 1, v + 1);
        if (verb(v[0], "new", "n")) return new_summary();  // prompt-hook: prints, never asks
        if (v[0][0] == '-') {
            usage();
            return 2;
        }
        ensure_signed();
        if (verb(v[0], "marks", "m")) return marks({v + 1, v + n});
        if (verb(v[0], "timetable", "r")) return timetable();
        // no short alias for next: `n` is `new`
        if (!strcmp(v[0], "next")) return n > 1 && !strcmp(v[1], "--help") ? next_help() : next_lesson();
        if (verb(v[0], "dismiss", "d")) return dismiss(n - 1, v + 1);
        if (verb(v[0], "open", "o")) return open_item(n - 1, v + 1);
        return show({v, v + n});
    } catch (const std::exception &e) {
        fprintf(stderr, CLI_NAME ": %s\n", e.what());
        return 1;
    }
}

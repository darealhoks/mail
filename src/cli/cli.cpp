#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "config.h"
#include "registry.h"
#include "store.h"
#include "term.h"
#include "paint.h"
#include "view.h"

#define CLI_NAME APP_NAME "c"
#define NEXT_FMT_SIMPLE "%t %s %r"
#define VERB_LIST "dismiss, open, marks, absence, timetable, next, new, auth, help"
#ifndef VERSION  // set by the makefile
#define VERSION "dev"
#endif

namespace {

using view::abbrev;
using view::ago;
using view::fold;
using view::fold_all;
using view::local_at;
using view::monday_of;
using view::ymd_local;
using view::ymd_plus;

void err(const std::string &what) {
    fprintf(stderr, "%s %s\n", c("1;31", CLI_NAME ":", 2).c_str(), what.c_str());
}

std::string head(const std::string &label) { return c("90", "#") + " " + c("1", label); }

std::string source_names() {
    std::string n;
    for (const Source &s : sources()) n += (n.empty() ? "" : ", ") + std::string(s.name);
    return n;
}

void row(const char *cmd, const char *desc) {
    printf("  %s%s%s\n", cmd, std::string(strlen(cmd) < 21 ? 21 - strlen(cmd) : 1, ' ').c_str(),
           c("90", desc).c_str());
}

int help() {
    printf("usage: %s [filter...]        %s\n", CLI_NAME,
           c("90", "show the feed, most urgent last").c_str());
    printf("       %s <command> [args]\n\n", CLI_NAME);
    printf("%s\n", head("commands").c_str());
    row("dismiss, d <n>...", "hide items by their feed number");
    row("open, o <n>", "open item <n> in a browser");
    row("marks, m [subj...]", "marks, newest first; subject abbrevs filter");
    row("absence, b [subj...]", "absence per subject; subject abbrevs filter");
    row("timetable, r [week]", "the week's timetable; +n/-n or a date picks another");
    row("next", "the upcoming lesson; nothing (exit 1) if none");
    row("new, n", "one-line summary of what you haven't seen");
    row("auth, a [source]", "sign-in state; a source name to sign in");
    row("help, h", "this; --help works after any command");
    row("--version", "print the version");
    printf("\n%s %s\n", head("flags").c_str(),
           c("90", "one-shot overrides of " + config_path()).c_str());
    row("-n, --limit <n>", "cap the listing (0 = all)");
    row("--links, --no-links", "show or hide the item urls");
    row("-b, --blacklist a,b", "replace the class blacklist");
    row("-B, --no-blacklist", "ignore the blacklist this once");
    row("-a, --all", "uncapped, unfiltered: -B with --limit 0");
    row("-s, --simple", "next: alias for -f \"" NEXT_FMT_SIMPLE "\"");
    row("-f, --format <fmt>", "next: %t %e begin/end, %s %S subject, %r room, %u teacher,");
    row("", "%h hour, %d date, %m \"in 12m\", %! changed, %% literal");
    printf("\n%s %s\n", head("filters").c_str(),
           c("90", "bare words, ANDed: a kind (info, task, test, mark, change), "
                   "a class abbrev, a source (" + source_names() + ")")
               .c_str());
    return 0;
}

void usage() {
    fprintf(stderr, "usage: %s [filter...] | %s <%s>\n", CLI_NAME, CLI_NAME, VERB_LIST);
    fprintf(stderr, "       %s\n", c("90", CLI_NAME " help  for the full list", 2).c_str());
}

using paint::term_cols;

int show(const std::vector<std::string> &argf) {
    std::vector<std::string> filters = fold_all(argf);
    Store s;
    view::Feed f = view::feed_rows(s, filters, (size_t)config().num("general.limit"));
    if (!f.bad_filter.empty()) {
        err("no such filter '" + f.bad_filter + "'");
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

    size_t w = (size_t)term_cols();
    bool tty = isatty(1);
    for (const paint::Post &p : paint::feed_posts(f, w))
        for (const std::string &l : p.lines) puts(tty ? paint::fit(l, w).c_str() : l.c_str());
    return 0;
}

int marks(const std::vector<std::string> &argf) {
    std::vector<std::string> filters = fold_all(argf);
    Store s;
    view::Marks m = view::marks_rows(s, filters, (size_t)config().num("general.limit"));
    if (filters.empty()) m.averages.clear();  // a mixed-subject average means nothing
    for (const std::string &l : paint::mark_lines(m, (size_t)term_cols())) puts(l.c_str());
    if (m.rows.empty())
        puts(c("90", filters.empty() ? "no marks" : "no marks for that subject").c_str());
    return 0;
}

int absence(const std::vector<std::string> &argf) {
    std::vector<std::string> filters = fold_all(argf);
    Store s;
    view::Absences ab = view::absence_rows(s, filters);
    for (const std::string &l : paint::absence_lines(ab)) puts(l.c_str());
    if (ab.rows.empty())
        puts(c("90", filters.empty() ? "no absences" : "no absences for that subject").c_str());
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
    err("no item " + std::string(n) + " in the last listing; run " CLI_NAME " to number them");
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
        err("item " + std::string(argv[0]) + " has no link");
        return 1;
    }
    return paint::open_url(url);
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
    printf("dismissed %zu\n", ids.size());
    return 0;
}

int timetable(int argc, char **argv) {
    std::string monday;
    if (argc > 0) {
        std::string a = argv[0];
        if (a[0] == '+' || a[0] == '-')
            monday = view::ymd_plus(view::wanted_monday(), 7 * atoi(a.c_str()));
        else if (long long t = view::local_at(a, "12:00"))  // noon: no dst edge either way
            monday = monday_of(t);
        else {
            fprintf(stderr, "usage: %s timetable [+n|-n|YYYY-MM-DD]\n", CLI_NAME);
            return 2;
        }
    }
    Store s;
    view::Timetable tt = view::timetable(s, monday);
    paint::Geom g;
    for (const std::string &l : paint::grid_lines(tt, paint::NO_CELL, paint::NO_CELL, 0,
                                                  paint::term_cols(false), g))
        puts(l.c_str());
    return 0;
}

using paint::hhmm;

std::string in_mins(long long d) { return d < 60 ? "now" : "in " + view::rel_span(d); }

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
                                     " (see " CLI_NAME " help)");
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

    struct tm tm {};
    time_t tt = (time_t)best;
    localtime_r(&tt, &tm);
    long long now = (long long)time(nullptr);
    std::string day = bl.date == ymd_local(now) ? "" : std::string(paint::DAYNAME[tm.tm_wday]) + " ";
    printf("%s%s  %s  %s\n", c("90", day).c_str(),
           c("1", hhmm(bl.begins) + "-" + hhmm(bl.ends)).c_str(),
           fmt_lesson(bl.subject_name.empty() ? "%s" : "%S (%s)", bl, best).c_str(),
           c("90", trim(bl.room + (bl.teacher.empty() ? "" : "  " + bl.teacher) +
                        (bl.state == "!" ? "  changed" : "") + "  " + in_mins(best - now)))
               .c_str());
    return 0;
}

void line(const char *label, const char *sgr, const std::string &state, const std::string &note) {
    std::string l = std::string(label) + ":";
    l.resize(11, ' ');
    printf("  %s%s%s\n", c("1", l).c_str(), c(sgr, state).c_str(),
           note.empty() ? "" : c("90", "  " + note).c_str());
}

void source_status(const view::SourceStatus &st, bool off) {
    printf("%s\n", head(st.name).c_str());
    if (!st.signed_in)
        line("session", "1;31", "NOT SIGNED IN", "run: " CLI_NAME " auth " + st.name);
    else if (!st.error.empty())
        line("session", "1;31", "EXPIRED", st.error + "; run: " CLI_NAME " auth " + st.name);
    else {
        line("session", "1;32", "ok", "");
        line("refreshed", "90", ago(st.refreshed_at), "");
    }
    bool loud = st.stale && !off;
    line("fetched", loud ? "0;33" : "0;32", ago(st.fetched_at),
         off ? "no internet" : loud && st.fetched_at ? "stale" : "");
}

// anything that reads the feed says what it is missing first: a dead session means the store
// stopped growing, so the listing is quietly incomplete. never prompts off a terminal.
// one health sweep for both halves — each source costs a session probe
void ready() {
    Store s;
    view::Health h = view::health(s);
    for (const std::string &name : h.unsigned_names) {
        const Source *src = source(name);
        if (!src || !isatty(0) || !isatty(1)) {
            fprintf(stderr, "%s\n",
                    c("1;31", name + " not authed, run " CLI_NAME " auth " + name, 2).c_str());
            continue;
        }
        printf("%s ", c("1;31", std::string(src->pretty) + " is unsigned. Sign in? [Y/n]").c_str());
        fflush(stdout);
        char l[8] = "";
        if (!fgets(l, sizeof l, stdin) || l[0] == 'n' || l[0] == 'N') continue;
        try {
            src->login();
        } catch (const std::exception &e) {
            err(e.what());
        }
        putchar('\n');
    }
    for (const view::Gripe &g : h.gripes)
        fprintf(stderr, "%s %s\n", c(g.error ? "1;31" : "1;33", APP_NAME "d:", 2).c_str(),
                c(g.error ? "1;31" : "1;33", g.text, 2).c_str());
}

// this lands in a shell prompt: one short line each, and the fix spelled out
int new_summary() {
    Store s;
    view::NewCounts n = view::new_counts(s);
    auto count = [](int k, const char *one, const char *many) {
        if (k) printf("%s %s\n", c("1", std::to_string(k)).c_str(), k == 1 ? one : many);
    };
    count(n.msgs, "new message", "new messages");
    count(n.work, "new task", "new tasks");
    count(n.marks, "new mark", "new marks");
    view::Health h = view::health(s);
    for (const std::string &name : h.unsigned_names)
        printf("%s\n", c("1;31", name + " not authed, " CLI_NAME " auth " + name).c_str());
    for (const view::Gripe &g : h.gripes)
        printf("%s\n", c(g.error ? "1;31" : "1;33", g.brief).c_str());
    return 0;
}

int status() {
    Store s;
    view::Health h = view::health(s);
    std::string names;
    for (const view::SourceStatus &st : h.sources) {
        source_status(st, h.offline);
        putchar('\n');
        names += (names.empty() ? "" : "|") + st.name;
    }
    printf("%s %s\n", c("90", "sign in with").c_str(),
           c(paint::accent_bold(), CLI_NAME " auth " + names).c_str());
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
            const char *v = val(a, "--limit");
            if (!v) continue;
            char *end = nullptr;
            long n = strtol(v, &end, 10);
            if (!*v || *end || n < 0) {
                fprintf(stderr, "%s: --limit wants a count >= 0, not '%s'\n", CLI_NAME, v);
                rc = 2;
                continue;
            }
            config().set("general.limit", v);
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
        } else if (f.size() > 1 && f[0] == '-' && f != "--" &&
                   f.find_first_not_of("0123456789", 1) != std::string::npos) {
            // "-2" stays a word: `timetable -2` is a relative week, not a flag
            fprintf(stderr, "%s: unknown flag '%s'\n", CLI_NAME, argv[a]);
            fprintf(stderr, "%s\n", c("90", "  " CLI_NAME " help  for the flags", 2).c_str());
            rc = 2;
        } else rest.push_back(argv[a]);
    }
    return rest;
}

bool verb(const char *a, const char *full, const char *alias) {
    return !strcmp(a, full) || !strcmp(a, alias);
}

int auth(int argc, char **argv) {
    if (!argc) return status();
    if (const Source *src = source(argv[0])) return src->login();
    err("no source '" + std::string(argv[0]) + "'; try " + source_names());
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--version")) return puts(CLI_NAME " " VERSION), 0;
            if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return help();
        }
        std::vector<std::string> ex = expand_bind({argv + 1, argv + argc});
        std::vector<char *> bound{argv[0]};
        for (auto &w : ex) bound.push_back(&w[0]);
        if (bound.size() > 1 && verb(bound[1], "help", "h")) return help();
        int rc = 0;
        std::vector<char *> a = take_flags((int)bound.size(), bound.data(), rc);
        if (rc) return rc;
        int n = (int)a.size();
        char **v = a.data();
        if (!n) {
            ready();
            return show({});
        }
        if (verb(v[0], "auth", "a")) return auth(n - 1, v + 1);
        if (verb(v[0], "new", "n")) return new_summary();  // prompt-hook: prints, never asks
        if (v[0][0] == '-') {
            usage();
            return 2;
        }
        ready();
        if (verb(v[0], "marks", "m")) return marks({v + 1, v + n});
        if (verb(v[0], "absence", "b")) return absence({v + 1, v + n});
        if (verb(v[0], "timetable", "r")) return timetable(n - 1, v + 1);
        // no short alias for next: `n` is `new`
        if (!strcmp(v[0], "next")) return next_lesson();
        if (verb(v[0], "dismiss", "d")) return dismiss(n - 1, v + 1);
        if (verb(v[0], "open", "o")) return open_item(n - 1, v + 1);
        return show({v, v + n});
    } catch (const std::exception &e) {
        fprintf(stderr, CLI_NAME ": %s\n", e.what());
        return 1;
    }
}

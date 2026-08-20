#include "config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "store.h"
#include "term.h"

namespace {
// yellow, non-fatal; to stderr (fd 2) so color follows that tty
void warn(const std::string &msg) {
    fprintf(stderr, "%s\n", c("33", APP_NAME ": config: " + msg, 2).c_str());
}
}  // namespace

namespace {

// every default lives here; seed=false keeps a key out of the written file (protocol, not taste)
const struct Default {
    const char *key, *val;
    bool seed;
} DEFAULTS[] = {
    {"general.notify", "wispctl notify", true},
    {"general.browser", "xdg-open", true},
    {"general.accent", "blue", true},
    {"general.bar", "#070b14", true},
    {"general.limit", "0", true},
    {"general.links", "no", true},
    {"general.blacklist", "anj", true},
    {"general.years", "auto", true},
    {"general.stale_warn", "yes", true},
    {"general.interval", "900", true},  // must match the crontab period; only feeds staleness
    {"general.raw_names", "no", true},
    {"general.date", "%-d. %-m.", true},  // strftime; the year is appended when it differs
    {"general.marks_newest_last", "yes", true},
    {"source.bakalari.url", "", true},
    {"source.bakalari.enabled", "yes", true},
    {"source.bakalari.client_id", "ANDR", false},
    {"source.teams.enabled", "yes", true},
    {"table.time", "yes", true},
    {"table.room", "yes", true},
    {"table.teacher", "yes", true},
    {"school.half_end", "01-31", true},
    {"school.avg_round", "1.5, 2.5, 3.5, 4.5", true},
    {"school.mark_scale", "1-5", true},
    {"school.points", "90, 75, 60, 40", true},
    {"school.absence_warn", "15", true},
    {"school.absence_max", "25", true},
};

// extra is written verbatim after the section's keys: free-form sections have no defaults
const struct {
    const char *name, *comment, *extra;
} SECTIONS[] = {
    {"general", "limit caps whichever listing runs (0 = all); every key here is also a flag", ""},
    {"source.bakalari", "one section per source; enabled = no hides the source everywhere", ""},
    {"source.teams", "", ""},
    {"table", "what a timetable cell carries; each one off makes the grid narrower", ""},
    {"school", "H1 end date MM-DD (year rolls 1 Aug), average rounding floors, mark scale, percent floors 1..4", ""},
    {"key", "single-key mode switch in the tui; \"<char> = feed|marks|table\"",
     "f          = feed\nm          = marks\nt          = timetable\n"},
    {"bind", "your own words for a command; a bind wins over the builtin verb of that name",
     "rozvrh     = timetable\n# t        = next -s\n"},
    {"classes", "short name for a raw class name the heuristic gets wrong; \"<raw> = <short>\"",
     "# left side is the full team/class name as it appears upstream, spaces and all\n"
     "# Cizi jazyk skupina 1 = ANJ\n"},
};

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(sep, p);
        std::string t = trim(s.substr(p, e == std::string::npos ? e : e - p));
        if (!t.empty()) out.push_back(t);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return out;
}

bool known(const std::string &key) {
    if (!key.compare(0, 4, "key.")) return true;     // [key] keys are single tui keystrokes
    if (!key.compare(0, 5, "bind.")) return true;     // [bind] keys are the user's own words
    if (!key.compare(0, 8, "classes.")) return true;  // [classes] keys are raw class names
    for (const auto &d : DEFAULTS)
        if (key == d.key) return true;
    return false;
}

std::string seed_file() {
    std::string o = "# " APP_NAME " config - [section] headers, \"key = value\", # comments.\n"
                    "# unknown keys warn on stderr; see .map/config.md for what each one does.\n";
    for (const auto &s : SECTIONS) {
        o += "\n";
        if (*s.comment) o += std::string("# ") + s.comment + "\n";
        o += "[" + std::string(s.name) + "]\n";
        std::string prefix = std::string(s.name) + ".";
        for (const auto &d : DEFAULTS) {
            std::string k = d.key;
            if (!d.seed || k.compare(0, prefix.size(), prefix) || k.find('.', prefix.size()) != std::string::npos)
                continue;
            std::string name = k.substr(prefix.size());
            name.resize(name.size() < 10 ? 10 : name.size(), ' ');
            o += name + " = " + d.val + "\n";
        }
        o += s.extra;
    }
    return o;
}

void parse(std::istream &f, Config &c, bool quiet = false) {
    std::string section, l;
    while (std::getline(f, l)) {
        std::string t = trim(l);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            size_t e = t.find(']');
            if (e == std::string::npos) {
                if (!quiet) warn("unclosed section: " + t);
                continue;
            }
            section = lower(trim(t.substr(1, e - 1)));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            if (!quiet) warn("ignoring line without '=': " + t);
            continue;
        }
        if (section.empty()) {
            if (!quiet) warn("ignoring line before any [section]: " + t);
            continue;
        }
        std::string k = section + "." + lower(trim(t.substr(0, eq))), v = trim(t.substr(eq + 1));
        if (!quiet && !known(k)) warn("unknown key '" + k + "'");
        c.v[k] = v;
    }
}

void load(Config &c) {
    for (const auto &d : DEFAULTS) c.v[d.key] = d.val;
    std::ifstream f(config_path());
    if (!f) {
        std::ofstream o(config_path());
        o << seed_file();
        std::ifstream f2(config_path());
        f.swap(f2);
        if (!f) return;
    }
    parse(f, c);
}

// school-year ordering of a MM*100+DD date, so January sorts after November
int roll(int md) { return md >= 801 ? md : md + 1200; }

int half(const struct tm &tm) {
    std::string b = config().str("school.half_end");
    int m = atoi(b.c_str()), d = atoi(b.c_str() + b.find('-') + 1);
    return roll((tm.tm_mon + 1) * 100 + tm.tm_mday) > roll(m * 100 + d) ? 2 : 1;
}

}  // namespace

std::string Config::str(const std::string &key) const {
    auto it = v.find(key);
    return it == v.end() ? std::string() : it->second;
}

int Config::num(const std::string &key) const { return atoi(str(key).c_str()); }

std::string class_override(const std::string &raw) {
    auto it = config().v.find("classes." + lower(raw));  // keys are stored lowercased
    return it == config().v.end() ? std::string() : it->second;
}

bool Config::flag(const std::string &key) const {
    std::string l = lower(str(key));
    return l == "1" || l == "yes" || l == "on" || l == "true";
}

const std::vector<std::string> &Config::list(const std::string &key) const {
    // memoised on the raw value: blacklisted() and the points tables run per item, per render
    static std::map<std::string, std::pair<std::string, std::vector<std::string>>> memo;
    std::string raw = str(key);
    auto it = memo.find(key);
    if (it == memo.end() || it->second.first != raw) {
        std::vector<std::string> out = split(raw, ',');
        for (auto &e : out) e = lower(e);
        it = memo.insert_or_assign(key, std::make_pair(raw, std::move(out))).first;
    }
    return it->second.second;
}

std::string config_path() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string base = xdg && *xdg ? std::string(xdg)
                                   : std::string(home && *home ? home : ".") + "/.config";
    std::string dir = base + "/" APP_NAME;
    mkdir(dir.c_str(), 0700);
    return dir + "/config";
}

bool config_save(const std::string &key, const std::string &val) {
    size_t dot = key.rfind('.');
    if (dot == std::string::npos) return false;
    std::string section = key.substr(0, dot), name = key.substr(dot + 1);
    std::vector<std::string> lines;
    {
        std::ifstream f(config_path());
        for (std::string l; std::getline(f, l);) lines.push_back(l);
    }
    std::string cur = "general";
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; i++) {
        std::string t = trim(lines[i]);
        if (!t.empty() && t[0] == '[') {
            size_t e = t.find(']');
            if (e != std::string::npos) cur = lower(trim(t.substr(1, e - 1)));
            continue;
        }
        if (cur != section || t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos || lower(trim(t.substr(0, eq))) != name) continue;
        lines[i] = name + " = " + val;
        done = true;
    }
    if (!done) lines.push_back("[" + section + "]"), lines.push_back(name + " = " + val);
    std::ofstream o(config_path());
    if (!o) return false;
    for (const std::string &l : lines) o << l << "\n";
    if (!o) return false;
    config().set(key, val);
    return true;
}

Config &config() {
    static Config c = [] {
        Config x;
        load(x);
        return x;
    }();
    return c;
}

std::string school_year(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    int y = tm.tm_year + 1900 - (tm.tm_mon < 7 ? 1 : 0);  // rolls on 1 August
    char b[8];
    snprintf(b, sizeof b, "%02d/%02d", y % 100, (y + 1) % 100);
    return b;
}

long long school_year_start(const std::string &label) {
    int y = atoi(label.c_str());
    if (y < 1 || y > 99) return 0;
    struct tm tm {};
    tm.tm_year = 2000 + y - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;
    return (long long)mktime(&tm);
}

std::string period_label(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    return school_year(t) + " · H" + std::to_string(half(tm));
}

std::vector<std::string> active_years() {
    std::vector<std::string> y = config().list("general.years");
    if (y.size() == 1 && y[0] == "auto") y.clear();
    return y.empty() ? std::vector<std::string>{school_year((long long)time(nullptr))} : y;
}

long long scrape_since() {
    long long earliest = 0;
    for (const auto &y : active_years()) {
        long long s = school_year_start(y);
        if (s && (!earliest || s < earliest)) earliest = s;
    }
    return earliest ? earliest : (long long)time(nullptr) - 60 * 86400;
}

bool blacklisted(const std::string &klass, const std::string &abbrev) {
    std::string k = lower(klass), a = lower(abbrev);
    for (const auto &b : config().list("general.blacklist"))
        if (b == a || k.find(b) != std::string::npos) return true;
    return false;
}

int points_mark(double pct) {
    std::vector<std::string> floors = config().list("school.points");
    for (size_t i = 0; i < floors.size(); i++)
        if (pct >= atof(floors[i].c_str())) return (int)i + 1;
    return (int)floors.size() + 1;
}

int avg_mark(double avg) {
    std::vector<std::string> floors = config().list("school.avg_round");
    int m = 1;
    for (const auto &f : floors)
        if (avg >= atof(f.c_str())) m++;
    return m;
}

std::pair<char, char> mark_scale() {
    std::string s = config().str("school.mark_scale");
    size_t d = s.find('-');
    if (d == std::string::npos || d == 0 || d + 1 >= s.size()) return {'1', '5'};
    return {s[0], s[d + 1]};
}

int config_check() {
    Config c;
    std::istringstream in("# c\n[general]\nlimit = 7\nlinks = yes\nblacklist = ANJ, Mat\n"
                          "[source.bakalari]\nurl=http://x\nenabled = no\n[school]\npoints=80,60\n");
    parse(in, c, true);
    if (c.num("general.limit") != 7 || !c.flag("general.links") ||
        c.list("general.blacklist") != std::vector<std::string>{"anj", "mat"} ||
        c.str("source.bakalari.url") != "http://x" || c.flag("source.bakalari.enabled") ||
        !c.str("general.missing").empty())
        return 1;

    Config head;  // keys before any header are dropped, not filed under [general]
    std::istringstream lin("limit = 3\n[general]\nnotify = x\n");
    parse(lin, head, true);
    if (head.num("general.limit") != 0 || head.str("general.notify") != "x") return 1;

    if (html_unescape("&#xD800;a&#65;") != "&#xD800;aA") return 1;  // lone surrogate stays literal

    if (points_mark(95) != 1 || points_mark(80) != 2 || points_mark(10) != 5) return 1;
    if (mark_scale() != std::pair<char, char>{'1', '5'}) return 1;
    if (avg_mark(1.49) != 1 || avg_mark(1.5) != 2 || avg_mark(2.49) != 2 || avg_mark(5) != 5)
        return 1;
    if (seed_file().find("client_id") != std::string::npos) return 1;
    if (seed_file().find("[source.teams]\nenabled") == std::string::npos) return 1;

    {  // config_save round-trip, in a throwaway config home
        std::string dir = "/tmp/" APP_NAME "-cfgcheck-" + std::to_string(getpid());
        mkdir(dir.c_str(), 0700);
        const char *old = getenv("XDG_CONFIG_HOME");
        std::string keep = old ? old : "";
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
        std::ofstream(config_path()) << "[source.bakalari]\nurl        = \n";
        bool ok = config_save("source.bakalari.url", "http://y");
        Config back;
        std::ifstream f(config_path());
        parse(f, back, true);
        unlink(config_path().c_str());
        rmdir((dir + "/" APP_NAME).c_str());
        rmdir(dir.c_str());
        if (old) setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
        else unsetenv("XDG_CONFIG_HOME");
        if (!ok || back.str("source.bakalari.url") != "http://y") return 1;
    }
    return 0;
}

#include "view.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>

#include "classify.h"
#include "config.h"
#include "http.h"
#include "oauth.h"

namespace view {
namespace {

// fallback threshold when no daemon has ever published a poll rate (cron, manual runs)
const long long STALE_AFTER = 6 * 3600;

// one skipped tick is jitter, two means nobody is fetching
long long stale_after(Store &s) {
    long long iv = atoll(s.get_state("daemon.interval").c_str());
    return iv ? iv * 2 : STALE_AFTER;
}

// the wall clock runs while the box is off; only time we were awake could have held a poll
long long stale_age(long long at) {
    long long age = (long long)time(nullptr) - at, up = 0;
    if (FILE *f = fopen("/proc/uptime", "r")) {
        double u = 0;
        if (fscanf(f, "%lf", &u) == 1) up = (long long)u;
        fclose(f);
    }
    return up > 0 && up < age ? up : age;
}

}  // namespace

std::string fold(const std::string &s) { return classify::norm(s); }

std::string abbrev(const std::string &raw) {
    if (config().flag("general.raw_names")) return raw;
    std::string over = class_override(raw);
    if (!over.empty()) return over;
    for (size_t i = 0; i < raw.size();) {
        size_t e = i;
        while (e < raw.size() && isupper((unsigned char)raw[e])) e++;
        if (e - i >= 2 && e - i <= 4 && (e == raw.size() || !isalnum((unsigned char)raw[e])))
            return raw.substr(i, e - i);
        i = e;
        while (i < raw.size() && !isupper((unsigned char)raw[i])) i++;
    }
    return raw;
}

bool matches(const Item &i, const std::vector<std::string> &filters) {
    for (const auto &f : filters)
        if (f != fold(i.kind) && f != fold(abbrev(i.klass)) && f != fold(i.source)) return false;
    return true;
}

long long watermark(Store &s, const char *key) { return atoll(s.get_state(key).c_str()); }

void set_watermark(Store &s, const char *key, const std::vector<Item> &items) {
    long long m = watermark(s, key);
    for (const auto &i : items) m = std::max(m, i.id);
    s.set_state(key, std::to_string(m));
}

std::string ymd_local(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

std::string monday_of(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    tm.tm_mday += tm.tm_wday == 0 ? -6 : 1 - tm.tm_wday;
    return ymd_local((long long)mktime(&tm));
}

std::string wanted_monday() {
    time_t t = time(nullptr);
    struct tm tm {};
    localtime_r(&t, &tm);
    return monday_of(t + (tm.tm_wday == 0 ? 1 : tm.tm_wday == 6 ? 2 : 0) * 86400);
}

// epoch() is utc midnight, so format from noon to stay on the same date
std::string ymd_plus(const std::string &date, int days) {
    return ymd_local(classify::epoch(date) + days * 86400 + 43200);
}

long long local_at(const std::string &date, const std::string &hm) {
    if (date.size() < 10 || hm.size() < 4) return 0;
    struct tm tm {};
    if (!strptime((date.substr(0, 10) + " " + hm).c_str(), "%Y-%m-%d %H:%M", &tm)) return 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t == (time_t)-1 ? 0 : (long long)t;
}

std::string rel_span(long long secs) {
    if (secs < 90) return std::to_string(secs) + "s";
    if (secs < 5400) return std::to_string(secs / 60) + "m";
    if (secs < 172800) return std::to_string(secs / 3600) + "h";
    return std::to_string(secs / 86400) + "d";
}

std::string ago(long long t) {
    return t ? rel_span((long long)time(nullptr) - t) + " ago" : "never";
}

std::string dur(long long s) {
    if (s % 3600 == 0) return std::to_string(s / 3600) + "h";
    if (s % 60 == 0) return std::to_string(s / 60) + "m";
    return std::to_string(s) + "s";
}

int points_of(const std::string &t) {
    size_t sl = t.find('/');
    if (sl == std::string::npos) return 0;
    double got = atof(t.c_str()), max = atof(t.c_str() + sl + 1);
    return max <= 0 ? 0 : points_mark(100 * got / max);
}

double mark_value(const std::string &t) {
    if (int m = points_of(t)) return m;
    if (t.find('/') != std::string::npos) return 0;
    if (t.empty() || t[0] < mark_scale().first || t[0] > mark_scale().second) return 0;
    return (t[0] - mark_scale().first + 1) + (t.size() > 1 && t[1] == '-' ? 0.5 : 0);
}

Feed feed_rows(Store &s, const std::vector<std::string> &filters, size_t limit) {
    Feed f;
    // blacklisted classes drop out before numbering, so `d 4` and `o 4` stay contiguous
    std::vector<std::string> ab, fk, fc, fs;  // abbrev and the folded match keys, per item
    std::vector<std::string> known;
    auto add = [&](std::vector<std::string> &v, const std::string &t) {
        if (std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
        if (std::find(known.begin(), known.end(), t) == known.end()) known.push_back(t);
        return t;
    };
    for (auto &i : s.feed()) {
        std::string a = abbrev(i.klass);
        if (blacklisted(i.klass, a)) continue;
        fk.push_back(add(f.kinds, fold(i.kind)));
        fc.push_back(add(f.classes, fold(a)));
        fs.push_back(add(f.sources, fold(i.source)));
        ab.push_back(std::move(a));
        f.items.push_back(std::move(i));
    }
    for (const auto &w : filters)
        if (std::find(known.begin(), known.end(), w) == known.end()) {
            f.bad_filter = w;  // nothing is read or written past here
            return f;
        }
    auto hit = [&](size_t n) {
        for (const auto &w : filters)
            if (w != fk[n] && w != fc[n] && w != fs[n]) return false;
        return true;
    };

    size_t shown = 0;
    for (size_t n = 0; n < f.items.size(); n++) shown += hit(n);
    if (!shown) return f;

    long long wm = watermark(s, "seen_feed");
    set_watermark(s, "seen_feed", f.items);

    // the cap keeps the tail: the feed ends with the most urgent items
    size_t skip = limit && shown > limit ? shown - limit : 0;
    for (size_t n = 0; n < f.items.size(); n++) {
        const Item &i = f.items[n];
        if (!hit(n)) continue;
        if (skip) {
            skip--;
            continue;
        }
        f.rows.push_back({n + 1, i.id > wm,
                          !i.due_at ? 0 : i.due_at >= time(nullptr) ? 1 : 2, ab[n]});
    }
    return f;
}

Marks marks_rows(Store &s, const std::vector<std::string> &filters, size_t limit,
                 bool consume_new) {
    Marks out;
    std::vector<Item> all = s.marks();
    std::vector<Item> items;
    std::vector<std::string> ab;
    for (auto &i : all) {
        std::string a = abbrev(i.klass);
        if (!filters.empty()) {
            if (std::find(filters.begin(), filters.end(), fold(a)) == filters.end()) continue;
        } else if (blacklisted(i.klass, a)) continue;
        ab.push_back(std::move(a));
        items.push_back(std::move(i));
    }
    // averages come from every mark of the subject, not just the ones the limit leaves visible
    std::map<std::string, std::map<std::string, std::pair<double, double>>> acc;
    for (size_t n = 0; n < items.size(); n++) {
        const Item &i = items[n];
        double v = mark_value(i.title.substr(0, i.title.find(' ')));
        if (v <= 0) continue;
        auto &a = acc[ab[n]][period_label(i.event_at ? i.event_at : i.fetched_at)];
        a.first += i.weight * v;
        a.second += i.weight;
    }
    for (const auto &[k, per] : acc)
        for (const auto &[p, a] : per) out.averages.push_back({k, p, a.first / a.second});

    if (limit && items.size() > limit) items.resize(limit);  // newest first, so the head is the cap

    long long wm = watermark(s, "seen_marks");
    if (consume_new) set_watermark(s, "seen_marks", all);

    for (size_t n = 0; n < items.size(); n++) {
        const Item &i = items[n];
        MarkRow r;
        r.is_new = i.id > wm;
        r.event_at = i.event_at;
        r.period = period_label(i.event_at ? i.event_at : i.fetched_at);
        r.klass = ab[n];
        r.mark = i.title.substr(0, i.title.find(' '));
        std::string caption = trim(i.title.substr(r.mark.size())), body = trim(i.body);
        r.note = caption + (body.empty() ? "" : (caption.empty() ? "" : " — ") + body);
        out.rows.push_back(std::move(r));
    }
    if (config().flag("general.marks_newest_last")) std::reverse(out.rows.begin(), out.rows.end());

    for (const Absence &a : s.absences())
        if (std::find(ab.begin(), ab.end(), a.subject) != ab.end()) out.absences.push_back(a);
    return out;
}

std::vector<Absence> absence_rows(Store &s, const std::vector<std::string> &filters) {
    std::vector<Absence> out;
    for (Absence &a : s.absences()) {
        if (!filters.empty()) {
            if (std::find(filters.begin(), filters.end(), fold(a.subject)) == filters.end()) continue;
        } else if (blacklisted(a.subject, abbrev(a.subject))) continue;
        out.push_back(std::move(a));
    }
    return out;
}

void compact(Timetable &t) {
    // an hour nobody has all week (a 0th hour, the long tail) is not a column worth its width
    auto used = [&](size_t d, size_t h) { return t.grid[d * t.hours.size() + h] != nullptr; };
    std::vector<size_t> keep_h, keep_d;
    for (size_t h = 0; h < t.hours.size(); h++)
        for (size_t d = 0; d < t.days.size(); d++)
            if (used(d, h)) { keep_h.push_back(h); break; }
    for (size_t d = 0; d < t.days.size(); d++)
        for (size_t h = 0; h < t.hours.size(); h++)
            if (used(d, h)) { keep_d.push_back(d); break; }
    std::vector<const Lesson *> grid;
    for (size_t d : keep_d)
        for (size_t h : keep_h) grid.push_back(t.grid[d * t.hours.size() + h]);
    std::vector<std::string> days, hours;
    for (size_t d : keep_d) days.push_back(t.days[d]);
    for (size_t h : keep_h) hours.push_back(t.hours[h]);
    t.days = days;
    t.hours = hours;
    t.grid = grid;
}

Timetable timetable(Store &s, const std::string &monday) {
    Timetable t;
    t.monday = monday.empty() ? wanted_monday() : monday;
    t.rows = s.lessons(t.monday, ymd_plus(t.monday, 6));
    t.permanent = t.monday == PERM_MONDAY;  // picked explicitly: the stored week is the grid
    if (t.rows.empty()) {  // out of season, or a week outside what maild fetched
        t.rows = s.lessons(PERM_MONDAY, PERM_SUNDAY);
        t.permanent = !t.rows.empty();
        // PERM_MONDAY is the 5th: the day of month carries the weekday offset
        for (Lesson &l : t.rows) l.date = ymd_plus(t.monday, atoi(l.date.c_str() + 8) - 5);
    }
    // rows arrive date-major, hour ascending: both axes come out in display order
    for (const Lesson &l : t.rows) {
        if (t.days.empty() || t.days.back() != l.date) t.days.push_back(l.date);
        if (std::find(t.hours.begin(), t.hours.end(), l.hour) == t.hours.end())
            t.hours.push_back(l.hour);
    }
    std::sort(t.hours.begin(), t.hours.end(), [](const std::string &a, const std::string &b) {
        return atoi(a.c_str()) < atoi(b.c_str());
    });
    t.grid.assign(t.days.size() * t.hours.size(), nullptr);
    for (const Lesson &l : t.rows) {
        if (l.subject.empty()) continue;  // free periods are stored as empty lessons
        size_t d = std::find(t.days.begin(), t.days.end(), l.date) - t.days.begin();
        size_t h = std::find(t.hours.begin(), t.hours.end(), l.hour) - t.hours.begin();
        t.grid[d * t.hours.size() + h] = &l;
    }
    compact(t);
    return t;
}

Next next_lesson(Store &s) {
    Next out;
    long long now = (long long)time(nullptr);
    bool any_time = false;
    std::string mon = monday_of(now);
    // this week and the next, the two weeks maild stores
    std::vector<Lesson> rows = s.lessons(mon, ymd_plus(mon, 13));
    for (const Lesson &l : rows) {
        if (l.state == "x") continue;
        long long t = local_at(l.date, l.begins);
        any_time |= t != 0;
        if (!t || t <= now || (out.start && t >= out.start)) continue;
        out.start = t;
        out.lesson = l;
    }
    if (rows.empty()) return out;  // holidays, or nothing fetched yet: `auth` shows staleness
    if (!any_time) {
        out.state = Next::NoTimes;
        return out;
    }
    if (out.start) out.state = Next::Ok;  // else nothing left this week or next
    return out;
}

// every source that ran last failed for lack of connectivity: not an error, just no net
bool offline(Store &s) {
    bool any = false;
    for (const Source &src : sources()) {
        Store::Fetch f = s.last_fetch(src.name);
        if (!f.at) continue;
        if (f.ok || f.error.rfind(OFFLINE_TAG, 0) != 0) return false;
        any = true;
    }
    return any;
}

std::vector<SourceStatus> status(Store &s) {
    std::vector<SourceStatus> out;
    bool off = offline(s);
    for (const Source &src : sources()) {
        SourceStatus st;
        st.name = src.name;
        st.pretty = src.pretty;
        st.error = src.session_error();
        st.signed_in = src.have_session();
        if (st.signed_in && st.error.empty()) st.refreshed_at = oauth::last_refresh_at(src.name);
        st.fetched_at = s.last_ok_fetch(src.name);
        st.offline = off;
        st.stale = !st.fetched_at || stale_age(st.fetched_at) > stale_after(s);
        out.push_back(std::move(st));
    }
    return out;
}

std::vector<Gripe> gripes(Store &s) {
    std::vector<Gripe> out;
    if (offline(s)) return {{"no internet, showing last data", false}};
    long long limit = stale_after(s), iv = atoll(s.get_state("daemon.interval").c_str());
    std::string rate = iv ? " (" + dur(iv) + ")" : "";
    for (const Source &source : sources()) {
        const char *src = source.name;
        Store::Fetch f = s.last_fetch(src);
        if (!f.at) {
            if (config().flag("general.stale_warn"))
                out.push_back({std::string(src) + ": never fetched, is " APP_NAME "d running?",
                               false});
        } else if (!f.ok) {
            out.push_back({std::string(src) + ": fetch failing since " +
                               ago(f.failing_since ? f.failing_since : f.at) +
                               (f.error.empty() ? "" : " — " + f.error),
                           true});
        } else if (stale_age(f.at) > limit && config().flag("general.stale_warn")) {
            out.push_back({std::string(src) + ": data is older than the poll rate" + rate +
                               ", is " APP_NAME "d running?",
                           false});
        }
    }
    return out;
}

NewCounts new_counts(Store &s) {
    NewCounts n;
    for (const auto &i : s.feed(watermark(s, "seen_feed")))
        if (!blacklisted(i.klass, abbrev(i.klass)))
            (i.kind == "task" || i.kind == "test" ? n.work : n.msgs)++;
    for (const auto &i : s.marks(watermark(s, "seen_marks")))
        if (!blacklisted(i.klass, abbrev(i.klass))) n.marks++;
    for (const Source &src : sources())
        if (!src.session_error().empty()) n.unsigned_pretty.push_back(src.pretty);
    return n;
}

}  // namespace view

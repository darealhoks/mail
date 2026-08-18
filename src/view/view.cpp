#include "view.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>

#include "classify.h"
#include "config.h"
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

std::string ago(long long t) {
    if (!t) return "never";
    long long d = (long long)time(nullptr) - t;
    if (d < 90) return std::to_string(d) + "s ago";
    if (d < 5400) return std::to_string(d / 60) + "m ago";
    if (d < 172800) return std::to_string(d / 3600) + "h ago";
    return std::to_string(d / 86400) + "d ago";
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
    f.items = s.feed();
    // blacklisted classes drop out before numbering, so `d 4` and `o 4` stay contiguous
    f.items.erase(std::remove_if(f.items.begin(), f.items.end(),
                                 [](const Item &i) { return blacklisted(i.klass, abbrev(i.klass)); }),
                  f.items.end());
    std::vector<std::string> known;
    auto add = [&](std::vector<std::string> &v, const std::string &t) {
        if (std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
        if (std::find(known.begin(), known.end(), t) == known.end()) known.push_back(t);
    };
    for (const auto &i : f.items) {
        add(f.kinds, fold(i.kind));
        add(f.classes, fold(abbrev(i.klass)));
        add(f.sources, fold(i.source));
    }
    for (const auto &w : filters)
        if (std::find(known.begin(), known.end(), w) == known.end()) {
            f.bad_filter = w;  // nothing is read or written past here
            return f;
        }

    size_t shown = 0;
    for (const auto &i : f.items) shown += matches(i, filters);
    if (!shown) return f;

    long long wm = watermark(s, "seen_feed");
    set_watermark(s, "seen_feed", f.items);

    // the cap keeps the tail: the feed ends with the most urgent items
    size_t skip = limit && shown > limit ? shown - limit : 0;
    for (size_t n = 0; n < f.items.size(); n++) {
        const Item &i = f.items[n];
        if (!matches(i, filters)) continue;
        if (skip) {
            skip--;
            continue;
        }
        f.rows.push_back({n + 1, i.id > wm,
                          !i.due_at ? 0 : i.due_at >= time(nullptr) ? 1 : 2, abbrev(i.klass)});
    }
    return f;
}

Marks marks_rows(Store &s, const std::vector<std::string> &filters, size_t limit) {
    Marks out;
    std::vector<Item> all = s.marks();
    std::vector<Item> items;
    for (auto &i : all) {
        std::string cl = fold(abbrev(i.klass));
        if (!filters.empty() && std::find(filters.begin(), filters.end(), cl) == filters.end())
            continue;
        if (filters.empty() && blacklisted(i.klass, abbrev(i.klass))) continue;
        items.push_back(std::move(i));
    }
    // averages come from every mark of the subject, not just the ones the limit leaves visible
    std::map<std::string, std::pair<double, double>> avg;  // half-year -> {sum(w*v), sum(w)}
    if (!filters.empty())
        for (const auto &i : items) {
            double v = mark_value(i.title.substr(0, i.title.find(' ')));
            if (v <= 0) continue;
            std::string p = period_label(i.event_at ? i.event_at : i.fetched_at);
            p = p.substr(0, p.rfind(" · "));  // drop the quarter, average is per half-year
            avg[p].first += i.weight * v;
            avg[p].second += i.weight;
        }
    for (const auto &[p, a] : avg) out.averages.push_back({p, a.first / a.second});

    if (limit && items.size() > limit) items.resize(limit);  // newest first, so the head is the cap

    long long wm = watermark(s, "seen_marks");
    set_watermark(s, "seen_marks", all);

    for (const auto &i : items) {
        MarkRow r;
        r.is_new = i.id > wm;
        r.event_at = i.event_at;
        r.period = period_label(i.event_at ? i.event_at : i.fetched_at);
        r.klass = abbrev(i.klass);
        r.mark = i.title.substr(0, i.title.find(' '));
        std::string caption = trim(i.title.substr(r.mark.size())), body = trim(i.body);
        r.note = caption + (body.empty() ? "" : (caption.empty() ? "" : " — ") + body);
        out.rows.push_back(std::move(r));
    }
    return out;
}

Timetable timetable(Store &s) {
    Timetable t;
    t.monday = wanted_monday();
    t.rows = s.lessons(t.monday, ymd_plus(t.monday, 6));
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
        size_t d = std::find(t.days.begin(), t.days.end(), l.date) - t.days.begin();
        size_t h = std::find(t.hours.begin(), t.hours.end(), l.hour) - t.hours.begin();
        t.grid[d * t.hours.size() + h] = &l;
    }
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

std::vector<SourceStatus> status(Store &s) {
    std::vector<SourceStatus> out;
    for (const Source &src : sources()) {
        SourceStatus st;
        st.name = src.name;
        st.pretty = src.pretty;
        st.error = src.session_error();
        st.signed_in = src.have_session();
        if (st.signed_in && st.error.empty()) st.refreshed_at = oauth::last_refresh_at(src.name);
        st.fetched_at = s.last_ok_fetch(src.name);
        st.stale = !st.fetched_at || (long long)time(nullptr) - st.fetched_at > stale_after(s);
        out.push_back(std::move(st));
    }
    return out;
}

std::vector<Gripe> gripes(Store &s) {
    std::vector<Gripe> out;
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
            out.push_back({std::string(src) + ": last fetch failed " + ago(f.at) +
                               (f.error.empty() ? "" : " — " + f.error),
                           true});
        } else if ((long long)time(nullptr) - f.at > limit && config().flag("general.stale_warn")) {
            out.push_back({std::string(src) + ": data is older than the poll rate" + rate +
                               ", is " APP_NAME "d running?",
                           false});
        }
    }
    return out;
}

NewCounts new_counts(Store &s) {
    NewCounts n;
    long long fw = watermark(s, "seen_feed"), mw = watermark(s, "seen_marks");
    for (const auto &i : s.feed()) {
        if (i.id <= fw || blacklisted(i.klass, abbrev(i.klass))) continue;
        (i.kind == "task" || i.kind == "test" ? n.work : n.msgs)++;
    }
    for (const auto &i : s.marks())
        if (i.id > mw && !blacklisted(i.klass, abbrev(i.klass))) n.marks++;
    for (const Source &src : sources())
        if (!src.session_error().empty()) n.unsigned_pretty.push_back(src.pretty);
    return n;
}

}  // namespace view

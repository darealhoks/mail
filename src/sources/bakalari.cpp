#include "bakalari.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

#include <unistd.h>

#include "classify.h"
#include "config.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "term.h"
#include "teams.h"

namespace bakalari {
namespace {

const char *CREDS = "bakalari";

std::string base() {
    std::string u = config().str("source.bakalari.url");
    // creds must not go out over the plaintext first hop curl would guess
    if (u.empty()) return u;
    if (u.compare(0, 8, "https://") == 0) return u;
    if (u.compare(0, 7, "http://") == 0) throw std::runtime_error("bakalari: url must be https");
    u = "https://" + u;
    return u;
}

// built per call: the url can be set from the tui sign-in, after static init has run
oauth::Config cfg() {
    return {CREDS, config().str("source.bakalari.client_id"), base() + "/api/login", ""};
}

long long now() { return (long long)time(nullptr); }

using oauth::form;

std::string access_token() {
    return oauth::access_token(cfg(), [](long status, const Json &) {
        return status == 400 || status == 401;
    });
}

std::string api(const std::string &path, bool post = false, const std::string &body = "") {
    std::vector<std::string> h{"Authorization: Bearer " + access_token()};
    HttpResponse r = post ? http_post_form(base() + path, body, h)
                          : http_get(base() + path, h);
    if (r.status == 401) throw SessionExpired("bakalari: token rejected on " + path);
    if (r.status != 200)
        throw std::runtime_error("bakalari: " + path + " -> http " + std::to_string(r.status));
    return r.body;
}

using El = simdjson::dom::element;

// ids come back as strings on some endpoints and as numbers on others
std::string s(El e, const char *k) {
    auto v = e.at_key(k).get_string();
    if (!v.error()) return std::string(v.value());
    auto n = e.at_key(k).get_int64();
    return n.error() ? std::string() : std::to_string(n.value());
}

// sub-object lookups chain, so accept the result type too; a missing object reads as empty
std::string s(simdjson::simdjson_result<El> e, const char *k) {
    El v;
    return e.get(v) ? std::string() : s(v, k);
}

// counts come back as numbers, but a missing or non-numeric one must read as zero
double num(El e, const char *k) {
    double d = 0;
    if (e.at_key(k).get(d)) {
        int64_t i = 0;
        return e.at_key(k).get(i) ? 0 : (double)i;
    }
    return d;
}

simdjson::dom::array arr(El e, const char *k, const char *what) {
    auto a = e.at_key(k).get_array();
    if (a.error()) throw std::runtime_error(std::string("bakalari: ") + what + " missing array " + k);
    return a.value();
}


std::string ymd(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

const long long DAY = 86400;

// every timetable payload (actual and permanent alike) ships the same five side tables
struct Side {
    std::map<std::string, std::string> subject, subject_name, room, hour, begins, ends, teacher,
        teacher_name;
};

Side side_tables(El root, std::map<std::string, std::string> *by_name) {
    Side t;
    // Subjects[].Id is space-padded (" 7") while Atoms[].SubjectId is not
    for (auto sub : arr(root, "Subjects", "timetable")) {
        std::string id = trim(s(sub, "Id"));
        t.subject[id] = s(sub, "Abbrev");
        t.subject_name[id] = s(sub, "Name");
        if (by_name && !t.subject_name[id].empty() && !t.subject[id].empty())
            (*by_name)[t.subject_name[id]] = t.subject[id];
    }
    for (auto r : arr(root, "Rooms", "timetable")) t.room[trim(s(r, "Id"))] = s(r, "Abbrev");
    for (auto e : arr(root, "Teachers", "timetable")) {
        std::string id = trim(s(e, "Id"));
        t.teacher[id] = s(e, "Abbrev");
        t.teacher_name[id] = s(e, "Name");
    }
    for (auto h : arr(root, "Hours", "timetable")) {
        std::string id = trim(s(h, "Id"));
        t.hour[id] = s(h, "Caption");
        t.begins[id] = s(h, "BeginTime");
        t.ends[id] = s(h, "EndTime");
    }
    return t;
}

Lesson lesson_of(Side &t, El at, const std::string &date) {
    std::string hid = trim(s(at, "HourId")), sid = trim(s(at, "SubjectId"));
    Lesson l;
    l.date = date;
    l.hour = t.hour[hid];
    l.subject = t.subject[sid];
    l.subject_name = t.subject_name[sid];
    l.room = t.room[trim(s(at, "RoomId"))];
    l.teacher = t.teacher[trim(s(at, "TeacherId"))];
    l.teacher_name = t.teacher_name[trim(s(at, "TeacherId"))];
    l.begins = t.begins[hid];
    l.ends = t.ends[hid];
    return l;
}

}  // namespace


void login(const std::string &user, const std::string &pass) {
    std::string body = form("client_id", cfg().client_id) + "&" + form("grant_type", "password") + "&" +
                       form("username", user) + "&" + form("password", pass);
    HttpResponse r = http_post_form(base() + "/api/login", body);
    Json j(r.body);
    if (r.status != 200)
        throw std::runtime_error("bakalari: login failed (http " + std::to_string(r.status) + " " +
                                 j.str("error_description", j.str("error", "unknown")) + ")");
    oauth::save_tokens(cfg(), j);
}

bool have_session() { return oauth::have_session(cfg()); }

// one line of input, trimmed; empty (or eof) means the user backed out
bool ask(const char *label, std::string &out) {
    char buf[256];
    printf("%s ", c("1", label).c_str());
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) return false;
    out = buf;
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    return !out.empty();
}

int login_interactive() {
    printf("%s\n", c("1;33", "Bakalari").c_str());
    if (base().empty()) {
        std::string url;
        if (!ask("Url:", url)) return 1;
        if (!config_save("source.bakalari.url", url))
            fprintf(stderr, "%s\n", c("1;31", "could not write " + config_path()).c_str());
    } else {
        printf("%s\n", c("90", "url set in config, change in " + config_path()).c_str());
    }
    std::string u;
    if (!ask("Username:", u)) return 1;
    char *pass = getpass((c("1", "Password:") + " ").c_str());
    if (!pass || !*pass) return 1;
    std::string p(pass);
    memset(pass, 0, p.size());
    login(u, p);
    printf("%s %s\n", c("1;33", "Bakalari").c_str(), c("1;32", "signed in").c_str());
    return 0;
}

std::vector<Item> fetch(Store &store) {
    if (base().empty())
        throw std::runtime_error("bakalari: set [source.bakalari] url in the config first");
    std::vector<Item> out;

    // marks?from, komens dateFrom and homeworks?from filter on the item's own date, so an old
    // item edited today would never come back incrementally: re-pull everything every sweep.
    // wall clock, not a tick count, so the first tick after the laptop was off still sweeps;
    // an unset key is a cold start, and that is what maild --cold clears to force one
    long long swept = strtoll(store.get_state("bakalari.swept_at").c_str(), nullptr, 10);
    bool full = now() - swept > 2 * 3600;
    std::string since = ymd(now() - 30 * DAY);

    {
        long long from = full ? scrape_since() : now() - 30 * DAY;
        Json j(api("/api/3/homeworks?from=" + ymd(from) + "&to=" + ymd(now() + 60 * DAY)));
        for (auto e : arr(j.root, "Homeworks", "homeworks")) {
            std::string id = s(e, "ID");
            if (id.empty()) continue;
            Item i;
            i.source = "bakalari";
            i.kind = "task";
            i.klass = trim(s(e.at_key("Subject"), "Abbrev"));
            i.body = teams::plain_text(s(e, "Content"));
            i.title = i.body.substr(0, i.body.find('\n'));
            i.due_at = classify::epoch(s(e, "DateEnd"));
            i.src_uid = "hw:" + id;
            out.push_back(std::move(i));
        }
    }

    {
        Json j(api(full ? "/api/3/marks" : "/api/3/marks?from=" + since));
        for (auto subj : arr(j.root, "Subjects", "marks")) {
            std::string abbrev = trim(s(subj.at_key("Subject"), "Abbrev"));
            for (auto m : arr(subj, "Marks", "marks")) {
                std::string id = s(m, "Id");
                if (id.empty()) continue;
                Item i;
                i.source = "bakalari";
                i.kind = "mark";
                i.klass = abbrev;
                bool points = false;
                if (m.at_key("IsPoints").get(points)) points = false;
                std::string max = s(m, "MaxPoints");
                i.title = s(m, "MarkText") + (points && !max.empty() && max != "0" ? "/" + max : "")
                          + " " + s(m, "Caption");
                i.body = s(m, "Theme");
                i.event_at = classify::epoch(s(m, "MarkDate"));
                long w = strtol(s(m, "Weight").c_str(), nullptr, 10);
                i.weight = (int)std::clamp(w, 1L, 100L);
                i.src_uid = "mark:" + id;
                out.push_back(std::move(i));
            }
        }
    }

    {
        Json j(api("/api/3/komens/messages/received", true,
                   full ? "" : "dateFrom=" + since));
        for (auto e : arr(j.root, "Messages", "komens")) {
            std::string id = s(e, "Id");
            if (id.empty()) continue;
            Item i;
            i.source = "bakalari";
            i.klass = s(e.at_key("Sender"), "Name");
            i.title = s(e, "Title");
            i.body = teams::plain_text(s(e, "Text"));
            // a "see attached" message is empty text: name the files, same marker teams writes
            simdjson::dom::array atts;
            if (!e.at_key("Attachments").get_array().get(atts)) {
                size_t na = 0;
                for (auto a : atts) {
                    std::string name = trim(s(a, "Name"));
                    if (!name.empty()) i.body += (na++ ? ", " : " [att: ") + name;
                }
                if (na) i.body += "]";
            }
            std::string sent = s(e, "SentDate");
            classify::Result c = classify::run(i.title + " || " + i.body, false, sent);
            i.kind = classify::kind(c);
            i.due_at = classify::epoch(c.deadline);
            i.event_at = classify::epoch(sent);
            i.src_uid = "komens:" + id;
            out.push_back(std::move(i));
        }
    }

    // this week and the next; the lessons table is what `mailc timetable` renders
    std::map<std::string, std::string> by_name;  // full subject name -> abbrev, for absences
    for (long long week : {now(), now() + 7 * DAY}) {
        Json j(api("/api/3/timetable/actual?date=" + ymd(week)));
        Side S = side_tables(j.root, &by_name);
        std::vector<Lesson> grid;
        std::string monday;
        for (auto day : arr(j.root, "Days", "timetable")) {
            std::string dd = s(day, "Date").substr(0, 10);
            if (monday.empty()) monday = dd;
            for (auto at : arr(day, "Atoms", "timetable")) {
                simdjson::dom::element ch;
                bool changed = !at.at_key("Change").get(ch) && !ch.is_null();
                std::string type = changed ? s(ch, "ChangeType") : "";
                std::string what = changed ? s(ch, "TypeName") : "";
                if (what.empty()) what = type;
                std::string desc = changed ? s(ch, "Description") : "";
                Lesson l = lesson_of(S, at, dd);
                l.state = type == "Canceled" || type == "Removed" ? "x" : changed ? "!" : "";
                l.change = changed ? trim(desc.empty() ? what : what + ": " + desc) : "";
                if (!l.hour.empty() && !l.subject.empty()) grid.push_back(std::move(l));
                if (!changed) continue;
                // a change listed under one day can carry another day's date
                std::string date = s(ch, "Day");
                if (date.empty()) date = s(day, "Date");
                Item i;
                i.source = "bakalari";
                i.kind = "change";
                i.klass = S.subject[trim(s(at, "SubjectId"))];
                i.title = desc.empty() ? what : what + ": " + desc;
                i.body = trim(s(ch, "Hours") + " " + s(ch, "Time"));
                i.event_at = classify::epoch(date);
                i.src_uid = "tt:" + date.substr(0, 10) + ":" + s(at, "HourId") + ":" + type;
                out.push_back(std::move(i));
            }
        }
        if (!monday.empty())
            store.put_lessons("bakalari", monday, ymd(classify::epoch(monday) + 6 * DAY), grid);
    }

    // the recurring grid, what the table falls back to out of season. it changes per semester,
    // so fetch it only on a full sweep or when nothing is stored yet
    if (full || store.lessons(PERM_MONDAY, PERM_SUNDAY).empty()) {
        Json j(api("/api/3/timetable/permanent"));
        Side S = side_tables(j.root, &by_name);
        std::vector<Lesson> grid;
        int idx = 0;
        for (auto day : arr(j.root, "Days", "timetable")) {
            int dow = (int)num(day, "DayOfWeek");  // 1 = monday; missing means take the order
            int off = dow >= 1 && dow <= 7 ? dow - 1 : idx;
            idx++;
            if (off > 6) continue;
            for (auto at : arr(day, "Atoms", "timetable")) {
                Lesson l = lesson_of(S, at, ymd(classify::epoch(PERM_MONDAY) + off * DAY));
                if (!l.hour.empty() && !l.subject.empty()) grid.push_back(std::move(l));
            }
        }
        store.put_lessons("bakalari-perm", PERM_MONDAY, PERM_SUNDAY, grid);
    }

    {
        Json j(api("/api/3/absence/student"));
        // 0.2 and 20 both mean 20%: which one the school sends is not documented
        double thr = num(j.root, "PercentageThreshold");
        if (thr > 0 && thr <= 1) thr *= 100;
        std::vector<Absence> abs;
        for (auto e : arr(j.root, "AbsencesPerSubject", "absence")) {
            Absence a;
            a.subject = s(e, "SubjectName");
            if (a.subject.empty()) a.subject = "?";  // upstream sends one nameless row; still counts
            auto it = by_name.find(a.subject);
            if (it != by_name.end()) a.subject = it->second;  // marks key on the abbrev
            a.lessons = (int)num(e, "LessonsCount");
            // web's Zameškáno is Base alone; Late/Soon/School are tracked apart and don't count
            a.absent = (int)num(e, "Base");
            if (a.lessons < 0 || a.absent < 0 || a.absent > a.lessons) continue;
            a.threshold = thr;
            abs.push_back(std::move(a));
        }
        store.put_absences("bakalari", abs);
    }

    {
        Json j(api("/api/3/events/my?from=" + ymd(now() - 14 * DAY) +
                   "&to=" + ymd(now() + 60 * DAY)));
        for (auto e : arr(j.root, "Events", "events")) {
            std::string id = s(e, "Id");
            if (id.empty()) continue;
            Item i;
            i.source = "bakalari";
            i.kind = "info";
            i.klass = s(e.at_key("EventType"), "Name");
            i.title = s(e, "Title");
            i.body = teams::plain_text(s(e, "Description"));
            // the start sits in Times[]; some deployments hoist it to the top level
            std::string start = s(e, "StartTime");
            if (start.empty()) {
                auto times = e.at_key("Times").get_array();
                if (!times.error() && times.value().size()) start = s(times.value().at(0), "StartTime");
            }
            i.event_at = classify::epoch(start);
            i.src_uid = "event:" + id;
            out.push_back(std::move(i));
        }
    }

    if (full) store.set_state("bakalari.swept_at", std::to_string(now()));
    return out;
}

}  // namespace bakalari

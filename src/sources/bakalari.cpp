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
    for (long long week : {now(), now() + 7 * DAY}) {
        Json j(api("/api/3/timetable/actual?date=" + ymd(week)));
        std::map<std::string, std::string> subject, subject_name, room, hour, begins, ends, teacher;
        // Subjects[].Id is space-padded (" 7") while Atoms[].SubjectId is not
        for (auto sub : arr(j.root, "Subjects", "timetable")) {
            std::string id = trim(s(sub, "Id"));
            subject[id] = s(sub, "Abbrev");
            subject_name[id] = s(sub, "Name");
        }
        for (auto r : arr(j.root, "Rooms", "timetable")) room[trim(s(r, "Id"))] = s(r, "Abbrev");
        for (auto t : arr(j.root, "Teachers", "timetable")) teacher[trim(s(t, "Id"))] = s(t, "Abbrev");
        for (auto h : arr(j.root, "Hours", "timetable")) {
            std::string id = trim(s(h, "Id"));
            hour[id] = s(h, "Caption");
            begins[id] = s(h, "BeginTime");
            ends[id] = s(h, "EndTime");
        }
        std::vector<Lesson> grid;
        std::string monday;
        for (auto day : arr(j.root, "Days", "timetable")) {
            std::string dd = s(day, "Date").substr(0, 10);
            if (monday.empty()) monday = dd;
            for (auto at : arr(day, "Atoms", "timetable")) {
                simdjson::dom::element ch;
                bool changed = !at.at_key("Change").get(ch) && !ch.is_null();
                std::string type = changed ? s(ch, "ChangeType") : "";
                std::string hid = trim(s(at, "HourId")), sid = trim(s(at, "SubjectId"));
                Lesson l;
                l.date = dd;
                l.hour = hour[hid];
                l.subject = subject[sid];
                l.subject_name = subject_name[sid];
                l.room = room[trim(s(at, "RoomId"))];
                l.teacher = teacher[trim(s(at, "TeacherId"))];
                l.state = type == "Canceled" || type == "Removed" ? "x" : changed ? "!" : "";
                l.begins = begins[hid];
                l.ends = ends[hid];
                if (!l.hour.empty() && !l.subject.empty()) grid.push_back(std::move(l));
                if (!changed) continue;
                // a change listed under one day can carry another day's date
                std::string date = s(ch, "Day");
                if (date.empty()) date = s(day, "Date");
                Item i;
                i.source = "bakalari";
                i.kind = "change";
                i.klass = subject[trim(s(at, "SubjectId"))];
                std::string what = s(ch, "TypeName");
                if (what.empty()) what = type;
                i.title = s(ch, "Description");
                i.title = i.title.empty() ? what : what + ": " + i.title;
                i.body = trim(s(ch, "Hours") + " " + s(ch, "Time"));
                i.event_at = classify::epoch(date);
                i.src_uid = "tt:" + date.substr(0, 10) + ":" + s(at, "HourId") + ":" + type;
                out.push_back(std::move(i));
            }
        }
        if (!monday.empty())
            store.put_lessons("bakalari", monday, ymd(classify::epoch(monday) + 6 * DAY), grid);
    }

    if (full) store.set_state("bakalari.swept_at", std::to_string(now()));
    return out;
}

}  // namespace bakalari

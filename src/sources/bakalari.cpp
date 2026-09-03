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
#include "creds.h"
#include "http.h"
#include "json.h"
#include "oauth.h"
#include "term.h"
#include "text.h"

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

bool expired(long status, const Json &) { return status == 400 || status == 401; }

// the refresh token can die (password change, server-side revoke) while the password still
// works: replay the stored sign-in once instead of making the user re-auth by hand
std::string access_token() {
    try {
        return oauth::access_token(cfg(), expired);
    } catch (const SessionExpired &) {
        auto kv = creds_load(CREDS);
        if (kv["username"].empty() || kv["password"].empty()) throw;
        try {
            login(kv["username"], kv["password"]);
        } catch (const SessionExpired &e) {
            // only an outright rejection drops the password; it would otherwise be replayed on
            // every fetch. a server that is merely unreachable or broken must never cost the
            // user a working credential, so every other failure passes through untouched
            kv.erase("password");
            creds_save(CREDS, kv);
            throw SessionExpired("bakalari: stored password rejected: " + std::string(e.what()));
        }
        return oauth::access_token(cfg(), expired);
    }
}

// soft, when set, takes the http status of a non-200 instead of throwing (401 still retries)
std::string api(const std::string &path, bool post = false, const std::string &body = "",
                long *soft = nullptr) {
    std::string token = access_token();
    bool reminted = false;
    for (;;) {
        std::vector<std::string> h{"Authorization: Bearer " + token};
        HttpResponse r = post ? http_post_form(base() + path, body, h)
                              : http_get(base() + path, h);
        if (r.status == 401) {
            // a valid token can outlive its server-side session; only a 401 on a freshly
            // refreshed token is a dead session
            if (reminted) throw SessionExpired("bakalari: token rejected on " + path);
            reminted = true;
            oauth::forget_access(cfg());
            token = access_token();
            continue;
        }
        if (r.status != 200) {
            if (soft) {
                *soft = r.status;
                return r.body;
            }
            throw std::runtime_error("bakalari: " + path + " -> http " +
                                     std::to_string(r.status));
        }
        return r.body;
    }
}

using El = simdjson::dom::element;

std::string s(El e, const char *k) { return jstr(e, k); }
// sub-object lookups chain, so accept the result type too; a missing object reads as empty
std::string s(simdjson::simdjson_result<El> e, const char *k) {
    El v;
    return e.get(v) ? std::string() : jstr(v, k);
}

// a payload that lost an array we read is a shape change, not an empty result: say so
simdjson::dom::array arr(El e, const char *k, const char *what) {
    auto a = e.at_key(k).get_array();
    if (a.error()) throw std::runtime_error(std::string("bakalari: ") + what + " missing array " + k);
    return a.value();
}

std::string ymd(long long t) { return text::utc(t, "%Y-%m-%d"); }

const long long DAY = 86400;

// the web ui has no per-item deep links, only module pages; base()+path is as close as it gets
std::string web(const char *page) { return base() + "/next/" + page; }

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

struct Change {
    std::string date, type, title;
    std::vector<std::string> klass;
    std::vector<Span> hours;
};

}  // namespace

// the times stay true to the hours actually named: a gap in the run starts a new one
std::string hour_runs(std::vector<Span> h) {
    std::sort(h.begin(), h.end(), [](const Span &a, const Span &b) {
        return a.n != b.n ? a.n < b.n : a.caption < b.caption;
    });
    h.erase(std::unique(h.begin(), h.end(),
                        [](const Span &a, const Span &b) {
                            return a.n == b.n && a.caption == b.caption;
                        }),
            h.end());
    std::string out;
    for (size_t i = 0; i < h.size();) {
        size_t j = i;
        while (j + 1 < h.size() && h[j].n > 0 && h[j + 1].n == h[j].n + 1) j++;
        if (!out.empty()) out += ", ";
        out += h[i].caption + ".";
        if (j > i) out += "-" + h[j].caption + ".";
        out += " hod";
        std::string t = trim(h[i].begins + (h[j].ends.empty() ? "" : " - " + h[j].ends));
        if (!t.empty()) out += " " + t;
        i = j + 1;
    }
    return out;
}

void login(const std::string &user, const std::string &pass) {
    std::string body = form("client_id", cfg().client_id) + "&" + form("grant_type", "password") + "&" +
                       form("username", user) + "&" + form("password", pass);
    HttpResponse r = http_post_form(base() + "/api/login", body);
    Json j(r.body);
    if (r.status != 200) {
        std::string why = "bakalari: login failed (http " + std::to_string(r.status) + " " +
                          j.str("error_description", j.str("error", "unknown")) + ")";
        // 400/401 is the server rejecting these credentials; anything else is its own problem
        if (r.status == 400 || r.status == 401) throw SessionExpired(why);
        throw std::runtime_error(why);
    }
    oauth::save_tokens(cfg(), j);
    // kept for the auto re-sign-in in access_token(); same 0600 file as the refresh token
    auto kv = creds_load(CREDS);
    kv["username"] = user;
    kv["password"] = pass;
    creds_save(CREDS, kv);
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
    printf("%s\n", c("1", "Bakalari").c_str());
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
    printf("%s %s\n", c("1", "Bakalari").c_str(), c("1;32", "signed in").c_str());
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
            i.body = text::plain_text(s(e, "Content"));
            i.title = i.body.substr(0, i.body.find('\n'));
            i.due_at = classify::due_epoch(s(e, "DateEnd"));
            i.url = web("vyuka.aspx");
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
                i.url = web("prubzna.aspx");
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
            i.body = text::plain_text(s(e, "Text"));
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
            i.due_at = classify::due_epoch(c.deadline);
            i.event_at = classify::epoch(sent);
            i.url = web("komens.aspx");
            i.src_uid = "komens:" + id;
            out.push_back(std::move(i));
        }
    }

    // whole-day events (holidays, ředitelské volno) before the grid: they go into the same
    // lessons rows, as a note per day, so an empty week says why it is empty
    std::map<std::string, std::string> day_note;
    {
        // wide enough back that a holiday which started weeks ago still covers today
        Json j(api("/api/3/events/my?from=" + ymd(now() - 90 * DAY) +
                   "&to=" + ymd(now() + 60 * DAY)));
        for (auto e : arr(j.root, "Events", "events")) {
            std::string id = s(e, "Id");
            if (id.empty()) continue;
            // the span sits in Times[]; some deployments hoist the start to the top level
            std::string start = s(e, "StartTime"), end, title = s(e, "Title");
            bool whole = false;
            for (auto t : jarr(e, "Times")) {
                if (start.empty()) start = s(t, "StartTime");
                end = s(t, "EndTime");
                whole = jflag(t, "WholeDay");
                break;
            }
            if (whole && !start.empty() && !title.empty()) {
                long long a = classify::epoch(start);
                long long b = end.empty() ? a : classify::epoch(end);
                // a runaway range would write a note per day for years
                for (long long d = a; d <= b && d - a <= 400 * DAY; d += DAY) day_note[ymd(d)] = title;
            }
            Item i;
            i.source = "bakalari";
            i.kind = "info";
            i.klass = s(e.at_key("EventType"), "Name");
            i.title = title;
            i.body = text::plain_text(s(e, "Description"));
            i.event_at = classify::epoch(start);
            i.url = web("planakci.aspx");
            i.src_uid = "event:" + id;
            out.push_back(std::move(i));
        }
    }

    // this week and the next; the lessons table is what `mailc timetable` renders
    std::map<std::string, std::string> by_name;  // full subject name -> abbrev, for absences
    // keyed date first so the emitted items come out in day order; both weeks share the map
    // because a change listed in one week can carry a date that lands in the other
    std::map<std::string, Change> changes;
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
                if (changed) {
                    // a change listed under one day can carry another day's date
                    std::string date = s(ch, "Day");
                    if (date.empty()) date = s(day, "Date");
                    date = date.substr(0, 10);
                    std::string title = trim(desc.empty() ? what : what + ": " + desc);
                    Change &g = changes[date + "\x1f" + type + "\x1f" + title];
                    g.date = date;
                    g.type = type;
                    g.title = title;
                    std::string cap = trim(l.hour);
                    while (!cap.empty() && cap.back() == '.') cap.pop_back();
                    if (!cap.empty())
                        g.hours.push_back({(int)strtol(cap.c_str(), nullptr, 10), cap, l.begins,
                                           l.ends});
                    if (!l.subject.empty() &&
                        std::find(g.klass.begin(), g.klass.end(), l.subject) == g.klass.end())
                        g.klass.push_back(l.subject);
                }
                if (!l.hour.empty() && !l.subject.empty()) grid.push_back(std::move(l));
            }
        }
        if (monday.empty()) continue;
        std::string sunday = ymd(classify::epoch(monday) + 6 * DAY);
        // an hourless row is a day note, not a lesson; view::timetable splits them back out
        for (const auto &[date, note] : day_note)
            if (date >= monday && date <= sunday) {
                Lesson n;
                n.date = date;
                n.subject = note;
                grid.push_back(std::move(n));
            }
        store.put_lessons("bakalari", monday, sunday, grid);
    }

    for (auto &kv : changes) {
        Change &g = kv.second;
        Item i;
        i.source = "bakalari";
        i.kind = "change";
        for (const auto &k : g.klass) i.klass += (i.klass.empty() ? "" : ", ") + k;
        i.title = g.title;
        i.body = hour_runs(g.hours);
        i.event_at = classify::epoch(g.date);
        i.url = base() + "/timetable";  // the only module not under /next/*.aspx
        i.src_uid = "tt:" + g.date + ":" + g.type + ":" + g.title;
        out.push_back(std::move(i));
    }

    // the recurring grid, what the table falls back to out of season. it changes per semester,
    // so fetch it only on a full sweep or when nothing is stored yet
    if (full || store.lessons(PERM_MONDAY, PERM_SUNDAY).empty()) {
        Json j(api("/api/3/timetable/permanent"));
        Side S = side_tables(j.root, &by_name);
        std::vector<Lesson> grid;
        int idx = 0;
        for (auto day : arr(j.root, "Days", "timetable")) {
            int dow = (int)jnum(day, "DayOfWeek");  // 1 = monday; missing means take the order
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
        // out of the school year's timetable range the module answers 400 "Datum je mimo
        // rozsah rozvrhu.". that is upstream having no data, not a failed fetch — but the
        // stored absences then belong to an older year, so the note is what the ui shows
        long st = 0;
        std::string raw = api("/api/3/absence/student", false, "", &st);
        if (st) {
            store.set_state("bakalari.absence_note",
                            "not updated: " + trim(text::first_line(raw, 80)));
        } else {
            Json j(raw);
            store.set_state("bakalari.absence_note", "");
            // 0.2 and 20 both mean 20%: which one the school sends is not documented
            double thr = jnum(j.root, "PercentageThreshold");
            if (thr > 0 && thr <= 1) thr *= 100;
            std::vector<Absence> abs;
            for (auto e : arr(j.root, "AbsencesPerSubject", "absence")) {
                Absence a;
                a.subject = s(e, "SubjectName");
                if (a.subject.empty()) a.subject = "?";  // upstream sends one nameless row; still counts
                auto it = by_name.find(a.subject);
                if (it != by_name.end()) a.subject = it->second;  // marks key on the abbrev
                a.lessons = (int)jnum(e, "LessonsCount");
                // web's Zameškáno is Base alone; Late/Soon/School are tracked apart and don't count
                a.absent = (int)jnum(e, "Base");
                if (a.lessons < 0 || a.absent < 0 || a.absent > a.lessons) continue;
                a.threshold = thr;
                abs.push_back(std::move(a));
            }
            // /absence/student takes no date: the totals are whatever year upstream calls
            // current, and Absences[] is the only thing in the payload that dates them
            std::string last;
            for (auto d : jarr(j.root, "Absences")) last = std::max(last, s(d, "Date"));
            store.set_state("bakalari.absence_year",
                            school_year(last.empty() ? now() : classify::epoch(last)));
            store.put_absences("bakalari", abs);
        }
    }

    if (full) store.set_state("bakalari.swept_at", std::to_string(now()));
    return out;
}

}  // namespace bakalari

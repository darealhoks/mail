#include "outlook.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "classify.h"
#include "config.h"
#include "json.h"
#include "teams.h"
#include "teams_auth.h"

namespace outlook {
namespace {

const char *INBOX = "https://graph.microsoft.com/v1.0/me/mailFolders/inbox";
const char *SELECT =
    "$select=id,subject,bodyPreview,receivedDateTime,isRead,hasAttachments,from,webLink";
// $top is ignored on this endpoint, Prefer is not (10/page otherwise)
const char *PAGE = "Prefer: odata.maxpagesize=100";
// ponytail: 200 pages = 20k mails per run; hitting the cap fails the fetch rather than
// storing no cursor, raise it if anyone's inbox is bigger
const int MAX_PAGES = 200;
// not under "outlook." — maild --cold wipes every <source>.% cursor and this must survive
const char *CURSOR = "outlook.delta", *CONFIRMED = "consent.outlook";

long long now() { return (long long)time(nullptr); }

std::string iso_utc(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

int recent_days() {
    int d = config().num("source.outlook.recent_days");
    return d > 0 ? d : 14;
}

std::string sv(simdjson::dom::element e, const char *k) {
    auto v = e.at_key(k).get_string();
    return v.error() ? std::string() : std::string(v.value());
}

// marketing mail pads its preheader with zero-width joiners so the preview line looks
// short; left in, bodyPreview renders as a screen of blanks. u+200b..u+200d, u+2060, u+feff
}  // namespace

std::string strip_invisible(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size()) {
            unsigned char b = (unsigned char)s[i + 1], d = (unsigned char)s[i + 2];
            if ((b == 0x80 && d >= 0x8B && d <= 0x8D) || (b == 0x81 && d == 0xA0)) {
                i += 2;
                continue;
            }
        }
        if (c == 0xEF && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xBB &&
            (unsigned char)s[i + 2] == 0xBF) {
            i += 2;
            continue;
        }
        o += (char)c;
    }
    return o;
}

namespace {

bool bv(simdjson::dom::element e, const char *k) {
    auto v = e.at_key(k).get_bool();
    return v.error() ? false : v.value();
}

// one message -> zero or one item; @removed entries and read mail outside "all" fall out here.
// a read mail is one the user has already dealt with elsewhere: this is a to-do list, not a
// mail client, so it never enters the store at all
bool to_item(simdjson::dom::element m, Item &out) {
    std::string id = sv(m, "id");
    if (id.empty() || !m.at_key("@removed").error()) return false;
    if (mode() != "all" && bv(m, "isRead")) return false;

    std::string subject = trim(strip_invisible(sv(m, "subject")));
    // ponytail: bodyPreview is graph's own 255-char plain-text cut; the full body is html
    // and would cost a fetch + strip per mail for text nothing renders
    std::string body = trim(strip_invisible(sv(m, "bodyPreview")));
    if (subject.empty() && body.empty()) return false;
    if (bv(m, "hasAttachments")) body += " [att]";  // same marker teams/bakalari write

    std::string sender;
    simdjson::dom::element addr;
    if (!m.at_key("from").at_key("emailAddress").get(addr)) {
        sender = trim(sv(addr, "name"));
        if (sender.empty()) sender = trim(sv(addr, "address"));
    }

    std::string received = sv(m, "receivedDateTime");
    classify::Result r = classify::run(subject + " " + body, false, received);
    out.source = "outlook";
    out.klass = sender;
    out.kind = classify::kind(r);
    out.title = subject;
    if (out.title.empty()) {
        size_t n = std::min<size_t>(120, body.size());
        while (n && ((unsigned char)body[n] & 0xC0) == 0x80) n--;  // don't cut mid-codepoint
        out.title = body.substr(0, n);
    }
    out.body = body;
    out.due_at = classify::due_epoch(r.deadline);
    out.event_at = classify::epoch(received);
    out.src_uid = "mail:" + id;
    out.url = sv(m, "webLink");
    return true;
}

// folder counts, for the sign-in warning
void counts(long long &total, long long &unread) {
    std::string token = teams::access_token();
    Json j(teams::graph_get(token, std::string(INBOX) + "?$select=totalItemCount,unreadItemCount"));
    total = j.num("totalItemCount");
    unread = j.num("unreadItemCount");
}

}  // namespace

std::string mode() {
    std::string m = config().str("source.outlook.sync");
    return m == "unread" || m == "all" ? m : "recent";
}

long long cold_size(long long total, long long unread) {
    return mode() == "all" ? total : unread;
}

bool have_session() { return teams::have_session(); }

std::string session_error() {
    if (!have_session()) return "not signed in";
    try {
        teams::access_token();
    } catch (const SessionExpired &e) {
        return e.what();
    } catch (const std::exception &) {
        return "";  // transport failure is not a session verdict
    }
    return "";
}

std::vector<Item> fetch(Store &st) {
    std::string token = teams::access_token();
    std::vector<Item> out;

    std::string cold = std::string(INBOX) + "/messages/delta?" + SELECT;
    // "recent" is the only bounded cold run; the other two sweep the whole mailbox and are
    // gated on the sign-in confirmation, which records the mode it was given for
    if (mode() == "recent")
        cold += "&$filter=receivedDateTime%20ge%20" + iso_utc(now() - (long long)recent_days() * 86400);
    else if (st.get_state(CONFIRMED) != mode())
        throw std::runtime_error("outlook: sync=" + mode() + " needs confirming, run: " APP_NAME
                                 "c auth outlook");

    std::string url = st.get_state(CURSOR);
    bool resumed = !url.empty();
    if (!resumed) url = cold;

    for (int page = 0; page < MAX_PAGES && !url.empty(); page++) {
        std::string body;
        try {
            body = teams::graph_get(token, url, {PAGE});
        } catch (const teams::GraphError &e) {
            // an expired delta token costs one cold sweep, nothing else
            if (!resumed || page || (e.status != 400 && e.status != 410)) throw;
            st.set_state(CURSOR, "");
            url = cold;
            resumed = false;
            body = teams::graph_get(token, url, {PAGE});
        }
        Json j(body);
        simdjson::dom::array v;
        if (j.root.at_key("value").get(v)) throw std::runtime_error("outlook: delta missing value array");
        for (auto m : v) {
            Item i;
            if (to_item(m, i)) out.push_back(std::move(i));
        }
        std::string next = sv(j.root, "@odata.nextLink");
        if (!next.empty()) {
            url = next;
            continue;
        }
        st.set_state(CURSOR, sv(j.root, "@odata.deltaLink"));
        url.clear();
    }
    if (!url.empty())
        throw std::runtime_error("outlook: inbox longer than " + std::to_string(MAX_PAGES) +
                                 " pages, no cursor stored; raise MAX_PAGES");
    return out;
}

int login_interactive() {
    if (!teams::have_session() && teams::login_interactive() != 0) return 1;

    if (mode() == "recent") {
        printf("outlook: signed in; first run takes unread mail from the last %d days\n",
               recent_days());
        return 0;
    }
    Store st;
    long long total = 0, unread = 0;
    counts(total, unread);
    long long n = cold_size(total, unread);
    printf("outlook: sync=%s pulls %lld mails into the store on the next run.\n", mode().c_str(), n);
    printf("really do that? [y/N] ");
    fflush(stdout);
    char answer[8] = {0};
    if (!fgets(answer, sizeof answer, stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
        puts("left alone; set [source.outlook] sync = recent to keep it small");
        return 1;
    }
    st.set_state(CONFIRMED, mode());  // per mode: widening it asks again
    puts("ok, next run syncs them");
    return 0;
}

}  // namespace outlook

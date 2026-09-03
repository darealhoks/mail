#include "outlook.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "classify.h"
#include "config.h"
#include "http.h"
#include "json.h"
#include "teams.h"
#include "teams_auth.h"
#include "text.h"

namespace outlook {
namespace {

const char *INBOX = "https://graph.microsoft.com/v1.0/me/mailFolders/inbox";
const char *SELECT =
    "$select=id,subject,body,receivedDateTime,isRead,hasAttachments,from,webLink";
const char *ISO = "%Y-%m-%dT%H:%M:%SZ";
// $top is ignored on this endpoint, Prefer is not (10/page otherwise)
const char *PAGE = "Prefer: odata.maxpagesize=100";
// graph bakes $select into the delta token, so a cursor minted under an older SELECT keeps
// serving the old fields: rename the key whenever SELECT changes and the next run goes cold.
// CONFIRMED is not under "outlook." — maild --cold wipes every <source>.% cursor and this must survive
const char *CURSOR = "outlook.delta.body", *CONFIRMED = "consent.outlook";

long long now() { return (long long)time(nullptr); }

int recent_days() {
    int d = config().num("source.outlook.recent_days");
    return d > 0 ? d : 14;
}

// one message -> zero or one item; @removed entries and read mail outside "all" fall out here.
// a read mail is one the user has already dealt with elsewhere: this is a to-do list, not a
// mail client, so it never enters the store at all
bool to_item(simdjson::dom::element m, Item &out) {
    std::string id = jstr(m, "id");
    if (id.empty() || !m.at_key("@removed").error()) return false;
    if (mode() != "all" && jflag(m, "isRead")) return false;

    std::string subject = trim(text::strip_invisible(jstr(m, "subject")));
    // the whole mail, not graph's 255-char bodyPreview: contentType is html on all but the
    // plainest, and plain_text passes text/plain through unchanged
    simdjson::dom::element bd;
    std::string body = m.at_key("body").get(bd)
                           ? std::string()
                           : trim(text::strip_invisible(text::plain_text(jstr(bd, "content"))));
    if (subject.empty() && body.empty()) return false;
    if (jflag(m, "hasAttachments")) body += " [att]";  // same marker teams/bakalari write

    std::string sender;
    simdjson::dom::element addr;
    if (!m.at_key("from").at_key("emailAddress").get(addr)) {
        sender = trim(jstr(addr, "name"));
        if (sender.empty()) sender = trim(jstr(addr, "address"));
    }

    std::string received = jstr(m, "receivedDateTime");
    classify::Result r = classify::run(subject + " " + body, false, received);
    out.source = "outlook";
    out.klass = sender;
    out.kind = classify::kind(r);
    out.title = subject.empty() ? text::first_line(body, 120) : subject;
    out.body = body;
    out.due_at = classify::due_epoch(r.deadline);
    out.event_at = classify::epoch(received);
    out.src_uid = "mail:" + id;
    out.url = jstr(m, "webLink");
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

std::vector<Item> fetch(Store &st) {
    std::string token = teams::access_token();
    std::vector<Item> out;

    std::string cold = std::string(INBOX) + "/messages/delta?" + SELECT;
    // "recent" is the only bounded cold run; the other two sweep the whole mailbox and are
    // gated on the sign-in confirmation, which records the mode it was given for
    if (mode() == "recent")
        cold += "&$filter=receivedDateTime%20ge%20" +
                text::utc(now() - (long long)recent_days() * 86400, ISO);
    else if (st.get_state(CONFIRMED) != mode())
        throw std::runtime_error("outlook: sync=" + mode() + " needs confirming, run: " APP_NAME
                                 "c auth outlook");

    teams::graph_delta(token, st, CURSOR, cold, {PAGE}, [&](simdjson::dom::array v) {
        for (auto m : v) {
            Item i;
            if (to_item(m, i)) out.push_back(std::move(i));
        }
    });
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

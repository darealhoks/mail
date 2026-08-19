#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <fstream>
#include <vector>

#include <ctime>
#include <unistd.h>

#include "config.h"
#include "classify.h"
#include "json.h"
#include "notify.h"
#include "oauth.h"
#include "registry.h"
#include "http.h"
#include "store.h"
#include "view.h"
#include "teams.h"

namespace {

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "selfcheck failed: %s (%d)\n", #cond, __LINE__); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int classify_check() {
    struct C {
        const char *text;
        bool card;
        const char *dl, *tags;
    } cases[] = {
        {"Test 16. 4. Připomínám, že 16. 4. píšete opakovací test", false, "2026-04-16", "test"},
        {"Desetiminutovka - rovnice a nerovnice / Termín splnění 9. dub", true, "2026-04-09",
         "test+task"},
        {"Náhradní termín - kmitání - pátek 17. 4. 8:55 učebna 309.", false, "2026-04-17T08:55",
         "test"},
        {"Opravný test - opravu si napíšete ve středu 8. ledna ve 14:30", false,
         "2026-01-08T14:30", "test"},
        {"Metody / Due Jan 21", true, "2026-01-21", "task"},
        {"Mluvní cvičení J. K. Tyl - Fidlovačka", false, "", "task"},
        {"Domácí úkol - příprava na test / Termín splnění 11. bře", true, "2026-03-11", "task"},
        {"Aktivace účtu u Autodesku Prosím všechny studenty, aby si nejpozději do 2. 2. "
         "aktivovali účet",
         false, "2026-02-02", "task"},
        {"Zrušení hodiny Dobrý den, dnešní hodina 12. 1. je bohužel zrušena", false, "", ""},
        {"Opakování - Výstavba literárního díla", false, "", ""},
        {"📢 Změnil se termín splnění zadání.", false, "", ""},
        {"class TestBot(BotAI): import random", false, "", ""},
    };
    for (const auto &c : cases) {
        auto r = classify::run(c.text, c.card, "2026-01-15");
        std::string got = std::string(r.test ? "test" : "") + (r.test && r.task ? "+" : "") +
                          (r.task ? "task" : "");
        if (got != c.tags || r.deadline != c.dl) {
            fprintf(stderr, "classify: want %s,%s got %s,%s: %s\n", c.tags, c.dl, got.c_str(),
                    r.deadline.c_str(), c.text);
            return 1;
        }
    }

    return 0;
}

int selfcheck() {
    CHECK(classify_check() == 0);
    CHECK(config_check() == 0);
    Json j(R"({"a":"x\ny","n":42})");
    CHECK(j.str("a") == "x\ny");
    CHECK(j.num("n") == 42);
    CHECK(j.str("n", "def") == "def");     // wrong type -> default
    CHECK(j.num("missing", -1) == -1);
    bool threw = false;
    try {
        Json bad("{not json");
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK(threw);

    auto item = [](const char *src) {
        Item i;
        i.source = src;
        i.klass = "3.A";
        i.kind = "post";
        i.title = "t";
        i.src_uid = "uid1";
        return i;
    };
    std::string tmp = "/tmp/" APP_NAME "d-selfcheck.db";
    remove(tmp.c_str());
    {
        Store s(tmp);
        CHECK(s.insert_item(item("teams")));
        CHECK(!s.insert_item(item("teams")));
        CHECK(s.insert_item(item("othersrc")));
        CHECK(s.last_ok_fetch("teams") == 0);
        s.log_fetch("teams", 100, false, "boom", 0);
        CHECK(s.last_ok_fetch("teams") == 0);
        s.log_fetch("teams", 100, true, "", 1);
        CHECK(s.last_ok_fetch("teams") > 0);
        CHECK(s.last_fetch("teams").failing_since == 0);
        s.log_fetch("teams", 100, false, "boom", 0);
        long long first = s.last_fetch("teams").failing_since;
        s.log_fetch("teams", 100, false, "boom", 0);
        CHECK(first && s.last_fetch("teams").failing_since == first);

        for (const Source &src : sources()) s.log_fetch(src.name, 100, false, OFFLINE_TAG "x", 0);
        CHECK(view::offline(s));
        CHECK(view::gripes(s).size() == 1 && !view::gripes(s)[0].error);
        s.log_fetch(sources()[0].name, 100, false, "http: boom", 0);
        CHECK(!view::offline(s));
    }
    remove(tmp.c_str());

    {
        Store s(tmp);
        CHECK(s.get_state("k").empty());
        s.set_state("k", "v1");
        s.set_state("k", "v2");
        CHECK(s.get_state("k") == "v2");
        CHECK(s.insert_item(item("teams")));
    }
    remove(tmp.c_str());

    {
        Store s(tmp);
        Item i = item("teams");
        i.title = "a\x1b[31mb";
        CHECK(s.insert_item(i));
        CHECK(s.feed().at(0).title == "a[31mb");
    }
    remove(tmp.c_str());

    CHECK(teams::plain_text("<p>a<br/>b</p><div>c &amp;amp; d</div>") == "a\nb\nc & d");
    CHECK(teams::plain_text("see https://x.y/z now") == "see https://x.y/z now");
    CHECK(teams::plain_text("<a href=\"https://x.y/z\">https://x.y/z</a>.") == "https://x.y/z .");
    CHECK(teams::plain_text("&#268;au&nbsp;&#268;au") == "Čau Čau");
    CHECK(teams::plain_text("<div></div>  ") == "");

    CHECK(classify::epoch("2026-08-16T00:00:00+02:00") == 1786838400);
    CHECK(classify::epoch("2026-08-16T14:30") == 1786890600);
    CHECK(classify::epoch("nope") == 0);

    // hook args are remote text: one quoting slip and the shell runs it
    std::string nf = "/tmp/" APP_NAME "d-notify";
    config().set("general.notify", "printf '%s|' >" + nf);
    notify(2, "You've got mail", "$(touch /tmp/pwned)\n+1 tests", ICON_MAIL);
    std::ifstream nfs(nf);
    std::string got((std::istreambuf_iterator<char>(nfs)), std::istreambuf_iterator<char>());
    CHECK(got == "2|You've got mail|$(touch /tmp/pwned)\n+1 tests|" ICON_MAIL "|");
    CHECK(access("/tmp/pwned", F_OK) != 0);
    remove(nf.c_str());
    config().set("general.notify", "");

    puts(APP_NAME "d: selfcheck ok");
    return 0;
}

// items table kinds -> what the notification calls them, always plural
const struct {
    const char *kind, *label;
} KIND_LABELS[] = {{"task", "tasks"}, {"test", "tests"},     {"info", "messages"},
                   {"mark", "marks"}, {"change", "changes"}};

int fetch_all(bool cold_flag) {
    Store store;
    int rc = 0;
    std::map<std::string, int> fresh_kinds;
    // teams resumes from a per-channel delta link; drop them and it rescrapes the whole window
    if (cold_flag) {
        store.clear_state("teams.delta.%");
        store.clear_state("teams.channels%");
        store.clear_state("bakalari.swept_at");
    }
    bool tty = isatty(2);
    teams::progress = [tty](size_t done, size_t total, const std::string &what) {
        if (!tty) return;
        fprintf(stderr, "\rteams: %zu/%zu  %-30.30s", done, total, what.c_str());
        if (done == total) fputs("\r\033[K", stderr);
    };
    store.set_state("daemon.interval", config().str("general.interval"));
    auto run = [&](const char *name, auto &&fn) {
        long long started = (long long)time(nullptr);
        bool cold = cold_flag || store.last_ok_fetch(name) == 0;
        try {
            int fresh = 0;
            // a source that advances its own cursor mid-fetch must not outlive the inserts
            store.begin();
            try {
                for (const auto &i : fn())
                    if (store.insert_item(i)) {
                        fresh++;
                        if (!cold) fresh_kinds[i.kind]++;
                    }
            } catch (...) {
                store.rollback();
                throw;
            }
            store.commit();
            store.log_fetch(name, started, true, "", cold ? 0 : fresh);
            store.set_state(std::string("expired.") + name, "");
            if (cold) {
                std::string ys;
                for (const auto &y : active_years()) ys += (ys.empty() ? "" : ", ") + y;
                printf("%s: cold run, backfilled %s\n", name, ys.c_str());
            } else printf("%s: %d new\n", name, fresh);
        } catch (const SessionExpired &e) {
            store.log_fetch(name, started, false, e.what(), 0);
            fprintf(stderr, "%s: %s\n", name, e.what());
            rc = 1;
            // one notification per outage; cleared by the next fetch that works
            std::string flag = std::string("expired.") + name;
            if (store.get_state(flag).empty()) {
                store.set_state(flag, "1");
                notify(2, std::string(name) + " signed out",
                       APP_NAME "c auth " + std::string(name), ICON_LOCK);
            }
        } catch (const std::exception &e) {
            store.log_fetch(name, started, false, e.what(), 0);
            fprintf(stderr, "%s: %s\n", name, e.what());
            if (std::string(e.what()).rfind(OFFLINE_TAG, 0) != 0) rc = 1;  // no net is not a failure worth mailing from cron
        }
    };
    for (const Source &src : sources()) run(src.name, [&] { return src.fetch(store); });

    std::string body;
    for (const auto &kl : KIND_LABELS)
        if (int n = fresh_kinds[kl.kind])
            body += (body.empty() ? "" : "\n") + ("+" + std::to_string(n) + " " + kl.label);
    if (!body.empty()) notify(0, "You've got mail", body, ICON_MAIL);
    return rc;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        setvbuf(stdout, nullptr, _IOLBF, 0);  // stdout is a log pipe under cron
        bool cold = false;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--selfcheck")) return selfcheck();
            else if (!strcmp(argv[i], "--cold")) cold = true;
            else {
                fprintf(stderr, "usage: " APP_NAME "d [--cold]\n");
                return 2;
            }
        }
        return fetch_all(cold);
    } catch (const std::exception &e) {
        fprintf(stderr, APP_NAME "d: %s\n", e.what());
        return 1;
    }
}

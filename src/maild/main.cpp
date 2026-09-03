#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ctime>
#include <unistd.h>

#include "http.h"
#include "notify.h"
#include "oauth.h"
#include "registry.h"
#include "store.h"

namespace {

// items table kinds -> what the notification calls them, always plural
const struct {
    const char *kind, *label;
} KIND_LABELS[] = {{"task", "tasks"}, {"test", "tests"},     {"info", "messages"},
                   {"mark", "marks"}, {"change", "changes"}};

int fetch_all(bool cold_flag) {
    Store store;
    int rc = 0;
    std::map<std::string, int> fresh_kinds;
    // every cursor key is namespaced by its source; a cold run drops the lot and rescrapes
    if (cold_flag)
        for (const Source &src : sources()) store.clear_state(std::string(src.name) + ".%");
    // inside a catch: a throwing log would escape run() and cost every source after this one
    auto log_fail = [&](const char *name, long long started, const char *err) {
        try {
            store.log_fetch(name, started, false, err, 0);
        } catch (const std::exception &e) {
            fprintf(stderr, "%s: cannot log failure: %s\n", name, e.what());
        }
    };
    auto run = [&](const char *name, auto &&fn) {
        long long started = (long long)time(nullptr);
        bool cold = cold_flag || store.last_ok_fetch(name) == 0;
        try {
            int fresh = 0;
            // a source that advances its own cursor mid-fetch must not outlive the inserts, but
            // the transaction must not span the network either — queue those writes, replay them
            // with the inserts in one short transaction
            store.defer = true;
            std::vector<Item> got;
            try {
                got = fn();
            } catch (...) {
                store.drop_deferred();
                throw;
            }
            store.begin();
            try {
                store.flush();
                for (const auto &i : got)
                    if (store.insert_item(i)) {
                        fresh++;
                        if (!cold) fresh_kinds[i.kind]++;
                    }
                store.commit();
            } catch (...) {
                store.rollback();
                throw;
            }
            store.log_fetch(name, started, true, "", cold ? 0 : fresh);
            store.set_state(std::string("expired.") + name, "");
            printf("%s: %s%d new\n", name, cold ? "cold run, " : "", fresh);
        } catch (const SessionExpired &e) {
            log_fail(name, started, e.what());
            fprintf(stderr, "%s: %s\n", name, e.what());
            rc = 1;
            // the verdict the frontends read: they never probe a session themselves. also
            // dedupes the notification to one per outage; cleared by the next fetch that works
            std::string flag = std::string("expired.") + name;
            if (store.get_state(flag).empty()) {
                store.set_state(flag, e.what());
                notify(2, std::string(name) + " signed out",
                       APP_NAME "c auth " + std::string(name), ICON_LOCK);
            }
        } catch (const std::exception &e) {
            log_fail(name, started, e.what());
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
            if (!strcmp(argv[i], "--cold")) cold = true;
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

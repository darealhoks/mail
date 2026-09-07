#include "notify.h"

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

#include "config.h"

namespace {

// single-quote for /bin/sh: the hook is a user command line, the args are remote text
std::string q(const std::string &s) {
    std::string o = "'";
    for (char ch : s) {
        if (ch == '\'') o += "'\\''";
        else o += ch;
    }
    return o + "'";
}

}  // namespace

void notify(int urgency, const std::string &summary, const std::string &body, const char *icon) {
    std::string hook = config().str("general.notify");
    if (hook.empty()) return;
    // cron hands the hook no session env and a PATH without ~/.local/bin, where the hook
    // lives beside maild; wispctl and friends find their socket by the runtime dir
    if (!getenv("XDG_RUNTIME_DIR"))
        setenv("XDG_RUNTIME_DIR", ("/run/user/" + std::to_string(getuid())).c_str(), 0);
    const char *home = getenv("HOME"), *path = getenv("PATH");
    if (home) {
        std::string local = std::string(home) + "/.local/bin", p = path ? path : "";
        if (p.find(local) == std::string::npos) setenv("PATH", (local + ":" + p).c_str(), 1);
    }
    std::string cmd = hook + " " + std::to_string(urgency) + " " + q(summary) + " " + q(body) +
                      " " + icon;
    if (system(cmd.c_str()) != 0) fprintf(stderr, APP_NAME ": notify hook failed: %s\n", hook.c_str());
}

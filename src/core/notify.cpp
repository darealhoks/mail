#include "notify.h"

#include <cstdio>
#include <cstdlib>

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
    std::string cmd = hook + " " + std::to_string(urgency) + " " + q(summary) + " " + q(body) +
                      " " + icon;
    if (system(cmd.c_str()) != 0) fprintf(stderr, APP_NAME ": notify hook failed: %s\n", hook.c_str());
}

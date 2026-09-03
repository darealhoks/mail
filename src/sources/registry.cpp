#include "registry.h"

#include "bakalari.h"
#include "config.h"
#include "outlook.h"
#include "teams.h"
#include "teams_auth.h"

namespace {

// teams and outlook share one refresh token, so they share one session
const Source ALL[] = {
    {"bakalari", "Bakaláři", "bakalari", bakalari::have_session, bakalari::fetch,
     bakalari::login_interactive},
    {"teams", "Teams", "teams", teams::have_session, teams::fetch, teams::login_interactive},
    {"outlook", "Outlook", "teams", teams::have_session, outlook::fetch,
     outlook::login_interactive},
};

}  // namespace

const std::vector<Source> &sources() {
    static const std::vector<Source> on = [] {
        std::vector<Source> v;
        for (const auto &s : ALL)
            if (config().flag(std::string("source.") + s.name + ".enabled")) v.push_back(s);
        return v;
    }();
    return on;
}

const Source *source(const std::string &name) {
    for (const auto &s : sources())
        if (name == s.name) return &s;
    return nullptr;
}

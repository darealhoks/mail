#include "registry.h"

#include "bakalari.h"
#include "config.h"
#include "outlook.h"
#include "teams.h"
#include "teams_auth.h"

namespace {

std::string bakalari_error() { return bakalari::have_session() ? "" : "not signed in"; }

// teams and outlook share one refresh token, so they share one probe
const Source ALL[] = {
    {"bakalari", "Bakaláři", "bakalari", bakalari::have_session, bakalari_error, bakalari::fetch,
     bakalari::login_interactive, nullptr},
    {"teams", "Teams", "teams", teams::have_session, outlook::session_error, teams::fetch,
     teams::login_interactive, &teams::progress},
    {"outlook", "Outlook", "teams", outlook::have_session, outlook::session_error, outlook::fetch,
     outlook::login_interactive, nullptr},
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

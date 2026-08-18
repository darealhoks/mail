#include "registry.h"

#include "bakalari.h"
#include "config.h"
#include "teams.h"
#include "teams_auth.h"

namespace {

std::string bakalari_error() { return bakalari::have_session() ? "" : "not signed in"; }

// teams holds a refresh token that upstream can reject; probing it is the only real check
std::string teams_error() {
    if (!teams::have_session()) return "not signed in";
    try {
        teams::access_token();
    } catch (const SessionExpired &e) {
        return e.what();
    }
    return "";
}

const Source ALL[] = {
    {"bakalari", "Bakaláři", bakalari::have_session, bakalari_error, bakalari::fetch,
     bakalari::login_interactive},
    {"teams", "Teams", teams::have_session, teams_error, teams::fetch, teams::login_interactive},
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

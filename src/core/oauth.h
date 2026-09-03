#pragma once
#include <functional>
#include <stdexcept>
#include <string>

#include "json.h"

// refresh token rejected upstream: only a fresh sign-in fixes it (.map/auth-ux.md)
struct SessionExpired : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace oauth {

struct Config {
    std::string creds_name, client_id, token_url;
    std::string scope;  // sent on refresh only when non-empty
};

std::string form(const std::string &k, const std::string &v);

// persists access+refresh from a token response and returns the access token;
// throws if the shape is not what we expect
std::string save_tokens(const Config &c, const Json &j);

// cached access token, refreshed when stale. expired(status, body) decides whether a
// non-200 refresh means re-auth: the two providers signal it differently
using Expired = std::function<bool(long status, const Json &)>;
std::string access_token(const Config &c, const Expired &expired);

bool have_session(const Config &c);
long long last_refresh_at(const std::string &creds_name);  // unix seconds, 0 if never

// drop the cached access token so the next access_token() must really refresh: the only way
// to recover from a token a provider invalidated before its nominal expiry
void forget_access(const Config &c);

}  // namespace oauth

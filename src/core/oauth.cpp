#include "oauth.h"

#include <cstdlib>
#include <ctime>

#include "creds.h"
#include "http.h"

namespace oauth {
namespace {

long long now() { return (long long)time(nullptr); }

}  // namespace

std::string form(const std::string &k, const std::string &v) {
    return url_encode(k) + "=" + url_encode(v);
}

std::string save_tokens(const Config &c, const Json &j) {
    std::string access = j.str("access_token");
    std::string refresh = j.str("refresh_token");
    long long expires_in = j.num("expires_in", 0);
    if (access.empty() || refresh.empty() || expires_in <= 0)
        throw std::runtime_error(c.creds_name +
                                 ": token response missing access/refresh/expires_in");
    auto kv = creds_load(c.creds_name);
    kv["access_token"] = access;
    kv["refresh_token"] = refresh;
    kv["access_expires_at"] = std::to_string(now() + expires_in);
    kv["last_refresh_at"] = std::to_string(now());
    creds_save(c.creds_name, kv);
    return access;
}

std::string access_token(const Config &c, const Expired &expired) {
    auto kv = creds_load(c.creds_name);
    auto refresh = kv.find("refresh_token");
    if (refresh == kv.end() || refresh->second.empty())
        throw SessionExpired(c.creds_name + ": not signed in");

    auto acc = kv.find("access_token");
    auto exp = kv.find("access_expires_at");
    if (acc != kv.end() && exp != kv.end() && !acc->second.empty() &&
        strtoll(exp->second.c_str(), nullptr, 10) > now() + 120)
        return acc->second;

    std::string body = form("client_id", c.client_id) + "&" + form("grant_type", "refresh_token") +
                       "&" + form("refresh_token", refresh->second);
    if (!c.scope.empty()) body += "&" + form("scope", c.scope);
    HttpResponse r = http_post_form(c.token_url, body);
    Json j(r.body);
    if (r.status == 200) return save_tokens(c, j);
    std::string err = j.str("error");
    if (expired(r.status, j))
        throw SessionExpired(c.creds_name + ": session expired (" +
                             j.str("error_description", err) + ")");
    throw std::runtime_error(c.creds_name + ": refresh failed (http " +
                             std::to_string(r.status) + " " + err + ")");
}

bool have_session(const Config &c) {
    auto kv = creds_load(c.creds_name);
    auto it = kv.find("refresh_token");
    return it != kv.end() && !it->second.empty();
}

long long last_refresh_at(const std::string &creds_name) {
    auto kv = creds_load(creds_name);
    auto it = kv.find("last_refresh_at");
    return it == kv.end() ? 0 : strtoll(it->second.c_str(), nullptr, 10);
}

void forget_access(const Config &c) {
    auto kv = creds_load(c.creds_name);
    kv.erase("access_token");
    kv.erase("access_expires_at");
    creds_save(c.creds_name, kv);
}


}  // namespace oauth

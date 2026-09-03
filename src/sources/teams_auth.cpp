#include "teams_auth.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>

#include "http.h"
#include "json.h"
#include "paint.h"
#include "term.h"

namespace teams {
namespace {

// first-party Microsoft Teams client, pre-consented — user has no entra access to
// register one (.map/sources.md)
const char *CLIENT_ID = "1fec8e78-bce4-4aaf-ab1b-5451cc387264";
const char *SCOPE = "https://graph.microsoft.com/.default offline_access";
const char *AUTHORITY = "https://login.microsoftonline.com/organizations/oauth2/v2.0";
const char *CREDS = "teams";

const oauth::Config CFG{CREDS, CLIENT_ID, std::string(AUTHORITY) + "/token", SCOPE};

long long now() { return (long long)time(nullptr); }

using oauth::form;

}  // namespace

DeviceCode device_start() {
    std::string body = form("client_id", CLIENT_ID) + "&" + form("scope", SCOPE);
    HttpResponse r = http_post_form(std::string(AUTHORITY) + "/devicecode", body);
    Json j(r.body);
    if (r.status != 200)
        throw std::runtime_error("teams: devicecode failed (" + std::to_string(r.status) + " " +
                                 j.str("error", "unknown") + ": " + j.str("error_description") + ")");
    DeviceCode dc;
    dc.device_code = j.str("device_code");
    dc.user_code = j.str("user_code");
    dc.verification_uri = j.str("verification_uri");
    dc.interval = (int)j.num("interval", 5);
    dc.expires_in = (int)j.num("expires_in", 900);
    if (dc.device_code.empty() || dc.user_code.empty() || dc.verification_uri.empty())
        throw std::runtime_error("teams: devicecode response missing fields");
    if (dc.interval < 1 || dc.interval > 60) dc.interval = 5;
    if (dc.expires_in < 30 || dc.expires_in > 3600) dc.expires_in = 900;
    return dc;
}

void device_poll(const DeviceCode &dc) {
    std::string body = form("grant_type", "urn:ietf:params:oauth:grant-type:device_code") + "&" +
                       form("client_id", CLIENT_ID) + "&" + form("device_code", dc.device_code);
    long long deadline = now() + dc.expires_in;
    int interval = dc.interval;
    while (now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        HttpResponse r = http_post_form(std::string(AUTHORITY) + "/token", body);
        Json j(r.body);
        if (r.status == 200) {
            oauth::save_tokens(CFG, j);
            return;
        }
        std::string err = j.str("error");
        if (err == "authorization_pending") continue;
        if (err == "slow_down") {
            interval += 5;
            continue;
        }
        throw std::runtime_error("teams: sign-in failed (" + (err.empty() ? "http " + std::to_string(r.status) : err) +
                                 ": " + j.str("error_description") + ")");
    }
    throw std::runtime_error("teams: device code expired before sign-in completed; run "
                             APP_NAME "c auth teams again");
}

std::string access_token() {
    return oauth::access_token(CFG, [](long, const Json &j) {
        std::string e = j.str("error");
        return e == "invalid_grant" || e == "interaction_required";
    });
}

bool have_session() { return oauth::have_session(CFG); }

void forget_access() { oauth::forget_access(CFG); }


// an all-uppercase url encodes in qr alphanumeric mode (5.5 bits/char) instead of byte mode
// (8), which drops login.microsoft.com/device from version 3 to version 2: 25 modules, not 29.
// only safe on a bare https path: scheme and host are case-insensitive per rfc 3986, query and
// fragment are not. bail unless every char lands in the alphanumeric set, or qrencode falls
// back to byte mode and the uppercasing is a pure loss.
std::string qr_upper(const std::string &url) {
    static const std::string ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    if (url.find_first_of("?#") != std::string::npos) return url;
    std::string u = url;
    for (char &c : u) c = (char)toupper((unsigned char)c);
    return u.find_first_not_of(ALNUM) == std::string::npos ? u : url;
}

namespace {

// optional: qrencode is not a build dep, absence just means no qr
void print_qr(const std::string &text) {
    if (text.find('\'') != std::string::npos) return;
    // -m 2: UTF8 packs two module rows per glyph, so a 1-module quiet zone loses the top row
    std::string cmd = "qrencode -t UTF8 -m 2 -o - '" + text + "' 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return;
    std::vector<std::string> rows;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) {
        std::string l(buf);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!l.empty()) rows.push_back(l);
    }
    pclose(p);
    // quiet zone is an odd number of module rows, so the bottom gets one blank row more
    // than the top; drop it so the frame is symmetric
    if (rows.size() > 2 && rows.back() == rows.front() && rows[rows.size() - 2] == rows.front())
        rows.pop_back();
    for (const auto &l : rows) printf("\033[38;2;255;255;255;48;2;0;0;0m%s\033[0m\n", l.c_str());
}

}  // namespace

int login_interactive() {
    DeviceCode dc = device_start();
    // no prefill: v2.0 devicecode returns no verification_uri_complete and ?otc= is ignored
    std::string url_sgr = "4;" + std::string(paint::accent());
    printf("%s\n%s\n%s %s %s\n\n", c("1", "to sign in, open:").c_str(),
           c(url_sgr.c_str(), dc.verification_uri).c_str(), c("1", "and enter code:").c_str(),
           c(paint::accent(), dc.user_code).c_str(),
           c("90", "(valid for " + std::to_string(dc.expires_in / 60) + "m)").c_str());
    print_qr(qr_upper(dc.verification_uri));
    puts(c("90", "waiting for sign-in...").c_str());
    fflush(stdout);  // device_poll blocks for minutes; don't sit in a pipe buffer
    device_poll(dc);
    printf("%s %s\n", c("1", "teams").c_str(), c("1;32", "signed in").c_str());
    return 0;
}

}  // namespace teams

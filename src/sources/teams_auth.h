#pragma once
#include <string>

#include "oauth.h"

namespace teams {

struct DeviceCode {
    std::string device_code, user_code, verification_uri;
    int interval = 5, expires_in = 900;
};

DeviceCode device_start();
// blocks until the user completes sign-in; persists the refresh token. throws on timeout/denial
void device_poll(const DeviceCode &dc);

// valid access token, refreshing when needed. throws SessionExpired if re-auth is required
std::string access_token();

bool have_session();
long long last_refresh_at();  // unix seconds, 0 if never

// device-code sign-in on the tty: prints url, code and qr, then blocks. 0 = signed in
int login_interactive();
// uppercases a bare https url when that keeps it in qr alphanumeric mode; exposed for selfcheck
std::string qr_upper(const std::string &url);

}  // namespace teams

#pragma once
#include <string>
#include <vector>

#include "store.h"

namespace outlook {

// inbox delta; rides the teams graph token, so the session is teams' session
std::vector<Item> fetch(Store &st);
// signs teams in if needed, then confirms an unbounded cold sync before maild may run one
int login_interactive();

// sync mode from config, clamped to the known set
std::string mode();
// how many mails a cold run in this mode would pull, from the folder counts
long long cold_size(long long total, long long unread);

}  // namespace outlook

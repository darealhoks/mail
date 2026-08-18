#pragma once
#include <string>
#include <vector>

#include "oauth.h"
#include "store.h"

namespace bakalari {

// exchanges username+password for tokens and persists them; throws on bad credentials
void login(const std::string &user, const std::string &pass);
bool have_session();
// prompts for username+password on the tty; 0 = signed in
int login_interactive();

// homework, marks, komens messages and timetable changes for the current window
std::vector<Item> fetch(Store &store);

}  // namespace bakalari

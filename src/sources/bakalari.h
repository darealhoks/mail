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

// one day's changed hours; n is 0 when the caption is not a plain lesson number
struct Span {
    int n;
    std::string caption, begins, ends;
};
// "1.-4. hod 8:00 - 11:45, 6.-8. hod 13:00 - 13:45" — contiguous hours collapse, gaps do not
std::string hour_runs(std::vector<Span> h);

}  // namespace bakalari

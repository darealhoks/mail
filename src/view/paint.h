#pragma once
#include <string>
#include <vector>

// shared paint helpers: the sgr literals of the feed live here and in the frontends only
namespace paint {

int term_cols();
size_t utf8_len(const std::string &s);
// greedy wrap on spaces, counting codepoints; long words are left to overflow
std::vector<std::string> wrap(const std::string &s, size_t width);
const char *kind_color(const std::string &k);
const char *due_color(long long due);
std::string when(long long due);
// urls survive into the body text; make them stand out without breaking the wrap width
std::string link_up(const std::string &l);
inline const char *NEW_CHIP = "1;97;41";

}  // namespace paint

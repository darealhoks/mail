#pragma once
#include <string>
#include <vector>

#include "view.h"

// shared paint helpers: the sgr literals of the feed live here and in the frontends only
namespace paint {

int term_cols(bool cap = true);  // cap: clamp to 100 for prose; the grid passes false
size_t utf8_len(const std::string &s);
// cut to w codepoints, padding with spaces to exactly w; no sgr in the input
std::string plain_cut(const std::string &s, size_t w);
// greedy wrap on spaces, counting codepoints; long words are left to overflow
std::vector<std::string> wrap(const std::string &s, size_t width);
// general.accent as sgr params: a colour name, a 0-255 palette index, or #rrggbb
std::string accent_sgr(std::string v);  // parse one accent value; testable half of accent()
const char *accent();     // foreground
const char *accent_bg();  // as a background, via reverse video: text takes the terminal bg
std::string bg_sgr(const std::string &fg);  // turn a foreground sgr into its background twin
// general.bar: the status bar fill, same value grammar as the accent; "none" is unfilled
const char *bar_bg();
const char *kind_color(const std::string &k);
// mark 1..5 for a mark text or a "<got>/<max>" fraction; 39 when it is not gradeable
const char *mark_color(const std::string &t);
// "1,50 (2)": the average and the whole mark it rounds to (school.avg_round)
std::string avg_str(double a);
const char *avg_color(double a);
const char *due_color(long long due);
std::string when(long long due);
// "YYYY-MM-DD" in general.date form ("17. 8."), plus the year when it is not the current one
std::string date_short(const std::string &ymd);
// urls survive into the body text; make them stand out without breaking the wrap width
std::string link_up(const std::string &l);
// timetable geometry, shared by the cli and the tui: one size decision for both
struct TableLayout {
    size_t cw = 3;      // inner cell width, separators excluded
    size_t time_rows = 0;             // 0 none, 1 t0 only, 2 t0 over t1
    std::vector<std::string> t0, t1;  // per hour column, already in the chosen form
    bool room = true, teacher = true;  // table.room / table.teacher
};
// `need` is the widest cell content the caller will draw; the result never exceeds what the
// content plus the times actually need, nor what `cols` can hold
TableLayout table_layout(const view::Timetable &tt, size_t need, size_t cols, size_t gut);

inline const char *NEW_CHIP = "1;7;31";  // reverse: red block, text in the terminal bg

}  // namespace paint

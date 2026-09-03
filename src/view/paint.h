#pragma once
#include <string>
#include <vector>

#include "view.h"

// shared paint helpers: every sgr literal lives in these and in the frontends, nowhere else
namespace paint {

int term_cols(bool cap = true);  // cap: clamp to 68 for prose; the grid passes false
// the real terminal size, uncapped: what a full-screen frontend owns
void term_size(int &rows, int &cols);
size_t utf8_len(const std::string &s);
// a painted line with its escapes dropped, for measuring or matching against
std::string strip_sgr(const std::string &l);
// `t` centred in `w` columns, cut to fit
std::string centre(const std::string &t, size_t w);
// cut to w codepoints, padding with spaces to exactly w; no sgr in the input
std::string plain_cut(const std::string &s, size_t w);
// greedy wrap on spaces, counting codepoints; long words are left to overflow
std::vector<std::string> wrap(const std::string &s, size_t width);
// general.accent as sgr params: a colour name, a 0-255 palette index, or #rrggbb
std::string accent_sgr(std::string v);  // parse one accent value; testable half of accent()
const char *accent();     // foreground
const char *accent_bold();  // "1;<accent>"
const char *accent_bg();  // as a background, via reverse video: text takes the terminal bg
// general.bar: the status bar fill, same value grammar as the accent; "none" is unfilled
const char *bar_bg();
// mark 1..5 for a mark text or a "<got>/<max>" fraction; 39 when it is not gradeable
const char *mark_color(const std::string &t);
// "1,50 (2)": the average and the whole mark it rounds to (school.avg_round)
std::string avg_str(double a);
const char *avg_color(double a);
// "YYYY-MM-DD" in general.date form ("17. 8."), plus the year when it is not the current one
std::string date_short(const std::string &ymd);
// text::plain_text markers -> sgr, one wrapped line at a time; `open` is the span state
// carried onto the next line, so a bold phrase survives the wrap
std::string style_up(const std::string &l, unsigned &open);
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

// truncate a painted line to `width` visible columns, skipping the sgr escapes
std::string fit(const std::string &l, size_t width);
// "09:55" -> "9:55"
std::string hhmm(const std::string &t);
// general.browser (xdg-open by default), detached; 1 if the url is empty or unquotable
int open_url(const std::string &url);

// one painted feed post: terminal rows, sgr included, ending in a blank line
struct Post {
    std::vector<std::string> lines;
    std::string url;
};
// each post opens with its feed position, the number `dismiss`/`open` take
std::vector<Post> feed_posts(const view::Feed &f, size_t width);
// the school year the totals cover, then "MAT  3/48  6%" per subject; the percent goes red past
// the school's threshold, yellow past school.absence_warn, uncoloured when it sent none
std::vector<std::string> absence_lines(const view::Absences &a);
// period headers, the mark rows, then the averages; the class column appears only when the
// rows hold more than one class
std::vector<std::string> mark_lines(const view::Marks &m, size_t width);
// one row per subject: name, newest average, mark count. `subjects` comes back parallel to
// the lines, so a cursor on line n opens subjects[n]
std::vector<std::string> mark_subject_lines(const view::Marks &m,
                                            std::vector<std::string> &subjects);

// where the grid landed on screen, so a click maps back to a cell
struct Geom {
    size_t cw = 0, gut = 3, blk = 1, top = 2, left = 0, nd = 0, nh = 0;
    size_t hdr = 0, lab = 0;  // "< label >" line index and label width, from `left`
};
inline const size_t NO_CELL = (size_t)-1;  // grid_lines: no cursor cell
// rows > 0 fills that many terminal rows: taller cells, the block centred in the pane.
// rows <= 0 gives one line per cell and no padding, for a listing that just scrolls past
std::vector<std::string> grid_lines(const view::Timetable &tt, size_t cd, size_t ch, int rows,
                                    int cols, Geom &g);

inline const char *DAYNAME[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

}  // namespace paint

#pragma once
#include <string>

// what every source does to a remote body before it reaches the store: html in, one plain
// string out, lightly marked up (* bold, _ italic, ` code, ~ strike). paint::style_up turns
// the markers back into sgr; classify sees them stripped
namespace text {

// appended to task-bot posts, whose real text graph will not give us (.map/sources.md);
// frontends render it as a marker, not as body text
inline const char *TASK_NOTE = "task set in teams only";

// strftime in utc: "%Y-%m-%dT%H:%M:%SZ" for graph, "%Y-%m-%d" for bakalari
std::string utc(long long t, const char *fmt);

// html entity decode (&nbsp;, &#241;, &amp;)
std::string html_unescape(const std::string &s);
// collapse whitespace, keep urls whole, block tags become single newlines
std::string collapse(const std::string &s);
// html -> collapsed text with style markers
std::string plain_text(const std::string &html);
// drop the style markers again
std::string style_strip(const std::string &s);
// marketing mail pads its preheader with zero-width joiners so the preview line looks short;
// left in, it renders as a screen of blanks. u+200b..u+200d, u+2060, u+feff
std::string strip_invisible(const std::string &s);
// a title for a body that came without one: up to the first newline, else word-cut at `max`
std::string first_line(const std::string &s, size_t max);

}  // namespace text

#pragma once
#include <string>
#include <tuple>
#include <vector>

#include "registry.h"
#include "store.h"

// store -> rows. no output, no sgr, no terminal width: the cli owns every byte of paint.
namespace view {

// filters and labels are compared folded: "Čeština" types and matches as "cestina"
std::string fold(const std::string &s);
// teams course names carry group and year noise ("3A_GRS_SK1_25/26"); bakalari already ships
// the bare abbrev, so pick the first 2-4 letter uppercase token to make the two agree
std::string abbrev(const std::string &raw);
// argv words -> the folded filter list every rows() call takes
std::vector<std::string> fold_all(const std::vector<std::string> &words);

std::string ymd_local(long long t);
// monday of the week containing t
std::string monday_of(long long t);
// monday of this week; on the weekend, of the next one
std::string wanted_monday();
// ymd n days on
std::string ymd_plus(const std::string &date, int days);
// "YYYY-MM-DD" + "H:MM" in local time; 0 if either is malformed
long long local_at(const std::string &date, const std::string &hm);

// 90s / 25m / 4h / 3d: one ladder behind every "how long ago" and "in how long" in the ui
std::string rel_span(long long secs);
std::string ago(long long t);

// mark 1..5 for a "<got>/<max>" text via school.points, 0 when it is not a fraction
int points_of(const std::string &t);
// 1..5 for the average; 0 = not gradeable (N, absent). "1-" is a half grade worse, as in bakalari
double mark_value(const std::string &t);

struct FeedRow {
    size_t n;  // 1-based position in the whole feed, the number the cli prints
    bool is_new;
    int bucket;  // 0 no deadline, 1 upcoming, 2 overdue
    std::string klass;
};

struct Feed {
    std::vector<Item> items;  // whole feed minus blacklisted classes, in numbering order
    std::vector<FeedRow> rows;  // what to show: filtered, then capped from the front
    std::vector<std::string> kinds, classes, sources;  // vocabulary, for the bad-filter message
    std::string bad_filter;  // non-empty: unknown word, nothing was read or written
};
// filters must already be folded. limit 0 = uncapped
Feed feed_rows(Store &s, const std::vector<std::string> &filters, size_t limit);

struct MarkRow {
    bool is_new;
    long long event_at;  // 0 = unknown date
    std::string period, klass, mark, note;
};

struct Absences {
    std::vector<Absence> rows;
    std::string year;  // "25/26"; the totals upstream sent are for exactly one school year
    std::string note;  // non-empty: why the numbers are not this year's after all
};
// filters must already be folded; with none, the blacklist applies
Absences absence_rows(Store &s, const std::vector<std::string> &filters);

struct Marks {
    std::vector<MarkRow> rows;
    Absences absences;  // only the subjects the rows show, alphabetical
    // {class abbrev, half-year label, weighted mean}, class-major, oldest period first
    std::vector<std::tuple<std::string, std::string, double>> averages;
};
// filters must already be folded; with none, the blacklist applies. consume_new=false leaves
// the seen-marks watermark alone, so a live view can re-read without wiping its own chips
Marks marks_rows(Store &s, const std::vector<std::string> &filters, size_t limit,
                 bool consume_new = true);

struct Timetable {
    std::string monday;
    bool permanent = false;  // no lessons stored for this week: the recurring grid, re-dated
    // whole-day events covering part of this week, date-major ("Hlavní prázdniny")
    std::vector<std::pair<std::string, std::string>> notes;
    std::vector<Lesson> rows;
    std::vector<std::string> days;   // dates, display order
    std::vector<std::string> hours;  // hour numbers, ascending
    std::vector<const Lesson *> grid;  // days.size() * hours.size(), null where free
    const Lesson *at(size_t day, size_t hour) const { return grid[day * hours.size() + hour]; }
    // an hour column's start (end=false) or end time, from the first lesson that has them
    std::string edge(size_t hour, bool end) const {
        for (const Lesson &l : rows)
            if (l.hour == hours[hour] && !l.begins.empty()) {
                const std::string &t = end ? l.ends : l.begins;
                return t.size() > 1 && t[0] == '0' ? t.substr(1) : t;
            }
        return "";
    }
    // "8:00-8:45"; empty when the hour has no stored times
    std::string span(size_t hour) const {
        std::string b = edge(hour, false);
        return b.empty() ? b : b + "-" + edge(hour, true);
    }
};
// monday empty = wanted_monday(); any other week reads whatever is stored for it, and falls
// back to the permanent grid when nothing is
Timetable timetable(Store &s, const std::string &monday = "");

struct Next {
    enum State { None, NoTimes, Ok } state = None;
    Lesson lesson;
    long long start = 0;
};
Next next_lesson(Store &s);

struct SourceStatus {
    std::string name, pretty, error;
    bool signed_in = false;
    long long refreshed_at = 0, fetched_at = 0;
    bool stale = false;
};
// errors are red, staleness is a yellow warning: a cron user with a long period is not
// broken, just slow, and can silence it (stale_warn)
struct Gripe {
    std::string text;   // the whole story, for a status line with room for it
    std::string brief;  // "bakalari: stale" — for `new`, which lands in a shell prompt
    bool error;
};

// one sweep of every source. status() and gripes() both cost a session probe per source, and
// the tui asks for them on every repaint, so they are answered together or not at all
struct Health {
    std::vector<SourceStatus> sources;
    std::vector<Gripe> gripes;
    bool offline = false;
    // sources not signed in, by argv name — what `mailc auth <name>` wants
    std::vector<std::string> unsigned_names;
};
Health health(Store &s);
// every source that ran last failed for lack of connectivity; no session probe, so cheap
bool offline(Store &s);

struct NewCounts {
    int msgs = 0, work = 0, marks = 0;
};
NewCounts new_counts(Store &s);

}  // namespace view

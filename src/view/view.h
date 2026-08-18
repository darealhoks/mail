#pragma once
#include <string>
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
// a filter word matches an item's kind, class abbrev or source; several words are ANDed
bool matches(const Item &i, const std::vector<std::string> &filters);

// "new" = id above the watermark the last listing left behind; ids grow with insertion order
long long watermark(Store &s, const char *key);
void set_watermark(Store &s, const char *key, const std::vector<Item> &items);

std::string ymd_local(long long t);
// monday of the week containing t
std::string monday_of(long long t);
// monday of this week; on the weekend, of the next one
std::string wanted_monday();
// ymd n days on
std::string ymd_plus(const std::string &date, int days);
// "YYYY-MM-DD" + "H:MM" in local time; 0 if either is malformed
long long local_at(const std::string &date, const std::string &hm);

std::string ago(long long t);
std::string dur(long long s);

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

struct Marks {
    std::vector<MarkRow> rows;
    std::vector<std::pair<std::string, double>> averages;  // half-year label -> weighted mean
};
// filters must already be folded; with none, the blacklist applies and no average is computed
Marks marks_rows(Store &s, const std::vector<std::string> &filters, size_t limit);

struct Timetable {
    std::string monday;
    std::vector<Lesson> rows;
    std::vector<std::string> days;   // dates, display order
    std::vector<std::string> hours;  // hour numbers, ascending
    std::vector<const Lesson *> grid;  // days.size() * hours.size(), null where free
    const Lesson *at(size_t day, size_t hour) const { return grid[day * hours.size() + hour]; }
};
Timetable timetable(Store &s);

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
std::vector<SourceStatus> status(Store &s);

// short health check shared by `new`. errors are red, staleness is a yellow warning:
// a cron user with a long period is not broken, just slow, and can silence it (stale_warn)
struct Gripe {
    std::string text;
    bool error;
};
std::vector<Gripe> gripes(Store &s);

struct NewCounts {
    int msgs = 0, work = 0, marks = 0;
    std::vector<std::string> unsigned_pretty;
};
NewCounts new_counts(Store &s);

}  // namespace view

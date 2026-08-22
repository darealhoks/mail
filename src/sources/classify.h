#pragma once
#include <string>

namespace classify {

// empty tag set (no task, no test) renders as "info"
struct Result {
    bool task = false, test = false;
    std::string deadline;  // "" | "YYYY-MM-DD" | "YYYY-MM-DDTHH:MM"
};

// posted: "YYYY-MM-DD..." of the post, anchors the missing year
Result run(const std::string &text, bool is_task, const std::string &posted);

// unified items.kind for a classified text; a post about a test is a test first
inline std::string kind(const Result &r) { return r.test ? "test" : r.task ? "task" : "info"; }

// "YYYY-MM-DD[THH:MM]" / rfc3339 -> epoch, 0 if unparseable
long long epoch(const std::string &iso);

// same input as a deadline: local end-of-day when date-only, else the local wall time given
long long due_epoch(const std::string &iso);

// exposed for selfcheck
std::string norm(const std::string &s);

}  // namespace classify

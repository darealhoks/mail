#pragma once
#include <functional>
#include <string>
#include <vector>

#include "store.h"

namespace teams {

// appended to task-bot posts, whose real text graph will not give us (.map/sources.md);
// frontends render it as a marker, not as body text
inline const char *TASK_NOTE = "task set in teams only";

// set by maild to report cold-run progress; unset means silent
extern std::function<void(size_t done, size_t total, const std::string &what)> progress;

// channel-message delta per channel; delta links and the channel list live in store state
std::vector<Item> fetch(Store &st);

// exposed for selfcheck
std::string plain_text(const std::string &html);

}  // namespace teams

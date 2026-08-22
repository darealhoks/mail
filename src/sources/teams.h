#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "store.h"

namespace teams {

struct GraphError : std::runtime_error {
    GraphError(long s, const std::string &m) : std::runtime_error(m), status(s) {}
    long status;
};

// graph GET with retry, 401-remint and paging left to the caller; shared with outlook,
// which rides the same token
std::string graph_get(std::string &token, const std::string &url,
                      const std::vector<std::string> &extra_headers = {});

// appended to task-bot posts, whose real text graph will not give us (.map/sources.md);
// frontends render it as a marker, not as body text
inline const char *TASK_NOTE = "task set in teams only";

// set by maild to report cold-run progress; unset means silent
extern std::function<void(size_t done, size_t total, const std::string &what)> progress;

// channel-message delta per channel; delta links and the channel list live in store state
std::vector<Item> fetch(Store &st);

// html -> text, keeping the styling a terminal can show as markers: *bold* _italic_
// `code` ~strike~; paint::style_up turns them into sgr
std::string plain_text(const std::string &html);
// drop the markers again, for text that is painted with its own attributes (titles) or
// matched against (classification)
std::string style_strip(const std::string &s);

}  // namespace teams

#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.h"
#include "store.h"

namespace teams {

struct GraphError : std::runtime_error {
    GraphError(long s, const std::string &m) : std::runtime_error(m), status(s) {}
    long status;
};

// graph GET with retry and 401-remint; shared with outlook, which rides the same token
std::string graph_get(std::string &token, const std::string &url,
                      const std::vector<std::string> &extra_headers = {});

// walk a graph delta feed: resume from the cursor in state[key], cold on the first run or on a
// cursor upstream rejects, and hand each page's `value` array to `page`. the cursor is stored
// only when a deltaLink arrives, so a run that stops early never claims to be caught up
void graph_delta(std::string &token, Store &st, const std::string &key, const std::string &cold,
                 const std::vector<std::string> &headers,
                 const std::function<void(simdjson::dom::array)> &page);

// channel-message delta per channel; delta links and the channel list live in store state
std::vector<Item> fetch(Store &st);

}  // namespace teams

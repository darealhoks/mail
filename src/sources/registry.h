#pragma once
#include <functional>
#include <string>
#include <vector>

#include "store.h"

struct Source {
    const char *name, *pretty;
    const char *creds;  // creds file this source's token lives in; outlook rides teams'

    bool (*have_session)();
    std::string (*session_error)();  // "" = healthy
    std::vector<Item> (*fetch)(Store &);
    int (*login)();  // interactive, tty only
    // the source's cold-run progress hook, or nullptr if it reports none
    std::function<void(size_t done, size_t total, const std::string &what)> *progress;
};

// built-in list, filtered by [source.X] enabled
const std::vector<Source> &sources();
// nullptr when the name is unknown or the source is disabled
const Source *source(const std::string &name);

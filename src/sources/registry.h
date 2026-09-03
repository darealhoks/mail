#pragma once
#include <string>
#include <vector>

#include "store.h"

struct Source {
    const char *name, *pretty;
    const char *creds;  // creds file this source's token lives in; outlook rides teams'

    bool (*have_session)();  // local: reads the creds file, never the network
    std::vector<Item> (*fetch)(Store &);
    int (*login)();  // interactive, tty only
};

// built-in list, filtered by [source.X] enabled
const std::vector<Source> &sources();
// nullptr when the name is unknown or the source is disabled
const Source *source(const std::string &name);

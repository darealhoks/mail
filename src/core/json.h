#pragma once
#include <simdjson.h>

#include <cstdint>
#include <string>

// read-only view over an untrusted response body; every getter returns the default
// when the field is missing or of the wrong type, so callers shape-check explicitly
struct Json {
    explicit Json(const std::string &text);  // throws std::runtime_error on malformed json
    Json(const Json &) = delete;
    Json &operator=(const Json &) = delete;

    std::string str(const char *key, const std::string &def = "") const;
    int64_t num(const char *key, int64_t def = 0) const;

    simdjson::padded_string buf;
    simdjson::dom::parser parser;
    simdjson::dom::element root;
};

// html entity decode (&nbsp;, &#241;, &amp;) — shared by every source that ships html
std::string html_unescape(const std::string &s);

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

// the same getters one level down, where every source actually reads its payload. missing or
// wrong-typed is the default, never a throw: shape-checking is the caller's job
std::string jstr(simdjson::dom::element e, const char *k);
double jnum(simdjson::dom::element e, const char *k);
bool jflag(simdjson::dom::element e, const char *k);
// empty when the key is missing or is not an array
simdjson::dom::array jarr(simdjson::dom::element e, const char *k);

#include "json.h"

#include <stdexcept>

#include "text.h"

Json::Json(const std::string &text, bool tolerant) : buf(text) {
    auto r = parser.parse(buf);
    if (!r.error()) {
        root = r.value();
        return;
    }
    if (tolerant) {
        buf = simdjson::padded_string("{}", 2);  // an empty object: every getter takes its default
        root = parser.parse(buf).value();
        return;
    }
    // the body is the diagnosis: a proxy's html error page reads nothing like a truncated payload
    throw std::runtime_error(std::string("json: ") + simdjson::error_message(r.error()) +
                             " body: " + text::first_line(text, 120));
}

std::string Json::str(const char *key, const std::string &def) const {
    auto v = root.at_key(key).get_string();
    if (v.error()) return def;
    return std::string(v.value());
}

int64_t Json::num(const char *key, int64_t def) const {
    auto v = root.at_key(key).get_int64();
    if (v.error()) return def;
    return v.value();
}

std::string jstr(simdjson::dom::element e, const char *k) {
    auto v = e.at_key(k).get_string();
    if (!v.error()) return std::string(v.value());
    // bakalari ships some ids as strings and some as numbers on the same shape
    auto n = e.at_key(k).get_int64();
    return n.error() ? std::string() : std::to_string(n.value());
}

double jnum(simdjson::dom::element e, const char *k) {
    auto v = e.at_key(k).get_double();
    return v.error() ? 0.0 : v.value();
}

bool jflag(simdjson::dom::element e, const char *k) {
    auto v = e.at_key(k).get_bool();
    return v.error() ? false : v.value();
}

simdjson::dom::array jarr(simdjson::dom::element e, const char *k) {
    simdjson::dom::array a;
    if (e.at_key(k).get(a)) return simdjson::dom::array();
    return a;
}

#include "json.h"

#include <cstdlib>
#include <stdexcept>

Json::Json(const std::string &text) : buf(text) {
    auto r = parser.parse(buf);
    if (r.error()) throw std::runtime_error(std::string("json: ") + simdjson::error_message(r.error()));
    root = r.value();
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

std::string html_unescape(const std::string &s) {
    static const struct {
        const char *ent;
        const char *rep;
    } NAMED[] = {{"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},
                 {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
    std::string o;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            o += s[i++];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            o += s[i++];
            continue;
        }
        std::string e = s.substr(i, semi - i + 1);
        const char *rep = nullptr;
        for (const auto &n : NAMED)
            if (e == n.ent) rep = n.rep;
        if (rep) {
            o += rep;
        } else if (e.size() > 3 && e[1] == '#') {
            long cp = strtol(e.c_str() + (e[2] == 'x' || e[2] == 'X' ? 3 : 2), nullptr,
                             e[2] == 'x' || e[2] == 'X' ? 16 : 10);
            if (cp <= 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                o += e;
            } else if (cp < 0x80) {
                o += (char)cp;
            } else if (cp < 0x800) {
                o += (char)(0xC0 | (cp >> 6));
                o += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                o += (char)(0xE0 | (cp >> 12));
                o += (char)(0x80 | ((cp >> 6) & 0x3F));
                o += (char)(0x80 | (cp & 0x3F));
            } else {
                o += (char)(0xF0 | (cp >> 18));
                o += (char)(0x80 | ((cp >> 12) & 0x3F));
                o += (char)(0x80 | ((cp >> 6) & 0x3F));
                o += (char)(0x80 | (cp & 0x3F));
            }
        } else {
            o += e;
        }
        i = semi + 1;
    }
    return o;
}


#include "text.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <vector>

namespace text {
namespace {

bool tag_at(const std::string &s, size_t i, const char *tag) {
    size_t n = strlen(tag);
    if (strncasecmp(s.c_str() + i, tag, n)) return false;
    char c = s[i + n];  // <br>, <br/>, <br /> and <li class=…> all count
    return c == '>' || c == '/' || isspace((unsigned char)c);
}

}  // namespace

std::string utc(long long t, const char *fmt) {
    struct tm tm {};
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
    char b[64];
    strftime(b, sizeof b, fmt, &tm);
    return b;
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

std::string collapse(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        if (!strncmp(s.c_str() + i, "http://", 7) || !strncmp(s.c_str() + i, "https://", 8)) {
            size_t b = i;
            while (i < s.size() && !isspace((unsigned char)s[i])) i++;
            std::string u = s.substr(b, i - b);
            std::string marks;  // a styled url swallows its closing marker into the word
            while (!u.empty() && strchr(".,;:)]}*_`~", u.back())) {
                if (strchr("*_`~", u.back())) marks.insert(marks.begin(), u.back());
                u.pop_back();
            }
            if (o.find(u) == std::string::npos) o += u;  // teams repeats the href as link text
            o += marks;
            continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r') {
            if (!o.empty() && o.back() != '\n') o += '\n';
        } else if (isspace(c) || (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA0)) {
            if (c == 0xC2) i++;
            if (!o.empty() && o.back() != ' ' && o.back() != '\n') o += ' ';
        } else {
            o += (char)c;
        }
        i++;
    }
    std::string r;
    for (size_t i = 0; i < o.size(); i++) {
        if (o[i] != '\n') {
            r += o[i];
            continue;
        }
        while (!r.empty() && r.back() == ' ') r.pop_back();
        if (!r.empty()) r += '\n';
        while (i + 1 < o.size() && (o[i + 1] == '\n' || o[i + 1] == ' ')) i++;
    }
    while (!r.empty() && r.back() == '\n') r.pop_back();
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

std::string plain_text(const std::string &html) {
    static const char *BREAK[] = {"<br", "</p", "</div", "</li", "</tr", "<li"};
    // ponytail: element tags only; teams also bolds via <span style="font-weight:bold">,
    // which needs span-close tracking to pair up
    static const struct {
        const char *tag;
        char mark;
    } STYLE[] = {{"b", '*'},    {"strong", '*'}, {"i", '_'},  {"em", '_'},
                 {"code", '`'}, {"s", '~'},      {"del", '~'}};
    std::string t;
    std::vector<std::pair<char, size_t>> open;  // marker + where its opener landed in t
    bool in_tag = false;
    for (size_t i = 0; i < html.size(); i++) {
        if (html[i] == '<') {
            for (const char *b : BREAK)
                if (tag_at(html, i, b)) t += '\n';
            size_t n = i + 1 + (html[i + 1] == '/');
            for (const auto &s : STYLE) {
                if (!tag_at(html, n, s.tag)) continue;
                if (html[i + 1] != '/') {
                    open.emplace_back(s.mark, t.size());
                    t += s.mark;
                    break;
                }
                size_t at = std::string::npos;
                for (size_t k = open.size(); k-- > 0;)
                    if (open[k].first == s.mark && open[k].second < t.size()) {
                        at = open[k].second;
                        open.erase(open.begin() + (long)k);
                        break;
                    }
                if (at == std::string::npos) break;
                // a marker must sit against its text, or the wrap can strand it on a line
                // of its own and paint will show it literally
                std::string in = t.substr(at + 1);
                t.resize(at);
                size_t b = in.find_first_not_of(" \t\n"), e = in.find_last_not_of(" \t\n");
                if (b == std::string::npos) t += in;
                else
                    t += in.substr(0, b) + s.mark + in.substr(b, e - b + 1) + s.mark +
                         in.substr(e + 1);
                break;
            }
            in_tag = true;
        } else if (html[i] == '>') {
            in_tag = false;
            t += ' ';
        } else if (!in_tag) {
            t += html[i];
        }
    }
    // card text arrives double-escaped
    return collapse(html_unescape(html_unescape(t)));
}

std::string style_strip(const std::string &s) {
    std::string o;
    for (char c : s)
        if (!strchr("*_`~", c)) o += c;
    return o;
}

std::string strip_invisible(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size()) {
            unsigned char b = (unsigned char)s[i + 1], d = (unsigned char)s[i + 2];
            if ((b == 0x80 && d >= 0x8B && d <= 0x8D) || (b == 0x81 && d == 0xA0)) {
                i += 2;
                continue;
            }
        }
        if (c == 0xEF && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xBB &&
            (unsigned char)s[i + 2] == 0xBF) {
            i += 2;
            continue;
        }
        o += (char)c;
    }
    return o;
}

std::string first_line(const std::string &s, size_t max) {
    size_t n = std::min(s.find('\n'), s.size());
    if (n > max) {
        size_t sp = s.rfind(' ', max);
        n = sp == std::string::npos ? max : sp;
    }
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;  // never cut mid-codepoint
    return s.substr(0, n);
}

}  // namespace text

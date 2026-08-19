#include "classify.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace classify {
namespace {

// U+00C0..U+017F -> base ascii letter, '?' = keep the codepoint (no NFD decomposition)
const char *FOLD =
    "aaaaaa?ceeeeiiii"
    "?nooooo??uuuuy??"
    "aaaaaa?ceeeeiiii"
    "?nooooo??uuuuy?y"
    "aaaaaaccccccccdd"
    "??eeeeeeeeeegggg"
    "gggghh??iiiiiiii"
    "i???jjkk?lllllll"
    "l??nnnnnn???oooo"
    "oo??rrrrrrssssss"
    "sstttt??uuuuuuuu"
    "uuuuwwyyyzzzzzz?";

bool isw(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || (u >= 'A' && u <= 'Z');
}
bool isl(char c) { return c >= 'a' && c <= 'z'; }
bool isd(char c) { return c >= '0' && c <= '9'; }

bool bound_l(const std::string &t, size_t p) { return p == 0 || !isw(t[p - 1]); }
bool bound_r(const std::string &t, size_t p) { return p >= t.size() || !isw(t[p]); }
size_t word_end(const std::string &t, size_t p) {
    while (p < t.size() && isw(t[p])) p++;
    return p;
}

// word-start `pre` plus a \w* tail; end = end of that word
size_t find_pre(const std::string &t, const char *pre, size_t from, size_t *end = nullptr) {
    size_t n = strlen(pre);
    for (size_t p = t.find(pre, from); p != std::string::npos; p = t.find(pre, p + 1))
        if (bound_l(t, p)) {
            if (end) *end = word_end(t, p + n);
            return p;
        }
    return std::string::npos;
}

bool has_pre(const std::string &t, const char *pre) {
    return find_pre(t, pre, 0) != std::string::npos;
}

// whole word / phrase, no tail allowed
bool has_word(const std::string &t, const char *w) {
    size_t n = strlen(w);
    for (size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1))
        if (bound_l(t, p) && bound_r(t, p + n)) return true;
    return false;
}

// stem + at most 4 trailing letters, whole word ("test/testu/testech", "pisemn*")
bool has_stem(const std::string &t, const char *pre) {
    size_t n = strlen(pre);
    for (size_t p = t.find(pre); p != std::string::npos; p = t.find(pre, p + 1)) {
        if (!bound_l(t, p)) continue;
        size_t e = p + n, k = 0;
        while (e < t.size() && isl(t[e]) && k < 5) e++, k++;
        if (k <= 4 && bound_r(t, e)) return true;
    }
    return false;
}

const char *TEST_STEM[] = {
    "pisemk", "pisemna prac", "pisemnou prac", "pisemne prac", "pisemne opakovan",
    "pisemnem opakovan", "pisemneho opakovan", "pisemne zkousen",
    "desetiminutovk", "petiminutovk", "patnactiminutovk", "ctvrtletk", "ctvrtletn prac",
    "slohov prac", "sloh", "opakovac", "provere", "zkousen", "prezkousen", nullptr};

// explicit endings so "testbot" in a pasted snippet does not count
const char *TEST_END[] = {"", "u", "y", "em", "ech", "ovi", "ik", "iku", "iky", nullptr};

const char *TASK_STEM[] = {
    "odevzd", "vypracuj", "vypracovan", "zpracuj", "vytvor", "nahraj", "zaslat", "zaslete",
    "posli", "poslete", "doplnte", "vyplnte", "prines", "prineste", "doneste",
    "ukol", "referat", "seminarn prac", "pololetn prac", "pripadov studi", "esej", nullptr};

const char *ADMIN_PRE[] = {"aktiv", "zaregistr", "registr", "prihlas", "potvrd", "zapis",
                           "objednej", "objednat", "uhrad", "zaplat", nullptr};

const char *MON3[] = {"led", "uno", "bre", "dub", "kve", "cvn",
                      "cvc", "srp", "zar", "rij", "lis", "pro"};
const char *EN3[] = {"jan", "feb", "mar", "apr", "may", "jun",
                     "jul", "aug", "sep", "oct", "nov", "dec"};
const char *WMON[] = {"ledna", "unora", "brezna", "dubna", "kvetna", "cervna",
                      "cervence", "srpna", "zari", "rijna", "listopadu", "prosince"};

bool word_test(const std::string &t) {
    for (size_t p = t.find("test"); p != std::string::npos; p = t.find("test", p + 1)) {
        if (!bound_l(t, p)) continue;
        size_t e = word_end(t, p + 4);
        std::string tail = t.substr(p + 4, e - p - 4);
        for (int i = 0; TEST_END[i]; i++)
            if (tail == TEST_END[i]) return true;
    }
    return false;
}

// "náhradní/opravný/dodatečný (a jiný) termín"
bool alt_termin(const std::string &t) {
    const char *pre[] = {"nahradn", "opravn", "dodatecn", nullptr};
    for (int i = 0; pre[i]; i++) {
        size_t e, p = 0;
        while ((p = find_pre(t, pre[i], p, &e)) != std::string::npos) {
            if (e < t.size() && t[e] == ' ') {
                size_t q = e + 1;
                if (t.compare(q, 2, "a ") == 0) {
                    size_t w = word_end(t, q + 2);
                    if (w > q + 2 && w < t.size() && t[w] == ' ') q = w + 1;
                }
                if (t.compare(q, 6, "termin") == 0) return true;
            }
            p++;
        }
    }
    return false;
}

// "aktivujte si účet do 2. 2." -- admin verb and a due date within 120 chars, either order
bool admin_due(const std::string &t) {
    for (int i = 0; ADMIN_PRE[i]; i++) {
        size_t ae, ap = 0;
        while ((ap = find_pre(t, ADMIN_PRE[i], ap, &ae)) != std::string::npos) {
            for (size_t dp = 0; (dp = t.find("do ", dp)) != std::string::npos; dp++) {
                if (!bound_l(t, dp)) continue;
                size_t q = dp + 3;
                while (q < t.size() && t[q] == ' ') q++;
                size_t ds = q;
                while (q < t.size() && isd(t[q]) && q - ds < 2) q++;
                if (q == ds || q >= t.size() || t[q] != '.') continue;
                size_t de = q + 1;
                if (ae <= dp && dp - ae <= 120) return true;
                if (de <= ap && ap - de <= 120) return true;
            }
            ap++;
        }
    }
    return false;
}

bool mluvni(const std::string &t) {
    size_t e, p = 0;
    while ((p = find_pre(t, "mluvn", p, &e)) != std::string::npos) {
        if (e < t.size() && t[e] == ' ' && t.compare(e + 1, 6, "cvicen") == 0) return true;
        p++;
    }
    p = 0;
    while ((p = find_pre(t, "cvicen", p, &e)) != std::string::npos) {
        if (e < t.size() && t[e] == ' ' && t.compare(e + 1, 5, "mluvn") == 0) return true;
        p++;
    }
    return false;
}

// "příprava na test" is homework about a test; blank it before the test rules run
void mask_prep(std::string &t) {
    size_t e, p = 0;
    while ((p = find_pre(t, "priprav", p, &e)) != std::string::npos) {
        size_t q = e;
        if (q < t.size() && t[q] == ' ') {
            q++;
            if (t.compare(q, 3, "se ") == 0) q += 3;
            if (t.compare(q, 3, "na ") == 0) {
                q += 3;
                size_t w = word_end(t, q);
                size_t cand = w > q && w < t.size() && t[w] == ' ' &&
                                      t.compare(w + 1, 4, "test") == 0
                                  ? w + 1
                                  : q;
                if (t.compare(cand, 4, "test") == 0) {
                    size_t end = word_end(t, cand + 4);
                    t.replace(p, end - p, end - p, ' ');
                    p = end;
                    continue;
                }
            }
        }
        p++;
    }
    p = 0;
    while ((p = find_pre(t, "na nadchazejici test", p, &e)) != std::string::npos) {
        t.replace(p, e - p, e - p, ' ');
        p = e;
    }
    for (size_t q = t.find("pred testem"); q != std::string::npos; q = t.find("pred testem", q + 1))
        if (bound_l(t, q) && bound_r(t, q + 11)) t.replace(q, 11, 11, ' ');
    p = 0;
    while ((p = find_pre(t, "studijni", p, &e)) != std::string::npos) {
        size_t q = e;
        if (q < t.size() && t[q] == ' ') {
            size_t w = word_end(t, q + 1);
            if (w > q + 1 && t.compare(w, 13, " pro pripravu") == 0) {
                t.replace(p, w + 13 - p, w + 13 - p, ' ');
                p = w + 13;
                continue;
            }
        }
        p++;
    }
}

bool due_ctx(const std::string &w) {
    const char *pre[] = {"termin", "odevzd", "pisem", "test", "prinest", "sloh", "pisemk",
                         "provere", "opakovan", "desetiminutovk", "petiminutovk", "ctvrtletk",
                         nullptr};
    for (int i = 0; pre[i]; i++)
        if (has_pre(w, pre[i])) return true;
    const char *words[] = {"do", "nejpozdeji", "pisete", "napiste", "prineste", "budeme psat",
                           nullptr};
    for (int i = 0; words[i]; i++)
        if (has_word(w, words[i])) return true;
    return false;
}

long long civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool valid(int y, int m, int d) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return false;
    int max = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) max = 29;
    return d <= max;
}

// cards carry no year: roll forward when the date lands more than a week before the post
std::string mk(int y, int m, int d, long long anchor) {
    if (!valid(y, m, d)) return "";
    if (anchor >= 0 && civil(y, m, d) < anchor - 7) {
        y++;
        if (!valid(y, m, d)) return "";
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

// 1-2 digits at *p (word start not checked here), advances p
bool num2(const std::string &t, size_t &p, int &out) {
    size_t s = p;
    while (p < t.size() && isd(t[p]) && p - s < 2) p++;
    if (p == s) return false;
    out = atoi(t.substr(s, p - s).c_str());
    return true;
}

std::string find_time(const std::string &w) {
    for (size_t c = w.find(':'); c != std::string::npos; c = w.find(':', c + 1)) {
        size_t hs = c;
        while (hs > 0 && isd(w[hs - 1])) hs--;
        size_t hn = c - hs;
        if (hn < 1 || hn > 2 || !bound_l(w, hs)) continue;
        size_t me = c + 1;
        while (me < w.size() && isd(w[me])) me++;
        if (me - c - 1 != 2 || !bound_r(w, me)) continue;
        int h = atoi(w.substr(hs, hn).c_str()), mi = atoi(w.substr(c + 1, 2).c_str());
        if (h < 0 || h > 23 || mi < 0 || mi > 59) continue;
        char buf[16];
        snprintf(buf, sizeof buf, "T%02d:%02d", h, mi);
        return buf;
    }
    return "";
}

}  // namespace

long long epoch(const std::string &iso) {
    struct tm tm {};
    if (iso.size() < 10 || !strptime(iso.substr(0, 10).c_str(), "%Y-%m-%d", &tm)) return 0;
    long long t = (long long)timegm(&tm);
    if (t < -62135596800LL || t > 253402300799LL) return 0;  // year 1..9999
    if (iso.size() >= 16 && iso[10] == 'T') {
        long long h = strtoll(iso.c_str() + 11, nullptr, 10);
        long long mi = strtoll(iso.c_str() + 14, nullptr, 10);
        if (h < 0 || h > 23 || mi < 0 || mi > 59) return t;
        t += h * 3600 + mi * 60;
    }
    return t;
}

std::string norm(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    bool sp = false;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int cp = -1, len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            len = 2;
        } else {
            len = (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        }
        if (cp >= 0 && cp < 0x80) {
            char ch = (char)(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
            if (ch == ' ' || (ch >= 9 && ch <= 13)) {
                if (!sp) o += ' ';
                sp = true;
            } else {
                o += ch;
                sp = false;
            }
        } else {
            char f = cp >= 0xC0 && cp <= 0x17F ? FOLD[cp - 0xC0] : '?';
            if (f != '?') {
                o += f;
            } else {
                o.append(s, i, len);
            }
            sp = false;
        }
        i += len;
    }
    return o;
}

Result run(const std::string &text, bool is_task, const std::string &posted) {
    Result r;
    size_t b = text.find_first_not_of(" \t\r\n");
    bool banner = b != std::string::npos && text.compare(b, 4, "\xf0\x9f\x93\xa2") == 0;

    std::string t = norm(text);

    long long anchor = -1;
    int py = 0, pm = 0, pd = 0;
    if (posted.size() >= 10 && sscanf(posted.c_str(), "%4d-%2d-%2d", &py, &pm, &pd) == 3 &&
        valid(py, pm, pd))
        anchor = civil(py, pm, pd);
    int year = py ? py : 2026;

    if (!banner) {
        std::string tt = t;
        mask_prep(tt);
        bool test = word_test(tt) || alt_termin(tt) || has_pre(tt, "dopisovan") ||
                    has_pre(tt, "dopsan");
        for (int i = 0; TEST_STEM[i] && !test; i++) test = has_stem(tt, TEST_STEM[i]);
        // bare "opakování" names a topic as often as a test; only counts next to written wording
        if (!test && has_pre(tt, "opakovan"))
            test = has_pre(tt, "pisemn") || has_pre(tt, "napis") || has_word(tt, "psat");
        r.test = test;

        bool task = is_task;
        for (int i = 0; TASK_STEM[i] && !task; i++) task = has_stem(t, TASK_STEM[i]);
        if (!task) {
            size_t e, p = 0;
            while (!task && (p = find_pre(t, "termin", p, &e)) != std::string::npos) {
                if (e < t.size() && t[e] == ' ') {
                    size_t q = e + 1;
                    task = t.compare(q, 8, "odevzdan") == 0 || t.compare(q, 6, "splnen") == 0 ||
                           t.compare(q, 9, "dokonceni") == 0;
                }
                p++;
            }
        }
        r.task = task || mluvni(t) || admin_due(t);
    }

    // card due lines first, both ui locales occur
    bool hit = false;
    for (size_t p = t.find("termin splneni "); p != std::string::npos && !hit;
         p = t.find("termin splneni ", p + 1)) {
        size_t q = p + 15;
        int d;
        if (!num2(t, q, d) || q + 1 >= t.size() || t[q] != '.' || t[q + 1] != ' ') continue;
        q += 2;
        for (int m = 0; m < 12; m++)
            if (t.compare(q, 3, MON3[m]) == 0) {
                r.deadline = mk(year, m + 1, d, anchor);
                hit = true;
                break;
            }
    }
    if (!r.deadline.empty()) return r;
    hit = false;
    for (size_t p = 0; !hit && (p = find_pre(t, "due ", p)) != std::string::npos; p++) {
        for (int m = 0; m < 12; m++) {
            if (t.compare(p + 4, 3, EN3[m])) continue;
            size_t q = word_end(t, p + 4);
            if (q >= t.size() || t[q] != ' ') break;
            q++;
            int d;
            size_t ds = q;
            if (num2(t, q, d) && bound_r(t, q) && q > ds) {
                r.deadline = mk(year, m + 1, d, anchor);
                hit = true;
            }
            break;
        }
    }
    if (!r.deadline.empty()) return r;

    for (size_t p = 0; p + 1 < t.size(); p++) {
        if (!isd(t[p]) || !bound_l(t, p)) continue;
        size_t q = p;
        int d = 0, m = 0, y = year;
        if (!num2(t, q, d) || q >= t.size() || t[q] != '.') {
            p = word_end(t, p) - 1;
            continue;
        }
        q++;
        if (q < t.size() && t[q] == ' ') q++;
        size_t after = q;
        bool got = false;
        if (q < t.size() && isd(t[q])) {
            size_t mq = q;
            if (num2(t, mq, m) && mq < t.size() && t[mq] == '.') {
                after = mq + 1;
                got = true;
                size_t yq = after;
                if (yq < t.size() && t[yq] == ' ') yq++;
                size_t ys = yq;
                while (yq < t.size() && isd(t[yq]) && yq - ys < 4) yq++;
                if (yq - ys == 4) {
                    y = atoi(t.substr(ys, 4).c_str());
                    after = yq;
                }
            }
        } else {
            for (int i = 0; i < 12 && !got; i++) {
                size_t n = strlen(WMON[i]);
                if (t.compare(q, n, WMON[i]) == 0 && bound_r(t, q + n)) {
                    m = i + 1;
                    after = q + n;
                    got = true;
                }
            }
        }
        if (!got) {
            p = word_end(t, p) - 1;
            continue;
        }
        // a bare date is only a deadline next to due/test wording
        size_t ws = p > 60 ? p - 60 : 0;
        std::string w = t.substr(ws, after + 20 - ws);
        if (!due_ctx(w)) continue;
        std::string iso = mk(y, m, d, anchor);
        if (iso.empty()) continue;
        r.deadline = iso + find_time(w);
        return r;
    }
    return r;
}

}  // namespace classify

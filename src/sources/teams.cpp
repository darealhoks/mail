#include <algorithm>
#include "teams.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

#include "classify.h"
#include "config.h"
#include "creds.h"
#include "http.h"
#include "json.h"
#include "teams_auth.h"

namespace teams {
namespace {

const char *G = "https://graph.microsoft.com/v1.0";
const long long DAY = 86400;
const long long CHANNELS_TTL = DAY;
const int MAX_PAGES = 200;
const char *TASK_BOT = "7254e396-868c-4bf7-96b2-6fe763590b5a";

long long now() { return (long long)time(nullptr); }

// oauth::access_token hands back the cached token until it is near expiry; dropping it
// from the creds file ("teams" == CREDS in teams_auth.cpp) is what forces a real refresh
std::string remint() {
    auto kv = creds_load("teams");
    kv.erase("access_token");
    kv.erase("access_expires_at");
    creds_save("teams", kv);
    return access_token();
}

}  // namespace

std::string graph_get(std::string &token, const std::string &url,
                      const std::vector<std::string> &extra_headers) {
    std::vector<std::string> h{"Authorization: Bearer " + token};
    h.insert(h.end(), extra_headers.begin(), extra_headers.end());
    bool reminted = false;
    for (int attempt = 0;; attempt++) {
        HttpResponse r = http_get(url, h);
        if (r.status == 200) return r.body;
        // ponytail: blind backoff, read Retry-After once http.cpp exposes response headers
        if ((r.status == 429 || r.status >= 500) && attempt < 3) {
            std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
            continue;
        }
        if (r.status == 401) {
            // a sweep can outlive the token; only a 401 on a freshly minted one is a dead session
            if (reminted) throw SessionExpired("teams: token rejected on " + url);
            reminted = true;
            token = remint();
            h[0] = "Authorization: Bearer " + token;
            attempt = -1;
            continue;
        }
        throw GraphError(r.status, "graph: http " + std::to_string(r.status) + " on " + url);
    }
}

namespace {

std::string iso_utc(long long t) {
    struct tm tm {};
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

bool tag_at(const std::string &s, size_t i, const char *tag) {
    size_t n = strlen(tag);
    if (strncasecmp(s.c_str() + i, tag, n)) return false;
    char c = s[i + n];  // <br>, <br/>, <br /> and <li class=…> all count
    return c == '>' || c == '/' || isspace((unsigned char)c);
}

// same pipeline as analysis/clean.py: block tags -> breaks, tags out, entities twice
// (card text arrives double-escaped), urls kept whole, blocks kept as newlines
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

}  // namespace

std::function<void(size_t, size_t, const std::string &)> progress;

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
                        open.erase(open.begin() + k);
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
    return collapse(html_unescape(html_unescape(t)));
}

std::string style_strip(const std::string &s) {
    std::string o;
    for (char c : s)
        if (!strchr("*_`~", c)) o += c;
    return o;
}

namespace {

void card_text(simdjson::dom::element e, std::vector<std::string> &out, int depth = 0) {
    if (depth > 24) return;
    simdjson::dom::object o;
    if (!e.get(o)) {
        auto type = o.at_key("type").get_string();
        auto text = o.at_key("text").get_string();
        if (!type.error() && type.value() == "TextBlock" && !text.error() && !text.value().empty())
            out.emplace_back(text.value());
        for (auto kv : o) card_text(kv.value, out, depth + 1);
        return;
    }
    simdjson::dom::array a;
    if (!e.get(a))
        for (auto v : a) card_text(v, out, depth + 1);
}

std::string sv(simdjson::dom::element e, const char *k) {
    auto v = e.at_key(k).get_string();
    return v.error() ? std::string() : std::string(v.value());
}

struct Channel {
    std::string team_id, team, id, name;
};

// flat 0x1f-separated stream of 4-field groups; 0x1f cannot occur in a graph displayName
std::vector<Channel> load_channels(const std::string &blob) {
    std::vector<std::string> f;
    for (size_t p = 0; p <= blob.size();) {
        size_t e = blob.find('\x1f', p);
        if (e == std::string::npos) break;
        f.push_back(blob.substr(p, e - p));
        p = e + 1;
    }
    std::vector<Channel> out;
    for (size_t i = 0; i + 3 < f.size(); i += 4)
        if (!f[i].empty() && !f[i + 2].empty()) out.push_back({f[i], f[i + 1], f[i + 2], f[i + 3]});
    return out;
}

std::vector<Channel> channels(std::string &token, Store &st) {
    long long at = strtoll(st.get_state("teams.channels_at").c_str(), nullptr, 10);
    if (now() - at < CHANNELS_TTL) {
        auto c = load_channels(st.get_state("teams.channels"));
        if (!c.empty()) return c;
    }
    std::vector<Channel> out;
    std::string blob;
    Json teams(graph_get(token, std::string(G) + "/me/joinedTeams"));
    auto tv = teams.root.at_key("value").get_array();
    if (tv.error()) throw std::runtime_error("teams: joinedTeams missing value array");
    for (auto t : tv.value()) {
        std::string tid = sv(t, "id");
        if (tid.empty()) continue;
        std::string tname = sv(t, "displayName");
        Json chs(graph_get(token, std::string(G) + "/teams/" + tid + "/channels"));
        auto cv = chs.root.at_key("value").get_array();
        if (cv.error()) continue;
        for (auto c : cv.value()) {
            std::string cid = sv(c, "id");
            if (cid.empty()) continue;
            std::string cname = sv(c, "displayName");
            out.push_back({tid, tname, cid, cname});
            blob += tid + "\x1f" + tname + "\x1f" + cid + "\x1f" + cname + "\x1f";
        }
    }
    st.set_state("teams.channels", blob);
    st.set_state("teams.channels_at", std::to_string(now()));
    return out;
}

// one message -> zero or one item; returns false for system events, deletions and empties
bool to_item(simdjson::dom::element m, const Channel &ch, Item &out) {
    if (sv(m, "messageType") != "message") return false;
    if (!m.at_key("deletedDateTime").error() && !m.at_key("deletedDateTime").value().is_null())
        return false;
    std::string id = sv(m, "id");
    if (id.empty()) return false;

    std::string body;
    simdjson::dom::element b;
    if (!m.at_key("body").get(b)) body = plain_text(sv(b, "content"));
    std::string subject = plain_text(sv(m, "subject"));

    std::vector<std::string> cards, files;
    simdjson::dom::array atts;
    if (!m.at_key("attachments").get(atts)) {
        for (auto a : atts) {
            std::string ct = sv(a, "contentType"), content = sv(a, "content");
            if (ct.size() >= 13 && !ct.compare(ct.size() - 13, 13, "card.adaptive")) {
                std::vector<std::string> parts;
                try {
                    Json card(content);
                    card_text(card.root, parts);
                } catch (const std::exception &) {
                    continue;  // link-preview cards are sometimes not json we can read
                }
                std::string joined;
                for (const auto &p : parts) {
                    std::string t = collapse(p);
                    if (!t.empty()) joined += (joined.empty() ? "" : " / ") + t;
                }
                if (!joined.empty()) cards.push_back(joined);
            } else {
                std::string n = sv(a, "name");
                if (!n.empty()) files.push_back(collapse(n));
            }
        }
    }

    std::string text;  // subject-less; the subject is prepended only for classification
    for (const auto &c : cards) text += (text.empty() ? "" : "\n") + c;
    if (!body.empty()) text += (text.empty() ? "" : "\n") + body;
    for (size_t i = 0; i < files.size(); i++) text += (i ? ", " : " [att: ") + files[i];
    if (!files.empty()) text += "]";
    if (text.empty() && subject.empty()) return false;

    // an adaptive card counts as a task only if it carries a due line; youtube
    // link previews arrive as adaptive cards too (analysis/README.md)
    bool task = false;
    for (const auto &c : cards)
        if (c.find("Termín splnění") != std::string::npos || c.find("Due ") != std::string::npos)
            task = true;

    std::string created = sv(m, "createdDateTime");
    classify::Result r = classify::run(
        style_strip(subject + (subject.empty() ? "" : " ") + text), task, created);

    out.source = "teams";
    out.klass = ch.name.empty() || ch.name == "General" || ch.name == "Obecné"
                    ? ch.team
                    : ch.team + "/" + ch.name;
    out.kind = classify::kind(r);
    // posts usually carry a real subject; only headerless ones borrow the body's first slice
    out.title = !subject.empty()
                    ? subject
                    : text.substr(0, std::min(text.find('\n'),
                                                text.size() > 120 ? text.rfind(' ', 120) : text.size()));
    out.body = text;
    // the tasks bot posts only a title+due card; the instructions live on the education
    // object graph refuses to us (.map/sources.md) — say so instead of looking empty
    simdjson::dom::element app;
    if (!m.at_key("from").at_key("application").get(app) && sv(app, "id") == TASK_BOT)
        out.body += std::string("\n") + TASK_NOTE;
    out.due_at = classify::due_epoch(r.deadline);
    out.event_at = classify::epoch(created);
    out.src_uid = "msg:" + id;
    out.url = sv(m, "webUrl");
    return true;
}

}  // namespace

std::vector<Item> fetch(Store &st) {
    std::string token = access_token();
    std::vector<Item> out;

    std::vector<Channel> chs = channels(token, st);
    size_t done = 0;
    for (const auto &ch : chs) {
        if (progress) progress(++done, chs.size(), ch.team);
        std::string base = std::string(G) + "/teams/" + ch.team_id + "/channels/" + ch.id;
        std::string cold = base + "/messages/delta?$top=50&$filter=lastModifiedDateTime%20gt%20" +
                           iso_utc(scrape_since());
        std::string key = "teams.delta." + ch.id;
        std::string url = st.get_state(key);
        bool resumed = !url.empty();
        if (!resumed) url = cold;

        for (int page = 0; page < MAX_PAGES && !url.empty(); page++) {
            std::string body;
            try {
                body = graph_get(token, url);
            } catch (const GraphError &e) {
                // access to the channel is gone (left the team, archived, went private):
                // drop it and expire the channel cache so the next run re-lists without it
                if (e.status == 403 || e.status == 404) {
                    st.set_state(key, "");
                    st.set_state("teams.channels_at", "0");
                    fprintf(stderr, "teams: dropping %s / %s: http %ld\n", ch.team.c_str(),
                            ch.name.c_str(), e.status);
                    break;
                }
                // an expired or rejected delta token only resets that channel
                if (!resumed || page || (e.status != 400 && e.status != 410)) throw;
                st.set_state(key, "");
                url = cold;
                resumed = false;
                body = graph_get(token, url);
            }
            Json j(body);
            auto v = j.root.at_key("value").get_array();
            if (v.error()) throw std::runtime_error("teams: delta missing value array");
            for (auto m : v.value()) {
                Item i;
                if (to_item(m, ch, i)) out.push_back(std::move(i));
            }
            std::string next = sv(j.root, "@odata.nextLink");
            if (!next.empty()) {
                url = next;
                continue;
            }
            st.set_state(key, sv(j.root, "@odata.deltaLink"));
            break;
        }
    }
    return out;
}

}  // namespace teams

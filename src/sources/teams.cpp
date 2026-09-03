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
#include "http.h"
#include "json.h"
#include "text.h"
#include "teams_auth.h"

namespace teams {
namespace {

const char *G = "https://graph.microsoft.com/v1.0";
const long long DAY = 86400;
const long long CHANNELS_TTL = DAY;
const int MAX_PAGES = 200;
const char *TASK_BOT = "7254e396-868c-4bf7-96b2-6fe763590b5a";
const char *ISO = "%Y-%m-%dT%H:%M:%SZ";

long long now() { return (long long)time(nullptr); }

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
            forget_access();
            token = access_token();
            h[0] = "Authorization: Bearer " + token;
            attempt = -1;
            continue;
        }
        throw GraphError(r.status, "graph: http " + std::to_string(r.status) + " on " + url);
    }
}

void graph_delta(std::string &token, Store &st, const std::string &key, const std::string &cold,
                 const std::vector<std::string> &headers,
                 const std::function<void(simdjson::dom::array)> &page) {
    std::string url = st.get_state(key);
    bool resumed = !url.empty();
    if (!resumed) url = cold;

    for (int n = 0; n < MAX_PAGES; n++) {
        std::string body;
        try {
            body = graph_get(token, url, headers);
        } catch (const GraphError &e) {
            // an expired or rejected cursor costs one cold sweep, nothing else
            if (!resumed || n || (e.status != 400 && e.status != 410)) throw;
            st.set_state(key, "");
            url = cold;
            resumed = false;
            body = graph_get(token, url, headers);
        }
        Json j(body);
        simdjson::dom::array v;
        if (j.root.at_key("value").get(v))
            throw std::runtime_error("graph: delta missing value array on " + key);
        page(v);
        std::string next = jstr(j.root, "@odata.nextLink");
        if (next.empty()) {
            st.set_state(key, jstr(j.root, "@odata.deltaLink"));
            return;
        }
        url = next;
    }
    throw std::runtime_error("graph: " + key + " longer than " + std::to_string(MAX_PAGES) +
                             " pages, no cursor stored; raise MAX_PAGES");
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
        std::string tid = jstr(t, "id");
        if (tid.empty()) continue;
        std::string tname = jstr(t, "displayName");
        Json chs(graph_get(token, std::string(G) + "/teams/" + tid + "/channels"));
        auto cv = chs.root.at_key("value").get_array();
        if (cv.error()) continue;
        for (auto c : cv.value()) {
            std::string cid = jstr(c, "id");
            if (cid.empty()) continue;
            std::string cname = jstr(c, "displayName");
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
    if (jstr(m, "messageType") != "message") return false;
    if (!m.at_key("deletedDateTime").error() && !m.at_key("deletedDateTime").value().is_null())
        return false;
    std::string id = jstr(m, "id");
    if (id.empty()) return false;

    std::string body;
    simdjson::dom::element b;
    if (!m.at_key("body").get(b)) body = text::plain_text(jstr(b, "content"));
    std::string subject = text::plain_text(jstr(m, "subject"));

    std::vector<std::string> cards, files;
    simdjson::dom::array atts;
    if (!m.at_key("attachments").get(atts)) {
        for (auto a : atts) {
            std::string ct = jstr(a, "contentType"), content = jstr(a, "content");
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
                    std::string t = text::collapse(p);
                    if (!t.empty()) joined += (joined.empty() ? "" : " / ") + t;
                }
                if (!joined.empty()) cards.push_back(joined);
            } else {
                std::string n = jstr(a, "name");
                if (!n.empty()) files.push_back(text::collapse(n));
            }
        }
    }

    std::string full;  // subject-less; the subject is prepended only for classification
    for (const auto &c : cards) full += (full.empty() ? "" : "\n") + c;
    if (!body.empty()) full += (full.empty() ? "" : "\n") + body;
    for (size_t i = 0; i < files.size(); i++) full += (i ? ", " : " [att: ") + files[i];
    if (!files.empty()) full += "]";
    if (full.empty() && subject.empty()) return false;

    // an adaptive card counts as a task only if it carries a due line; youtube
    // link previews arrive as adaptive cards too
    bool task = false;
    for (const auto &c : cards)
        if (c.find("Termín splnění") != std::string::npos || c.find("Due ") != std::string::npos)
            task = true;

    std::string created = jstr(m, "createdDateTime");
    classify::Result r = classify::run(
        text::style_strip(subject + (subject.empty() ? "" : " ") + full), task, created);

    out.source = "teams";
    out.klass = ch.name.empty() || ch.name == "General" || ch.name == "Obecné"
                    ? ch.team
                    : ch.team + "/" + ch.name;
    out.kind = classify::kind(r);
    // posts usually carry a real subject; only headerless ones borrow the body's first slice
    out.title = subject.empty() ? text::first_line(full, 120) : subject;
    out.body = full;
    // the tasks bot posts only a title+due card; the instructions live on the education
    // object graph refuses to us (.map/sources.md) — say so instead of looking empty
    simdjson::dom::element app;
    if (!m.at_key("from").at_key("application").get(app) && jstr(app, "id") == TASK_BOT)
        out.body += std::string("\n") + text::TASK_NOTE;
    out.due_at = classify::due_epoch(r.deadline);
    out.event_at = classify::epoch(created);
    out.src_uid = "msg:" + id;
    out.url = jstr(m, "webUrl");
    return true;
}

}  // namespace

std::vector<Item> fetch(Store &st) {
    std::string token = access_token();
    std::vector<Item> out;

    for (const auto &ch : channels(token, st)) {
        std::string base = std::string(G) + "/teams/" + ch.team_id + "/channels/" + ch.id;
        std::string cold = base + "/messages/delta?$top=50&$filter=lastModifiedDateTime%20gt%20" +
                           text::utc(scrape_since(), ISO);
        std::string key = "teams.delta." + ch.id;
        try {
            graph_delta(token, st, key, cold, {}, [&](simdjson::dom::array v) {
                for (auto m : v) {
                    Item i;
                    if (to_item(m, ch, i)) out.push_back(std::move(i));
                }
            });
        } catch (const GraphError &e) {
            // access to the channel is gone (left the team, archived, went private):
            // drop it and expire the channel cache so the next run re-lists without it
            if (e.status != 403 && e.status != 404) throw;
            st.set_state(key, "");
            st.set_state("teams.channels_at", "0");
            fprintf(stderr, "teams: dropping %s / %s: http %ld\n", ch.team.c_str(),
                    ch.name.c_str(), e.status);
        }
    }
    return out;
}

}  // namespace teams

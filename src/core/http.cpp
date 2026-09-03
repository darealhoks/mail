#include "http.h"

#include <curl/curl.h>

#include <chrono>
#include <string>
#include <stdexcept>
#include <thread>

namespace {

const size_t MAX_BODY = 32u << 20;

// a c callback: no exception may cross it, and a short return aborts the transfer
size_t sink(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    auto *b = static_cast<std::string *>(ud);
    if (b->size() + n > MAX_BODY) return 0;
    try {
        b->append(ptr, n);
    } catch (...) {
        return 0;
    }
    return n;
}

struct Curl {
    CURL *h;
    Curl() : h(curl_easy_init()) {
        if (!h) throw std::runtime_error("curl_easy_init failed");
    }
    ~Curl() { curl_easy_cleanup(h); }
};

// one handle per thread so the connection (and its tls session) survives between calls
CURL *handle() {
    thread_local Curl c;
    curl_easy_reset(c.h);  // drops every option including a stale CURLOPT_HTTPHEADER slist
    return c.h;
}

HttpResponse perform(CURL *h, std::string &body) {
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1000L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, APP_NAME "/0.1");
    CURLcode rc = curl_easy_perform(h);
    // no bytes written, so a replay cannot duplicate anything. a timeout is not replayed: the
    // 20s low-speed window already gave up on a link that was answering, and a stalled server
    // would just cost that window twice per run
    if (rc != CURLE_OK && rc != CURLE_OPERATION_TIMEDOUT && body.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        rc = curl_easy_perform(h);
    }
    if (rc != CURLE_OK) {
        // OFFLINE_TAG is matched in view.cpp: no connectivity is not a fault worth shouting about.
        // a timeout or a dropped transfer only counts as offline if the connection never came up —
        // a server that takes the connection and then stalls is a fault, and must stay loud
        // primary ip, not connect_time: a retry over a kept-alive connection reports no connect
        char *ip = nullptr;
        curl_easy_getinfo(h, CURLINFO_PRIMARY_IP, &ip);
        bool connected = ip && *ip;
        bool off = rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY ||
                   rc == CURLE_COULDNT_CONNECT ||
                   (!connected && (rc == CURLE_OPERATION_TIMEDOUT ||
                                   rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR));
        std::string what = curl_easy_strerror(rc);
        if (!off && rc == CURLE_OPERATION_TIMEDOUT) what = "server accepted the connection and never answered";
        throw std::runtime_error(std::string(off ? OFFLINE_TAG : "http: ") + what);
    }
    HttpResponse r;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
    r.body = std::move(body);
    return r;
}

HttpResponse with_headers(CURL *h, const std::vector<std::string> &headers) {
    std::string out;
    curl_slist *hl = nullptr;
    for (const auto &s : headers) hl = curl_slist_append(hl, s.c_str());
    if (hl) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
    try {
        HttpResponse r = perform(h, out);
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(hl);
        return r;
    } catch (...) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(hl);
        throw;
    }
}

}  // namespace

HttpResponse http_post_form(const std::string &url, const std::string &body,
                            const std::vector<std::string> &headers) {
    CURL *h = handle();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body.size());
    return with_headers(h, headers);
}

HttpResponse http_get(const std::string &url, const std::vector<std::string> &headers) {
    CURL *h = handle();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);  // posts never follow: a 302 would re-send creds
    return with_headers(h, headers);
}

std::string url_encode(const std::string &s) {
    Curl c;
    char *e = curl_easy_escape(c.h, s.data(), (int)s.size());
    if (!e) throw std::runtime_error("url_encode failed");
    std::string out(e);
    curl_free(e);
    return out;
}

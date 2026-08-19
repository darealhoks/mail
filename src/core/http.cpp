#include "http.h"

#include <curl/curl.h>

#include <stdexcept>

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

HttpResponse perform(CURL *h, std::string &body) {
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, APP_NAME "/0.1");
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("http: ") + curl_easy_strerror(rc));
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
        curl_slist_free_all(hl);
        return r;
    } catch (...) {
        curl_slist_free_all(hl);
        throw;
    }
}

}  // namespace

HttpResponse http_post_form(const std::string &url, const std::string &body,
                            const std::vector<std::string> &headers) {
    Curl c;
    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c.h, CURLOPT_POSTFIELDSIZE, (long)body.size());
    return with_headers(c.h, headers);
}

HttpResponse http_get(const std::string &url, const std::vector<std::string> &headers) {
    Curl c;
    curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.h, CURLOPT_FOLLOWLOCATION, 1L);  // posts never follow: a 302 would re-send creds
    return with_headers(c.h, headers);
}

std::string url_encode(const std::string &s) {
    Curl c;
    char *e = curl_easy_escape(c.h, s.data(), (int)s.size());
    if (!e) throw std::runtime_error("url_encode failed");
    std::string out(e);
    curl_free(e);
    return out;
}

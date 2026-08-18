#pragma once
#include <string>
#include <vector>

struct HttpResponse {
    long status = 0;
    std::string body;
};

// throws std::runtime_error on transport failure; http error codes come back in status
HttpResponse http_post_form(const std::string &url, const std::string &body,
                            const std::vector<std::string> &headers = {});
HttpResponse http_get(const std::string &url, const std::vector<std::string> &headers = {});

std::string url_encode(const std::string &s);

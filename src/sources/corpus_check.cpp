// scores classify against the tests/ corpus; built by `make corpus`, not by `all`
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "classify.h"

namespace {

std::vector<std::string> split_tab(const std::string &l) {
    std::vector<std::string> f;
    for (size_t p = 0;;) {
        size_t q = l.find('\t', p);
        f.push_back(l.substr(p, q - p));
        if (q == std::string::npos) return f;
        p = q + 1;
    }
}

}  // namespace

int main() {
    std::ifstream cf("tests/corpus.tsv");
    if (!cf) {
        fputs("corpus: tests/corpus.tsv missing\n", stderr);
        return 2;
    }
    int n = 0, ok = 0, dl = 0, tasks = 0;
    // idx, class, date, sender, card flag, text, expected kind
    for (std::string l; std::getline(cf, l);) {
        auto f = split_tab(l);
        if (f.size() < 7) continue;
        auto r = classify::run(f[5], f[4] == "A", f[2]);
        const std::string &want = f[6];
        bool has = want == "task" ? r.task : want == "test" ? r.test : !r.task && !r.test;
        n++;
        if (has) ok++;
        else fprintf(stderr, "classify MISS idx=%s want=%s: %.80s\n", f[0].c_str(), want.c_str(),
                     f[5].c_str());
        if (r.task || r.test) {
            tasks++;
            if (!r.deadline.empty()) dl++;
        }
    }
    printf("classify: gold %d/%d, deadline %d/%d\n", ok, n, dl, tasks);
    return ok == n ? 0 : 1;
}

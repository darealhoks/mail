#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "paint.h"
#include "store.h"
#include "term.h"
#include "teams.h"
#include "view.h"

#define TUI_NAME APP_NAME "t"

namespace {

using namespace paint;

struct Post {
    std::vector<std::string> lines;  // painted, sgr included, one terminal row each
    std::string url;
};

volatile sig_atomic_t resized = 1;
void on_winch(int) { resized = 1; }

termios saved{};
void leave() {
    tcsetattr(0, TCSANOW, &saved);
    fputs("\033[?1006l\033[?1000l\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}
void die(int sig) {
    leave();
    _exit(128 + sig);
}

void enter() {
    tcgetattr(0, &saved);
    termios raw = saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    // 1000 = button press/release + wheel, 1006 = sgr coords so columns past 223 still work
    fputs("\033[?1049h\033[?25l\033[?1000h\033[?1006h", stdout);
}

std::vector<Post> build(Store &s, size_t width) {
    view::Feed f = view::feed_rows(s, {}, (size_t)config().num("general.limit"));
    std::vector<Post> out;
    int bucket = -1;
    for (const view::FeedRow &r : f.rows) {
        const Item &i = f.items[r.n - 1];
        Post p;
        if (r.bucket != bucket) {
            bucket = r.bucket;
            p.lines.push_back(c("1;90", bucket == 0   ? "— no deadline —"
                                        : bucket == 1 ? "— upcoming —"
                                                      : "— overdue —"));
        }
        size_t chips = utf8_len(r.klass) + i.kind.size() + i.source.size() + 10;
        std::vector<std::string> title = wrap(i.title, width > chips + 30 ? width - chips : 30);
        p.lines.push_back((r.is_new ? c(NEW_CHIP, " NEW ") + " " : "") +
                          c("1", title.empty() ? "" : title[0] + (title.size() > 1 ? "…" : "")) +
                          "  " + c("1;36", "<" + r.klass + ">") + " " +
                          c(kind_color(i.kind), "<" + i.kind + ">") + " " +
                          c("90", "<" + i.source + ">"));
        std::string rest = i.body.compare(0, i.title.size(), i.title) == 0
                               ? i.body.substr(i.title.size())
                               : i.body;
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '|' || rest[0] == '\n')) rest.erase(0, 1);
        for (const auto &l : wrap(rest, width))
            p.lines.push_back(l == teams::TASK_NOTE ? c("1;33", "<" + l + ">") : link_up(c("37", l)));
        if (!i.url.empty() && config().flag("general.links")) p.lines.push_back(c("4;34", i.url));
        if (i.due_at) p.lines.push_back(c(due_color(i.due_at), when(i.due_at)));
        p.url = i.url;
        p.lines.push_back("");
        out.push_back(std::move(p));
    }
    return out;
}

long long now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// the whole ui keeps this line; mode chip left, transient action in the middle, position right
std::string status(const std::string &mode, const std::string &msg, const std::string &pos,
                   size_t width) {
    std::string bare = " " + mode + " " + (msg.empty() ? "" : " " + msg) + " ";
    std::string right = " " + pos + " ";
    size_t used = utf8_len(bare) + utf8_len(right);
    std::string gap(used < width ? width - used : 0, ' ');
    // colours mirror ~/.config/nvim/colors/theme.lua: ModeMsg, StatusLine, TabLineSel
    return c("1;38;2;224;199;155;48;2;7;11;20", " " + mode + " ") +
           c("38;2;151;165;190;48;2;7;11;20", (msg.empty() ? "" : " " + msg) + " " + gap) +
           c("1;38;2;7;11;20;48;2;127;163;212", right);
}

int open_url(const std::string &url) {
    if (url.empty() || url.find('\'') != std::string::npos) return 1;
    std::string opener = config().str("general.browser");
    if (opener.empty()) opener = "xdg-open";
    return system((opener + " '" + url + "' >/dev/null 2>&1 &").c_str()) == 0 ? 0 : 1;
}

int term_rows() {
    struct winsize w {};
    return ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 2 ? w.ws_row : 24;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        fprintf(stderr, "usage: %s\n", TUI_NAME);
        return 2;
    }
    (void)argv;
    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "%s: not a terminal\n", TUI_NAME);
        return 2;
    }
    try {
        Store store;
        std::vector<Post> posts;
        std::vector<size_t> owner;  // flat line -> post index
        std::vector<size_t> start;  // post index -> first flat line
        std::vector<std::string> flat;
        size_t sel = 0, top = 0;
        int rows = 24, cols = 80;
        bool first = true;
        std::string msg;
        long long msg_at = 0, click_at = 0;
        size_t click_post = (size_t)-1;

        enter();
        signal(SIGINT, die);
        signal(SIGTERM, die);
        signal(SIGWINCH, on_winch);
        atexit(leave);

        std::string pending;
        for (;;) {
            if (resized) {
                resized = 0;
                rows = term_rows() - 1;
                cols = term_cols();
                posts = build(store, (size_t)cols - 2);  // 2 = the selection bar column + a space
                flat.clear();
                owner.clear();
                start.clear();
                for (size_t p = 0; p < posts.size(); p++) {
                    start.push_back(flat.size());
                    for (const auto &l : posts[p].lines) {
                        flat.push_back(l);
                        owner.push_back(p);
                    }
                }
                if (first || sel >= posts.size()) sel = posts.empty() ? 0 : posts.size() - 1;
                first = false;
            }
            if (msg_at && now_ms() - msg_at > 4000) {
                msg.clear();
                msg_at = 0;
            }
            if (flat.empty()) {
                std::string out = "\033[H\033[90mnothing to show\033[0m\033[K\033[J\033[" +
                                  std::to_string(rows + 1) + ";1H" +
                                  status("feed", msg, "0/0", (size_t)cols);
                fwrite(out.data(), 1, out.size(), stdout);
                fflush(stdout);
            } else {
                // keep the selected post visible: its top, or its tail if it is taller than the screen
                size_t s0 = start[sel], s1 = s0 + posts[sel].lines.size();
                if (s0 < top) top = s0;
                else if (s1 > top + (size_t)rows)
                    top = s1 - (size_t)rows < s0 ? s1 - (size_t)rows : s0;
                size_t max_top = flat.size() > (size_t)rows ? flat.size() - (size_t)rows : 0;
                if (top > max_top) top = max_top;

                std::string out = "\033[H";
                for (int r = 0; r < rows; r++) {
                    size_t li = top + (size_t)r;
                    if (li < flat.size())
                        out += (owner[li] == sel ? "\033[38;2;127;163;212m▎\033[0m " : "  ") + flat[li];
                    out += "\033[K";
                    if (r < rows - 1) out += "\r\n";
                }
                out += "\033[J\r\n" +
                       status("feed", msg, std::to_string(sel + 1) + "/" +
                                               std::to_string(posts.size()), (size_t)cols);
                fwrite(out.data(), 1, out.size(), stdout);
                fflush(stdout);
            }

            char buf[64];
            if (msg_at) {  // wake on the message deadline so it expires without a keypress
                pollfd pf{0, POLLIN, 0};
                long long left = 4000 - (now_ms() - msg_at);
                if (poll(&pf, 1, (int)(left > 0 ? left : 0)) == 0) continue;
            }
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                if (errno == EINTR) continue;
                break;
            }
            pending.append(buf, (size_t)n);
            bool quit = false;
            while (!pending.empty()) {
                std::string &b = pending;
                auto open_sel = [&] {
                    if (posts.empty()) return;
                    msg = posts[sel].url.empty()          ? "no link"
                          : open_url(posts[sel].url) == 0 ? "opened in browser"
                                                          : "open failed";
                    msg_at = now_ms();
                };
                auto move = [&](long d) {
                    if (posts.empty()) return;
                    long v = (long)sel + d;
                    sel = (size_t)(v < 0 ? 0 : v >= (long)posts.size() ? (long)posts.size() - 1 : v);
                };
                if (b[0] == 'q' || b[0] == 3) {
                    quit = true;
                    break;
                }
                if (b[0] == '\r' || b[0] == '\n') { open_sel(); b.erase(0, 1); continue; }
                if (b[0] == 'j') { move(1); b.erase(0, 1); continue; }
                if (b[0] == 'k') { move(-1); b.erase(0, 1); continue; }
                if (b[0] == 'g') { sel = 0; b.erase(0, 1); continue; }
                if (b[0] == 'G') { move((long)posts.size()); b.erase(0, 1); continue; }
                if (b[0] != 27) { b.erase(0, 1); continue; }
                if (b.size() == 1) break;  // a bare esc, or a sequence still arriving
                if (b[1] != '[') { b.erase(0, 2); continue; }
                size_t e = b.find_first_of("mM~ABCDHF", 2);
                if (e == std::string::npos) break;
                std::string seq = b.substr(2, e - 2);
                char fin = b[e];
                b.erase(0, e + 1);
                if (fin == 'A') move(-1);
                else if (fin == 'B') move(1);
                else if (fin == 'H') sel = 0;
                else if (fin == 'F') move((long)posts.size());
                else if (fin == '~' && seq == "5") move(-5);
                else if (fin == '~' && seq == "6") move(5);
                else if (seq[0] == '<') {  // sgr mouse: <btn;col;row M|m
                    int btn = 0, mx = 0, my = 0;
                    if (sscanf(seq.c_str(), "<%d;%d;%d", &btn, &mx, &my) != 3) continue;
                    if (btn == 64) move(-1);
                    else if (btn == 65) move(1);
                    else if (btn == 0 && fin == 'M') {
                        size_t li = top + (size_t)(my - 1);
                        if (li >= flat.size()) continue;
                        sel = owner[li];
                        long long t = now_ms();
                        if (click_post == sel && t - click_at < 400) {
                            open_sel();
                            click_at = 0;
                        } else {
                            click_at = t;
                            click_post = sel;
                        }
                    }
                }
            }
            if (quit) break;
        }
        return 0;
    } catch (const std::exception &e) {
        leave();
        fprintf(stderr, TUI_NAME ": %s\n", e.what());
        return 1;
    }
}

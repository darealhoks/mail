#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include "bakalari.h"
#include "classify.h"
#include "config.h"
#include "http.h"
#include "json.h"
#include "notify.h"
#include "outlook.h"
#include "paint.h"
#include "registry.h"
#include "store.h"
#include "teams_auth.h"
#include "text.h"
#include "view.h"

namespace {

// variadic: a braced init list in the condition would otherwise split into two arguments
#define CHECK(...)                                                                \
    do {                                                                          \
        if (!(__VA_ARGS__)) {                                                     \
            fprintf(stderr, "check failed: %s (%d)\n", #__VA_ARGS__, __LINE__);   \
            return 1;                                                             \
        }                                                                         \
    } while (0)

int check_config() {
    {  // the seed file is written on the first config() read; protocol keys stay out of it
        config();
        std::ostringstream raw;
        raw << std::ifstream(config_path()).rdbuf();
        std::string seed = raw.str();
        CHECK(seed.find("client_id") == std::string::npos);
        CHECK(seed.find("[source.teams]\nenabled") != std::string::npos);
    }

    Config c;
    std::istringstream in("# c\n[general]\nlimit = 7\nlinks = yes\nblacklist = ANJ, Mat\n"
                          "[source.bakalari]\nurl=http://x\nenabled = no\n[school]\npoints=80,60\n");
    config_parse(in, c);
    CHECK(c.num("general.limit") == 7);
    CHECK(c.flag("general.links"));
    CHECK(c.list("general.blacklist") == std::vector<std::string>{"anj", "mat"});
    CHECK(c.str("source.bakalari.url") == "http://x");
    CHECK(!c.flag("source.bakalari.enabled"));
    CHECK(c.str("general.missing").empty());

    Config head;  // keys before any header are dropped, not filed under [general]
    std::istringstream lin("limit = 3\n[general]\nnotify = x\n");
    config_parse(lin, head);
    CHECK(head.num("general.limit") == 0 && head.str("general.notify") == "x");

    CHECK(text::html_unescape("&#xD800;a&#65;") == "&#xD800;aA");  // lone surrogate stays literal

    CHECK(points_mark(95) == 1 && points_mark(80) == 2 && points_mark(10) == 5);
    CHECK(mark_scale() == std::pair<char, char>{'1', '5'});
    CHECK(avg_mark(1.49) == 1 && avg_mark(1.5) == 2 && avg_mark(2.49) == 2 && avg_mark(5) == 5);

    auto at = [](const char *iso) { return classify::epoch(iso); };
    CHECK(school_year(at("2026-08-16")) == "26/27");
    CHECK(school_year(at("2026-07-31")) == "25/26");
    CHECK(school_year(at("2026-01-05")) == "25/26");
    CHECK(school_year(school_year_start("26/27")) == "26/27");
    CHECK(period_label(at("2025-09-10")) == "25/26 · H1");
    CHECK(period_label(at("2026-01-20")) == "25/26 · H1");
    CHECK(period_label(at("2026-03-01")) == "25/26 · H2");
    CHECK(period_label(at("2026-05-01")) == "25/26 · H2");

    config().set("general.blacklist", "anj");
    CHECK(blacklisted("ANJ", "ANJ") && blacklisted("3A_ANJ_SK1_25/26", "ANJ"));
    CHECK(!blacklisted("3A_MAT_SK1_25/26", "MAT"));

    config().set("key.x", "timetable");
    config().set("key.z", "nonsense");
    std::map<char, std::string> k = key_modes();
    CHECK(k['f'] == "feed" && k['m'] == "marks" && k['x'] == "table" && k['b'] == "absence");
    CHECK(!k.count('z'));

    config().set("bind.t", "next -s");
    config().set("bind.marks", "timetable");  // a bind must beat the builtin verb of that name
    CHECK(expand_bind({"-b", "t", "t", "x"}) ==
          std::vector<std::string>{"-b", "t", "next", "-s", "x"});
    CHECK(expand_bind({"marks"}) == std::vector<std::string>{"timetable"});

    {  // config_save round-trip; XDG_CONFIG_HOME already points at a throwaway dir
        std::ofstream(config_path()) << "[source.bakalari]\nurl        = \n";
        CHECK(config_save("source.bakalari.url", "http://y"));
        CHECK(config_save("source.bakalari.enabled", "no"));  // key absent, section present
        Config back;
        std::ifstream f(config_path());
        config_parse(f, back);
        std::ostringstream raw;
        raw << std::ifstream(config_path()).rdbuf();
        std::string text = raw.str();
        // no second header for a key added to an existing section
        CHECK(text.find("[source.bakalari]", text.find("[source.bakalari]") + 1) ==
              std::string::npos);
        CHECK(!back.flag("source.bakalari.enabled"));
        CHECK(back.str("source.bakalari.url") == "http://y");
        config().set("source.bakalari.enabled", "yes");  // config_save wrote it into the singleton
    }
    return 0;
}

int check_classify() {
    struct C {
        const char *text;
        bool card;
        const char *dl, *tags;
    } cases[] = {
        {"Test 16. 4. Připomínám, že 16. 4. píšete opakovací test", false, "2026-04-16", "test"},
        {"Desetiminutovka - rovnice a nerovnice / Termín splnění 9. dub", true, "2026-04-09",
         "test+task"},
        {"Náhradní termín - kmitání - pátek 17. 4. 8:55 učebna 309.", false, "2026-04-17T08:55",
         "test"},
        {"Opravný test - opravu si napíšete ve středu 8. ledna ve 14:30", false,
         "2026-01-08T14:30", "test"},
        {"Metody / Due Jan 21", true, "2026-01-21", "task"},
        {"Mluvní cvičení J. K. Tyl - Fidlovačka", false, "", "task"},
        {"Domácí úkol - příprava na test / Termín splnění 11. bře", true, "2026-03-11", "task"},
        {"Aktivace účtu u Autodesku Prosím všechny studenty, aby si nejpozději do 2. 2. "
         "aktivovali účet",
         false, "2026-02-02", "task"},
        {"Zrušení hodiny Dobrý den, dnešní hodina 12. 1. je bohužel zrušena", false, "", ""},
        {"Opakování - Výstavba literárního díla", false, "", ""},
        {"📢 Změnil se termín splnění zadání.", false, "", ""},
        {"class TestBot(BotAI): import random", false, "", ""},
    };
    for (const auto &c : cases) {
        auto r = classify::run(c.text, c.card, "2026-01-15");
        std::string got = std::string(r.test ? "test" : "") + (r.test && r.task ? "+" : "") +
                          (r.task ? "task" : "");
        if (got != c.tags || r.deadline != c.dl) {
            fprintf(stderr, "classify: want %s,%s got %s,%s: %s\n", c.tags, c.dl, got.c_str(),
                    r.deadline.c_str(), c.text);
            return 1;
        }
    }
    CHECK(classify::epoch("2026-08-16T00:00:00+02:00") == 1786838400);
    CHECK(classify::epoch("2026-08-16T14:30") == 1786890600);
    CHECK(classify::epoch("nope") == 0);
    return 0;
}

int check_text() {
    using text::plain_text;
    CHECK(plain_text("<p>a<br/>b</p><div>c &amp;amp; d</div>") == "a\nb\nc & d");
    CHECK(plain_text("see https://x.y/z now") == "see https://x.y/z now");
    CHECK(plain_text("<a href=\"https://x.y/z\">https://x.y/z</a>.") == "https://x.y/z .");
    CHECK(plain_text("&#268;au&nbsp;&#268;au") == "Čau Čau");
    CHECK(plain_text("<div></div>  ") == "");
    CHECK(plain_text("<b>tučně</b> a <i>kurzíva</i>") == "*tučně* a _kurzíva_");
    CHECK(plain_text("<p><strong> dvě slova </strong>tady</p>") == "*dvě slova* tady");
    CHECK(plain_text("<b><i>obě</i></b>") == "*_obě_*");
    CHECK(plain_text("<b> </b>x") == "x");   // empty span leaves no marker behind
    CHECK(plain_text("</b>x") == "x");       // stray close is not a marker
    CHECK(plain_text("<b>https://x.y/z</b>") == "*https://x.y/z*");
    CHECK(text::style_strip("*a* _b_") == "a b");
    CHECK(text::strip_invisible("a‌​ b﻿") == "a b");  // preheader padding
    CHECK(text::strip_invisible("– dash stays") == "– dash stays");

    struct {
        const char *in, *want;
    } urls[] = {
        {"https://login.microsoft.com/device", "HTTPS://LOGIN.MICROSOFT.COM/DEVICE"},
        {"https://microsoft.com/devicelogin", "HTTPS://MICROSOFT.COM/DEVICELOGIN"},
        {"https://login.microsoft.com/device?otc=ABC", "https://login.microsoft.com/device?otc=ABC"},
        {"https://ha.mr/a_b", "https://ha.mr/a_b"},
    };
    for (const auto &u : urls) CHECK(teams::qr_upper(u.in) == u.want);
    return 0;
}

int check_json() {
    Json j(R"({"a":"x\ny","n":42})");
    CHECK(j.str("a") == "x\ny");
    CHECK(j.num("n") == 42);
    CHECK(j.str("n", "def") == "def");     // wrong type -> default
    CHECK(j.num("missing", -1) == -1);
    bool threw = false;
    try {
        Json bad("{not json");
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK(threw);
    return 0;
}

int check_outlook() {
    config().set("source.outlook.sync", "");
    CHECK(outlook::mode() == "recent");            // unset and junk both fall back
    config().set("source.outlook.sync", "every");
    CHECK(outlook::mode() == "recent");
    CHECK(outlook::cold_size(4000, 12) == 12);     // recent/unread cost the unread count
    config().set("source.outlook.sync", "unread");
    CHECK(outlook::mode() == "unread" && outlook::cold_size(4000, 12) == 12);
    config().set("source.outlook.sync", "all");
    CHECK(outlook::mode() == "all" && outlook::cold_size(4000, 12) == 4000);
    config().set("source.outlook.sync", "recent");
    return 0;
}

Item stub(const char *src) {
    Item i;
    i.source = src;
    i.klass = "3.A";
    i.kind = "post";
    i.title = "t";
    i.src_uid = "uid1";
    return i;
}

int check_store() {
    CHECK(!sources().empty());
    std::string tmp = "/tmp/" APP_NAME "-check-" + std::to_string(getpid()) + ".db";
    auto scrub = [&] {
        remove(tmp.c_str());
        remove((tmp + "-wal").c_str());
        remove((tmp + "-shm").c_str());
    };
    scrub();
    {
        Store s(tmp);
        CHECK(s.insert_item(stub("teams")));
        CHECK(!s.insert_item(stub("teams")));
        CHECK(s.insert_item(stub("othersrc")));
        CHECK(s.last_ok_fetch("teams") == 0);
        s.log_fetch("teams", 100, false, "boom", 0);
        CHECK(s.last_ok_fetch("teams") == 0);
        s.log_fetch("teams", 100, true, "", 1);
        CHECK(s.last_ok_fetch("teams") > 0);
        CHECK(s.last_fetch("teams").failing_since == 0);
        s.log_fetch("teams", 100, false, "boom", 0);
        long long first = s.last_fetch("teams").failing_since;
        s.log_fetch("teams", 100, false, "boom", 0);
        CHECK(first && s.last_fetch("teams").failing_since == first);

        for (const Source &src : sources()) s.log_fetch(src.name, 100, false, OFFLINE_TAG "x", 0);
        CHECK(view::offline(s));
        s.log_fetch(sources()[0].name, 100, false, "http: boom", 0);
        CHECK(!view::offline(s));
    }
    scrub();

    {
        Store s(tmp);
        CHECK(s.get_state("k").empty());
        s.set_state("k", "v1");
        s.set_state("k", "v2");
        CHECK(s.get_state("k") == "v2");
        s.defer = true;
        s.set_state("k", "v3");
        CHECK(s.get_state("k") == "v2");  // queued, not written
        s.flush();
        CHECK(s.get_state("k") == "v3" && !s.defer);
        s.defer = true;
        s.set_state("k", "v4");
        s.drop_deferred();
        CHECK(s.get_state("k") == "v3" && !s.defer);
        CHECK(s.insert_item(stub("teams")));
    }
    scrub();

    {
        Store s(tmp);
        Item i = stub("teams");
        i.title = "a\x1b[31mb";
        CHECK(s.insert_item(i));
        CHECK(s.feed().at(0).title == "a[31mb");
    }
    scrub();

    {
        Store s(tmp);
        long long now = (long long)time(nullptr);
        auto add = [&](const char *uid, const char *kind, long long due, long long ev) {
            Item i = stub("teams");
            i.src_uid = uid;
            i.title = uid;
            i.kind = kind;
            i.due_at = due;
            i.event_at = ev;
            return s.insert_item(i);
        };
        CHECK(add("old", "task", now - 90 * 86400, 0));   // past the 30d window
        CHECK(add("task", "task", 0, now - 3 * 86400));
        CHECK(add("post", "post", 0, now));
        auto f = s.feed();
        CHECK(f.size() == 2);                                  // overdue term-old task dropped
        CHECK(f[0].title == "post" && f[1].title == "task");   // undated actionables sink
    }
    scrub();

    {
        Lesson l;
        l.date = "2026-08-17";
        l.hour = "2";
        l.subject = "MAT";
        l.subject_name = "Matematika";
        l.room = "309";
        l.teacher = "NOV";
        l.begins = "09:55";
        l.ends = "10:40";
        Lesson gone = l;
        gone.hour = "1";
        gone.subject = "CJL";
        gone.room = "";
        gone.state = "x";

        Store t(tmp);
        t.put_lessons("bakalari", "2026-08-17", "2026-08-23", {l, gone});
        std::vector<Lesson> got = t.lessons("2026-08-17", "2026-08-23");
        CHECK(got.size() == 2 && got[0].hour == "1" && got[0].state == "x");
        CHECK(got[0].label() == "CJL" && got[1].label() == "MAT 309");
        CHECK(got[1].teacher == "NOV" && got[1].subject_name == "Matematika");
        CHECK(got[1].begins == "09:55" && got[1].ends == "10:40" && got[1].source == "bakalari");
        // re-fetch of the same week without CJL: the stale row must not survive
        t.put_lessons("bakalari", "2026-08-17", "2026-08-23", {l});
        std::vector<Lesson> after = t.lessons("2026-08-17", "2026-08-23");
        CHECK(after.size() == 1 && after[0].hour == "2");
        CHECK(t.lessons("2026-08-24", "2026-08-30").empty());

        Absence a{"bakalari", "MAT", 48, 12, 20};
        t.put_absences("bakalari", {a, {"bakalari", "CJL", 0, 0, 20}});
        a.absent = 3;
        t.put_absences("bakalari", {a});  // a re-fetch replaces, never accumulates
        std::vector<Absence> ab = t.absences();
        CHECK(ab.size() == 1 && ab[0].subject == "MAT" && ab[0].absent == 3);
        CHECK(ab[0].lessons == 48 && ab[0].pct() > 6.24 && ab[0].pct() < 6.26);
        CHECK(Absence{}.pct() == 0);

        // the permanent grid stands in for a week with nothing stored, re-dated onto it
        Lesson perm = l;
        perm.date = "1970-01-07";  // wednesday of the PERM_MONDAY week
        t.put_lessons("bakalari-perm", PERM_MONDAY, PERM_SUNDAY, {perm});
        view::Timetable have = view::timetable(t, "2026-08-17");
        view::Timetable fall = view::timetable(t, "2026-08-24");
        CHECK(!have.permanent && have.days == std::vector<std::string>{"2026-08-17"});
        CHECK(fall.permanent && fall.days == std::vector<std::string>{"2026-08-26"});
        CHECK(fall.rows[0].subject == "MAT");
    }
    scrub();
    return 0;
}

int check_view() {
    struct {
        const char *in, *want;
    } abbrevs[] = {
        {"3A_GRS_SK1_25/26", "GRS"}, {"3 MAT", "MAT"},   {"3A_CJL_CELÁ", "CJL"},
        {"WBA", "WBA"},              {"OSE1", "OSE1"},   {"Jan Novák", "Jan Novák"},
    };
    for (const auto &a : abbrevs) CHECK(view::abbrev(a.in) == a.want);

    CHECK(view::fold("Čeština") == "cestina" && view::fold("MAT") == "mat");

    long long start = view::local_at("2026-08-17", "09:55");
    CHECK(start && !view::local_at("2026-08-17", ""));
    CHECK(view::ymd_local(start) == "2026-08-17");
    CHECK(view::ymd_plus("2026-08-17", 6) == "2026-08-23");
    CHECK(view::ymd_plus("2026-08-17", 13) == "2026-08-30");
    CHECK(view::monday_of(start) == "2026-08-17");
    CHECK(view::monday_of(start + 6 * 86400) == "2026-08-17");
    return 0;
}

int check_paint() {
    {  // date_short: czech day. month., year only when it is not the current one
        time_t n = time(nullptr);
        struct tm cur {};
        localtime_r(&n, &cur);
        char y[8];
        strftime(y, sizeof y, "%Y", &cur);
        CHECK(paint::date_short(std::string(y) + "-08-17") == "17. 8.");
        CHECK(paint::date_short("1999-01-05") == "5. 1. 1999");
    }
    // a wide glyph must cost two columns, or a grid row shears every row below it
    CHECK(paint::utf8_len("čeština") == 7);
    CHECK(paint::plain_cut("abcdef", 3) == "abc");
    CHECK(paint::plain_cut("ab", 4) == "ab  ");
    CHECK(paint::plain_cut("čeština", 3) == "češ");
    CHECK(paint::fit("abcdef", 4).compare(0, 6, "abc…") == 0);
    if (wcwidth(L'日') == 2) {
        CHECK(paint::utf8_len("日x") == 3);
        CHECK(paint::plain_cut("日x", 2) == "日");
    }
    CHECK(paint::wrap("ab cd ef", 5) == std::vector<std::string>{"ab cd", "ef"});
    CHECK(paint::wrap("příliš žluťoučký", 8) == std::vector<std::string>{"příliš", "žluťoučký"});

    using paint::mark_color;
    CHECK(!strcmp(mark_color("1"), mark_color("1-")));
    CHECK(strcmp(mark_color("2"), mark_color("5")));
    CHECK(!strcmp(mark_color("N"), "39"));
    CHECK(!strcmp(mark_color("45/50"), "1;32"));
    CHECK(!strcmp(mark_color("0/25"), "1;31"));
    CHECK(!strcmp(mark_color("5/0"), "39"));
    CHECK(paint::avg_str(1.5) == "1,50 (2)" && paint::avg_str(2.494) == "2,49 (2)");
    CHECK(!strcmp(paint::avg_color(1.4), mark_color("1")));
    CHECK(!strcmp(paint::avg_color(1.5), mark_color("2")));

    {  // the bucket heading, and a body lined up under the number gutter
        view::Feed f;
        Item it = stub("teams");
        it.title = "Title";
        it.body = "Title\nbody word";
        f.items = {it};
        f.rows = {{1, false, 1, "MAT"}};
        std::vector<paint::Post> posts = paint::feed_posts(f, 60);
        CHECK(posts.size() == 1 && posts[0].lines.size() == 5);
        std::vector<std::string> pl;
        for (const auto &l : posts[0].lines) pl.push_back(paint::strip_sgr(l));
        CHECK(pl[0] == "# upcoming" && pl[1].empty());
        CHECK(pl[2].compare(0, 2, "1 ") == 0 && pl[3] == "  body word");
    }

    CHECK(paint::accent_sgr("Magenta") == "35");
    CHECK(paint::accent_sgr("brightcyan") == "96");
    CHECK(paint::accent_sgr("9") == "91");
    CHECK(paint::accent_sgr("208") == "38;5;208");
    CHECK(paint::accent_sgr("#7fa3d4") == "38;2;127;163;212");
    CHECK(paint::accent_sgr("nope") == "34");

    {  // a span split by the wrap reopens on the next line; colour depends on the tty
        unsigned open = 0;
        std::string a = paint::style_up("*a", open), b = paint::style_up("c*", open);
        CHECK(open == 0);
        CHECK(a.find('*') == std::string::npos && a.find('a') != std::string::npos);
        CHECK(b.find('*') == std::string::npos && b.find('c') != std::string::npos);
        CHECK(a.find("\033[1m") != std::string::npos || a == "a");
        CHECK((a.find("\033[1m") == std::string::npos) == (b.find("\033[1m") == std::string::npos));
    }

    view::Timetable tt;
    tt.monday = "2026-08-17";
    tt.days = {"2026-08-17", "2026-08-18"};
    tt.hours = {"1", "2"};
    tt.rows = {{"bakalari", "2026-08-17", "1", "CJL", "Cestina", "214", "Novak", "", "8:00",
                "8:45", "Jan Novak", ""}};
    tt.grid = {&tt.rows[0], nullptr, nullptr, nullptr};
    {  // widest time form that fits: full span, minutes-only, start over end, then nothing
        paint::TableLayout wide = paint::table_layout(tt, 3, 200, 3);
        paint::TableLayout m = paint::table_layout(tt, 3, 20, 3);
        paint::TableLayout e = paint::table_layout(tt, 3, 14, 3);
        paint::TableLayout n = paint::table_layout(tt, 3, 11, 3);
        CHECK(wide.t0[0] == "8:00-8:45" && wide.cw == 9);
        CHECK(m.t0[0] == "00-45" && m.time_rows == 1);
        CHECK(e.time_rows == 2 && e.t1[0] == "8:45");
        CHECK(!n.time_rows);
    }

    paint::Geom big{}, small{}, flat{};
    std::vector<std::string> b = paint::grid_lines(tt, 0, 0, 40, 200, big);
    std::vector<std::string> s2 = paint::grid_lines(tt, 0, 0, 5, 24, small);
    // wide terminal: three lines a cell plus a spacer row; cramped: one line, no spacer
    CHECK(big.blk == 4 && small.blk == 2);
    CHECK(b.size() == big.top + 2 * 4 && s2.size() == small.top + 2 * 2);
    // a grid smaller than the pane sits in the middle of it, not in the top left corner
    CHECK(big.left && b[0] == "");
    CHECK(b[big.top - 1].compare(0, big.left, std::string(big.left, ' ')) == 0);
    // the cli path draws no pane padding to offset a click by
    paint::grid_lines(tt, 0, 0, 0, 200, flat);
    CHECK(!flat.left && flat.top && flat.top < big.top);
    CHECK(small.gut + tt.hours.size() * (small.cw + 1) + 1 <= 24);
    // this grid fits any width in the sweep: a cut row means the geometry overran the terminal
    for (int cx = 20; cx <= 200; cx++) {
        paint::Geom g{};
        for (const auto &line : paint::grid_lines(tt, 0, 0, 24, cx, g))
            if (paint::fit(line, (size_t)cx) != line) {
                fprintf(stderr, "check failed: table row overflows %d columns\n", cx);
                return 1;
            }
    }

    {  // the state is in the text, not only in the colour: a colourless terminal keeps it
        view::Timetable st;
        st.monday = "2026-08-17";
        st.days = {"2026-08-17"};
        st.hours = {"1", "2"};
        st.rows = {{"bakalari", "2026-08-17", "1", "CJL", "", "", "", "x", "", "", "", ""},
                   {"bakalari", "2026-08-17", "2", "MAT", "", "", "", "!", "", "", "", ""}};
        st.grid = {&st.rows[0], &st.rows[1]};
        paint::Geom g{};
        std::string row;
        for (const auto &line : paint::grid_lines(st, paint::NO_CELL, paint::NO_CELL, 0, 80, g))
            if (line.find("CJL") != std::string::npos) row = paint::strip_sgr(line);
        CHECK(row.find("~CJL~") != std::string::npos && row.find("MAT!") != std::string::npos);
    }

    {  // the running lesson stars itself, so it stays findable with the cursor parked elsewhere
        time_t nt = time(nullptr);
        struct tm ltm {};
        localtime_r(&nt, &ltm);
        int m = ltm.tm_hour * 60 + ltm.tm_min;
        auto hm = [](int v) {
            return std::to_string(v / 60) + ":" + (v % 60 < 10 ? "0" : "") + std::to_string(v % 60);
        };
        view::Timetable nt2;
        nt2.days = {view::ymd_local((long long)nt)};
        nt2.monday = nt2.days[0];
        nt2.hours = {"1", "2"};
        nt2.rows = {{"bakalari", nt2.days[0], "1", "CJL", "", "", "", "", hm(m ? m - 1 : 0),
                     hm(m + 1), "", ""},
                    {"bakalari", nt2.days[0], "2", "MAT", "", "", "", "", "00:00", "00:00", "",
                     ""}};
        nt2.grid = {&nt2.rows[0], &nt2.rows[1]};
        paint::Geom g{};
        std::string row;
        for (const auto &line : paint::grid_lines(nt2, paint::NO_CELL, paint::NO_CELL, 0, 80, g))
            if (line.find("CJL") != std::string::npos) row = paint::strip_sgr(line);
        CHECK(row.find("CJL") < row.find('*') && row.find("MAT") > row.find('*'));
        CHECK(row.find('*', row.find("MAT")) == std::string::npos);
    }
    return 0;
}

int check_notify() {
    // hook args are remote text: one quoting slip and the shell runs it
    std::string nf = "/tmp/" APP_NAME "-check-notify-" + std::to_string(getpid());
    config().set("general.notify", "printf '%s|' >" + nf);
    notify(2, "You've got mail", "$(touch /tmp/pwned)\n+1 tests", ICON_MAIL);
    std::ifstream nfs(nf);
    std::string got((std::istreambuf_iterator<char>(nfs)), std::istreambuf_iterator<char>());
    CHECK(got == "2|You've got mail|$(touch /tmp/pwned)\n+1 tests|" ICON_MAIL "|");
    CHECK(access("/tmp/pwned", F_OK) != 0);
    remove(nf.c_str());
    config().set("general.notify", "");
    return 0;
}

int check_hours() {
    using bakalari::hour_runs;
    std::vector<bakalari::Span> day{{1, "1", "8:00", "8:45"},   {2, "2", "9:00", "9:45"},
                                    {3, "3", "10:00", "10:45"}, {4, "4", "11:00", "11:45"},
                                    {6, "6", "13:00", "13:45"}, {7, "7", "14:00", "14:45"},
                                    {8, "8", "15:00", "15:45"}};
    CHECK(hour_runs(day) == "1.-4. hod 8:00 - 11:45, 6.-8. hod 13:00 - 15:45");
    CHECK(hour_runs({day[4]}) == "6. hod 13:00 - 13:45");
    CHECK(hour_runs({day[1], day[0], day[1]}) == "1.-2. hod 8:00 - 9:45");  // unsorted, duplicate
    CHECK(hour_runs({{0, "0", "7:10", ""}, {1, "1", "8:00", "8:45"}}) ==
          "0. hod 7:10, 1. hod 8:00 - 8:45");  // an unnumbered caption never joins a run
    CHECK(hour_runs({}).empty());
    return 0;
}

}  // namespace

int main() {
    // the checks write config and mutate the singleton: keep the user's own file out of it
    std::string dir = "/tmp/" APP_NAME "-check-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    int rc = check_config() || check_classify() || check_text() || check_json() || check_view() ||
             check_paint() || check_store() || check_outlook() || check_notify() ||
             check_hours();
    unlink(config_path().c_str());
    rmdir((dir + "/" APP_NAME).c_str());
    rmdir(dir.c_str());
    if (!rc) puts(APP_NAME ": checks ok");
    return rc;
}

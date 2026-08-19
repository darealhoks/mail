#pragma once
#include <string>
#include <vector>

struct sqlite3;

// ~/.local/share/<name> (honours XDG_DATA_HOME); created 0700 if missing
std::string data_dir();

inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

struct Item {
    long long id = 0;
    std::string source, klass, kind, title, body, src_uid, url;
    long long due_at = 0, event_at = 0;
    int weight = 1;  // marks only; bakalari mark weight, drives the average
    long long fetched_at = 0;  // read side only; insert_item stamps its own
};

// one timetable cell; state "x" cancelled, "!" changed, "" normal
struct Lesson {
    std::string source, date, hour, subject, subject_name, room, teacher, state, begins, ends;
    std::string label() const { return subject + (room.empty() ? "" : " " + room); }
};

// opens <name>.db and applies the schema; throws on failure
struct Store {
    explicit Store(const std::string &path = data_dir() + "/" APP_NAME ".db");
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // one fetch+insert per source is atomic; put_lessons nests inside via SAVEPOINT
    void begin();
    void commit();
    void rollback();

    // returns true if the item was new ((source, src_uid) not already stored)
    bool insert_item(const Item &i);
    void log_fetch(const std::string &source, long long started_at, bool ok,
                   const std::string &error, int items_new);
    // 0 if the source never fetched successfully
    long long last_ok_fetch(const std::string &source);
    struct Fetch {
        long long at = 0;
        long long failing_since = 0;  // first failure of the current streak, 0 if the last run was ok
        bool ok = false;
        std::string error;
    };
    // most recent attempt, ok or not; at==0 if the source never ran
    Fetch last_fetch(const std::string &source);

    // replaces the source's lessons in [from, to] wholesale: lessons dropped upstream go away
    void put_lessons(const std::string &source, const std::string &from, const std::string &to,
                     const std::vector<Lesson> &rows);
    // all sources, dates in [from, to] ("YYYY-MM-DD"), by date then hour numerically
    std::vector<Lesson> lessons(const std::string &from, const std::string &to);

    // per-source cursors (teams delta links, channel cache); "" when absent
    std::string get_state(const std::string &key);
    void set_state(const std::string &key, const std::string &value);
    // drops cursors so the next fetch starts cold; `like` is a sqlite LIKE pattern
    void clear_state(const std::string &like);

    // everything the frontend shows: not dismissed, marks excluded (own section, later)
    std::vector<Item> feed();
    // kind='mark' only, newest first
    std::vector<Item> marks();
    void dismiss(const std::vector<long long> &ids);

    sqlite3 *db = nullptr;
};

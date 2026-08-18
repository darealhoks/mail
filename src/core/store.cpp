#include "store.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cstdlib>
#include <stdexcept>

namespace {

const char *SCHEMA = R"(
CREATE TABLE IF NOT EXISTS items(
  id INTEGER PRIMARY KEY,
  source TEXT NOT NULL,
  class TEXT NOT NULL DEFAULT '',
  kind TEXT NOT NULL,
  title TEXT NOT NULL,
  body TEXT NOT NULL DEFAULT '',
  due_at INTEGER,
  event_at INTEGER,
  fetched_at INTEGER NOT NULL,
  dismissed INTEGER NOT NULL DEFAULT 0,
  src_uid TEXT NOT NULL,
  url TEXT NOT NULL DEFAULT '',
  weight INTEGER NOT NULL DEFAULT 1,
  UNIQUE(source, src_uid)
);
CREATE TABLE IF NOT EXISTS fetch_log(
  id INTEGER PRIMARY KEY,
  source TEXT NOT NULL,
  started_at INTEGER NOT NULL,
  finished_at INTEGER NOT NULL,
  ok INTEGER NOT NULL,
  error TEXT NOT NULL DEFAULT '',
  items_new INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS fetch_log_source ON fetch_log(source, finished_at);
CREATE TABLE IF NOT EXISTS state(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS lessons(
  source TEXT NOT NULL,
  date TEXT NOT NULL,
  hour TEXT NOT NULL,
  subject TEXT NOT NULL DEFAULT '',
  subject_name TEXT NOT NULL DEFAULT '',
  room TEXT NOT NULL DEFAULT '',
  teacher TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL DEFAULT '',
  begins TEXT NOT NULL DEFAULT '',
  ends TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(source, date, hour)
);
)";

void exec(sqlite3 *db, const char *sql) {
    char *err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error("store: " + m);
    }
}

void bind(sqlite3_stmt *s, int i, const std::string &v) {
    sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
}

}  // namespace

std::string data_dir() {
    const char *xdg = getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) throw std::runtime_error("HOME not set");
        base = std::string(home) + "/.local/share";
    }
    std::string dir = base + "/" APP_NAME;
    if (mkdir(dir.c_str(), 0700) != 0) {
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) throw std::runtime_error("cannot create " + dir);
    }
    return dir;
}

Store::Store(const std::string &path) {
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::string m = db ? sqlite3_errmsg(db) : "open failed";
        sqlite3_close(db);
        throw std::runtime_error("store: " + m);
    }
    exec(db, "PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000;");
    exec(db, SCHEMA);
    // pre-weight databases: fails harmlessly once the column exists
    sqlite3_exec(db, "ALTER TABLE items ADD COLUMN weight INTEGER NOT NULL DEFAULT 1", nullptr,
                 nullptr, nullptr);
    exec(db, "UPDATE items SET kind='task' WHERE kind='ukol'");
    exec(db, "UPDATE items SET kind='info' WHERE kind='message'");
    // drops the old '<source>.tt.<monday>' grid blobs; the lessons table replaced them
    exec(db, "DELETE FROM state WHERE key LIKE '%.tt.%'");
}

Store::~Store() { sqlite3_close(db); }

bool Store::insert_item(const Item &i) {
    sqlite3_stmt *s = nullptr;
    const char *sql =
        "INSERT OR IGNORE INTO items(source,class,kind,title,body,due_at,event_at,fetched_at,"
        "src_uid,url,weight) VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, i.source);
    bind(s, 2, i.klass);
    bind(s, 3, i.kind);
    bind(s, 4, i.title);
    bind(s, 5, i.body);
    i.due_at ? sqlite3_bind_int64(s, 6, i.due_at) : sqlite3_bind_null(s, 6);
    i.event_at ? sqlite3_bind_int64(s, 7, i.event_at) : sqlite3_bind_null(s, 7);
    bind(s, 8, i.src_uid);
    bind(s, 9, i.url);
    sqlite3_bind_int(s, 10, i.weight);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    if (sqlite3_changes(db) > 0) return true;
    // a known item still gets its text refreshed: upstream edits show up, and rows stored by
    // an older build (no url, undecoded entities) heal on the next fetch. not "new" either way.
    sqlite3_stmt *u = nullptr;
    const char *up = "UPDATE items SET title=?,body=?,url=?,weight=? WHERE source=? AND src_uid=?";
    if (sqlite3_prepare_v2(db, up, -1, &u, nullptr) == SQLITE_OK) {
        bind(u, 1, i.title);
        bind(u, 2, i.body);
        bind(u, 3, i.url);
        sqlite3_bind_int(u, 4, i.weight);
        bind(u, 5, i.source);
        bind(u, 6, i.src_uid);
        sqlite3_step(u);
        sqlite3_finalize(u);
    }
    return false;
}

void Store::log_fetch(const std::string &source, long long started_at, bool ok,
                      const std::string &error, int items_new) {
    sqlite3_stmt *s = nullptr;
    const char *sql =
        "INSERT INTO fetch_log(source,started_at,finished_at,ok,error,items_new)"
        " VALUES(?,?,strftime('%s','now'),?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, source);
    sqlite3_bind_int64(s, 2, started_at);
    sqlite3_bind_int(s, 3, ok ? 1 : 0);
    bind(s, 4, error);
    sqlite3_bind_int(s, 5, items_new);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
}

std::string Store::get_state(const std::string &key) {
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM state WHERE key=?", -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, key);
    std::string out;
    if (sqlite3_step(s) == SQLITE_ROW)
        if (const unsigned char *v = sqlite3_column_text(s, 0))
            out.assign((const char *)v, sqlite3_column_bytes(s, 0));
    sqlite3_finalize(s);
    return out;
}

void Store::set_state(const std::string &key, const std::string &value) {
    sqlite3_stmt *s = nullptr;
    const char *sql = "INSERT INTO state(key,value) VALUES(?,?)"
                      " ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, key);
    bind(s, 2, value);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
}

void Store::clear_state(const std::string &like) {
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM state WHERE key LIKE ?", -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, like);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
}

void Store::put_lessons(const std::string &source, const std::string &from, const std::string &to,
                        const std::vector<Lesson> &rows) {
    exec(db, "BEGIN IMMEDIATE");
    try {
        sqlite3_stmt *d = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM lessons WHERE source=? AND date BETWEEN ? AND ?", -1,
                               &d, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
        bind(d, 1, source);
        bind(d, 2, from);
        bind(d, 3, to);
        int rc = sqlite3_step(d);
        sqlite3_finalize(d);
        if (rc != SQLITE_DONE) throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));

        sqlite3_stmt *s = nullptr;
        const char *sql = "INSERT OR REPLACE INTO lessons(source,date,hour,subject,subject_name,"
                          "room,teacher,state,begins,ends) VALUES(?,?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
        for (const Lesson &l : rows) {
            bind(s, 1, source);
            bind(s, 2, l.date);
            bind(s, 3, l.hour);
            bind(s, 4, l.subject);
            bind(s, 5, l.subject_name);
            bind(s, 6, l.room);
            bind(s, 7, l.teacher);
            bind(s, 8, l.state);
            bind(s, 9, l.begins);
            bind(s, 10, l.ends);
            rc = sqlite3_step(s);
            sqlite3_reset(s);
            if (rc != SQLITE_DONE) {
                sqlite3_finalize(s);
                throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
            }
        }
        sqlite3_finalize(s);
    } catch (...) {
        exec(db, "ROLLBACK");
        throw;
    }
    exec(db, "COMMIT");
}

std::vector<Lesson> Store::lessons(const std::string &from, const std::string &to) {
    sqlite3_stmt *s = nullptr;
    const char *sql =
        "SELECT source,date,hour,subject,subject_name,room,teacher,state,begins,ends FROM lessons"
        " WHERE date BETWEEN ? AND ? ORDER BY date, CAST(hour AS INTEGER), hour, source";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, from);
    bind(s, 2, to);
    auto text = [&](int c) {
        const unsigned char *v = sqlite3_column_text(s, c);
        return v ? std::string((const char *)v, sqlite3_column_bytes(s, c)) : std::string();
    };
    std::vector<Lesson> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        Lesson l;
        l.source = text(0);
        l.date = text(1);
        l.hour = text(2);
        l.subject = text(3);
        l.subject_name = text(4);
        l.room = text(5);
        l.teacher = text(6);
        l.state = text(7);
        l.begins = text(8);
        l.ends = text(9);
        out.push_back(std::move(l));
    }
    sqlite3_finalize(s);
    return out;
}

namespace {

const char *COLS =
    "SELECT id,source,class,kind,title,body,due_at,event_at,fetched_at,src_uid,url,weight FROM "
    "items ";

std::vector<Item> read_items(sqlite3_stmt *s) {
    auto text = [&](int c) {
        const unsigned char *v = sqlite3_column_text(s, c);
        return v ? std::string((const char *)v, sqlite3_column_bytes(s, c)) : std::string();
    };
    std::vector<Item> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        Item i;
        i.id = sqlite3_column_int64(s, 0);
        i.source = text(1);
        i.klass = text(2);
        i.kind = text(3);
        i.title = text(4);
        i.body = text(5);
        i.due_at = sqlite3_column_int64(s, 6);
        i.event_at = sqlite3_column_int64(s, 7);
        i.fetched_at = sqlite3_column_int64(s, 8);
        i.src_uid = text(9);
        i.url = text(10);
        i.weight = sqlite3_column_int(s, 11);
        out.push_back(std::move(i));
    }
    sqlite3_finalize(s);
    return out;
}

}  // namespace

// urgency increases downward: info, then upcoming (soonest last), then overdue (least overdue last)
std::vector<Item> Store::feed() {
    sqlite3_stmt *s = nullptr;
    std::string sql = std::string(COLS) +
                      "WHERE dismissed=0 AND kind<>'mark'"
                      " ORDER BY CASE WHEN COALESCE(due_at,0)=0 THEN 0"
                      "   WHEN due_at>=strftime('%s','now') THEN 1 ELSE 2 END,"
                      " CASE WHEN COALESCE(due_at,0)=0 THEN COALESCE(event_at,fetched_at)"
                      "   WHEN due_at>=strftime('%s','now') THEN -due_at ELSE due_at END, id";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    return read_items(s);
}

std::vector<Item> Store::marks() {
    sqlite3_stmt *s = nullptr;
    std::string sql = std::string(COLS) +
                      "WHERE dismissed=0 AND kind='mark'"
                      " ORDER BY COALESCE(event_at,fetched_at) DESC, id DESC";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    return read_items(s);
}

void Store::dismiss(const std::vector<long long> &ids) {
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE items SET dismissed=1 WHERE id=?", -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    for (long long id : ids) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) != SQLITE_DONE)
            throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
        sqlite3_reset(s);
    }
    sqlite3_finalize(s);
}

Store::Fetch Store::last_fetch(const std::string &source) {
    sqlite3_stmt *s = nullptr;
    const char *sql = "SELECT finished_at,ok,error FROM fetch_log WHERE source=?"
                      " ORDER BY finished_at DESC, id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, source);
    Fetch f;
    if (sqlite3_step(s) == SQLITE_ROW) {
        f.at = sqlite3_column_int64(s, 0);
        f.ok = sqlite3_column_int(s, 1) != 0;
        if (const unsigned char *v = sqlite3_column_text(s, 2))
            f.error.assign((const char *)v, sqlite3_column_bytes(s, 2));
    }
    sqlite3_finalize(s);
    return f;
}

long long Store::last_ok_fetch(const std::string &source) {
    sqlite3_stmt *s = nullptr;
    const char *sql = "SELECT MAX(finished_at) FROM fetch_log WHERE source=? AND ok=1";
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
    bind(s, 1, source);
    long long out = 0;
    if (sqlite3_step(s) == SQLITE_ROW) out = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return out;
}

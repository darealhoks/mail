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
  teacher_name TEXT NOT NULL DEFAULT '',
  change TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(source, date, hour)
);
CREATE TABLE IF NOT EXISTS absences(
  source TEXT NOT NULL,
  subject TEXT NOT NULL,
  lessons INTEGER NOT NULL DEFAULT 0,
  absent INTEGER NOT NULL DEFAULT 0,
  threshold REAL NOT NULL DEFAULT 0,
  PRIMARY KEY(source, subject)
);
)";

constexpr int SCHEMA_VERSION = 5;

std::runtime_error err(sqlite3 *db) {
    return std::runtime_error(std::string("store: ") + sqlite3_errmsg(db));
}

void exec(sqlite3 *db, const char *sql) {
    char *e = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        std::string m = e ? e : "sqlite error";
        sqlite3_free(e);
        throw std::runtime_error("store: " + m);
    }
}

// bind() takes the next free parameter in order; reset() rewinds it
struct Stmt {
    Stmt(sqlite3 *d, const char *sql) : db(d) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) throw err(db);
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    Stmt &bind(int v) {
        sqlite3_bind_int(s, ++n, v);
        return *this;
    }
    Stmt &bind(long long v) {
        sqlite3_bind_int64(s, ++n, v);
        return *this;
    }
    Stmt &bind(double v) {
        sqlite3_bind_double(s, ++n, v);
        return *this;
    }
    Stmt &bind(const std::string &v) {
        sqlite3_bind_text(s, ++n, v.data(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    // 0 is "unset" for due_at/event_at and must land as NULL, not as the epoch
    Stmt &bind_or_null(long long v) {
        ++n;
        v ? sqlite3_bind_int64(s, n, v) : sqlite3_bind_null(s, n);
        return *this;
    }

    bool step() {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc != SQLITE_DONE) throw err(db);
        return false;
    }
    void reset() {
        sqlite3_reset(s);
        n = 0;
    }

    int col_int(int c) { return sqlite3_column_int(s, c); }
    long long col_i64(int c) { return sqlite3_column_int64(s, c); }
    double col_double(int c) { return sqlite3_column_double(s, c); }
    std::string col_text(int c) {
        const unsigned char *v = sqlite3_column_text(s, c);
        return v ? std::string((const char *)v, (size_t)sqlite3_column_bytes(s, c)) : std::string();
    }

  private:
    sqlite3 *db;
    sqlite3_stmt *s = nullptr;
    int n = 0;
};

int user_version(sqlite3 *db) {
    Stmt s(db, "PRAGMA user_version");
    return s.step() ? s.col_int(0) : 0;
}

// delete-then-insert under one savepoint; both binders run against their own statement
void replace_rows(sqlite3 *db, const char *name, const char *del_sql, const char *ins_sql,
                  const std::function<void(Stmt &)> &bind_del,
                  const std::function<void(Stmt &)> &insert) {
    std::string sp(name);
    exec(db, ("SAVEPOINT " + sp).c_str());
    try {
        Stmt d(db, del_sql);
        bind_del(d);
        d.step();
        Stmt s(db, ins_sql);
        insert(s);
    } catch (...) {
        sqlite3_exec(db, ("ROLLBACK TO " + sp + "; RELEASE " + sp).c_str(), nullptr, nullptr,
                     nullptr);
        throw;
    }
    exec(db, ("RELEASE " + sp).c_str());
}

// human-visible columns only: set_state values carry a \x1f-delimited blob
std::string safe(const std::string &v) {
    std::string o;
    o.reserve(v.size());
    for (char c : v)
        if ((unsigned char)c >= 0x20 || c == '\n' || c == '\t') o += c;
    return o;
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
    // first, and alone: a busy_timeout armed later cannot cover the statements before it
    exec(db, "PRAGMA busy_timeout=15000");
    exec(db, "PRAGMA journal_mode=WAL");
    // the schema write must not run on every open — a reader would then need the write lock and
    // fail against a fetching maild. bump SCHEMA_VERSION with any SCHEMA change
    if (user_version(db) < SCHEMA_VERSION) {
        exec(db, SCHEMA);
        // must be a literal number: sqlite takes an unquoted identifier here as the string 0
        exec(db, ("PRAGMA user_version=" + std::to_string(SCHEMA_VERSION)).c_str());
    }
}

Store::~Store() { sqlite3_close(db); }

bool Store::queued(std::function<void()> f) {
    if (!defer) return false;
    deferred.push_back(std::move(f));
    return true;
}

void Store::flush() {
    defer = false;  // the replayed calls must reach the db, not re-queue
    for (auto &f : deferred) f();
    deferred.clear();
}

void Store::drop_deferred() {
    defer = false;
    deferred.clear();
}

void Store::begin() { exec(db, "BEGIN IMMEDIATE"); }
void Store::commit() { exec(db, "COMMIT"); }
void Store::rollback() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }

bool Store::insert_item(const Item &i) {
    Stmt s(db,
           "INSERT OR IGNORE INTO items(source,class,kind,title,body,due_at,event_at,fetched_at,"
           "src_uid,url,weight) VALUES(?,?,?,?,?,?,?,strftime('%s','now'),?,?,?)");
    s.bind(i.source)
        .bind(safe(i.klass))
        .bind(i.kind)
        .bind(safe(i.title))
        .bind(safe(i.body))
        .bind_or_null(i.due_at)
        .bind_or_null(i.event_at)
        .bind(i.src_uid)
        .bind(safe(i.url))
        .bind(i.weight);
    s.step();
    if (sqlite3_changes(db) > 0) return true;
    // a known item still gets its text refreshed: upstream edits show up, and rows stored by
    // an older build (no url, undecoded entities) heal on the next fetch. not "new" either way.
    Stmt u(db, "UPDATE items SET title=?,body=?,url=?,weight=?,kind=?,class=?,due_at=?,"
               "event_at=? WHERE source=? AND src_uid=?");
    u.bind(safe(i.title))
        .bind(safe(i.body))
        .bind(safe(i.url))
        .bind(i.weight)
        .bind(i.kind)
        .bind(safe(i.klass))
        .bind_or_null(i.due_at)
        .bind_or_null(i.event_at)
        .bind(i.source)
        .bind(i.src_uid);
    u.step();
    return false;
}

void Store::log_fetch(const std::string &source, long long started_at, bool ok,
                      const std::string &error, int items_new) {
    Stmt s(db, "INSERT INTO fetch_log(source,started_at,finished_at,ok,error,items_new)"
               " VALUES(?,?,strftime('%s','now'),?,?,?)");
    s.bind(source).bind(started_at).bind(ok ? 1 : 0).bind(error).bind(items_new);
    s.step();

    // ~35k rows/source/year at the cron rate; nothing reads past the last outage
    Stmt t(db, "DELETE FROM fetch_log WHERE source=?1 AND id <= (SELECT id FROM fetch_log"
               " WHERE source=?1 ORDER BY id DESC LIMIT 1 OFFSET 500)");
    t.bind(source);
    t.step();
}

std::string Store::get_state(const std::string &key) {
    Stmt s(db, "SELECT value FROM state WHERE key=?");
    s.bind(key);
    return s.step() ? s.col_text(0) : std::string();
}

void Store::set_state(const std::string &key, const std::string &value) {
    if (queued([this, key, value] { set_state(key, value); })) return;
    Stmt s(db, "INSERT INTO state(key,value) VALUES(?,?)"
               " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    s.bind(key).bind(value);
    s.step();
}

void Store::clear_state(const std::string &like) {
    Stmt s(db, "DELETE FROM state WHERE key LIKE ?");
    s.bind(like);
    s.step();
}

void Store::put_lessons(const std::string &source, const std::string &from, const std::string &to,
                        const std::vector<Lesson> &rows) {
    if (queued([this, source, from, to, rows] { put_lessons(source, from, to, rows); })) return;
    replace_rows(
        db, "lessons", "DELETE FROM lessons WHERE source=? AND date BETWEEN ? AND ?",
        "INSERT OR REPLACE INTO lessons(source,date,hour,subject,subject_name,room,teacher,state,"
        "begins,ends,teacher_name,change) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        [&](Stmt &d) { d.bind(source).bind(from).bind(to); },
        [&](Stmt &s) {
            for (const Lesson &l : rows) {
                s.bind(source)
                    .bind(l.date)
                    .bind(l.hour)
                    .bind(safe(l.subject))
                    .bind(safe(l.subject_name))
                    .bind(safe(l.room))
                    .bind(safe(l.teacher))
                    .bind(l.state)
                    .bind(l.begins)
                    .bind(l.ends)
                    .bind(safe(l.teacher_name))
                    .bind(safe(l.change));
                s.step();
                s.reset();
            }
        });
}

void Store::put_absences(const std::string &source, const std::vector<Absence> &rows) {
    if (queued([this, source, rows] { put_absences(source, rows); })) return;
    replace_rows(
        db, "absences", "DELETE FROM absences WHERE source=?",
        "INSERT OR REPLACE INTO absences(source,subject,lessons,absent,threshold)"
        " VALUES(?,?,?,?,?)",
        [&](Stmt &d) { d.bind(source); },
        [&](Stmt &s) {
            for (const Absence &a : rows) {
                s.bind(source).bind(safe(a.subject)).bind(a.lessons).bind(a.absent).bind(
                    a.threshold);
                s.step();
                s.reset();
            }
        });
}

std::vector<Absence> Store::absences() {
    Stmt s(db, "SELECT source,subject,lessons,absent,threshold FROM absences ORDER BY subject");
    std::vector<Absence> out;
    while (s.step()) {
        Absence a;
        a.source = s.col_text(0);
        a.subject = s.col_text(1);
        a.lessons = s.col_int(2);
        a.absent = s.col_int(3);
        a.threshold = s.col_double(4);
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<Lesson> Store::lessons(const std::string &from, const std::string &to) {
    Stmt s(db,
           "SELECT source,date,hour,subject,subject_name,room,teacher,state,begins,ends,"
           "teacher_name,change FROM lessons"
           " WHERE date BETWEEN ? AND ? ORDER BY date, CAST(hour AS INTEGER), hour, source");
    s.bind(from).bind(to);
    std::vector<Lesson> out;
    while (s.step()) {
        Lesson l;
        l.source = s.col_text(0);
        l.date = s.col_text(1);
        l.hour = s.col_text(2);
        l.subject = s.col_text(3);
        l.subject_name = s.col_text(4);
        l.room = s.col_text(5);
        l.teacher = s.col_text(6);
        l.state = s.col_text(7);
        l.begins = s.col_text(8);
        l.ends = s.col_text(9);
        l.teacher_name = s.col_text(10);
        l.change = s.col_text(11);
        out.push_back(std::move(l));
    }
    return out;
}

namespace {

#define ITEM_COLS \
    "SELECT id,source,class,kind,title,body,due_at,event_at,fetched_at,src_uid,url,weight FROM items "

std::vector<Item> read_items(Stmt &s) {
    std::vector<Item> out;
    while (s.step()) {
        Item i;
        i.id = s.col_i64(0);
        i.source = s.col_text(1);
        i.klass = s.col_text(2);
        i.kind = s.col_text(3);
        i.title = s.col_text(4);
        i.body = s.col_text(5);
        i.due_at = s.col_i64(6);
        i.event_at = s.col_i64(7);
        i.fetched_at = s.col_i64(8);
        i.src_uid = s.col_text(9);
        i.url = s.col_text(10);
        i.weight = s.col_int(11);
        out.push_back(std::move(i));
    }
    return out;
}

}  // namespace

// urgency increases downward: info, then upcoming (soonest last), then overdue (least overdue last)
std::vector<Item> Store::feed(long long since) {
    static const char *sql = ITEM_COLS
        "WHERE dismissed=0 AND kind<>'mark' AND id>?"
        // 30 days: a term of undismissed homework otherwise buries today
        " AND (COALESCE(due_at,0)=0 OR due_at > strftime('%s','now')-2592000)"
        " ORDER BY CASE WHEN COALESCE(due_at,0)=0 THEN 0"
        "   WHEN due_at>=strftime('%s','now') THEN 1 ELSE 2 END,"
        " CASE WHEN COALESCE(due_at,0)=0"
        "   THEN (CASE WHEN kind IN ('task','test') THEN 1 ELSE 0 END) ELSE 0 END,"
        " CASE WHEN COALESCE(due_at,0)=0 THEN COALESCE(event_at,fetched_at)"
        "   WHEN due_at>=strftime('%s','now') THEN -due_at ELSE due_at END, id";
    Stmt s(db, sql);
    s.bind(since);
    return read_items(s);
}

std::vector<Item> Store::marks(long long since) {
    static const char *sql = ITEM_COLS
        "WHERE dismissed=0 AND kind='mark' AND id>?"
        " ORDER BY COALESCE(event_at,fetched_at) DESC, id DESC";
    Stmt s(db, sql);
    s.bind(since);
    return read_items(s);
}

void Store::dismiss(const std::vector<long long> &ids) {
    Stmt s(db, "UPDATE items SET dismissed=1 WHERE id=?");
    for (long long id : ids) {
        s.bind(id);
        s.step();
        s.reset();
    }
}

Store::Fetch Store::last_fetch(const std::string &source) {
    Fetch f;
    {
        Stmt s(db, "SELECT finished_at,ok,error FROM fetch_log WHERE source=?"
                   " ORDER BY finished_at DESC, id DESC LIMIT 1");
        s.bind(source);
        if (s.step()) {
            f.at = s.col_i64(0);
            f.ok = s.col_int(1) != 0;
            f.error = s.col_text(2);
        }
    }
    if (f.at && !f.ok) {
        Stmt s(db, "SELECT MIN(finished_at) FROM fetch_log WHERE source=?1 AND ok=0"
                   " AND id > (SELECT COALESCE(MAX(id),0) FROM fetch_log"
                   " WHERE source=?1 AND ok=1)");  // by id, not time: same-second runs tie
        s.bind(source);
        if (s.step()) f.failing_since = s.col_i64(0);
    }
    return f;
}

long long Store::last_ok_fetch(const std::string &source) {
    Stmt s(db, "SELECT MAX(finished_at) FROM fetch_log WHERE source=? AND ok=1");
    s.bind(source);
    return s.step() ? s.col_i64(0) : 0;
}

# store

Sqlite at ~/.local/share/mail/mail.db. Written only by maild, read by all frontends.

Schema in src/core/store.cpp; creds in <data_dir>/creds/<source> as key=value lines, 0600
(src/core/creds.cpp).

- items(id, source, class, kind, title, body, due_at, event_at, fetched_at, src_uid)
  - src_uid = stable upstream id; new-item detection = insert-or-ignore on (source, src_uid)
  - kind: one vocabulary across sources — info | task | test | mark | change
    task/test/info come from src/sources/classify.cpp for free-text (teams posts, komens);
    bakalari homework is task outright, timetable changes are change (TypeName in title)
  - Store::feed() orders by urgency increasing downward — info, then upcoming (soonest last),
    then overdue (least overdue last) — so the last line of a listing is the one that matters.
    a due date more than 30 days past drops out entirely, or a term of stale homework buries
    today
- fetch_log(source, started_at, finished_at, ok, error, items_new)
  - staleness = now - last ok fetch per source; frontends must show it past a threshold
  - trimmed to the last 500 rows per source on every log_fetch
- lessons(source, date, hour, subject, subject_name, room, teacher, state, begins, ends,
  teacher_name, change)
  - primary key (source, date, hour); date "YYYY-MM-DD", hour the api caption ("1"), begins/ends
    "HH:MM" as sent, "" when upstream omits them
  - an empty hour means the row is a whole-day note, not a lesson: subject holds the event
    title and view::timetable moves it to Timetable::notes before building the grid
  - state: '' normal, 'x' cancelled, '!' changed
  - written by Store::put_lessons(source, from, to, rows): deletes that source's rows in
    [from, to] then inserts, one transaction — a re-fetched week replaces, lessons that
    disappeared upstream do not linger. bakalari writes monday..monday+6, this week and next
  - source "bakalari-perm" holds the permanent (recurring) grid, parked on the PERM_MONDAY
    (1970-01-05) week because it has no real dates; view::timetable re-dates it onto whatever
    week is asked for and flags Timetable::permanent. `next` never reads it: a recurring grid
    is no promise about a date
  - read by Store::lessons(from, to) for `mailc timetable` and `mailc next`; rows come back
    date-major, hour ascending, so the cli renders both axes without sorting dates
  - empty week falls back to the permanent grid, marked "permanent"; with neither it is
    "week <monday> — no lessons"; a never-fetched store looks the same, staleness
    is the fetch_log's job, not the grid's
- absences(source, subject, lessons, absent, threshold)
  - primary key (source, subject); a snapshot, not history — Store::put_absences(source, rows)
    deletes that source's rows then inserts, one SAVEPOINT, same discipline as put_lessons
  - subject is the abbrev when the timetable knew the full name, else the full name upstream sent
  - threshold is the school's absence limit in percent, duplicated on every row so no frontend
    has to name a source to find it; 0 when upstream sent none
  - read by Store::absences() for `mailc absence` and the marks block in paint::mark_lines
- state(key, value): source fetch cursors (teams delta links, channel cache), the outlook
  cold-sweep consent, `expired.<source>` (maild's session verdict — the message when the
  credentials were rejected, empty once a fetch works; this is what frontends read instead of
  probing a session themselves), and the two absence labels `bakalari.absence_year` ("25/26") and
  `bakalari.absence_note` (non-empty = why the totals are not this year's). not user data,
  droppable — losing a row costs one re-fetch of that channel's window

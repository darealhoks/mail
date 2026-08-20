# sources

Contract: one file pair per source. fetch(Store &) -> vector<Item>; throws on any
protocol/shape error (maild logs it to fetch_log and moves to the next source).
Fetch+insert per source is atomic, but the network half holds no write lock: maild sets
`store.defer`, so the writes a source makes mid-fetch (set_state cursors, put_lessons,
put_absences) queue in memory and are replayed by flush() inside the insert transaction.
A throw mid-fetch drops the queue, so a source's cursor advance never outlives its items.
Consequence for sources: get_state does not see a set_state made earlier in the same fetch.

Registry: `src/sources/registry.h` — `Source{name, pretty, have_session, session_error,
fetch, login}` and `sources()`, the built-in list filtered by `[source.<name>] enabled`.
`source(name)` looks one up (nullptr when unknown or disabled). Both binaries only iterate
it: nothing names a source longhand. A disabled source is invisible everywhere — no fetch,
no status line, no gripe, no sign-in prompt.

`login()` is the interactive tty sign-in and lives in the source
(`bakalari::login_interactive`, `teams::login_interactive`, qr included), not in the cli.

## bakalari

- host from `[source.bakalari] url` (no default; user sets it); API v3 3.50.1 (checked 2026-08-16)
- login: POST /api/login, form grant_type=password&username&password, plus client_id
  (`[source.bakalari] client_id`, default ANDR, kept out of the seeded file)
  password is sent plain (the sha512 "*login*"/"*sgn*ANDR" salt is v2-only and 400s here);
  refresh via grant_type=refresh_token on the same endpoint
- endpoints in use (src/sources/bakalari.cpp): GET /api/3/homeworks?from=&to=, GET /api/3/marks,
  POST /api/3/komens/messages/received (POST, empty body), GET /api/3/timetable/actual?date=,
  GET /api/3/timetable/permanent,
  GET /api/3/events/my?from=&to=, GET /api/3/absence/student
- window: homework and events -14d/+60d, timetable current week; absences are whole-year state
- /timetable/permanent parses through the same side tables (`side_tables`/`lesson_of`); its
  Days carry DayOfWeek (1 = monday) instead of a date, and it is fetched only on a full sweep
  or when the store holds no permanent rows — it changes per semester, not per week
- events -> info items, klass = EventType.Name, event_at = StartTime (top-level, else Times[0]),
  src_uid event:<Id>
- absences: AbsencesPerSubject[], absent = Base+Late+Soon+School (each optional, missing = 0);
  a row with no SubjectName, or absent > lessons, is dropped. runs after the timetable so it can
  map SubjectName -> Abbrev through the Subjects[] table and key like marks do; a subject not on
  the timetable keeps its full name. PercentageThreshold <= 1 is read as a fraction and scaled.
  UNVERIFIED against the live api: the threshold's polarity (assumed an absence ceiling, red at
  or above it) and the category field names — a renamed category under-counts silently
- komens Attachments[] (Id, Name, Type, Size) are named in the body as " [att: a, b]", the
  same marker teams::plain_text writes, before the classifier runs. bytes are not downloaded
  (GET /api/3/komens/attachment/{Id} would serve them)
- items: homework -> task (due_at=DateEnd), mark, komens -> classifier kind + due_at,
  timetable Atoms[].Change -> change; src_uid prefixes hw:/mark:/komens:/tt:
- timetable atoms also go to the lessons table via put_lessons (see .map/store.md); an atom
  with no hour caption or no subject is dropped, it has nothing to render
- ids are strings on some endpoints and numbers on others; Subjects[].Id and Abbrev are
  space-padded, Atoms[].SubjectId is not — trim before matching
- timetable side tables all key off trimmed Id: Subjects (Abbrev + Name), Rooms, Teachers,
  Hours (Caption + BeginTime/EndTime, "HH:MM" local) — Hours is where lesson times come from
- Atoms[].Change is present-but-null on normal lessons; on a cancellation SubjectId and
  TeacherId are null too, so those items carry no class
- Change.Day can differ from the enclosing day; it wins for event_at and src_uid

## teams

- user has NO entra access (cannot register app; ChannelMessage.Read.All admin wall
  assumed). route: device-code flow with a first-party microsoft client id
  (pre-consented), same as unofficial clients.
- fetch is per-channel `/messages/delta`: cold run filters `lastModifiedDateTime gt now-60d`
  and stores the returned deltaLink in state `teams.delta.<channel-id>`; every later run is
  one request per channel, almost always empty. 400/410 on a stored link resets that channel
  to the cold window. no replies are fetched (analysis: 19/239, no gold item lives in one)
- `/me/joinedTeams` + `/teams/*/channels` cached in state `teams.channels`, refreshed daily
- cold run backfills silently: main.cpp reports 0 new when last_ok_fetch was 0
- class = team name, plus `/channel` when the channel is not General/Obecné; kind comes from
  classify::kind (test wins over task, else info), due_at = classifier deadline
- src/sources/classify.cpp tags each post {task, test} + optional deadline; corpus in analysis/README.md
- message text is built exactly like analysis/clean.py (adaptive-card TextBlocks, then
  subject+body after ` || `, then `[att: …]`) or the classifier sees different input than
  the corpus it was tuned on; teams::plain_text mirrors clean.py's plain()
- classifier is plain substring scanning, no <regex>: rules are stem+suffix and fixed
  phrases, and <regex> costs more RSS than the whole daemon budget
- `make corpus` runs it over tests/corpus.tsv against gold.tsv (406/406 gold, 239/280
  deadlines, byte-identical to analysis/classify.py); scrubbed of personal data
- task posts carry only title + due in the adaptive card; the instructions text lives on
  the education assignment (id sits in the card's Action.OpenUrl context) and is unreachable —
  /education/classes/*/assignments/* is 403 under both first-party clients we can use
  (teams 1fec8e78, office d3590ed6); neither is preauthorized for EduAssignments.* and
  AADSTS65002 blocks asking for it. would need entra admin consent
- most fragile source: expect token/endpoint churn, log loudly

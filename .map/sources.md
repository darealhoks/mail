# sources

Contract: one file pair per source. fetch(Store &) -> vector<Item>; throws on any
protocol/shape error (maild logs it to fetch_log and moves to the next source).
Fetch+insert per source is atomic, but the network half holds no write lock: maild sets
`store.defer`, so the writes a source makes mid-fetch (set_state cursors, put_lessons,
put_absences) queue in memory and are replayed by flush() inside the insert transaction.
A throw mid-fetch drops the queue, so a source's cursor advance never outlives its items.
Consequence for sources: get_state does not see a set_state made earlier in the same fetch.

Registry: `src/sources/registry.h` — `Source{name, pretty, creds, have_session, fetch, login}`
and `sources()`, the built-in list filtered by `[source.<name>] enabled`.
`have_session` reads the creds file and nothing else. There is deliberately no live session
probe on this struct: a frontend must never reach the network, so the verdict "this session is
dead" is maild's to discover during a fetch and record in state `expired.<name>`, and every
frontend reads it from there. A probe here froze every `mailc` run for the length of the
http timeout the day the school's server started stalling.
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
- `login()` also stores username+password in the creds file; when the refresh token dies,
  `access_token()` replays that sign-in once. If it is rejected the password is dropped and
  SessionExpired surfaces as before, so sessions from before this still need one manual sign-in
- endpoints in use (src/sources/bakalari.cpp): GET /api/3/homeworks?from=&to=, GET /api/3/marks,
  POST /api/3/komens/messages/received (POST, empty body), GET /api/3/timetable/actual?date=,
  GET /api/3/timetable/permanent,
  GET /api/3/events/my?from=&to=, GET /api/3/absence/student
- /absence/student answers 400 `Datum je mimo rozsah rozvrhu.` outside the school year's
  timetable range (seen 2026-09-02, everything else 200): the block warns and keeps the
  stored absences instead of failing the whole sweep — that is the only tolerated non-200
- window: homework -30d/+60d, events -90d/+60d (a term-long whole-day event has to be caught
  from before it started), timetable this week + next; absences are whole-year state
- whole-day events also land on the grid as hourless `lessons` rows for the days they cover,
  which is how a week with no lessons still explains itself. view::timetable splits them back
  out into `Timetable::notes`
- `bakalari.absence_year` / `bakalari.absence_note` state keys carry what the absence totals
  are for; see .map/store.md
- /timetable/permanent parses through the same side tables (`side_tables`/`lesson_of`); its
  Days carry DayOfWeek (1 = monday) instead of a date, and it is fetched only on a full sweep
  or when the store holds no permanent rows — it changes per semester, not per week
- events -> info items, klass = EventType.Name, event_at = StartTime (top-level, else Times[0]),
  src_uid event:<Id>
- absences: AbsencesPerSubject[], absent = Base (Late/Soon/School are excused categories and
  do not count); a row with absent > lessons is dropped, one nameless row upstream sends is
  kept as "?". runs after the timetable so it can map SubjectName -> Abbrev through the
  Subjects[] table and key like marks do; a subject not on the timetable keeps its full name.
  PercentageThreshold <= 1 is read as a fraction and scaled.
  UNVERIFIED against the live api: the threshold's polarity (assumed an absence ceiling, red at
  or above it) and the category field names — a renamed category under-counts silently
- /absence/student takes no date, so the totals are whatever year upstream calls current.
  Absences[].Date is the only thing that dates them: its max sets `bakalari.absence_year`,
  which is what the ui labels the block with. the 400 above sets `bakalari.absence_note`
  instead, so stale totals say so rather than passing for this year's
- komens Attachments[] (Id, Name, Type, Size) are named in the body as " [att: a, b]", the
  same marker text::plain_text writes, before the classifier runs. bytes are not downloaded
  (GET /api/3/komens/attachment/{Id} would serve them)
- items: homework -> task (due_at=DateEnd), mark, komens -> classifier kind + due_at,
  timetable Atoms[].Change -> change; src_uid prefixes hw:/mark:/komens:/tt:
- one change item per day per (ChangeType, title), not per lesson: a day cut from the
  timetable is 8 atoms and read as 8 "new" items. body is bakalari::hour_runs() over the
  atoms' Hours captions — contiguous hours collapse to a range, a gap starts a new one, so
  the times never span an hour that did not change. src_uid tt:<date>:<type>:<title>, so a
  hour added to the same change later updates the row instead of making a second one
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
- fetch is per-channel `/messages/delta` through `teams::graph_delta`, the shared delta walker
  (outlook uses it too): cold run filters `lastModifiedDateTime gt now-60d` and stores the
  returned deltaLink in state `teams.delta.<channel-id>`; every later run is one request per
  channel, almost always empty. 400/410 on a stored link resets that channel to the cold
  window. the cursor is written only when a deltaLink arrives, so a run that hits MAX_PAGES
  throws rather than claiming to be caught up. no replies are fetched (19/239 posts had any,
  no gold item lives in one)
- `/me/joinedTeams` + `/teams/*/channels` cached in state `teams.channels`, refreshed daily
- cold run backfills silently: main.cpp reports 0 new when last_ok_fetch was 0
- class = team name, plus `/channel` when the channel is not General/Obecné; kind comes from
  classify::kind (test wins over task, else info), due_at = classifier deadline
- src/sources/classify.cpp tags each post {task, test} + optional deadline
- message text is built as adaptive-card TextBlocks, then subject+body after ` || `, then
  `[att: …]` — change this and the classifier sees different input than the corpus it was
  tuned on (tests/corpus.tsv was generated from exactly this shape)
- classifier is plain substring scanning, no <regex>: rules are stem+suffix and fixed
  phrases, and <regex> costs more RSS than the whole daemon budget
- `make corpus` runs it over tests/corpus.tsv, whose last column is the expected kind
  (406/406, 239/280 deadlines); scrubbed of personal data
- task posts carry only title + due in the adaptive card; the instructions text lives on
  the education assignment (id sits in the card's Action.OpenUrl context) and is unreachable —
  /education/classes/*/assignments/* is 403 under both first-party clients we can use
  (teams 1fec8e78, office d3590ed6); neither is preauthorized for EduAssignments.* and
  AADSTS65002 blocks asking for it. would need entra admin consent
- most fragile source: expect token/endpoint churn, log loudly
- `teams::graph_get` is the shared graph transport (retry, 401-remint, `GraphError.status`);
  outlook calls it and rides the same token, hence `creds = "teams"` on its registry row

## outlook

- school mailbox over the same graph token as teams; no second sign-in, no second client id.
  `Mail.Read` is in client 1fec8e78's preauthorized set (checked live 2026-08-20)
- `GET /me/mailFolders/inbox/messages/delta` through `teams::graph_delta`, deltaLink in state
  `outlook.delta.body`. the token bakes in `$select`, so the key is renamed whenever the field
  list changes or resumed runs keep serving the old fields. `$top` is ignored here — page size
  comes from `Prefer: odata.maxpagesize`
- delta takes `$filter` on receivedDateTime ONLY; `isRead` 400s, so unread is filtered
  client-side. `$deltatoken=latest` is ignored too (it pages the whole folder), so there is
  no cheap way to grab a cursor without a sweep
- sync modes (`[source.outlook] sync`): recent (default, unread mail newer than recent_days),
  unread (every unread mail), all (the whole inbox). recent is the only bounded cold run;
  the other two refuse to fetch until `mailc auth outlook` prints the mail count and gets a
  y, which records the mode in state `consent.outlook`
- a read mail never enters the store under recent/unread: read = already dealt with. the
  store holds what is still open, so old mail is absent rather than present-and-dismissed
- body is the whole `body` (not the 255-char `bodyPreview`): it rides the same delta page, so
  it costs no extra request. contentType is html on all but the plainest mail and goes through
  text::plain_text, which passes text/plain unchanged. `hasAttachments` appends the ` [att]`
  marker, names are not fetched
- class = sender display name, else the address. kind/due from the classifier, which was
  tuned on teams posts — mail is out-of-domain for it, expect info to dominate
- src_uid `mail:<id>`, url = webLink

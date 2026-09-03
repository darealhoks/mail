# config

`~/.config/<name>/config` (XDG_CONFIG_HOME honoured), written with commented defaults on
first read. Sectioned ini: `[section]` headers, `key = value`, `#` comments. Keys are
`section.key` in code (`config().str/num/flag/list`); defaults live in one table in
`src/core/config.cpp` and the seeded file is generated from it. Unknown keys warn on stderr.
Keys before the first header are dropped with a warning, not filed under `[general]`.
Warnings carry `<config path>:<line>`.

    [general]
    limit      0     cap whichever listing runs, 0 = all (the feed keeps the tail: most urgent)
    links      no    print the item url line
    blacklist  anj   comma list; matched against the class abbrev and the raw class name
    years      auto  school years to scrape; auto = current one. list form: 25/26, 26/27
    stale_warn yes   warn when the store is older than the fetch period
    interval   900   seconds; must match the crontab period, only feeds the staleness check
    marks_newest_last yes  marks oldest first, so the newest sit by the prompt; no = newest first
    notify           notification exec hook, empty = silent
    browser          opener for `mailc open`, default xdg-open

    [source.bakalari]
    url              your school's bakalari base url; empty by default, fetch errors until set
    enabled    yes   no = the source is invisible everywhere (no fetch, status, prompt)
    client_id  ANDR  protocol, not taste: not written to the seeded file

    [source.teams]
    enabled    yes

    [source.outlook]
    enabled    no      off by default; the mailbox is opt-in
    sync     recent    recent = unread mail newer than recent_days, unread = every unread
                       mail, all = the whole inbox. the last two ask for a y first
    recent_days  14

    [table]
    time       yes   the time row(s) over the timetable grid
    room       yes   the room in each cell
    teacher    yes   the teacher line (tui cells only, when there is vertical room)

    [school]
    half_end   01-31                 MM-DD H1 ends on (year rolls 1 Aug)
    avg_round  1.5, 2.5, 3.5, 4.5    average floors for marks 2..5 (avg_mark())
    mark_scale 1-5                   best-worst mark digits
    points     90, 75, 60, 40        percent floors for marks 1..4 (points_mark())
    absence_warn 15                  absence percent that turns a subject yellow
    absence_max  25                  ...and red; overrides the school's own threshold

    [key]
    f = feed                         tui mode switch; free-form keys, one char each
    m = marks
    b = absence
    t = timetable

    [bind]
    rozvrh = timetable               free-form keys, exempt from the unknown-key warning
    t      = next -s

The timetable grid sizes itself in one place, `paint::table_layout` (`src/view/paint.cpp`),
used by both `mailc timetable` and the tui: column width is the widest cell content the
frontend will draw, clamped to what the terminal holds — never padded past it. The time
header takes the widest form that fits, in order: `8:00-8:45`, `00-45`, start over end on
two rows, none. `view::compact` drops any hour column and any day row nothing occupies, so a
0th hour or a long tail costs nothing when the week does not use it. In the tui the block is
centred in the pane both ways. The header line doubles as the week control: `<` and `>` step
a week, the label opens a menu of this week ±2/+4 — clicked, or `[` `]` `w` from the keyboard.
The cli prints the same header without the arrows (`rows <= 0`), so a pipe stays plain. Turning a `[table]` key off shrinks the grid: `time = no` alone frees the
whole header and lets narrow columns hold the subject.

`[bind]` maps a word of your own to a command line. `expand_bind` in `src/cli/cli.cpp` runs
first in `main()` — before `take_flags` so flags inside a bind value are parsed, and before
the verb table so **a bind wins over the builtin verb of the same name** (`m = marks -n 1`,
even `help`). Only `--selfcheck` is unshadowable: it is a developer hook, not a verb. One
level of substitution: the first non-flag word only, value split on spaces, no recursion, no
shell. A bind whose value names nothing fails exactly like that word typed by hand.
`rozvrh = timetable` ships in the seeded file as the example.

`[key]` is the tui's own: one character to `feed`, `marks` or `table`/`timetable`, read once
at startup by `keymap()` in `src/tui/tui.cpp`. An unmapped char keeps the built-in f/m/t.


Every key has a flag that overrides it for one run: `-n/--limit`,
`--links/--no-links`, `-b/--blacklist`, `-B/--no-blacklist`, `-a/--all`. Flags are stripped
from argv before the command word, so they can sit anywhere.

`-s/--simple` and `-f/--format <fmt>` are `next`-only and back no config key; `-s` is a
literal alias for `-f "%t %s %r"`. `next --help` prints the verb table. Verbs: `%t %e` begin/end, `%s %S` abbrev/full name, `%r` room, `%u` teacher, `%h` hour,
`%d` date, `%m` "in 12m", `%!` change marker, `%%`. Unknown verb is an error, never silent.

`next` exit codes are the bar contract: 0 printed a lesson, 1 nothing upcoming (prints
nothing — holidays, or the week is over), 2 something is wrong and stderr says what.
A format line is printed as written: a lesson with no room leaves the gap.

Filters and class abbrevs are compared through `classify::norm` (fold() in cli.cpp): the
user types and matches ascii, so `Čeština` is `cestina`.

`years` limits fetching only, never the store: nothing already stored is hidden or deleted
when the year rolls, and dismissed stays dismissed (insert is OR IGNORE on (source,src_uid)).

School year rolls on 1 August; `school.half_end` splits it in two. Marks are printed under
`— 25/26 · H1 —` headers, oldest first (`general.marks_newest_last`); the feed is not.

Mark colors and the average both go through `school.points`/`school.mark_scale`
(`points_mark`, `mark_scale` in config.h): a `got/max` mark is scored by percent floors, a
plain digit by the scale, and `1-` is half a grade worse. An average prints as `1,50 (2)`
(`paint::avg_str`/`avg_color`), the bracket being `avg_mark()` on `school.avg_round`. In the
tui a subject whose every mark is ungradeable shows a green `N` instead.

Absence colours come from `school.absence_warn`/`absence_max`, not from upstream: Bakalari's
`PercentageThreshold` is stored per row but only used when `absence_max` is 0, since the limit
differs per programme (25 normally, 75 on an individual plan).

Blacklisted classes drop out of the feed before it is numbered, so `dismiss`/`open` indices
stay contiguous.

`maild --cold` drops the teams delta links and channel cache so the next run rescrapes the
whole configured window. Items already stored are untouched (OR IGNORE on (source,src_uid)),
so dismissed stays dismissed and nothing is reported as new.

## notifications (src/core/notify.cpp)

The hook is run through /bin/sh with wispctl's argv order, each argument single-quoted:

    <notify> <urgency 0|1|2> <summary> <body> <icon-codepoint>

Fired by maild only: one grouped "You've got mail" (urgency 0, icon f0e0) per fetch with a
`+N <kind>` line per kind that gained items, always plural; and one "<source> signed out"
(urgency 2, icon f023) per outage, gated by state key `expired.<source>`, which holds the
rejection message and is cleared by the next successful fetch of that source. That same key is
the session verdict every frontend reads. Cold runs never notify.

Staleness is never notified: `mailc` sees it by itself and prints it. Each `maild` run
publishes `general.interval` to state key `daemon.interval`; anything older than 2x that is a
yellow warning (one skipped tick is jitter), falling back to 6h when maild never ran.
A failed fetch stays a red error. `stale_warn = no` silences the warning.

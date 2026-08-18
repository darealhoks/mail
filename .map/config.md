# config

`~/.config/<name>/config` (XDG_CONFIG_HOME honoured), written with commented defaults on
first read. Sectioned ini: `[section]` headers, `key = value`, `#` comments. Keys are
`section.key` in code (`config().str/num/flag/list`); defaults live in one table in
`src/core/config.cpp` and the seeded file is generated from it. Unknown keys warn on stderr.
Keys before the first header are read as `[general]` with a warning — that is how a
pre-sections file keeps working; nothing is dropped.

    [general]
    limit      0     cap whichever listing runs, 0 = all (the feed keeps the tail: most urgent)
    links      no    print the item url line
    blacklist  anj   comma list; matched against the class abbrev and the raw class name
    years      auto  school years to scrape; auto = current one. list form: 25/26, 26/27
    stale_warn yes   warn when the store is older than the daemon's poll rate
    notify           notification exec hook, empty = silent
    browser          opener for `mailc open`, default xdg-open

    [source.bakalari]
    url              your school's bakalari base url; empty by default, fetch errors until set
    enabled    yes   no = the source is invisible everywhere (no fetch, status, prompt)
    client_id  ANDR  protocol, not taste: not written to the seeded file

    [source.teams]
    enabled    yes

    [school]
    quarters   11-15, 01-31, 04-15   quarter end dates MM-DD, ordered from 1 Aug
    mark_scale 1-5                   best-worst mark digits
    points     90, 75, 60, 40        percent floors for marks 1..4 (points_mark())

    [bind]
    rozvrh = timetable               free-form keys, exempt from the unknown-key warning
    t      = next -s

`[bind]` maps a word of your own to a command line. `expand_bind` in `src/cli/cli.cpp` runs
first in `main()` — before `take_flags` so flags inside a bind value are parsed, and before
the verb table so **a bind wins over the builtin verb of the same name** (`m = marks -n 1`,
even `help`). Only `--selfcheck` is unshadowable: it is a developer hook, not a verb. One
level of substitution: the first non-flag word only, value split on spaces, no recursion, no
shell. A bind whose value names nothing fails exactly like that word typed by hand.
`rozvrh = timetable` ships in the seeded file as the example.

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

School year rolls on 1 August. Quarters come from `school.quarters`: with the default
Q1 to 15 Nov, Q2 to 31 Jan (= H1), Q3 to 15 Apr, Q4 to 31 Jul. Any number of boundaries
parses; H1 is Q1-Q2. Marks are printed under `— 25/26 · H1 · Q2 —` headers; the feed is not.

Mark colors and the average both go through `school.points`/`school.mark_scale`
(`points_mark`, `mark_scale` in config.h): a `got/max` mark is scored by percent floors, a
plain digit by the scale, and `1-` is half a grade worse.

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
(urgency 2, icon f023) per outage, gated by state key `expired.<source>` which the next
successful fetch of that source clears. Cold runs never notify.

Staleness is never notified: `mailc` sees it by itself and prints it. The daemon publishes
its `--interval` to state key `daemon.interval` on startup; anything older than 2x that is a
yellow warning (one skipped tick is jitter), falling back to 6h when no daemon ever ran.
A failed fetch stays a red error. `stale_warn = no` silences the warning for cron users.

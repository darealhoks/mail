# map

- [store.md](store.md) — db schema, who reads/writes what
- [sources.md](sources.md) — source module contract + registry; bakalari + teams protocol facts
- [auth-ux.md](auth-ux.md) — teams session death handling, resign flow ux
- [config.md](config.md) — config file sections and keys, flags, binds, school-year periods
- [audit.md](audit.md) — 2026-08 full audit: measured footprint, ranked bugs, change/LoC/RAM table

## layout

    src/core/     store config creds json http oauth notify term   — no source, no ui
    src/sources/  bakalari teams teams_auth classify + registry.h  — one pair per source
    src/view/     store -> row structs, pure: no printf, no ansi, no term width
                  + paint.cpp: wrap/width/sgr helpers shared by cli and tui
    src/maild/    main.cpp     oneshot fetch + notify; run from cron: */15 * * * * maild
    src/cli/      cli.cpp      argv, binds, layout, paint
    src/tui/      tui.cpp      raw-mode feed/marks/timetable, tab strip, keys + mouse, no flags

`make` builds all three binaries from `src/core src/sources src/view` plus one main each.
`make check` runs the two `--selfcheck`s; `make corpus` scores the classifier against
`tests/gold.tsv`.

## cli verbs

    (none)          the feed, newest deadlines first; bare words are filters
    auth [src]      sign-in state, or sign into one source
    new             counts for the shell prompt hook; prints, never prompts
    marks [filter]  marks by period
    timetable       the week's grid
    next [--help]   the next lesson; -f/-s format it, --help lists the % verbs
    dismiss / open  by feed index

Aliases are one letter (`a n m r d o`; `next` has none — `n` is `new`) and any of them can be
taken over by a `[bind]` entry.

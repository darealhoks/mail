# mail

Read-only aggregator for school systems. `maild` fetches Bakaláři and MS Teams from cron
into a local sqlite db; `mailc` prints from that db, `mailt` browses it in the terminal.

Everything is local. No server, no cloud, no AI.

> [!WARNING]
> Read-only, but not a sanctioned integration: Teams is reached with a first-party
> Microsoft client id, built against an account with no Entra app registration rights.
> Microsoft's [APIs Terms of Use](https://learn.microsoft.com/en-us/legal/microsoft-apis/terms-of-use)
> license API access only for an app you register yourself (§2.b, §3.a), so this may
> breach them, and your school may block it. Your call, your responsibility. No warranty.

## Build

    ./install.sh    # deps check, build, install, optional crontab entry

or by hand:

    make            # -> build/maild build/mailc build/mailt
    make check      # self-checks
    make install    # to ~/.local/bin

Needs a C++20 compiler, libcurl, sqlite3, simdjson. `make NAME=x` builds everything
under another name (binaries, config dir, data dir).

## Use

    maild                        # fetch once
    */15 * * * * maild           # crontab: keep fetching
    mailc                        # the feed, nearest deadline first
    mailc <word>                 # filter it
    mailc marks, m               # marks by half (25/26 · H1), absence per subject under it
    mailc absence, b             # absence per subject
    mailc timetable, r [week]    # this week's grid; +n/-n or YYYY-MM-DD picks another
    mailc next                   # the next lesson (-s for bars; next --help lists the % verbs)
    mailc new, n                 # counts, for a shell prompt hook
    mailc auth, a [source]       # sign-in state, or sign in

    mailc dismiss <n>, d / open <n>, o    # by feed index

Flags, one-shot overrides of the config: `-n/--limit <n>`, `--links/--no-links`,
`-b/--blacklist a,b`, `-B/--no-blacklist`, `-a/--all`, and `next`-only `-s/--simple`,
`-f/--format <fmt>`. `mailc help` prints the lot.

    mailt                        # WIP: the same store in a full-screen TUI

`mailt` is a work in progress. `f` feed, `m` marks, `t` timetable, `enter` opens the
selected post, `a` sign in, `r` fetch now, `q` quits; mouse and wheel work. It redraws
when `maild` writes. Rebind the tabs in the config (`key.<char> = feed|marks|timetable`).

First run:

    ./install.sh
    mailc auth bakalari          # asks for the url, then username and password
    mailc auth teams             # device-code sign-in in a browser
    maild                        # first fetch

`~/.config/mail/config` is written with commented defaults on first run; every key is
documented in `.map/config.md`.

## Layout

    src/core/     store config creds http oauth json notify
    src/sources/  bakalari teams classify + registry
    src/view/     store rows -> render structs
    src/maild/    oneshot fetch, notify
    src/cli/      argv, layout, colors
    src/tui/      full-screen frontend (WIP)

One sqlite db (`~/.local/share/mail/mail.db`) is the only interface between them.
Every fetch is logged; a failed or stale source is shown as failed or stale in every
frontend.

More detail: `.map/` (start at `00_INDEX.md`).

## License

MIT.

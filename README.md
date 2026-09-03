# mail

Read-only aggregator for school systems. `maild` fetches Bakaláři, MS Teams and the school
Outlook mailbox from cron into a local sqlite db; `mailc` prints from that db, `mailt`
browses it in the terminal.

Everything is local. No server, no cloud, no AI.

> [!WARNING]
> Read-only, but not a sanctioned integration: Teams is reached with a first-party
> Microsoft client id, built against an account with no Entra app registration rights.
> Microsoft's [APIs Terms of Use](https://learn.microsoft.com/en-us/legal/microsoft-apis/terms-of-use)
> license API access only for an app you register yourself (§2.b, §3.a), so this may
> breach them, and your school may block it. Outlook rides the same token. Your call,
> your responsibility. No warranty.

## Install

    curl -fsSL https://raw.githubusercontent.com/darealhoks/mail/main/install.sh | sh

Runs from anywhere: clones to a temp dir, checks deps, builds, installs to `~/.local/bin`,
offers a crontab entry. `NAME=`, `PREFIX=`, `CXX=` and `REPO=` override. Inside a checkout,
`./install.sh` does the same without cloning.

By hand:

    make            # -> build/maild build/mailc build/mailt
    make check      # self-checks
    make install    # to ~/.local/bin
    make uninstall

Needs git, make, pkg-config, a C++20 compiler, libcurl, sqlite3 and simdjson.
`make NAME=x` builds everything under another name — binaries, config dir, data dir,
user agent. `DEV=1` (the default in a checkout) adds `-Werror`; the installer uses
`DEV=0`.

## First run

    mailc auth bakalari          # asks for the url, then username and password
    mailc auth teams             # device-code sign-in in a browser
    maild                        # first fetch
    */15 * * * * maild           # crontab: keep fetching

`~/.config/mail/config` is written with commented defaults on first run. The school
mailbox is off by default; `[source.outlook] enabled = yes` turns it on and it reuses the
Teams sign-in — no second login.

## mailc

    mailc                        # the feed, most urgent last
    mailc <word>                 # filter it
    mailc marks, m [subj]        # marks by half (25/26 · H1)
    mailc absence, b [subj]      # absence per subject, one school year
    mailc timetable, r [week]    # this week's grid; +n/-n or YYYY-MM-DD picks another
    mailc next                   # the next lesson (-s for bars; -f for a format)
    mailc new, n                 # one line of what you haven't seen, for a prompt hook
    mailc auth, a [source]       # sign-in state, or sign in
    mailc dismiss <n>, d         # hide items by feed number
    mailc open <n>, o            # open one in a browser
    mailc help / --version

Filters are bare words, ANDed: a kind (`info task test mark change`), a class abbrev, or
a source name.

One-shot overrides of the config: `-n/--limit <n>`, `--links/--no-links`,
`-b/--blacklist a,b`, `-B/--no-blacklist`, `-a/--all`, and `next`-only `-s/--simple` and
`-f/--format <fmt>`. `mailc help` prints the lot.

## mailt

    mailt

Four tabs — `f` feed, `m` marks, `t` timetable, `b` absence; `tab`/`shift-tab` cycle.
Everything scrolls with `j k ↓ ↑ space ^d ^u pgup pgdn g G`. The feed moves a post at a
time, centred, and by paragraph through one taller than the screen; `J`/`K` always jump
whole posts, `enter` opens the link, `X` dismisses. In the timetable `hjkl` move, `enter`
shows the lesson in full, `[` `]` (or the `<` `>` in the header) change week, `p` toggles
the permanent grid and `w` (or a click on the week label) picks one from a list.
`/` filters, `esc` clears; `a` signs in, `r` fetches now, `q` quits. Mouse and wheel work,
and it redraws when `maild` writes.

**`?` shows every key.**

Rebind the tabs in the config: `key.<char> = feed|marks|table|absence`.

## Config

`~/.config/mail/config`, ini-ish, written with every key commented on first run.
`XDG_CONFIG_HOME` and `XDG_DATA_HOME` are honoured. The notable ones:

    [general]        limit links blacklist accent bar date raw_names years
                     notify (default `wispctl notify`) browser interval stale_warn
    [source.<name>]  enabled, plus bakalari url and outlook sync / recent_days
    [school]         half_end, mark scale, rounding, absence thresholds
    [table]          which fields a timetable cell carries
    [key]            tui tab bindings
    [bind]           your own word for a command
    [classes]        a short name for a class the heuristic gets wrong

Full reference: `.map/config.md`.

## Layout

    src/core/     store config creds http oauth json text notify
    src/sources/  bakalari teams outlook classify + registry
    src/view/     store rows -> render structs (view.cpp), terminal render (paint.cpp)
    src/maild/    oneshot fetch, notify
    src/cli/      argv, layout, colors
    src/tui/      full-screen frontend
    src/tests/    make check

One sqlite db (`~/.local/share/mail/mail.db`) is the only interface between them.
Every fetch is logged; a failed or stale source is shown as failed or stale in every
frontend.

More detail: `.map/` (start at `00_INDEX.md`).

## License

MIT.

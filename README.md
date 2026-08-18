# mail

Read-only aggregator for school systems. `maild` fetches Bakaláři and MS Teams on a
timer into a local sqlite db; `mailc` prints from that db.

Everything is local. No server, no cloud, no AI.

> [!WARNING]
> Read-only, but not a sanctioned integration: Teams is reached with a first-party
> Microsoft client id, built against an account with no Entra app registration rights.
> Microsoft's [APIs Terms of Use](https://learn.microsoft.com/en-us/legal/microsoft-apis/terms-of-use)
> license API access only for an app you register yourself (§2.b, §3.a), so this may
> breach them, and your school may block it. Your call, your responsibility. No warranty.

## Build

    make            # -> build/maild build/mailc
    make check      # self-checks
    make install    # to ~/.local/bin

Needs a C++20 compiler, libcurl, sqlite3, simdjson. `make NAME=x` builds everything
under another name (binaries, config dir, data dir).

## Use

    maild                        # fetch once
    maild --daemon --interval=900   # keep fetching
    mailc                        # the feed, nearest deadline first
    mailc <word>                 # filter it
    mailc marks                  # marks by quarter
    mailc timetable              # this week's grid
    mailc next                   # the next lesson (-f "%t %s %r" for bars)
    mailc new                    # counts, for a shell prompt hook
    mailc auth [source]          # sign-in state, or sign in

    mailc dismiss <n> / open <n> # by feed index

Set your school's Bakaláři url in `~/.config/mail/config` (written with commented
defaults on first run), then `mailc auth`.

## Layout

    src/core/     store config creds http oauth json notify
    src/sources/  bakalari teams classify + registry
    src/view/     store rows -> render structs
    src/maild/    fetch loop, timers, notify
    src/cli/      argv, layout, colors

One sqlite db (`~/.local/share/mail/mail.db`) is the only interface between them.
Every fetch is logged; a failed or stale source is shown as failed or stale in every
frontend.

More detail: `.map/` (start at `00_INDEX.md`).

## License

MIT.

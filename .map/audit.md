# audit — 2026-08-19

Full-codebase audit (7 focused passes: daemon footprint, active-path perf, secrets/auth,
untrusted input, store/reliability, frontend over-engineering, core/sources + build hygiene).
5664 LoC at time of audit. Verdict: not over-engineered; a handful of real bugs; the idle-RAM
goal is reached by deleting daemon mode, not by trimming it.

## Measured facts

- maild idle: 13.3 MB RSS / 7.0 MB PSS / **2.6 MB private dirty**. 10.7 MB of RSS is clean
  file-backed library text (libcrypto 3.6M, sqlite 1.4M, libstdc++ 1.5M…), reclaimed by the
  kernel for free under pressure. `curl_global_cleanup()` measured: frees 0 kB.
- CPU idle is a true 0: single thread parked in `hrtimer_nanosleep`, no timerfd/poll, woken
  only by SIGUSR1/SIGTERM. ~2.9 s CPU over 2 h uptime, all fetch work.
- `malloc_trim(0)` already runs per tick; sqlite conn + curl handles already per-tick RAII.
  Nothing left to trim inside the daemon — the only way below the target is no resident process.
- clean build emits zero warnings; ASan+UBSan (clang, `-D_GLIBCXX_ASSERTIONS`) over all three
  selfchecks: zero findings, zero leaks.
- marks tab at 3000 synthetic items: ~120 ms (21 full-table scans); `mailc new` per shell
  prompt: ~6 ms full-feed scan.

## Bugs (ranked)

1. **teams delta cursor committed before items stored** — `teams.cpp:321` advances
   `teams.delta.<ch>` per channel; insert happens in `maild/main.cpp:177-181` after all
   channels. Throw in channel N or kill in the window = posts of channels 1..N-1 lost
   forever, silently. Fix: one transaction per source around fetch+insert (BEGIN IMMEDIATE
   in `run()`, `put_lessons` switches to SAVEPOINT so it nests). Also fixes partial-fetch
   items being demoted to "not new" after a crash.
2. **terminal escape injection** — no C0 stripping anywhere; ESC survives collapse/paint,
   `html_unescape` (`json.cpp:52`) even builds one from `&#27;`; TUI `fit()` treats CSI as
   zero-width. Fix: `safe()` strip at the store insert boundary (human-visible columns only —
   not `bind()` itself, `set_state` carries the `\x1f` channel blob).
3. **unbounded HTTP body** — `http.cpp:9` write callback has no cap; also `bad_alloc` thrown
   through a C callback is UB. Fix: 32 MB ceiling, return short write.
4. **redirect leaks** — `FOLLOWLOCATION` + `CURL_REDIR_POST_ALL` (`http.cpp:25,59`) re-sends
   the bakalari password / refresh token to whatever a 302 names, incl. plain http (default
   REDIR_PROTOCOLS allows it); custom `Authorization:` headers on GETs are stripped cross-host
   by modern curl but only implicitly. Fix: `PROTOCOLS_STR`/`REDIR_PROTOCOLS_STR "https"`,
   `MAXREDIRS 5`, no FOLLOWLOCATION on token/login POSTs.
5. **`http://` host accepted** — `bakalari.cpp:26-29` guard only checks prefix `http`, so a
   configured `http://` (or `httpfoo`) sends the password cleartext. Fix in `base()`.
6. **upstream edits to due_at/kind/class dropped** — refresh UPDATE (`store.cpp:133`) only
   touches title/body/url/weight; an edited deadline keeps the stale `due_at` forever.
7. **swallowed sqlite errors** — refresh UPDATE rc discarded (`store.cpp:134-143`); read
   loops treat BUSY/IOERR/CORRUPT as end-of-data → truncated feed rendered as complete
   (`store.cpp:261,291`).
8. **classify::epoch signed overflow** on hostile ISO date (`classify.cpp:288`), `atoi`
   overflow on `Weight` (`bakalari.cpp:180`) — both mis-file `due_at`/weight.
9. **`log_fetch` throw inside catch kills maild** (`main.cpp:194,201`) — wrap in try/catch.
   (Moot once daemon mode is deleted.)
10. Minor: every frontend open runs migrations + writes (`store.cpp:96-104`, no
    `user_version`); WAL result unverified; stale pre-rename `creds/*.json` on disk still
    hold tokens (delete them); creds dir perms not re-enforced if it pre-exists 0755;
    surrogate code points in `html_unescape` produce invalid UTF-8.

## Passed (checked, clean)

TLS verify never weakened; creds file 0600 at open, atomic rename, no umask race; no secret
in argv/env/sqlite/logs/URLs; git history clean of creds/hosts; device-code flow handles
expiry/slow_down/hostile intervals; SQL fully parameterized, no printf(userdata), no
std::regex (no ReDoS), simdjson errors disciplined, notify quoting correct + selfchecked;
dedup key sound, put_lessons atomic, UTC consistent, failures surface ungated in all
frontends, notifications coalesce; `make check` is real coverage, not theater.

## Proposed changes

RAM/latency vs LoC — owner's tradeoff table. "idle" deltas are for the maild process.

| # | proposed change | LoC | effect | risk |
|---|---|---|---|---|
| 1 | delete `--daemon` mode; user crontab `*/15 * * * * maild`; oneshot writes `daemon.interval` (or staleness reads config); `tui.cpp:277` spawns oneshot instead of `pkill -USR1` | −30 +5 | idle −13.3 MB RSS / −7.0 MB PSS → **0** | low-med: cron 1-min granularity, loses SIGUSR1 |
| 2 | per-source transaction around fetch+insert (bug 1+2) | +12 −3 | no permanent post loss; fewer fsyncs on cold run | low |
| 3 | `safe()` C0 strip at store boundary (bug 2) + selfcheck | +10 | closes terminal injection | low |
| 4 | http hardening: body cap, redirect protocol/hop limits, no-follow on auth POSTs, https-only `base()` (bugs 3-5) | +10 | closes OOM + cred-leak paths | low |
| 5 | refresh UPDATE: update due_at/event_at/kind/class + check rc; read loops throw on rc≠DONE (bugs 6-7) | +13 | no silently-stale deadlines / truncated feeds | low |
| 6 | bound epoch fields, `strtol`+clamp Weight, reject surrogates (bug 8, L1) | +7 −4 | correct due_at under hostile input | low |
| 7 | marks tab: compute averages in one pass over `msnap`, drop `avg_of`/`avg_cache` | +12 −30 | 21 scans → 1; ~120 ms → <1 ms at 3k items | low |
| 8 | `new_counts`: projected `id>?` queries instead of full feed+marks scans | +16 −2 | ~6 ms → ~0.05 ms **per shell prompt** | low |
| 9 | memoise `blacklisted()`/points tables on raw config string; hoist `abbrev` in `feed_rows` | +18 | −1-3 ms per feed render | low |
| 10 | unify triplicated renderers into paint: timetable grid, feed post, marks rows, open_url, rel-time ladder, hhmm | −114 net | one renderer per thing; cli/tui stop diverging | med (grid), low (rest) |
| 11 | small tidies: `marks_rows(consume_new)` root-fix, bleed/avg_cache/static-helpers/chop | −40 net | deletes the tui watermark workaround | low |
| 12 | Makefile: split `CXXFLAGS ?=` so env doesn't drop `-O2 -std=c++20`; add `-Wshadow -Wconversion -Wswitch-enum -Wduplicated-* -Wlogical-op -Wnull-dereference` (costs 2 shadow fixes); `check-asan` target (clang — system gcc lacks `sanitize` USE) | +8 | build can't silently go -O0; warning net widened at zero noise | none |
| 13 | commit classifier corpus (scrubbed if needed) — 406-case suite currently gitignored, exists on one laptop | data only | regression net for 459 lines of heuristics | none |
| 14 | drop legacy headerless-config path (verify live config has `[general]` first) | −12 | — | low |
| 15 | optional feature cuts, owner's call: `[bind]` aliases (−50, shell alias covers it), bad-filter vocabulary listing (−20, nice UX), `db_stamp` stat fallback (−14) | −84 | — | behaviour |

Rejected: curl_global_cleanup per tick (0 kB), MADV_PAGEOUT (cosmetic RSS, refault cost),
custom C scheduler (cron exists), LTO (nothing to win), shared bearer-GET helper
(abstraction > duplication), refactoring classify.cpp (406/406 corpus, every line load-bearing).

Net if all behaviour-preserving items land: ~**−100 LoC**, idle RAM → 0, all known bugs closed.

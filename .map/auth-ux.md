# auth ux (teams device-code)

Refresh token lifetime is not queryable; it dies from ~90d inactivity (cron's
constant refreshing prevents that) or unpredictable revocation (password change,
admin, CA policy). No advance warning possible; detection = refresh returns
invalid_grant on a fetch cycle.

On session death:
- maild: urgent notify "Teams session expired" + resign command; source marked
  failed/stale everywhere until re-auth
- `mailc auth teams`: runs device-code flow, prints link + user code + QR code
  rendered in terminal (qr the verification message)
- frontend launched while expired: prompt "Teams expired, resign Y/n?" inline,
  then same link/code/QR flow
- persist timestamp of last successful refresh; `mailc auth` shows
  "teams: session healthy, last refreshed Xm ago"

Implemented in src/sources/teams_auth.cpp: device-code start/poll + refresh, client_id
1fec8e78-bce4-4aaf-ab1b-5451cc387264 (first-party Teams), authority /organizations,
scope graph/.default + offline_access. `mailc auth teams` renders the QR via qrencode
if present (not a build dep). invalid_grant -> teams::SessionExpired.

Notify + prompt wiring: maild notifies once per outage (see config.md); `mailc new` prints
"Bakaláři unsigned" / "Bakaláři and Teams unsigned" in red and never prompts (it is the
prompt hook); every other command runs ensure_signed() first, which asks
"X is unsigned. Sign in? [Y/n]" and runs the flow, and only prints the red line when stdin
or stdout is not a tty.

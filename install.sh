#!/bin/sh
# install script: deps, build, install, optional crontab entry
set -eu

R='\033[31m'; G='\033[32m'; Y='\033[33m'; B='\033[1m'; N='\033[0m'
[ -t 1 ] || { R=; G=; Y=; B=; N=; }
say() { printf "${B}::${N} %s\n" "$1"; }
die() { printf "${R}!!${N} %s\n" "$1" >&2; exit 1; }

NAME=${NAME:-mail}
PREFIX=${PREFIX:-$HOME/.local}
MAKE=$(command -v gmake || command -v make) || die "no make"

say "checking dependencies"
missing=
CXX=${CXX:-c++}
for c in "$MAKE" "$CXX" pkg-config; do command -v "$c" >/dev/null 2>&1 || missing="$missing $c"; done
for p in libcurl sqlite3 simdjson; do pkg-config --exists "$p" 2>/dev/null || missing="$missing $p"; done
[ -z "$missing" ] || { printf "${R}Missing:${N}%s\n" "$missing"; exit 1; }
printf "${G}ok${N} all present\n"

jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
say "building"
"$MAKE" -j"$jobs" DEV=0 NAME="$NAME"
say "installing to $PREFIX/bin"
"$MAKE" install DEV=0 NAME="$NAME" PREFIX="$PREFIX"
printf "${G}ok${N} %sd %sc %st installed\n" "$NAME" "$NAME" "$NAME"

cron() {
    command -v crontab >/dev/null 2>&1 || return 0
    printf "${Y}?${N} add a crontab entry for %sd? [y/N] " "$NAME"; read -r a
    case $a in [yY]*) ;; *) return 0;; esac
    printf "${Y}?${N} run every how many minutes? [15] "; read -r m
    [ -n "${m:-}" ] || m=15
    case $m in *[!0-9]*|'') die "not a number: $m";; esac
    old=$(crontab -l 2>/dev/null | grep -v "/${NAME}d\$" || true)
    line="*/$m * * * * $PREFIX/bin/${NAME}d"
    # cron's default PATH is /usr/bin:/bin: the notify hook (wispctl) in ~/.local/bin would
    # never be found, and a silent notifier is the one failure this project cannot have
    path="PATH=$PREFIX/bin:$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin"
    case $old in *PATH=*) path=;; esac
    { echo "$old" | grep -v '^$' || true; [ -z "$path" ] || echo "$path"; echo "$line"; } | crontab -
    [ -z "$path" ] || printf "${G}ok${N} %s\n" "$path"
    printf "${G}ok${N} %s\n" "$line"
}
cron

cat <<EOF

next:
  ${NAME}c auth bakalari    # url, username, password
  ${NAME}c auth teams       # device-code sign-in in a browser
  ${NAME}d                  # first fetch; then ${NAME}c for the feed

config is written on first run: ${XDG_CONFIG_HOME:-$HOME/.config}/$NAME/config
EOF

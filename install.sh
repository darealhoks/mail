#!/bin/sh
# install script: deps, build, install, optional crontab entry
set -eu

R='\033[31m'; G='\033[32m'; Y='\033[33m'; B='\033[1m'; N='\033[0m'
[ -t 1 ] || { R=; G=; Y=; B=; N=; }
say() { printf "${B}::${N} %s\n" "$1"; }
die() { printf "${R}!!${N} %s\n" "$1" >&2; exit 1; }

NAME=${NAME:-mail}
PREFIX=${PREFIX:-$HOME/.local}
REPO=${REPO:-https://github.com/darealhoks/mail.git}
MAKE=$(command -v gmake || command -v make) || die "no make"

# piped from curl: stdin is the script itself, prompts must come from the terminal
if [ ! -t 0 ] && (exec 0</dev/tty) 2>/dev/null; then exec 0</dev/tty; fi
ask() { [ -t 0 ] || return 1; printf "${Y}?${N} %s" "$1"; read -r REPLY; }

if [ ! -f Makefile ] || [ ! -d src ]; then
    command -v git >/dev/null 2>&1 || die "no git, and not in a checkout"
    src=$(mktemp -d) || die "mktemp failed"
    trap 'rm -rf "$src"' EXIT INT TERM
    say "cloning $REPO"
    git clone --depth 1 "$REPO" "$src/mail" >/dev/null 2>&1 || die "clone failed: $REPO"
    cd "$src/mail"
fi

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
    ask "add a crontab entry for ${NAME}d? [y/N] " || return 0
    case $REPLY in [yY]*) ;; *) return 0;; esac
    ask "run every how many minutes? [15] " || REPLY=15
    m=${REPLY:-15}
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
  ${NAME}t                  # the same store full-screen; ? lists every key

config is written on first run: ${XDG_CONFIG_HOME:-$HOME/.config}/$NAME/config
EOF

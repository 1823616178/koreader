#!/bin/sh
# Dev helpers for the readingquota plugin, run inside WSL.
#   ./dev.sh check   -- syntax-check every Lua file with KOReader's own luajit
#   ./dev.sh test    -- quotadata unit tests (KOReader luajit, else any lua)
#   ./dev.sh link    -- symlink the plugin into the emulator's user plugin dir
#   ./dev.sh smoke   -- render every page state to preview-*.png without clicking
#   ./dev.sh run     -- launch the KOReader emulator
#   ./dev.sh log     -- tail the crash log

KO_DIR=/usr/lib/koreader
USER_DIR="$HOME/.config/koreader"
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$ROOT_DIR/readingquota.koplugin"

# Kindle Paperwhite geometry, so the emulator lays out like the target device.
EMULATE_READER_W=${EMULATE_READER_W:-1072}
EMULATE_READER_H=${EMULATE_READER_H:-1448}
export EMULATE_READER_W EMULATE_READER_H
export DISPLAY="${DISPLAY:-:0}"

case "$1" in
check)
    rc=0
    for f in "$SRC_DIR"/*.lua; do
        if "$KO_DIR/luajit" -e "assert(loadfile('$f'))"; then
            echo "ok   $(basename "$f")"
        else
            echo "FAIL $(basename "$f")"
            rc=1
        fi
    done
    exit $rc
    ;;
test)
    LUA_BIN=""
    if [ -x "$KO_DIR/luajit" ]; then
        LUA_BIN="$KO_DIR/luajit"
    elif command -v luajit >/dev/null 2>&1; then
        LUA_BIN="luajit"
    elif command -v lua >/dev/null 2>&1; then
        LUA_BIN="lua"
    else
        echo "dev.sh test: no luajit/lua found (CI builds stock Lua)" >&2
        exit 1
    fi
    "$LUA_BIN" "$ROOT_DIR/tests/test_quotadata.lua" "$SRC_DIR"
    ;;
link)
    mkdir -p "$USER_DIR/plugins"
    ln -sfn "$SRC_DIR" "$USER_DIR/plugins/readingquota.koplugin"
    ls -l "$USER_DIR/plugins"
    ;;
smoke)
    cd "$KO_DIR" || exit 1
    KO_MULTIUSER=1 ./luajit "$ROOT_DIR/smoke.lua" "$SRC_DIR" "$ROOT_DIR" >/dev/null
    ;;
run)
    # Extra args are passed straight through, e.g. `./dev.sh run -d` for debug
    # logging, or `./dev.sh run /path/to/book.epub`.
    shift
    cd "$KO_DIR" || exit 1
    # The .deb build logs to the terminal rather than crash.log, so keep a copy.
    ./koreader.sh "$@" 2>&1 | tee "$ROOT_DIR/koreader.log"
    ;;
log)
    tail -n 60 "$ROOT_DIR/koreader.log"
    ;;
*)
    echo "usage: $0 {check|test|link|smoke|run|log}" >&2
    exit 2
    ;;
esac

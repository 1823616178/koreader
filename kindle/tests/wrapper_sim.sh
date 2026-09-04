#!/bin/bash
# Launcher (bin/aiquota.sh) simulation on WSL: stubs every on-device command and
# path so the freeze/restore sequence and its traps can be exercised off-Kindle.
# Covers the guarantee the whole design rests on -- however the app goes away,
# restore_framework runs exactly once.
set -u
T=/tmp/wrtest
SRC=$(CDPATH= cd -- "$(dirname -- "$0")/../extension/bin" && pwd)
rm -rf "$T"; mkdir -p "$T/bin"

cp "$SRC/aiquota.sh" "$SRC/restore.sh" "$T/bin/"
# wake_lock/wake_unlock become appends (not the device's truncating writes) so
# the sandbox file line count records exactly how often each was grabbed/
# released; a plain ">" would hide a double-grab behind the truncation.
sed -i "s#^BIN=.*#BIN=$T/bin#; s#/var/tmp/aiquota-fb.dump#$T/fb.dump#g; s#/var/tmp/aiquota.ping#$T/ping#g; s#/dev/fb0#$T/fb0#g; s#> /sys/power/wake_lock#>> $T/wake_lock#" "$T/bin/aiquota.sh"
sed -i "s#^AIQUOTA_FB_DUMP=.*#AIQUOTA_FB_DUMP=$T/fb.dump#; s#/dev/fb0#$T/fb0#; s#> /sys/power/wake_unlock#>> $T/wake_unlock#" "$T/bin/restore.sh"
echo LIVE-SCREEN > "$T/fb0"

for c in killall lipc-set-prop usleep; do
    printf '#!/bin/sh\necho "%s $*" >> %s/log\nexit 0\n' "$c" "$T" > "$T/bin/$c"
done
# A really-stopped process, so restore.sh sees a genuine T state in /proc.
sleep 300 & cvm=$!
kill -STOP $cvm
printf '#!/bin/sh\n[ "$1" = cvm ] && echo %s\nexit 0\n' "$cvm" > "$T/bin/pidof"
# The watchdog has its own suite (watchdog_sim.sh); here it only needs to be
# observably launched.
printf '#!/bin/sh\necho "watchdog started $1" >> %s/log\n' "$T" > "$T/bin/watchdog.sh"
chmod +x "$T"/bin/killall "$T"/bin/lipc-set-prop "$T"/bin/usleep "$T"/bin/pidof "$T"/bin/watchdog.sh
export PATH="$T/bin:$PATH"

pass=0; fail=0
ok()  { if [ "$1" = "$2" ]; then echo "PASS $3"; pass=$((pass+1)); else echo "FAIL $3 (want [$1] got [$2])"; fail=$((fail+1)); fi; }
cnt() { grep -c "$1" "$T/log" 2>/dev/null; }
wlines() { if [ -f "$T/$1" ]; then wc -l < "$T/$1"; else echo 0; fi; }

# $1 = what the fake app binary should do. The app overwrites the framebuffer,
# so "the saved screen came back" is a real assertion and not just "nothing
# ever touched it".
setup_app() {
    : > "$T/log"
    echo LIVE-SCREEN > "$T/fb0"
    printf '#!/bin/sh\necho "app started" >> %s/log\necho APP-SCREEN > %s/fb0\n%s\n' \
        "$T" "$T" "$1" > "$T/bin/aiquota"
    chmod +x "$T/bin/aiquota"
}
restores() { cnt 'killall -CONT awesome cvm'; }
repainted() { grep -q LIVE-SCREEN "$T/fb0" && echo 1 || echo 0; }

# --- Test A: clean exit
setup_app 'exit 0'
sh "$T/bin/aiquota.sh"
sleep 1 # the watchdog is started detached; let its first line land
ok 1 "$(cnt 'app started')" "A: app launched"
ok 1 "$(cnt 'watchdog started')" "A: watchdog launched"
ok 1 "$(cnt 'killall -STOP awesome cvm')" "A: framework frozen"
ok 1 "$(restores)" "A: framework restored exactly once"
ok 1 "$(repainted)" "A: saved screen repainted"
ok 1 "$(wlines wake_lock)" "A: wake_lock grabbed exactly once"
ok aiquota "$(cat "$T/wake_lock" 2>/dev/null)" "A: wake_lock held by aiquota"
ok 1 "$(wlines wake_unlock)" "A: wake_unlock released exactly once"

# --- Test B: app crashes (the SIGSEGV path, seen from the wrapper)
setup_app 'kill -SEGV $$'
sh "$T/bin/aiquota.sh" 2>/dev/null
ok 1 "$(restores)" "B: crash still restores"

# --- Test C: app exits nonzero
setup_app 'exit 3'
sh "$T/bin/aiquota.sh"
ok 1 "$(restores)" "C: nonzero exit still restores"

# --- Test D: SIGTERM to the wrapper is ignored (KUAL's cascading TERM), and the
# restore happens once the app itself finishes -- not before.
setup_app 'sleep 4'
sh "$T/bin/aiquota.sh" & w=$!
sleep 2
kill -TERM $w 2>/dev/null
sleep 1
ok 0 "$(restores)" "D: SIGTERM ignored, framework still frozen"
ok 0 "$(repainted)" "D: app screen still up while frozen"
ok 1 "$(kill -0 $w 2>/dev/null && echo 1 || echo 0)" "D: wrapper survived SIGTERM"
wait $w 2>/dev/null
ok 1 "$(restores)" "D: restored when the app finished on its own"

# --- Test E: SIGHUP is trapped rather than ignored, so a session teardown can
# never strand the freeze. Note the shell defers a trap until the foreground app
# returns, and the EXIT trap then restores a second time -- which is exactly why
# restore_framework has to be idempotent (the repaint must still happen once).
setup_app 'sleep 3'
sh "$T/bin/aiquota.sh" & w=$!
sleep 1
kill -HUP $w 2>/dev/null
wait $w 2>/dev/null
ok 1 "$([ "$(restores)" -ge 1 ] && echo 1 || echo 0)" "E: SIGHUP path restores"
ok 1 "$(repainted)" "E: repeated restores still leave the saved screen up"

# --- Test F: a missing restore.sh aborts BEFORE anything is frozen
mv "$T/bin/restore.sh" "$T/bin/restore.sh.off"
setup_app 'exit 0'
sh "$T/bin/aiquota.sh" 2>/dev/null
rc=$?
mv "$T/bin/restore.sh.off" "$T/bin/restore.sh"
ok 1 "$([ $rc -ne 0 ] && echo 1 || echo 0)" "F: aborts when restore.sh is unavailable"
ok 0 "$(cnt 'killall -STOP')" "F: nothing was frozen"
ok 0 "$(cnt 'app started')" "F: app never launched"

kill -9 $cvm 2>/dev/null; wait $cvm 2>/dev/null
rm -rf "$T"
echo "RESULT: $pass passed, $fail failed"
[ $fail -eq 0 ]

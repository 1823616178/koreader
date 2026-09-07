#!/bin/bash
# Watchdog logic simulation on WSL: stubs lipc/killall/pidof, fakes wrapper + cvm.
# Session-scoped ping/fb/active paths are forced into the sandbox via AIQUOTA_TMP.
set -u
T=/tmp/wdtest
SRC=$(CDPATH= cd -- "$(dirname -- "$0")/../extension/bin" && pwd)
WD="$T/bin/watchdog.sh"
rm -rf "$T"; mkdir -p "$T/bin" "$T/tmp"

cp "$SRC/watchdog.sh" "$SRC/restore.sh" "$T/bin/"
sed -i "s#^BIN=.*#BIN=$T/bin#" "$WD"
sed -i "s#/dev/fb0#$T/fb0#g; s#> /sys/power/wake_unlock#>> $T/wake_unlock#" \
    "$T/bin/restore.sh"

export AIQUOTA_TMP="$T/tmp"
export AIQUOTA_LOCK="$T/tmp/aiquota.lock"
export AIQUOTA_ACTIVE="$T/tmp/aiquota.active"

# killall -9 aiquota kills the fake app AND the fake wrapper, mirroring the real
# wrapper exiting as soon as its child dies -- unless $T/unkillable exists, which
# models a task in uninterruptible sleep that SIGKILL cannot touch.
cat > "$T/bin/killall" <<'EOFK'
#!/bin/sh
echo "killall $*" >> /tmp/wdtest/log
if [ "$1" = "-9" ] && [ "$2" = "aiquota" ] && [ ! -f /tmp/wdtest/unkillable ]; then
    rm -f /tmp/wdtest/app.pid
    [ -f /tmp/wdtest/wrapper.pid ] && kill -9 "$(cat /tmp/wdtest/wrapper.pid)" 2>/dev/null
fi
exit 0
EOFK
cat > "$T/bin/lipc-set-prop" <<'EOFK'
#!/bin/sh
echo "lipc $*" >> /tmp/wdtest/log
EOFK
cat > "$T/bin/pidof" <<'EOFK'
#!/bin/sh
case "$1" in
  cvm)     [ -f /tmp/wdtest/cvm.pid ] && cat /tmp/wdtest/cvm.pid ;;
  aiquota) [ -f /tmp/wdtest/app.pid ] && cat /tmp/wdtest/app.pid ;;
  *) exit 1 ;;
esac
EOFK
chmod +x "$T/bin/killall" "$T/bin/lipc-set-prop" "$T/bin/pidof"
export PATH="$T/bin:$PATH"

: > "$T/log"
pass=0; fail=0
ok()  { if [ "$1" = "$2" ]; then echo "PASS $3"; pass=$((pass+1)); else echo "FAIL $3 (want [$1] got [$2])"; fail=$((fail+1)); fi; }
cnt() { grep -c "$1" "$T/log" 2>/dev/null; } # grep -c prints 0 on no match
alive() { kill -0 "$1" 2>/dev/null && echo 1 || echo 0; }
repainted() { grep -q SAVED-SCREEN "$T/fb0" && echo 1 || echo 0; }

# A really-stopped process, so restore.sh's /proc state check (which gates the
# framebuffer repaint) runs against a genuine T state rather than a stub.
start_stopped_cvm() {
    sleep 300 & cvm=$!
    kill -STOP $cvm
    echo $cvm > "$T/cvm.pid"
}
stop_cvm() { rm -f "$T/cvm.pid"; kill -9 "$cvm" 2>/dev/null; wait "$cvm" 2>/dev/null; }

# Begin a session owned by wrapper pid $1: active marker, fb dump, stale ping.
begin_session() {
    sid=$1
    mkdir -p "$T/tmp"
    echo "$sid" > "$AIQUOTA_ACTIVE"
    mkdir -p "$AIQUOTA_LOCK"
    echo "$sid" > "$AIQUOTA_LOCK/pid"
    echo SAVED-SCREEN > "$T/tmp/aiquota.${sid}.fb.dump"
    echo . > "$T/tmp/aiquota.${sid}.ping"
    touch -d '200 seconds ago' "$T/tmp/aiquota.${sid}.ping"
}

# --- Test A: stale heartbeat -> kill -9 lands, wrapper's own trap does the rest
echo 4242 > "$T/app.pid"
sleep 60 & wrapperA=$!
echo $wrapperA > "$T/wrapper.pid"
begin_session "$wrapperA"
sh "$WD" "$wrapperA" "$wrapperA" & wdA=$!
sleep 1
ok 1 "$([ -f "$T/tmp/aiquota.${wrapperA}.wdready" ] && echo 1 || echo 0)" "A: watchdog ready marker"
sleep 35 # two consecutive stale polls need ~2*POLL_S
ok 1 "$(cnt 'killall -9 aiquota')" "A: wedged app killed exactly once"
ok 0 "$(alive $wrapperA)" "A: the kill brought the wrapper down with it"
ok 0 "$(cnt 'killall -CONT')" "A: watchdog leaves the restore to the wrapper trap"
ok 0 "$(alive $wdA)" "A: watchdog exits after the wrapper"
ok 0 "$([ -f "$T/tmp/aiquota.${wrapperA}.ping" ] && echo 1 || echo 0)" "A: session ping cleaned"
wait $wrapperA 2>/dev/null

# --- Test B: wrapper SIGKILLed while cvm STOPped -> fallback restore
start_stopped_cvm
: > "$T/log"; : > "$T/fb0"
sleep 60 & wrapperB=$!
begin_session "$wrapperB"
echo SAVED-SCREEN > "$T/tmp/aiquota.${wrapperB}.fb.dump"
sh "$WD" "$wrapperB" "$wrapperB" & wdB=$!
sleep 2
kill -9 $wrapperB; wait $wrapperB 2>/dev/null
sleep 25 # death is noticed on the next poll, up to POLL_S later
ok 1 "$(cnt 'killall -CONT awesome cvm')" "B: fallback restore CONT"
ok 2 "$(cnt '^lipc ')" "B: fallback restore lipc x2"
ok 1 "$(repainted)" "B: saved screen repainted (cvm really was stopped)"
ok 0 "$(alive $wdB)" "B: watchdog exits after restore"
ok 0 "$([ -f "$AIQUOTA_ACTIVE" ] && echo 1 || echo 0)" "B: active session cleared"
stop_cvm

# --- Test C: normal exit (cvm running) -> watchdog does nothing
: > "$T/log"; : > "$T/fb0"
sleep 60 & wrapperC=$!
begin_session "$wrapperC"
# Simulate wrapper EXIT trap: restore already done, active cleared.
rm -f "$AIQUOTA_ACTIVE"
sh "$WD" "$wrapperC" "$wrapperC" & wdC=$!
sleep 2
kill $wrapperC; wait $wrapperC 2>/dev/null
sleep 25 # same detection window as B before asserting nothing happened
ok 0 "$(cnt 'killall')" "C: no restore on normal exit"
ok 0 "$(cnt '^lipc ')" "C: no lipc on normal exit"
ok 0 "$(repainted)" "C: no stale screen flashed over the live framework"

# --- Test D: kill -9 does not take (D state) -> escalate, restore, exit
# The wrapper stays alive throughout: waiting for its EXIT trap here would leave
# the device frozen forever, which is exactly what the escalation exists to stop.
start_stopped_cvm
touch "$T/unkillable"
echo 4242 > "$T/app.pid"
: > "$T/log"; : > "$T/fb0"
sleep 120 & wrapperD=$!
echo $wrapperD > "$T/wrapper.pid"
begin_session "$wrapperD"
echo SAVED-SCREEN > "$T/tmp/aiquota.${wrapperD}.fb.dump"
sh "$WD" "$wrapperD" "$wrapperD" & wdD=$!
sleep 65 # two kills then escalation: ~3 stale polls apart at POLL_S each
ok 2 "$(cnt 'killall -9 aiquota')" "D: two kill attempts, then escalation"
ok 1 "$(cnt 'killall -CONT awesome cvm')" "D: escalated restore while wrapper still alive"
ok 1 "$(repainted)" "D: saved screen repainted on escalation"
ok 1 "$(alive $wrapperD)" "D: wrapper untouched (still stuck on its child)"
ok 0 "$(alive $wdD)" "D: watchdog exits after escalating"
ok 0 "$([ -f "$T/tmp/aiquota.${wrapperD}.ping" ] && echo 1 || echo 0)" "D: session ping cleaned"
kill -9 $wrapperD 2>/dev/null; wait $wrapperD 2>/dev/null
stop_cvm
rm -f "$T/unkillable" "$T/app.pid" "$T/wrapper.pid"

# --- Test E: restore.sh run directly (the SSH escape hatch) is idempotent
: > "$T/log"; : > "$T/fb0"
rm -f "$AIQUOTA_ACTIVE"
sh "$T/bin/restore.sh"
ok 1 "$(cnt 'killall -CONT awesome cvm')" "E: manual restore always CONTs"
ok 2 "$(cnt '^lipc ')" "E: manual restore always resets lipc props"
ok 0 "$(repainted)" "E: nothing frozen -> no repaint"

# --- Test F: old watchdog must NOT restore a newer session's freeze
start_stopped_cvm
: > "$T/log"; : > "$T/fb0"
sleep 60 & old_wrap=$!
begin_session "$old_wrap"
echo OLD-SCREEN > "$T/tmp/aiquota.${old_wrap}.fb.dump"
sh "$WD" "$old_wrap" "$old_wrap" & old_wd=$!
sleep 1
# New session takes ownership while old watchdog is still running.
sleep 60 & new_wrap=$!
begin_session "$new_wrap"
echo NEW-SCREEN > "$T/tmp/aiquota.${new_wrap}.fb.dump"
: > "$T/tmp/aiquota.${new_wrap}.ping"  # fresh ping so new isn't stale
# Kill the OLD wrapper (SIGKILL = trap never ran); old watchdog will wake.
kill -9 $old_wrap; wait $old_wrap 2>/dev/null
sleep 25
ok 0 "$(cnt 'killall -CONT')" "F: old watchdog does not CONT a foreign session"
ok 0 "$(repainted)" "F: old session screen not flashed over new freeze"
ok 1 "$([ "$(cat "$AIQUOTA_ACTIVE" 2>/dev/null)" = "$new_wrap" ] && echo 1 || echo 0)" \
    "F: active session still the new one"
ok 0 "$([ -f "$T/tmp/aiquota.${old_wrap}.ping" ] && echo 1 || echo 0)" \
    "F: old session ping cleaned"
ok 1 "$([ -f "$T/tmp/aiquota.${new_wrap}.ping" ] && echo 1 || echo 0)" \
    "F: new session ping untouched"
ok 0 "$(alive $old_wd)" "F: old watchdog exited"
kill -9 $new_wrap 2>/dev/null; wait $new_wrap 2>/dev/null
stop_cvm

# --- Test G: manual restore ends the active session and kills the app
: > "$T/log"; : > "$T/fb0"
echo 9999 > "$T/app.pid"
sleep 60 & wrapG=$!
begin_session "$wrapG"
echo SAVED-SCREEN > "$T/tmp/aiquota.${wrapG}.fb.dump"
start_stopped_cvm
# Point dump for restore via active sid
sh "$T/bin/restore.sh"
ok 1 "$(cnt 'killall -9 aiquota')" "G: manual restore kills app"
ok 1 "$(cnt 'killall -CONT awesome cvm')" "G: manual restore CONTs"
ok 0 "$([ -f "$AIQUOTA_ACTIVE" ] && echo 1 || echo 0)" "G: active cleared"
ok 0 "$([ -d "$AIQUOTA_LOCK" ] && echo 1 || echo 0)" "G: lock released"
ok 0 "$([ -f "$T/tmp/aiquota.${wrapG}.ping" ] && echo 1 || echo 0)" "G: session files cleaned"
kill -9 $wrapG 2>/dev/null; wait $wrapG 2>/dev/null
stop_cvm

rm -rf "$T"
echo "RESULT: $pass passed, $fail failed"
[ $fail -eq 0 ]

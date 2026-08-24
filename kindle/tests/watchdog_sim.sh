#!/bin/bash
# Watchdog logic simulation on WSL: stubs lipc/killall/pidof, fakes wrapper + cvm.
# The scripts hard-code their on-device paths, so they are copied into a sandbox
# and the four absolute paths are rewritten to point inside it.
set -u
T=/tmp/wdtest
SRC=/mnt/g/code/koread_ext/kindle/extension/bin
WD="$T/bin/watchdog.sh"
rm -rf "$T"; mkdir -p "$T/bin"

cp "$SRC/watchdog.sh" "$SRC/restore.sh" "$T/bin/"
sed -i "s#^BIN=.*#BIN=$T/bin#" "$WD"
sed -i "s#^AIQUOTA_FB_DUMP=.*#AIQUOTA_FB_DUMP=$T/fb.dump#; s#/dev/fb0#$T/fb0#" \
    "$T/bin/restore.sh"
echo SAVED-SCREEN > "$T/fb.dump"
: > "$T/fb0"

# killall -9 aiquota kills the fake app AND the fake wrapper, mirroring the real
# wrapper exiting as soon as its child dies -- unless $T/unkillable exists, which
# models a task in uninterruptible sleep that SIGKILL cannot touch.
cat > "$T/bin/killall" <<'EOF'
#!/bin/sh
echo "killall $*" >> /tmp/wdtest/log
if [ "$1" = "-9" ] && [ "$2" = "aiquota" ] && [ ! -f /tmp/wdtest/unkillable ]; then
    rm -f /tmp/wdtest/app.pid
    [ -f /tmp/wdtest/wrapper.pid ] && kill -9 "$(cat /tmp/wdtest/wrapper.pid)" 2>/dev/null
fi
exit 0
EOF
cat > "$T/bin/lipc-set-prop" <<'EOF'
#!/bin/sh
echo "lipc $*" >> /tmp/wdtest/log
EOF
cat > "$T/bin/pidof" <<'EOF'
#!/bin/sh
case "$1" in
  cvm)     [ -f /tmp/wdtest/cvm.pid ] && cat /tmp/wdtest/cvm.pid ;;
  aiquota) [ -f /tmp/wdtest/app.pid ] && cat /tmp/wdtest/app.pid ;;
  *) exit 1 ;;
esac
EOF
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

stale_ping() { echo . > /var/tmp/aiquota.ping; touch -d '200 seconds ago' /var/tmp/aiquota.ping; }

# --- Test A: stale heartbeat -> kill -9 lands, wrapper's own trap does the rest
echo 4242 > "$T/app.pid"
stale_ping
sleep 60 & wrapperA=$!
echo $wrapperA > "$T/wrapper.pid"
sh "$WD" $wrapperA & wdA=$!
sleep 35 # two consecutive stale polls need ~2*POLL_S
ok 1 "$(cnt 'killall -9 aiquota')" "A: wedged app killed exactly once"
ok 0 "$(alive $wrapperA)" "A: the kill brought the wrapper down with it"
ok 0 "$(cnt 'killall -CONT')" "A: watchdog leaves the restore to the wrapper trap"
ok 0 "$(alive $wdA)" "A: watchdog exits after the wrapper"
ok 0 "$([ -f /var/tmp/aiquota.ping ] && echo 1 || echo 0)" "A: ping file cleaned"
wait $wrapperA 2>/dev/null

# --- Test B: wrapper SIGKILLed while cvm STOPped -> fallback restore
start_stopped_cvm
: > "$T/log"; : > "$T/fb0"
sleep 60 & wrapperB=$!
sh "$WD" $wrapperB & wdB=$!
sleep 2
kill -9 $wrapperB; wait $wrapperB 2>/dev/null
sleep 25 # death is noticed on the next poll, up to POLL_S later
ok 1 "$(cnt 'killall -CONT awesome cvm')" "B: fallback restore CONT"
ok 2 "$(cnt '^lipc ')" "B: fallback restore lipc x2"
ok 1 "$(repainted)" "B: saved screen repainted (cvm really was stopped)"
ok 0 "$(alive $wdB)" "B: watchdog exits after restore"
stop_cvm

# --- Test C: normal exit (cvm running) -> watchdog does nothing
: > "$T/log"; : > "$T/fb0"
sleep 60 & wrapperC=$!
sh "$WD" $wrapperC & wdC=$!
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
stale_ping
sleep 120 & wrapperD=$!
echo $wrapperD > "$T/wrapper.pid"
sh "$WD" $wrapperD & wdD=$!
sleep 65 # two kills then escalation: ~3 stale polls apart at POLL_S each
ok 2 "$(cnt 'killall -9 aiquota')" "D: two kill attempts, then escalation"
ok 1 "$(cnt 'killall -CONT awesome cvm')" "D: escalated restore while wrapper still alive"
ok 1 "$(repainted)" "D: saved screen repainted on escalation"
ok 1 "$(alive $wrapperD)" "D: wrapper untouched (still stuck on its child)"
ok 0 "$(alive $wdD)" "D: watchdog exits after escalating"
ok 0 "$([ -f /var/tmp/aiquota.ping ] && echo 1 || echo 0)" "D: ping file cleaned"
kill -9 $wrapperD 2>/dev/null; wait $wrapperD 2>/dev/null
stop_cvm
rm -f "$T/unkillable" "$T/app.pid" "$T/wrapper.pid"

# --- Test E: restore.sh run directly (the SSH escape hatch) is idempotent
: > "$T/log"; : > "$T/fb0"
sh "$T/bin/restore.sh"
ok 1 "$(cnt 'killall -CONT awesome cvm')" "E: manual restore always CONTs"
ok 2 "$(cnt '^lipc ')" "E: manual restore always resets lipc props"
ok 0 "$(repainted)" "E: nothing frozen -> no repaint"

rm -rf "$T" /var/tmp/aiquota.ping
echo "RESULT: $pass passed, $fail failed"
[ $fail -eq 0 ]

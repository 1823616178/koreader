#!/bin/bash
# Watchdog logic simulation on WSL: stubs lipc/killall, fakes wrapper + cvm.
set -u
T=/tmp/wdtest
WD=/mnt/g/code/koread_ext/kindle/extension/bin/watchdog.sh
rm -rf "$T"; mkdir -p "$T/bin"

cat > "$T/bin/killall" <<'EOF'
#!/bin/sh
echo "killall $*" >> /tmp/wdtest/log
EOF
cat > "$T/bin/lipc-set-prop" <<'EOF'
#!/bin/sh
echo "lipc $*" >> /tmp/wdtest/log
EOF
chmod +x "$T/bin/killall" "$T/bin/lipc-set-prop"
export PATH="$T/bin:$PATH"

: > "$T/log"
pass=0; fail=0
ok()  { if [ "$1" = "$2" ]; then echo "PASS $3"; pass=$((pass+1)); else echo "FAIL $3 (want [$1] got [$2])"; fail=$((fail+1)); fi; }
cnt() { grep -c "$1" "$T/log" 2>/dev/null; } # grep -c prints 0 on no match

# --- Test A: stale heartbeat -> killall -9 aiquota exactly once; wrapper alive -> no restore
sleep 60 & wrapperA=$!
echo . > /var/tmp/aiquota.ping
touch -d '200 seconds ago' /var/tmp/aiquota.ping
sh "$WD" $wrapperA & wdA=$!
sleep 13
ok 1 "$(cnt 'killall -9 aiquota')" "A: wedged app killed exactly once"
ok 1 "$(kill -0 $wdA 2>/dev/null && echo 1 || echo 0)" "A: watchdog alive while wrapper alive"
ok 0 "$(cnt 'killall -CONT')" "A: no restore while wrapper alive"
kill -9 $wrapperA; wait $wrapperA 2>/dev/null
sleep 9
ok 0 "$(kill -0 $wdA 2>/dev/null && echo 1 || echo 0)" "A: watchdog exits after wrapper death"
ok 0 "$(cnt 'killall -CONT')" "A: no false restore (no stopped cvm)"
ok 0 "$([ -f /var/tmp/aiquota.ping ] && echo 1 || echo 0)" "A: ping file cleaned"

# --- Test B: wrapper SIGKILLed while cvm STOPped -> fallback restore
cp /bin/sleep "$T/bin/cvm"; chmod +x "$T/bin/cvm"
"$T/bin/cvm" 300 & cvmB=$!
kill -STOP $cvmB
: > "$T/log"
sleep 60 & wrapperB=$!
sh "$WD" $wrapperB & wdB=$!
sleep 2
kill -9 $wrapperB; wait $wrapperB 2>/dev/null
sleep 9
ok 1 "$(cnt 'killall -CONT awesome cvm')" "B: fallback restore CONT"
ok 2 "$(cnt '^lipc ')" "B: fallback restore lipc x2"
ok 0 "$(kill -0 $wdB 2>/dev/null && echo 1 || echo 0)" "B: watchdog exits after restore"

# --- Test C: normal exit (cvm running) -> watchdog does nothing
kill -CONT $cvmB
: > "$T/log"
sleep 60 & wrapperC=$!
sh "$WD" $wrapperC & wdC=$!
sleep 2
kill $wrapperC; wait $wrapperC 2>/dev/null
sleep 9
ok 0 "$(cnt 'killall')" "C: no restore on normal exit"
ok 0 "$(cnt '^lipc ')" "C: no lipc on normal exit"

kill -9 $cvmB 2>/dev/null; wait $cvmB 2>/dev/null
rm -rf "$T" /var/tmp/aiquota.ping
echo "RESULT: $pass passed, $fail failed"
[ $fail -eq 0 ]

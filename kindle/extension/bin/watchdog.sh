#!/bin/sh
# aiquota watchdog: last-resort framework restore, started by aiquota.sh with
# the wrapper's PID as $1. Runs trap-immune (and via setsid when available) so
# it survives the wrapper being killed (e.g. SIGKILL from the OOM killer).
#
# Two duties:
#   1. While the wrapper lives, watch the app's heartbeat file; a stale beat
#      means the app is wedged -> kill it (the wrapper's EXIT trap restores).
#   2. When the wrapper dies, check whether cvm is still STOPped (state T in
#      /proc): if so, the wrapper died without its trap running, so restore
#      the framework here. A normal exit leaves cvm running and we do nothing
#      (idempotent, no stale-framebuffer flash over the live framework UI).

trap "" TERM INT HUP

wrapper=$1
ping=/var/tmp/aiquota.ping
dump=/var/tmp/aiquota-fb.dump
stale=0

# Worst legitimate blocking call is the ~95 s HTTP fetch; the app beats every
# 30 s, so 180 s of silence on two consecutive checks means wedged.
while kill -0 "$wrapper" 2>/dev/null; do
    if [ -f "$ping" ]; then
        now=$(date +%s)
        beat=$(stat -c %Y "$ping" 2>/dev/null || echo "$now")
        if [ $((now - beat)) -gt 180 ]; then
            if [ "$stale" -ge 1 ]; then
                killall -9 aiquota 2>/dev/null
                rm -f "$ping" # dead apps have no heartbeat; don't re-fire
            fi
            stale=$((stale + 1))
        else
            stale=0
        fi
    fi
    sleep 5
done

sleep 2 # let the wrapper's own EXIT trap finish a normal restore first
cvm_pid=$(pidof cvm)
if [ -n "$cvm_pid" ] && [ "$(awk '{print $3}' /proc/"$cvm_pid"/stat 2>/dev/null)" = "T" ]; then
    killall -CONT awesome cvm 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null
    [ -r "$dump" ] && [ -w /dev/fb0 ] && cat "$dump" > /dev/fb0
fi

rm -f "$ping"

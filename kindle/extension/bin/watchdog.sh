#!/bin/sh
# aiquota watchdog: last-resort framework restore, started by aiquota.sh with
# the wrapper's PID as $1. Runs trap-immune (and via setsid when available) so
# it survives the wrapper being killed, and inherits the wrapper's OOM
# immunity (oom_score_adj -1000), re-asserted below for a manual start.
#
# Three duties, escalating:
#   1. While the wrapper lives, watch the app's heartbeat file. The beat only
#      ticks while the app's main thread is inside a window it announced
#      (src/watchdog.h), so a stale beat means wedged -> kill it, and the
#      wrapper's EXIT trap restores the framework.
#   2. If SIGKILL does not take -- an uninterruptible (D state) task cannot be
#      killed at all -- stop waiting for the wrapper and restore the framework
#      ourselves. The user gets the device back immediately; the framework
#      repaints over the stuck app, which is left for the next reboot.
#   3. When the wrapper dies, check whether cvm is still STOPped (state T in
#      /proc): if so, the wrapper died without its trap running, so restore
#      here. A normal exit leaves cvm running and we do nothing.

trap "" TERM INT HUP

BIN=/mnt/us/extensions/aiquota/bin
AIQUOTA_RESTORE_LIB=1
. "$BIN/restore.sh" || exit 1

printf '%s\n' -1000 > /proc/self/oom_score_adj 2>/dev/null ||
    printf '%s\n' -17 > /proc/self/oom_adj 2>/dev/null

wrapper=$1
ping=/var/tmp/aiquota.ping

# The app beats every 30 s and every blocking phase it enters is bounded by a
# deadline of its own, so 120 s of silence is already four missed beats. The
# 15 s poll keeps fork churn low (date/stat per cycle) while still confirming
# a wedge twice before acting.
STALE_S=120
POLL_S=15
MAX_KILLS=2 # then escalate: SIGKILL is not going to work on this process

stale=0  # consecutive stale-heartbeat observations
kills=0  # kill -9 attempts made

while kill -0 "$wrapper" 2>/dev/null; do
    if [ -f "$ping" ]; then
        now=$(date +%s)
        beat=$(stat -c %Y "$ping" 2>/dev/null || echo "$now")
        if [ $((now - beat)) -gt "$STALE_S" ]; then
            stale=$((stale + 1))
            # Two consecutive stale reads before acting: one slow beat is not
            # a wedge. The ping file is deliberately NOT removed after a kill,
            # so a process that survived it stays stale and reaches step 2.
            if [ "$stale" -ge 2 ]; then
                if [ "$kills" -lt "$MAX_KILLS" ]; then
                    killall -9 aiquota 2>/dev/null
                    kills=$((kills + 1))
                elif pidof aiquota >/dev/null 2>&1; then
                    restore_framework
                    rm -f "$ping"
                    exit 0
                fi
            fi
        else
            stale=0
        fi
    fi
    sleep "$POLL_S"
done

sleep 2 # let the wrapper's own EXIT trap finish a normal restore first
cvm_pid=$(pidof cvm 2>/dev/null | awk '{print $1}')
if [ -n "$cvm_pid" ] &&
   [ "$(awk '{print $3}' /proc/"$cvm_pid"/stat 2>/dev/null)" = "T" ]; then
    restore_framework
fi

rm -f "$ping"

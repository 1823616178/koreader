#!/bin/sh
# aiquota launcher: freezes the Kindle framework (SIGSTOP, not `stop lab126_gui`),
# runs the aiquota binary, and always restores the framework on any exit.
# Sequence follows kindle-port.zh-CN.md §6 (order is load-bearing).
#
# Sessions are isolated: heartbeat, framebuffer dump and the active-session
# marker are keyed by this wrapper's PID (SID). An old watchdog that wakes
# after a quick restart must not restore or clean a newer session's files.

BIN=/mnt/us/extensions/aiquota/bin
AIQUOTA_TMP=${AIQUOTA_TMP:-/var/tmp}
AIQUOTA_LOCK=${AIQUOTA_LOCK:-$AIQUOTA_TMP/aiquota.lock}
AIQUOTA_ACTIVE=${AIQUOTA_ACTIVE:-$AIQUOTA_TMP/aiquota.active}

SID=$$
AIQUOTA_FB_DUMP=$AIQUOTA_TMP/aiquota.${SID}.fb.dump
AIQUOTA_PING_PATH=$AIQUOTA_TMP/aiquota.${SID}.ping
AIQUOTA_WD_READY=$AIQUOTA_TMP/aiquota.${SID}.wdready
export AIQUOTA_TMP AIQUOTA_LOCK AIQUOTA_ACTIVE AIQUOTA_FB_DUMP AIQUOTA_PING_PATH
export AIQUOTA_SESSION=$SID

# restore_framework() lives in bin/restore.sh; source it BEFORE freezing so the
# function is resident in memory (traps must never depend on re-reading a file)
# and so a missing/broken script aborts while the framework is still running.
AIQUOTA_RESTORE_LIB=1
. "$BIN/restore.sh" || exit 1

# Single-instance lock (mkdir is atomic even without flock). A stale lock from
# a dead wrapper is stolen; a live holder refuses the second launch.
if ! mkdir "$AIQUOTA_LOCK" 2>/dev/null; then
    old_pid=$(cat "$AIQUOTA_LOCK/pid" 2>/dev/null || true)
    if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
        echo "aiquota: already running (pid $old_pid)" >&2
        exit 1
    fi
    rm -rf "$AIQUOTA_LOCK"
    mkdir "$AIQUOTA_LOCK" || exit 1
fi
echo "$SID" > "$AIQUOTA_LOCK/pid"

# OOM immunity. The only realistic source of SIGKILL here is the OOM killer,
# and being killed mid-freeze is precisely the failure the trap below cannot
# cover -- so opt out of it. Inherited across fork, which covers the watchdog
# too; the app deliberately opts back IN below. Old kernels only have oom_adj.
printf '%s\n' -1000 > /proc/self/oom_score_adj 2>/dev/null ||
    printf '%s\n' -17 > /proc/self/oom_adj 2>/dev/null

# Session teardown on any exit: restore framework only while we still own the
# active marker (a newer session must not be undone by our EXIT trap), then
# drop only our own files and the lock we hold.
aiquota_on_exit() {
    if [ -f "$AIQUOTA_ACTIVE" ] && [ "$(cat "$AIQUOTA_ACTIVE" 2>/dev/null)" = "$SID" ]; then
        restore_framework
        aiquota_clear_active "$SID"
    fi
    aiquota_cleanup_session "$SID"
    aiquota_release_lock "$SID"
}

trap "" TERM                            # 挡住 KUAL 退出时的级联 SIGTERM
trap aiquota_on_exit EXIT INT HUP

# Publish the session before starting the watchdog so a delayed old watchdog
# that compares against AIQUOTA_ACTIVE can see the new owner.
echo "$SID" > "$AIQUOTA_ACTIVE"
rm -f "$AIQUOTA_WD_READY"

# 独立会话的看门狗：本脚本若被 SIGKILL 导致 trap 无法执行，由它兜底恢复；
# 同时监视应用心跳，进程卡死时强杀应用（见 bin/watchdog.sh）。
if command -v setsid >/dev/null 2>&1; then
    setsid sh "$BIN/watchdog.sh" "$SID" "$SID" &
else
    sh "$BIN/watchdog.sh" "$SID" "$SID" &
fi
wd_pid=$!

# Freeze only after the watchdog is ready: otherwise a hang before the first
# poll has no safety net. Bound the wait so a broken watchdog cannot stall us.
ready=0
i=0
while [ "$i" -lt 50 ]; do
    if [ -f "$AIQUOTA_WD_READY" ]; then
        ready=1
        break
    fi
    # Watchdog died before signalling ready.
    if ! kill -0 "$wd_pid" 2>/dev/null; then
        break
    fi
    usleep 100000 2>/dev/null || sleep 1
    i=$((i + 1))
done
if [ "$ready" != 1 ]; then
    echo "aiquota: watchdog failed to start" >&2
    exit 1
fi

cat /dev/fb0 > "$AIQUOTA_FB_DUMP"       # 备份当前画面（会话私有）
lipc-set-prop com.lab126.pillow disableEnablePillow disable
lipc-set-prop -i com.lab126.powerd preventScreenSaver 1
# 内核级唤醒锁：部分固件的 powerd 会在自己的空闲计时到点后无视
# preventScreenSaver 照样挂起，醒来即锁屏。拿着 wakelock 彻底挡住
# 自动挂起；文件不存在时静默跳过（应用侧还有周期续期作第二道防线）。
# 退出路径统一经 restore_framework 释放，见 bin/restore.sh。
echo aiquota > /sys/power/wake_lock 2>/dev/null
killall -STOP awesome cvm
usleep 250000

# Seed the heartbeat instead of just clearing it: the app only starts beating
# once it has opened the framebuffer, the font and the touch device, and that
# startup window would otherwise be invisible to the watchdog. A fresh file
# here means a hang before the first beat is still caught (and stale state
# from a previous run is still discarded).
: > "$AIQUOTA_PING_PATH"

# oom_score_adj 1000 makes the app the OOM killer's first choice: under memory
# pressure it dies, the trap above fires, and the framework comes back -- an
# OOM event becomes a clean close instead of a wedged device. The value is
# inherited across exec, so the subshell sets it for the binary. Old kernels
# only have oom_adj; its ceiling 15 maps to score 1000.
(
    printf '%s\n' 1000 > /proc/self/oom_score_adj 2>/dev/null ||
        printf '%s\n' 15 > /proc/self/oom_adj 2>/dev/null
    exec "$BIN/aiquota"
)

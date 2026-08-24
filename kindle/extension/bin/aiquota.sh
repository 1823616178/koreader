#!/bin/sh
# aiquota launcher: freezes the Kindle framework (SIGSTOP, not `stop lab126_gui`),
# runs the aiquota binary, and always restores the framework on any exit.
# Sequence follows kindle-port.zh-CN.md §6 (order is load-bearing).

BIN=/mnt/us/extensions/aiquota/bin

# restore_framework() lives in bin/restore.sh; source it BEFORE freezing so the
# function is resident in memory (traps must never depend on re-reading a file)
# and so a missing/broken script aborts while the framework is still running.
AIQUOTA_RESTORE_LIB=1
. "$BIN/restore.sh" || exit 1

# OOM immunity. The only realistic source of SIGKILL here is the OOM killer,
# and being killed mid-freeze is precisely the failure the trap below cannot
# cover -- so opt out of it. Inherited across fork, which covers the watchdog
# too; the app deliberately opts back IN below. Old kernels only have oom_adj.
printf '%s\n' -1000 > /proc/self/oom_score_adj 2>/dev/null ||
    printf '%s\n' -17 > /proc/self/oom_adj 2>/dev/null

trap "" TERM                            # 挡住 KUAL 退出时的级联 SIGTERM
trap restore_framework EXIT INT HUP     # 无论怎么退出都恢复

# 独立会话的看门狗：本脚本若被 SIGKILL 导致 trap 无法执行，由它兜底恢复；
# 同时监视应用心跳，进程卡死时强杀应用（见 bin/watchdog.sh）。
if command -v setsid >/dev/null 2>&1; then
    setsid sh "$BIN/watchdog.sh" $$ &
else
    sh "$BIN/watchdog.sh" $$ &
fi

cat /dev/fb0 > /var/tmp/aiquota-fb.dump # 备份当前画面
lipc-set-prop com.lab126.pillow disableEnablePillow disable
lipc-set-prop -i com.lab126.powerd preventScreenSaver 1
killall -STOP awesome cvm
usleep 250000

# Seed the heartbeat instead of just clearing it: the app only starts beating
# once it has opened the framebuffer, the font and the touch device, and that
# startup window would otherwise be invisible to the watchdog. A fresh file
# here means a hang before the first beat is still caught (and stale state
# from a previous run is still discarded).
: > /var/tmp/aiquota.ping

# oom_score_adj 1000 makes the app the OOM killer's first choice: under memory
# pressure it dies, the trap above fires, and the framework comes back -- an
# OOM event becomes a clean close instead of a wedged device. The value is
# inherited across exec, so the subshell sets it for the binary.
(
    printf '%s\n' 1000 > /proc/self/oom_score_adj 2>/dev/null
    exec "$BIN/aiquota"
)

#!/bin/sh
# aiquota launcher: freezes the Kindle framework (SIGSTOP, not `stop lab126_gui`),
# runs the aiquota binary, and always restores the framework on any exit.
# Sequence follows kindle-port.zh-CN.md §6 (order is load-bearing).

restore_framework() {
    killall -CONT awesome cvm
    lipc-set-prop com.lab126.pillow disableEnablePillow enable
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0
    cat /var/tmp/aiquota-fb.dump > /dev/fb0
}

trap "" TERM                            # 挡住 KUAL 退出时的级联 SIGTERM
trap restore_framework EXIT INT HUP     # 无论怎么退出都恢复

# 独立会话的看门狗：本脚本若被 SIGKILL（如 OOM）导致 trap 无法执行，由它兜底恢复；
# 同时监视应用心跳，进程卡死时强杀应用（见 bin/watchdog.sh）。
if command -v setsid >/dev/null 2>&1; then
    setsid sh /mnt/us/extensions/aiquota/bin/watchdog.sh $$ &
else
    sh /mnt/us/extensions/aiquota/bin/watchdog.sh $$ &
fi

cat /dev/fb0 > /var/tmp/aiquota-fb.dump # 备份当前画面
lipc-set-prop com.lab126.pillow disableEnablePillow disable
lipc-set-prop -i com.lab126.powerd preventScreenSaver 1
killall -STOP awesome cvm
usleep 250000

rm -f /var/tmp/aiquota.ping           # 清掉上次运行残留的心跳文件
/mnt/us/extensions/aiquota/bin/aiquota

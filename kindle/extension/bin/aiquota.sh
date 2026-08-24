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

cat /dev/fb0 > /var/tmp/aiquota-fb.dump # 备份当前画面
lipc-set-prop com.lab126.pillow disableEnablePillow disable
lipc-set-prop -i com.lab126.powerd preventScreenSaver 1
killall -STOP awesome cvm
usleep 250000

/mnt/us/extensions/aiquota/bin/aiquota

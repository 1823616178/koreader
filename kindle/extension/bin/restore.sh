#!/bin/sh
# Undo everything bin/aiquota.sh does to the Kindle framework. Two ways to use it:
#
#   sh /mnt/us/extensions/aiquota/bin/restore.sh
#       Run it. This is the manual SSH escape hatch: if the screen is ever
#       stuck on the app, this single command gives the device back.
#
#   AIQUOTA_RESTORE_LIB=1 . /mnt/us/extensions/aiquota/bin/restore.sh
#       Source it. Defines restore_framework() and does nothing else; both
#       aiquota.sh and watchdog.sh source it at startup so the function is
#       already in memory when they need it (a trap that has to read a file
#       off /mnt/us is a trap that can fail).
#
# restore_framework is idempotent: running it on an already-restored device
# only re-sends harmless CONT/lipc calls. The saved framebuffer is repainted
# only when cvm was actually stopped, so a second call cannot flash a stale
# screen over the live framework UI.

AIQUOTA_FB_DUMP=/var/tmp/aiquota-fb.dump

restore_framework() {
    aq_frozen=0
    # pidof can answer with several pids; /proc/<pid>/stat field 3 is the
    # state letter, and T means the process is SIGSTOPped.
    aq_cvm=$(pidof cvm 2>/dev/null | awk '{print $1}')
    if [ -n "$aq_cvm" ] &&
       [ "$(awk '{print $3}' /proc/"$aq_cvm"/stat 2>/dev/null)" = "T" ]; then
        aq_frozen=1
    fi

    killall -CONT awesome cvm 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null
    # Release the kernel wake lock aiquota.sh grabbed (harmless when none is
    # held), so the framework gets its normal suspend/screensaver back.
    echo aiquota > /sys/power/wake_unlock 2>/dev/null
    if [ "$aq_frozen" = 1 ] && [ -r "$AIQUOTA_FB_DUMP" ]; then
        cat "$AIQUOTA_FB_DUMP" > /dev/fb0
    fi
    return 0
}

[ "${AIQUOTA_RESTORE_LIB:-0}" = "1" ] || restore_framework

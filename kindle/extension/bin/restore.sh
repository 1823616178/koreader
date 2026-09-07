#!/bin/sh
# Undo everything bin/aiquota.sh does to the Kindle framework. Two ways to use it:
#
#   sh /mnt/us/extensions/aiquota/bin/restore.sh
#       Run it. This is the manual SSH escape hatch: if the screen is ever
#       stuck on the app, this single command gives the device back. It also
#       ends the active aiquota session so a lingering watchdog cannot reclaim
#       resources that now belong to a later run.
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

AIQUOTA_TMP=${AIQUOTA_TMP:-/var/tmp}
AIQUOTA_ACTIVE=${AIQUOTA_ACTIVE:-$AIQUOTA_TMP/aiquota.active}
AIQUOTA_LOCK=${AIQUOTA_LOCK:-$AIQUOTA_TMP/aiquota.lock}
# Default dump path; launchers overwrite AIQUOTA_FB_DUMP with a session file.
AIQUOTA_FB_DUMP=${AIQUOTA_FB_DUMP:-$AIQUOTA_TMP/aiquota-fb.dump}

# Clear the active-session marker when it still names $1 (our session). A
# foreign session must keep the marker so its own watchdog/restore can act.
aiquota_clear_active() {
    aq_sid=$1
    [ -n "$aq_sid" ] || return 0
    if [ -f "$AIQUOTA_ACTIVE" ]; then
        aq_cur=$(cat "$AIQUOTA_ACTIVE" 2>/dev/null || true)
        if [ "$aq_cur" = "$aq_sid" ]; then
            rm -f "$AIQUOTA_ACTIVE"
        fi
    fi
}

# Drop the single-instance lock when we still own it.
aiquota_release_lock() {
    aq_sid=$1
    [ -d "$AIQUOTA_LOCK" ] || return 0
    aq_owner=$(cat "$AIQUOTA_LOCK/pid" 2>/dev/null || true)
    if [ -z "$aq_sid" ] || [ "$aq_owner" = "$aq_sid" ]; then
        rm -rf "$AIQUOTA_LOCK"
    fi
}

# Remove only this session's heartbeat / ready / fb files.
aiquota_cleanup_session() {
    aq_sid=$1
    [ -n "$aq_sid" ] || return 0
    rm -f "$AIQUOTA_TMP/aiquota.${aq_sid}.ping" \
          "$AIQUOTA_TMP/aiquota.${aq_sid}.wdready" \
          "$AIQUOTA_TMP/aiquota.${aq_sid}.fb.dump"
}

restore_framework() {
    aq_frozen=0
    # pidof can answer with several pids; /proc/<pid>/stat field 3 is the
    # state letter, and T means the process is SIGSTOPped. Check them all: one
    # stopped instance among running ones still means a frozen framework.
    for aq_cvm in $(pidof cvm 2>/dev/null); do
        if [ "$(awk '{print $3}' /proc/"$aq_cvm"/stat 2>/dev/null)" = "T" ]; then
            aq_frozen=1
            break
        fi
    done

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

# Manual SSH escape: end the active session, stop the app, restore, clean up.
aiquota_manual_restore() {
    aq_sid=
    if [ -f "$AIQUOTA_ACTIVE" ]; then
        aq_sid=$(cat "$AIQUOTA_ACTIVE" 2>/dev/null || true)
    fi
    # Stop the app so it cannot keep writing the panel or renewing wake holds.
    killall -9 aiquota 2>/dev/null
    # Prefer the session dump when we know the sid; fall back to env/default.
    if [ -n "$aq_sid" ] && [ -r "$AIQUOTA_TMP/aiquota.${aq_sid}.fb.dump" ]; then
        AIQUOTA_FB_DUMP="$AIQUOTA_TMP/aiquota.${aq_sid}.fb.dump"
    fi
    restore_framework
    if [ -n "$aq_sid" ]; then
        aiquota_clear_active "$aq_sid"
        aiquota_release_lock "$aq_sid"
        aiquota_cleanup_session "$aq_sid"
    else
        # No marker: still drop a stale lock and the legacy shared paths.
        aiquota_release_lock ""
        rm -f "$AIQUOTA_TMP/aiquota.ping" "$AIQUOTA_TMP/aiquota-fb.dump"
    fi
}

[ "${AIQUOTA_RESTORE_LIB:-0}" = "1" ] || aiquota_manual_restore

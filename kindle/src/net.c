/* WiFi wakeup through Amazon's lipc IPC. lipc only exists on-device, so
 * every failure mode (missing binary, empty answer, stayed disconnected)
 * degrades to a plain -1 return -- never a crash. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* usleep under -std=c99 */

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <time.h>
#include <unistd.h>

#include "watchdog.h"

#define WIFI_POLL_US 500000L     /* 500 ms between cmState polls */
#define WIFI_TIMEOUT_US 20000000L /* 20 s overall wakeup budget */
/* Watchdog bound for one lipc call. popen/system fork a shell and neither has
 * a timeout, so a hung lipc binary would otherwise block here forever; 30 s is
 * far more than a fork+exec needs even under memory pressure. The bound is
 * re-armed per call rather than once for the whole function, because the poll
 * loop's own budget counts only its sleeps, not the probes in between. */
#define NET_CMD_S 30

/* Battery and link info sources. yoshi_battery exists on PW2 and later (the
 * whole kindlepw2 family); plain file reads, no daemon round-trip. `iw` is
 * only spawned while the radio is already up, so the title bar adds no
 * wakeups of its own. */
#define BATT_SYSFS "/sys/devices/system/yoshi_battery/yoshi_battery0"
#define IW_LINK_CMD "iw dev wlan0 link 2>/dev/null"

/* Last state wifi_probe actually observed: 1 CONNECTED, 0 not connected,
 * -1 never probed (or only failed probes so far). Feeds the dashboard's
 * WiFi badge; a failed probe keeps the previous value rather than
 * downgrading known-good state to "unknown". */
static int g_wifi_state = -1;

/* Probe wifid state through lipc. Returns 1 when cmState is CONNECTED,
 * 0 when lipc answered something else, -1 when there was no usable
 * output (lipc missing or broken). */
static int wifi_probe(void)
{
    FILE *p;
    char buf[128];
    int result = -1;

    wd_expect(NET_CMD_S);
    p = popen("lipc-get-prop com.lab126.wifid cmState 2>/dev/null", "r");
    if (!p)
        return -1;
    if (fgets(buf, sizeof(buf), p)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                           buf[len - 1] == ' ' || buf[len - 1] == '\t'))
            buf[--len] = '\0';
        result = (strcmp(buf, "CONNECTED") == 0) ? 1 : 0;
    }
    pclose(p);
    if (result >= 0)
        g_wifi_state = result; /* a failed probe leaves the last known value */
    return result;
}

/* Set when WE ran wirelessEnable 1 (including the give-up-timeout case: the
 * radio is on either way, so the release still owes a disable). Cleared by
 * net_release_wifi() or whenever a probe shows the connection predates us. */
static int g_wifi_we_enabled;

int net_wifi_state(void)
{
    return g_wifi_state;
}

void net_release_wifi(void)
{
    int rc;

    if (!g_wifi_we_enabled)
        return; /* nothing raised by us: a pre-existing connection stays up */
    g_wifi_we_enabled = 0; /* clear first: release stays idempotent even if lipc fails */
    g_wifi_state = 0;      /* we just asked for the radio to go down */
    wd_expect(NET_CMD_S);
    rc = system("lipc-set-prop -i com.lab126.cmd wirelessEnable 0 2>/dev/null");
    (void)rc;
}

int net_ensure_wifi(void)
{
    long waited = 0;
    int empty_streak = 0;
    int state = wifi_probe(); /* arms the watchdog itself */

    if (state == 1) {
        g_wifi_we_enabled = 0; /* was already up before we got here: not ours */
        return 0;
    }
    if (state < 0)
        empty_streak = 1;

    /* Ask the connection manager to enable WiFi (instant no-op off-device).
     * Known edge: a connection that was mid-setup at the first probe gets
     * attributed to us and dropped after the fetch; rare, and wifid just
     * brings it back up. */
    g_wifi_we_enabled = 1;
    {
        int rc;

        wd_expect(NET_CMD_S);
        rc = system("lipc-set-prop -i com.lab126.cmd wirelessEnable 1 2>/dev/null");
        (void)rc;
    }

    while (waited < WIFI_TIMEOUT_US) {
        usleep(WIFI_POLL_US);
        waited += WIFI_POLL_US;

        state = wifi_probe();
        if (state == 1)
            return 0;
        if (state < 0) {
            /* Two empty probes in a row mean lipc does not exist (host
             * builds): fail fast instead of polling silence for 20 s. */
            empty_streak++;
            if (empty_streak >= 2)
                return -1;
        } else {
            empty_streak = 0;
        }
    }
    return -1;
}

/* --- title-bar status: SSID + battery -------------------------------------- */

/* First line of `path` through strtol; -1 when missing or not numeric. */
static long sysfs_long(const char *path)
{
    FILE *f;
    char buf[64];
    long v = -1;

    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fgets(buf, sizeof(buf), f))
        v = strtol(buf, NULL, 10);
    fclose(f);
    return v;
}

/* yoshi reports "1" while charging and "0" otherwise; accept the generic
 * power_supply spellings too. "Full" counts as charging (on the plug) --
 * the badge means "wall power", not a charge in progress. -1 unreadable. */
static int sysfs_charging(void)
{
    FILE *f;
    char buf[64];
    int chg = -1;

    f = fopen(BATT_SYSFS "/battery_status", "r");
    if (!f)
        return -1;
    if (fgets(buf, sizeof(buf), f)) {
        if (buf[0] == '1' || strncasecmp(buf, "charg", 5) == 0 ||
            strncasecmp(buf, "full", 4) == 0)
            chg = 1;
        else
            chg = 0;
    }
    fclose(f);
    return chg;
}

int net_battery(int *pct, int *charging)
{
    long level = sysfs_long(BATT_SYSFS "/battery_level");

    *pct = (level >= 0 && level <= 100) ? (int)level : -1;
    *charging = sysfs_charging();
    return *pct >= 0 ? 0 : -1;
}

void net_sanitize_ssid(char *s)
{
    size_t r = 0, w = 0;

    if (!s)
        return;
    /* First cut to printable ASCII. */
    while (s[r] != '\0') {
        unsigned char c = (unsigned char)s[r];

        if (c < 0x20 || c > 0x7E)
            break;
        s[w++] = s[r++];
    }
    s[w] = '\0';
    /* Then trim the spaces the cut may have left at either end. */
    r = 0;
    while (s[r] == ' ')
        r++;
    if (r > 0) {
        w -= r;
        memmove(s, s + r, w + 1);
    }
    while (w > 0 && s[w - 1] == ' ')
        s[--w] = '\0';
}

void net_wifi_ssid(char *buf, size_t n)
{
    FILE *p;
    char line[256];

    if (!buf || n == 0)
        return;
    buf[0] = '\0';
    /* No spawn while the radio is down or unprobed: that is the idle state
     * most of the app's life, and the badge has nothing to name there. */
    if (g_wifi_state != 1)
        return;

    wd_expect(NET_CMD_S);
    p = popen(IW_LINK_CMD, "r");
    if (!p)
        return;
    while (fgets(line, sizeof(line), p)) {
        if (strncmp(line, "SSID:", 5) == 0) {
            size_t len = strlen(line + 5);
            size_t room = n < 65 ? n : 65; /* SSID caps at 32 octets anyway */

            if (len > room - 1)
                len = room - 1;
            memcpy(buf, line + 5, len);
            buf[len] = '\0';
            break; /* first SSID line is the connected BSS */
        }
    }
    pclose(p);
    net_sanitize_ssid(buf);
}

/* --- screensaver hold ------------------------------------------------------ */

/* Refresh cadence: comfortably under powerd's usual ~10 min idle timer, so
 * even a hold that expires silently is re-armed long before the lock screen
 * could ever engage. */
#define SCREENSAVER_HOLD_S 240

void net_hold_screensaver(void)
{
    static time_t last;
    time_t now = time(NULL);

    if (last != 0 && now - last < SCREENSAVER_HOLD_S)
        return;
    last = now;

    wd_expect(NET_CMD_S); /* system() has no timeout of its own */
    (void)system("lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null");
}

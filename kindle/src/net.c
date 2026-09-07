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

/* Battery: try cheap sysfs first (path varies by SoC), then powerd lipc.
 * yoshi is Touch/PW1; PW2+ is wario; Rex/MTK use power_supply. The old
 * single yoshi `battery_level` path silently returned -1 on the pw2-family
 * target, so the title-bar icon never appeared. `iw` is only spawned while
 * the radio is already up, so the title bar adds no wakeups of its own. iw
 * indents the connected SSID line with a tab ("\tSSID: name", see upstream
 * scan.c), so the parser below skips leading blanks before matching, and iw
 * quotes non-printable/non-ASCII bytes as the four literal chars "\xNN" --
 * those count as undisplayable and degrade to "". */
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
    /* Keep ownership until the disable command is issued successfully so a
     * failed release can be retried and "刷新后关 WiFi" still works. */
    wd_expect(NET_CMD_S);
    rc = system("lipc-set-prop -i com.lab126.cmd wirelessEnable 0 2>/dev/null");
    if (rc != 0) {
        /* Command failed (or lipc missing). Leave ownership set; display
         * state stays whatever the last probe said -- do not pretend the
         * radio is down. */
        return;
    }
    g_wifi_we_enabled = 0;
    g_wifi_state = 0;
}

int net_ensure_wifi(void)
{
    long waited = 0;
    int empty_streak = 0;
    int state = wifi_probe(); /* arms the watchdog itself */

    if (state == 1) {
        /* Already CONNECTED: only clear ownership when we did not raise it
         * earlier in this session. Clearing here after our own enable made
         * the next release a no-op (R04). */
        if (!g_wifi_we_enabled)
            return 0; /* pre-existing connection: not ours, leave it alone */
        return 0;     /* still ours from a prior ensure in this session */
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

/* First line of `path`; 0 on success. */
static int sysfs_line(const char *path, char *buf, size_t n)
{
    FILE *f;

    if (!path || !buf || n == 0)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* First line of `path` through strtol; -1 when missing or not numeric. */
static long sysfs_long(const char *path)
{
    char buf[64];
    char *end;
    long v;

    if (sysfs_line(path, buf, sizeof(buf)) != 0)
        return -1;
    v = strtol(buf, &end, 10);
    if (end == buf)
        return -1;
    return v;
}

/* "1" / "Charging" / "Full" -> 1; "0" / "Discharging" -> 0; else -1.
 * "Full" counts as charging: the badge means "on the plug". */
int net_parse_charging(const char *s)
{
    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '\0')
        return -1;
    if ((s[0] == '1' || s[0] == '0') &&
        (s[1] == '\0' || s[1] == '\n' || s[1] == '\r' || s[1] == ' ' ||
         s[1] == '\t'))
        return s[0] == '1' ? 1 : 0;
    if (strncasecmp(s, "charg", 5) == 0 || strncasecmp(s, "full", 4) == 0)
        return 1;
    if (strncasecmp(s, "discharg", 8) == 0)
        return 0;
    return -1;
}

static int lipc_powerd_int(const char *prop, int *out)
{
    char cmd[128];
    FILE *p;
    char buf[64];
    char *end;
    long v;

    if (!prop || !out)
        return -1;
    snprintf(cmd, sizeof(cmd),
             "lipc-get-prop com.lab126.powerd %s 2>/dev/null", prop);
    wd_expect(NET_CMD_S);
    p = popen(cmd, "r");
    if (!p)
        return -1;
    if (!fgets(buf, sizeof(buf), p)) {
        pclose(p);
        return -1;
    }
    pclose(p);
    v = strtol(buf, &end, 10);
    if (end == buf)
        return -1;
    *out = (int)v;
    return 0;
}

/* SoC-specific sysfs, then powerd lipc. First hit is cached. */
static const char *const BATT_PCT_PATHS[] = {
    "/sys/devices/system/yoshi_battery/yoshi_battery0/battery_level",
    "/sys/devices/system/yoshi_battery/yoshi_battery0/battery_capacity",
    "/sys/devices/system/wario_battery/wario_battery0/battery_capacity",
    "/sys/class/power_supply/max77696-battery/capacity",
    "/sys/class/power_supply/max77796-battery/capacity",
    "/sys/class/power_supply/bd71827_bat/capacity",
    "/sys/class/power_supply/bd7181x_bat/capacity",
    "/sys/class/power_supply/battery/capacity",
};

static const char *const BATT_CHG_PATHS[] = {
    "/sys/devices/system/yoshi_battery/yoshi_battery0/battery_status",
    "/sys/devices/system/wario_charger/wario_charger0/charging",
    "/sys/devices/platform/aplite_charger.0/charging",
    "/sys/devices/platform/fsl-usb2-udc/charging",
    "/sys/class/power_supply/max77696-battery/status",
    "/sys/class/power_supply/max77796-battery/status",
    "/sys/class/power_supply/bd71827_bat/status",
    "/sys/class/power_supply/bd7181x_bat/status",
    "/sys/class/power_supply/battery/status",
};

#define BATT_PCT_N ((int)(sizeof(BATT_PCT_PATHS) / sizeof(BATT_PCT_PATHS[0])))
#define BATT_CHG_N ((int)(sizeof(BATT_CHG_PATHS) / sizeof(BATT_CHG_PATHS[0])))

/* -2 unprobed, -1 lipc, >= 0 cached sysfs index. A cached path that stops
 * reading drops back to -2 so the table gets one rescan before lipc. */
static int g_pct_src = -2;
static int g_chg_src = -2;

static int read_batt_pct(void)
{
    int i, v;

    if (g_pct_src >= 0) {
        long level = sysfs_long(BATT_PCT_PATHS[g_pct_src]);

        if (level >= 0 && level <= 100)
            return (int)level;
        /* Stale cache (driver reloaded, sysfs renumbered): rescan below
         * instead of paying a lipc spawn at every render point. */
        g_pct_src = -2;
    }
    if (g_pct_src == -2) {
        for (i = 0; i < BATT_PCT_N; i++) {
            long level = sysfs_long(BATT_PCT_PATHS[i]);

            if (level >= 0 && level <= 100) {
                g_pct_src = i;
                return (int)level;
            }
        }
        g_pct_src = -1; /* fall through to lipc on this and later calls */
    }
    if (lipc_powerd_int("battLevel", &v) == 0 && v >= 0 && v <= 100)
        return v;
    return -1;
}

static int read_batt_charging(void)
{
    char buf[64];
    int i, v, parsed;

    if (g_chg_src >= 0) {
        if (sysfs_line(BATT_CHG_PATHS[g_chg_src], buf, sizeof(buf)) == 0) {
            parsed = net_parse_charging(buf);
            if (parsed >= 0)
                return parsed;
        }
        /* Same stale-cache rescan as read_batt_pct. */
        g_chg_src = -2;
    }
    if (g_chg_src == -2) {
        for (i = 0; i < BATT_CHG_N; i++) {
            if (sysfs_line(BATT_CHG_PATHS[i], buf, sizeof(buf)) != 0)
                continue;
            parsed = net_parse_charging(buf);
            if (parsed >= 0) {
                g_chg_src = i;
                return parsed;
            }
        }
        g_chg_src = -1;
    }
    if (lipc_powerd_int("isCharging", &v) == 0 && (v == 0 || v == 1))
        return v;
    return -1;
}

int net_battery(int *pct, int *charging)
{
    if (!pct || !charging)
        return -1;
    *pct = read_batt_pct();
    *charging = read_batt_charging();
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

/* One `iw ... link` output line -> SSID. Returns 1 when the line carried an
 * SSID (out filled, possibly empty), 0 for any other line. iw indents the
 * line ("\tSSID: name", upstream scan.c), so leading blanks are skipped
 * before the prefix match. iw's print_ssid_escaped quotes every
 * non-printable/non-ASCII byte as the four literal chars "\xNN" (and the
 * backslash itself as "\x5c", so a printed "\x" never came from a literal
 * name); net_sanitize_ssid only cuts raw bytes outside 0x20-0x7E and would
 * pass that ASCII-looking escape text through. A name containing "\x"
 * therefore counts as non-ASCII: emptied here, caller falls back to "WiFi". */
static int parse_ssid_line(const char *line, char *out, size_t n)
{
    const char *p = line;
    size_t len;
    size_t room;

    if (!line || !out || n == 0)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "SSID:", 5) != 0)
        return 0;
    p += 5;
    len = strlen(p);
    room = n < 65 ? n : 65; /* SSID caps at 32 octets anyway */
    if (len > room - 1)
        len = room - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    if (strstr(out, "\\x") != NULL)
        out[0] = '\0';
    return 1;
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
        if (parse_ssid_line(line, buf, n))
            break; /* first SSID line is the connected BSS */
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

/* Test seam: whether this session still owes a wirelessEnable 0. */
int net_wifi_owned_for_test(void)
{
    return g_wifi_we_enabled ? 1 : 0;
}

/* Test seam: force the ownership flag (host tests have no lipc). */
void net_wifi_set_owned_for_test(int owned)
{
    g_wifi_we_enabled = owned ? 1 : 0;
}

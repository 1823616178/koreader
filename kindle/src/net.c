/* WiFi wakeup through Amazon's lipc IPC. lipc only exists on-device, so
 * every failure mode (missing binary, empty answer, stayed disconnected)
 * degrades to a plain -1 return -- never a crash. */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE /* usleep under -std=c99 */

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    return result;
}

/* Set when WE ran wirelessEnable 1 (including the give-up-timeout case: the
 * radio is on either way, so the release still owes a disable). Cleared by
 * net_release_wifi() or whenever a probe shows the connection predates us. */
static int g_wifi_we_enabled;

void net_release_wifi(void)
{
    int rc;

    if (!g_wifi_we_enabled)
        return; /* nothing raised by us: a pre-existing connection stays up */
    g_wifi_we_enabled = 0; /* clear first: release stays idempotent even if lipc fails */
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

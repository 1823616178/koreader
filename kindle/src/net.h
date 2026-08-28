#ifndef AIQUOTA_NET_H
#define AIQUOTA_NET_H

#include <stddef.h>

/* Ensure WiFi is up: if lipc-get-prop com.lab126.wifid cmState != CONNECTED, run
 * lipc-set-prop -i com.lab126.cmd wirelessEnable 1 and poll up to 20 s. Returns 0 if connected,
 * -1 otherwise (caller shows an error card; never crashes). lipc only exists on-device;
 * off-device this must fail gracefully, not crash.
 *
 * Ownership rule "who raised it, drops it": net_ensure_wifi remembers whether
 * the radio was already CONNECTED before we touched anything. Only then does
 * net_release_wifi() actually run wirelessEnable 0; a connection the user had
 * going is never dropped out from under them. Calling release without a prior
 * ensure (or after it already released) is a no-op, so callers need no state
 * of their own. */
int  net_ensure_wifi(void);
void net_release_wifi(void);

/* Last radio state actually observed by the probes: 1 CONNECTED, 0 not
 * connected, -1 never probed. Zero-cost (cached); refreshed on every
 * ensure/release cycle, so it is exactly as fresh as the last fetch. */
int  net_wifi_state(void);

/* Current SSID for the title bar, "" when unknown. Probes `iw dev wlan0
 * link` once, and only while the radio is known to be up (g_wifi_state==1,
 * i.e. mid-fetch when the chip is already awake): disconnected -- the idle
 * norm with wifi_auto_off -- costs no process spawn at all. Missing binary,
 * hidden SSID or any other failure degrades to "" and the caller falls back
 * to its plain "WiFi" label. */
void net_wifi_ssid(char *buf, size_t n);

/* Battery through sysfs file reads (yoshi_battery, PW2 and later): one
 * open/read/close per value, no lipc subprocess, and callers invoke it only
 * at render points -- never polled -- which is the power-correct shape on an
 * e-ink device that repaints only when data changes. *pct: 0-100, -1 when
 * unreadable (caller draws no battery text). *charging: 1 charging, 0 not,
 * -1 unknown. Returns 0 when *pct is usable, -1 otherwise. */
int  net_battery(int *pct, int *charging);

/* Keep printable ASCII (0x20-0x7E) and trim surrounding whitespace, in
 * place: the bundled subset font has no glyphs beyond ASCII plus the UI
 * strings, so a CJK SSID would otherwise render as tofu boxes. A name that
 * turns mixed ASCII/UTF-8 gets cut at the first non-ASCII byte -- a
 * truncated name beats glyph garbage. Exported for the host unit tests. */
void net_sanitize_ssid(char *s);

/* Re-assert powerd's screensaver hold; internally rate-limited to roughly
 * one lipc call per 4 minutes, so calling it from every tap-wait chunk
 * costs ~30 ms of CPU per few minutes -- noise next to the SoC idle draw of
 * the always-open session. The wrapper arms the hold once before freezing
 * the framework; some powerd builds age it out (or autosuspend anyway once
 * their own idle timer fires) and the parked dashboard then rolls into the
 * lock screen. Off-device lipc does not exist and this is a silent no-op. */
void net_hold_screensaver(void);

#endif

#ifndef AIQUOTA_NET_H
#define AIQUOTA_NET_H

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

#endif

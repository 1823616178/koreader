#ifndef AIQUOTA_NET_H
#define AIQUOTA_NET_H

/* Ensure WiFi is up: if lipc-get-prop com.lab126.wifid cmState != CONNECTED, run
 * lipc-set-prop -i com.lab126.cmd wirelessEnable 1 and poll up to 20 s. Returns 0 if connected,
 * -1 otherwise (caller shows an error card; never crashes). lipc only exists on-device;
 * off-device this must fail gracefully, not crash. */
int  net_ensure_wifi(void);

#endif

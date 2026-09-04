#ifndef AIQUOTA_HTTP_H
#define AIQUOTA_HTTP_H

#include <stddef.h>

/* Plain HTTP GET over TCP (no TLS). Sends an HTTP/1.0 request line (forbids chunked
 * responses, which this client does not de-chunk) plus "Connection: close", reads to EOF.
 * Timeouts -- every bound stays at or above the Lua stack's
 * socketutil:set_timeout(45, 50) in quotadata.lua: connect gets 45 s (the
 * 45 s floor is REQUIRED, the API cold-starts local services), each single
 * recv/send gets 50 s via SO_RCVTIMEO/SO_SNDTIMEO, and the whole body read
 * is capped at 120 s wall-clock, because per-op timeouts alone cannot stop a
 * server dribbling one byte per window. DNS resolution has no timeout of its
 * own, only a 60 s watchdog bound.
 * On success returns a malloc'd NUL-terminated body and sets *status to the HTTP code.
 * Caller must free().
 * On failure returns NULL and fills errbuf with a short ASCII reason. */
char *http_get(const char *url, int *status, char *errbuf, size_t errbuf_size);

#endif

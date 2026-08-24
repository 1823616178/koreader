#ifndef AIQUOTA_HTTP_H
#define AIQUOTA_HTTP_H

#include <stddef.h>

/* Plain HTTP GET over TCP (no TLS). Sends an HTTP/1.0 request line (forbids chunked
 * responses, which this client does not de-chunk) plus "Connection: close", reads to EOF.
 * Connect timeout 45 s, per-op recv/send timeout 50 s (mirrors socketutil:set_timeout(45, 50)
 * in quotadata.lua -- the 45 s floor is REQUIRED, the API cold-starts local services).
 * On success returns a malloc'd NUL-terminated body and sets *status to the HTTP code.
 * Caller must free().
 * On failure returns NULL and fills errbuf with a short ASCII reason. */
char *http_get(const char *url, int *status, char *errbuf, size_t errbuf_size);

#endif

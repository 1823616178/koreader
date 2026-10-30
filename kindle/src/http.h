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

typedef enum {
    HTTP_PHASE_NONE = 0,
    HTTP_PHASE_URL,
    HTTP_PHASE_DNS,
    HTTP_PHASE_CONNECT,
    HTTP_PHASE_HTTP,
    HTTP_PHASE_DONE
} HttpPhase;

/* Structured timings for the last http_get (F03). Milliseconds; -1 = n/a. */
typedef struct {
    HttpPhase last_phase;
    HttpPhase failed_phase; /* NONE on success */
    int http_status;
    long dns_ms;
    long connect_ms;
    long http_ms;
    long total_ms;
    char err[256];
} HttpDiag;

void http_diag_reset(void);
const HttpDiag *http_diag_last(void);
/* Format a multi-line ASCII report into dst (for INFO / Diagnose popup). */
void http_diag_format(const HttpDiag *d, int wifi_ok, long wifi_ms,
                      int json_ok, long json_ms,
                      char *dst, size_t dst_size);

char *http_get(const char *url, int *status, char *errbuf, size_t errbuf_size);
/* Like http_get, but sends "Authorization: Bearer <token>" when token is a
 * valid non-empty token (printable ASCII, no spaces, <= 200 chars). A
 * present-but-malformed token fails the request with errbuf "bad token"
 * instead of being sent, so a broken config can never leak a partial or
 * CRLF-injected header. NULL/empty token behaves exactly like http_get. */
char *http_get_with_auth(const char *url, const char *token,
                         int *status, char *errbuf, size_t errbuf_size);

/* Cooperative cancel for a in-flight http_get (R06). The GET checks
 * between DNS/connect/recv phases and returns NULL with errbuf
 * "cancelled" when set. Reset before each new request. */
void http_cancel_reset(void);
void http_cancel(void);
int  http_cancel_requested(void);

#endif

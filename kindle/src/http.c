/* POSIX sockets: getaddrinfo/poll/timeouts need these feature test macros
 * when compiling with -std=c99 (must come before any system include). */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "watchdog.h"

#define CONNECT_TIMEOUT_MS 45000 /* 45 s floor: the API cold-starts local services */
#define IO_TIMEOUT_S 50
/* getaddrinfo has no timeout of its own; the resolver's own retry schedule
 * caps out around 40 s against an unreachable nameserver. Watchdog bound only. */
#define DNS_TIMEOUT_S 60
/* Per-recv timeouts alone do not bound the read loop: a server dribbling one
 * byte every 49 s would keep it alive forever. Cap the whole body read. */
#define BODY_TIMEOUT_S 120
#define MAX_RESPONSE_BYTES (1024 * 1024) /* payload is ~1 KB JSON; runaway protection */

static void set_err(char *errbuf, size_t errbuf_size, const char *msg)
{
    if (errbuf && errbuf_size > 0)
        snprintf(errbuf, errbuf_size, "%s", msg);
}

/* errbuf is the only field-debugging channel on device (printf over SSH),
 * so syscall failures carry the errno detail. */
static void set_err_errno(char *errbuf, size_t errbuf_size, const char *what, int errnum)
{
    if (errbuf && errbuf_size > 0)
        snprintf(errbuf, errbuf_size, "%s: %s", what, strerror(errnum));
}

/* Parses http://host[:port][/path]. No TLS: https:// is rejected outright. */
static int parse_url(const char *url, char *host, size_t host_size,
                     char *port, size_t port_size,
                     const char **path, char *errbuf, size_t errbuf_size)
{
    const char *p;
    const char *host_end;
    const char *colon;
    size_t hlen;

    if (strncmp(url, "https://", 8) == 0) {
        set_err(errbuf, errbuf_size, "https not supported");
        return -1;
    }
    if (strncmp(url, "http://", 7) != 0) {
        set_err(errbuf, errbuf_size, "bad url");
        return -1;
    }

    p = url + 7;
    *path = strchr(p, '/');
    host_end = *path ? *path : p + strlen(p);
    if (!*path)
        *path = "/";

    colon = memchr(p, ':', (size_t)(host_end - p));
    hlen = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (hlen == 0 || hlen >= host_size) {
        set_err(errbuf, errbuf_size, "bad url");
        return -1;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    if (colon) {
        size_t plen = (size_t)(host_end - colon - 1);
        size_t i;
        if (plen == 0 || plen >= port_size) {
            set_err(errbuf, errbuf_size, "bad url");
            return -1;
        }
        for (i = 0; i < plen; i++) {
            if (colon[1 + i] < '0' || colon[1 + i] > '9') {
                set_err(errbuf, errbuf_size, "bad url");
                return -1;
            }
        }
        memcpy(port, colon + 1, plen);
        port[plen] = '\0';
    } else {
        snprintf(port, port_size, "%s", "80");
    }
    return 0;
}

/* Non-blocking connect with a 45 s poll window. Returns a connected fd or -1. */
static int connect_timeout(const struct addrinfo *ai, char *errbuf, size_t errbuf_size)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int flags;
    int rc;

    if (fd < 0)
        return -1;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        set_err_errno(errbuf, errbuf_size, "connect failed", errno);
        close(fd);
        return -1;
    }
    if (rc < 0) {
        struct pollfd pfd;
        int err = 0;
        socklen_t errlen = sizeof(err);

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0) {
            set_err(errbuf, errbuf_size, "timeout");
            close(fd);
            return -1;
        }
        if (rc < 0) {
            set_err_errno(errbuf, errbuf_size, "connect failed", errno);
            close(fd);
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            set_err_errno(errbuf, errbuf_size, "connect failed", errno);
            close(fd);
            return -1;
        }
        if (err != 0) {
            set_err_errno(errbuf, errbuf_size, "connect failed", err);
            close(fd);
            return -1;
        }
    }

    /* Back to blocking mode; per-op timeouts come from SO_RCVTIMEO/SO_SNDTIMEO. */
    if (fcntl(fd, F_SETFL, flags) < 0) {
        close(fd);
        return -1;
    }
    {
        struct timeval tv;
        tv.tv_sec = IO_TIMEOUT_S;
        tv.tv_usec = 0;
        /* Refuse to run un-timed: without these a hung server would block forever. */
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
            set_err_errno(errbuf, errbuf_size, "setsockopt failed", errno);
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int send_all(int fd, const char *buf, size_t len, char *errbuf, size_t errbuf_size)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL /* avoids SIGPIPE; no signal() side effect needed */
#else
                         0
#endif
        );
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_err(errbuf, errbuf_size, "timeout");
            return -1;
        } else {
            set_err_errno(errbuf, errbuf_size, "send failed", errno);
            return -1;
        }
    }
    return 0;
}

char *http_get(const char *url, int *status, char *errbuf, size_t errbuf_size)
{
    char host[256];
    char port[8];
    char host_hdr[sizeof(host) + sizeof(port)]; /* "host[:port]" authority */
    const char *path;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int fd = -1;
    char request[1200];
    int request_len;
    char *data = NULL;
    size_t cap = 0;
    size_t len = 0;
    int code = 0;
    const char *body_start;
    char *hdr_end;
    char *body;
    time_t body_deadline;
    int gai_rc;

    if (errbuf && errbuf_size > 0)
        errbuf[0] = '\0';
    if (!url || parse_url(url, host, sizeof(host), port, sizeof(port),
                          &path, errbuf, errbuf_size) != 0)
        return NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    wd_expect(DNS_TIMEOUT_S);
    gai_rc = getaddrinfo(host, port, &hints, &res);
    if (gai_rc != 0) {
        if (errbuf && errbuf_size > 0)
            snprintf(errbuf, errbuf_size, "dns failed: %s", gai_strerror(gai_rc));
        return NULL;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        if (errbuf && errbuf_size > 0)
            errbuf[0] = '\0';
        wd_expect(CONNECT_TIMEOUT_MS / 1000 + WD_SLACK_S); /* re-armed per address */
        fd = connect_timeout(ai, errbuf, errbuf_size);
        if (fd >= 0)
            break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (!errbuf || errbuf_size == 0 || errbuf[0] == '\0')
            set_err(errbuf, errbuf_size, "connect failed");
        return NULL;
    }

    /* HTTP/1.0 on purpose: chunked transfer-coding is an HTTP/1.1 feature, so a
     * 1.0 request forbids chunked responses. This client does not de-chunk (the
     * Lua stack's LuaSocket did), and "Connection: close" + read-to-EOF loses
     * nothing else. Host names the port unless it is the default 80: virtual
     * hosts route on the exact authority string. */
    if (strcmp(port, "80") == 0)
        snprintf(host_hdr, sizeof(host_hdr), "%s", host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%s", host, port);
    request_len = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "Accept: application/json\r\n"
                           "Cache-Control: no-cache\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host_hdr);
    wd_expect(IO_TIMEOUT_S + WD_SLACK_S);
    if (request_len < 0 || (size_t)request_len >= sizeof(request) ||
        send_all(fd, request, (size_t)request_len, errbuf, errbuf_size) != 0) {
        if (errbuf && errbuf_size > 0 && errbuf[0] == '\0')
            set_err(errbuf, errbuf_size, "send failed");
        close(fd);
        return NULL;
    }

    /* Read to EOF (server closes because of "Connection: close"). */
    cap = 16384;
    data = malloc(cap);
    if (!data) {
        set_err(errbuf, errbuf_size, "out of memory");
        close(fd);
        return NULL;
    }
    body_deadline = time(NULL) + BODY_TIMEOUT_S;
    for (;;) {
        ssize_t n;
        if (time(NULL) > body_deadline) {
            set_err(errbuf, errbuf_size, "timeout");
            free(data);
            close(fd);
            return NULL;
        }
        if (len == cap) {
            char *bigger;
            /* A response reaching the cap is rejected, whether or not it
             * would have ended exactly there. */
            if (cap >= MAX_RESPONSE_BYTES) {
                set_err(errbuf, errbuf_size, "response too large");
                free(data);
                close(fd);
                return NULL;
            }
            cap *= 2;
            if (cap > MAX_RESPONSE_BYTES)
                cap = MAX_RESPONSE_BYTES;
            bigger = realloc(data, cap);
            if (!bigger) {
                set_err(errbuf, errbuf_size, "out of memory");
                free(data);
                close(fd);
                return NULL;
            }
            data = bigger;
        }
        n = recv(fd, data + len, cap - len, 0);
        if (n > 0) {
            len += (size_t)n;
            wd_expect(IO_TIMEOUT_S + WD_SLACK_S); /* progress: re-arm */
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            set_err(errbuf, errbuf_size, "timeout");
            free(data);
            close(fd);
            return NULL;
        } else {
            set_err_errno(errbuf, errbuf_size, "recv failed", errno);
            free(data);
            close(fd);
            return NULL;
        }
    }
    close(fd);

    /* NUL-terminate the raw response so header parsing can use string ops. */
    {
        char *done = realloc(data, len + 1);
        if (!done) {
            set_err(errbuf, errbuf_size, "out of memory");
            free(data);
            return NULL;
        }
        data = done;
    }
    data[len] = '\0';

    /* Status line: "HTTP/x.y NNN ..." */
    if (len >= 5 && memcmp(data, "HTTP/", 5) == 0) {
        const char *sp = memchr(data, ' ', len);
        if (sp)
            code = atoi(sp + 1);
    }
    if (code <= 0) {
        set_err(errbuf, errbuf_size, "bad response");
        free(data);
        return NULL;
    }

    /* Body starts after the first blank line; tolerate a missing terminator. */
    hdr_end = strstr(data, "\r\n\r\n");
    body_start = hdr_end ? hdr_end + 4 : data + len;

    body = malloc(len - (size_t)(body_start - data) + 1);
    if (!body) {
        set_err(errbuf, errbuf_size, "out of memory");
        free(data);
        return NULL;
    }
    strcpy(body, body_start);
    free(data);

    if (status)
        *status = code;
    return body;
}

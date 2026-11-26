/* hassmic's HTTP/1.1 client over its own TLS (tls.c; issue #236), in place of the firmware's libcurl: online updates
 * and the plugin index (update.c, plugin_fetch.c), media over HTTPS (https.c), Amazon's downloads (davs.c), Music
 * Assistant's images over https:// (mass.c).  One request per connection (Connection: close), redirects followed
 * as the caller says, bodies by length, chunks or the connection's end. */
#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

struct http {
    /* the request */
    const char *url;                    /* http:// or https:// (no user or password in it) */
    const char *method;                 /* NULL: GET, or POST where there is a body */
    const char *const *headers;         /* NULL or NULL-ended "Name: value" lines, sent as given */
    const void *body; size_t body_len;  /* a POST's (Content-Length added) */
    const char *agent;                  /* User-Agent; NULL: none sent */
    int redirects;                      /* how many to follow (0: a 3xx is the answer) */
    int redirect_https;                 /* 1: a redirect only to https:// (else it fails); 0: http:// too */
    int plain;                          /* 1: http:// only, no TLS (another Echo's settings port) */
    int lan;                            /* 1: a plain socket(), which the egress lock keeps on the local network; 0:
                                           net_socket (any address) */
    int verify;                         /* 1: the certificate checked (tls.c) */
    /* times (s; 0: none): CONNECT_S for each address's connection and TLS handshake, TOTAL_S for all of it from the
     * name lookup on, and the transfer fails when fewer than LOW_BYTES came in any LOW_S (libcurl's low speed limit:
     * LOW_BYTES 1 = nothing in LOW_S; a moving window, LOW_S at most 64) */
    int connect_s, total_s, low_s; long low_bytes;
    const volatile int *stop;           /* set by another thread: ends it within 250 ms (but a name lookup: getaddrinfo) */
    /* the answer: HEAD with the final response's status (not a redirect's) and Content-Length (-1: not given),
     * before any of its body; BODY with each piece.  Either returns nonzero to end it there (http_run: HTTP_ENDED) */
    int (*head)(void *arg, int status, long long length);
    int (*data)(void *arg, const void *d, size_t n);
    void *arg;
    /* out */
    int status;                         /* the final response's (0 if none came) */
};

enum { HTTP_OK = 0, HTTP_FAILED = -1, HTTP_ENDED = -2, HTTP_CANCELLED = -3 };

/* Runs R; HTTP_OK when the whole body came (whatever its status), HTTP_ENDED when a callback ended it, HTTP_CANCELLED
 * when STOP did, HTTP_FAILED with why in ERR.  Blocks; no lock held. */
int http_run(struct http *r, char *err, size_t cap);

/* A certificate's refusal, for the words around it ("the settings page's ... off accepts it"): ERR as http_run left it */
int http_cert_refused(const char *err);
#endif

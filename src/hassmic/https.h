/* Media over HTTPS (issue #30): what Home Assistant hands us for TTS, announcements and play_media when it serves
 * HTTPS itself, fetched through the firmware's libcurl and handed on as a socket like a plain http:// fetch. */
#ifndef HTTPS_H
#define HTTPS_H
#include <string.h>

/* How much of a URL a log line may show (boot.log is served by the settings page): up to its query (Home Assistant's
 * signed media URLs carry authSig there, an entity's picture a token) or to a hassmic_cards camera picture's token.
 * Shown as "%.*s%s", url_shown(u), u, u[url_shown(u)] ? "..." : "" */
static inline int url_shown(const char *u)
{
    static const char cam[] = "/api/hassmic_cards/camera/";
    const char *t = strstr(u, cam);
    size_t q = strcspn(u, "?"), n = t ? (size_t)(t - u) + sizeof cam - 1 : strlen(u);
    return (int)(q < n ? q : n);
}

/* GET url (https://, redirects followed); a socket that reads the body, or -1 (logged).  Blocks until the response's
 * headers are in.  shutdown() or close() on the socket stops the transfer. */
int https_open(const char *url);

/* Check the server's certificate against the system's CAs and its name (setting "https_verify", on by default); 0
 * accepts any certificate (self-signed, or not naming the address Home Assistant gives out).  -1 reads */
int https_verify(int set);
#endif

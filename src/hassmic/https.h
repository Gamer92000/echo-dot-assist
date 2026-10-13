/* Media over HTTPS (issue #30): what Home Assistant hands us for TTS, announcements and play_media when it serves
 * HTTPS itself, fetched through the firmware's libcurl and handed on as a socket like a plain http:// fetch. */
#ifndef HTTPS_H
#define HTTPS_H

/* GET url (https://, redirects followed); a socket that reads the body, or -1 (logged).  Blocks until the response's
 * headers are in.  shutdown() or close() on the socket stops the transfer. */
int https_open(const char *url);

/* Check the server's certificate against the system's CAs and its name (setting "https_verify", on by default); 0
 * accepts any certificate (self-signed, or not naming the address Home Assistant gives out).  -1 reads */
int https_verify(int set);
#endif

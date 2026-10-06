/* Amazon's DAVS artifacts (wake word sets, sound detection, whisper) downloaded by the Echo itself, for the settings
 * page: the Echo registers itself with a code pair the user enters at amazon.<domain>/code, then fetches what the
 * page asks for.  See davs.c for the login (the device attestation token is dha.c's) and where the downloads land. */
#ifndef DAVS_H
#define DAVS_H
#include <stddef.h>

void davs_start(void);                        /* thread; a saved registration (state/davs) is loaded */
/* the page's view: {"state":"none|waiting|registered|busy","dha":bool,"code","url","left_s","device","domain",
 * "busy":"<what>","progress":0..100,"error":"...","done":"<artifact id>"} — never a token (the page is plain HTTP) */
size_t davs_status_json(char *out, size_t cap);
/* an Amazon site ("de", "com", "co.uk", ... davs.c has the list): start a login; the code to enter shows up in
 * davs_status_json.  0, or -1 with the reason in ERR */
int davs_login(const char *domain, char *err, size_t errsz);
int davs_cancel(char *err, size_t errsz);     /* stop a login that waits for its code; an older registration stays */
int davs_logout(char *err, size_t errsz);     /* deregister from the account, forget the tokens */
/* KEY: a wake word (alexa, echo, computer, amazon, ziggy) with its LOCALE, "aed" (LOCALE picks the region) or
 * "whisper".  Downloads, checks and stages it (artifacts.c); installing is the page's install, as for a copy. */
int davs_fetch(const char *key, const char *locale, char *err, size_t errsz);
#endif

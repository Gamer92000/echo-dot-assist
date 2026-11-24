/* Where this Echo's software comes from (issue #175): the server online updates look at and the plugin index, each
 * the project's own (the default) or the owner's, with the owner's own signing key or no signature check at all.
 * The settings page's "Sources" (web.c /api/sources); not exported, not in Home Assistant.  sources.c has the whole
 * argument; docs/SECURITY.md "Updates" and "Plugins" say what each choice costs. */
#ifndef SOURCES_H
#define SOURCES_H
#include <stddef.h>
#include <stdint.h>

#define SRC_URL_MAX 200
enum { SRC_UPDATES, SRC_PLUGINS, SRC_N };
/* url "": the default.  has_key: KEY is trusted beside the project's.  unchecked: no signature is checked */
struct source { char url[SRC_URL_MAX + 1]; uint8_t key[32]; int has_key, unchecked; };
struct sources { struct source s[SRC_N]; };

/* main(), before update_start and the plugins: state/sources read, the plugin index told (plugin_index.c
 * pi_source), root asked again for the update key where its file is not what state/sources says */
void sources_load(void);
void sources_get(struct sources *out);
/* The page's lines ("<which>_url=", "<which>_key=", "<which>_unchecked=", which: updates, plugins; one missing keeps
 * what is set) over BASE (what sources_get gave) into OUT, checked: 0, or -1 and why in ERR */
int  sources_parse(const char *body, const struct sources *base, struct sources *out, char *err, size_t cap);
/* 1 where TO trusts more than FROM: a source that has a key or no check and is not exactly what it was (a key
 * added or changed, the check dropped, or the server of such a source changed).  Those wait for the action button
 * (web.c W_SOURCE); anything else (a server alone, back to the default, a key or no check taken away) is applied at once */
int  sources_widens(const struct sources *from, const struct sources *to);
/* 1 where both say the same (field by field: never memcmp, the addresses carry bytes past their end) */
int  sources_equal(const struct sources *a, const struct sources *b);
/* TO saved and in force, only over EXPECT (what the caller judged TO against: a change made since by another browser
 * and nothing is applied, -2): state/sources written, root asked for the update key it is to trust (state/update-key),
 * and where the plugin index changed a restart asked for (state/restart: plugin_index.c and plugin_fetch.c keep its
 * lists in memory) and, where what is trusted for it changed, what was kept of the old one forgotten.  0, or -1 / -2
 * and why in ERR */
int  sources_apply(const struct sources *expect, const struct sources *to, char *err, size_t cap);
/* 0 the project's both, 1 a server of the owner's, 2 a key of the owner's or no check (the page's red warning) */
int  sources_level(void);
const char *sources_default(int which);

/* update.c: the custom server's API and download bases (GitHub's or Forgejo's/Gitea's form, by its host): 1, or 0
 * for the default (the caller's own).  Either may be NULL */
int  sources_update_urls(char *api, size_t acap, char *dl, size_t dcap);
/* 1 where no signature is checked: update.c fetches none and signs the bundle with hassmic's own key
 * (sources_update_sign), which root was told to trust */
int  sources_update_unchecked(void);
int  sources_update_sign(const uint8_t *b, size_t len, uint8_t sig[64]);
/* the key ota_handoff takes beside the release key (the owner's, or hassmic's own with no check): 1 */
int  sources_update_key(uint8_t pk[32]);

/* the page's view: {"updates":{...},"plugins":{...},"level":N} */
size_t sources_json(char *o, size_t cap);
#endif

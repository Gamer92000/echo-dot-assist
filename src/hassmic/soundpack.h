/* Sets of the Sounds card's sounds kept on the Echo (the settings page's "Sound packs"): state/sound-packs/<id>/ holds
 * a name and up to one WAV per replaceable sound (sounds.h).  A pack changes nothing until applied.  Any thread. */
#ifndef SOUNDPACK_H
#define SOUNDPACK_H
#include <stddef.h>
#define SP_PACKS_MAX 16
#define SP_LIBRARY_MAX (30u << 20)      /* three packs at every sound's longest, or some 17 of stock-like length */
#define SP_NAME_MAX 63
/* A new pack: its id, or -1 (err says why).  current: with the sounds of one's own in use now, else empty */
int soundpack_new(const char *name, size_t len, int current, char *err, size_t errsz);
int soundpack_put(int id, int sound, const void *wav, size_t len, char *err, size_t errsz);
unsigned char *soundpack_get(int id, int sound, size_t *len);      /* the WAV, the caller's to free; NULL: none */
/* In use from now on: the pack's sounds, and Alexa's for those it has none of.  0, -1 */
int soundpack_apply(int id, char *err, size_t errsz);
int soundpack_delete(int id);
/* 0: there is such a pack.  name: SP_NAME_MAX + 1 bytes, sounds: bit per enum sound */
int soundpack_info(int id, char *name, size_t *bytes, unsigned *sounds);
size_t soundpack_bytes(void);           /* the library's */
size_t soundpack_max(void);             /* what it may hold */
#endif

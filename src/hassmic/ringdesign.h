/* The light ring's designs (issue #38): animations drawn on the settings page, and which of the Echo's own patterns
 * each one replaces (ring.h's slots).  A design is kept as drawn, unturned; the copies turned towards the talker are
 * made when root installs it for listening (ring.c).
 *
 * A design as text, on the page's wire and on disk:
 *     name <text>                   up to RD_NAME - 1 bytes, no control characters
 *     leds 12                       the ring's LEDs
 *     loop <k> | once               the loop starts at frame k (0: all of it repeats); once: plays, then dark
 *     <ms>:RRGGBB,RRGGBB,...        one line per frame, a colour per LED as seen (gamma 2.2 to the driver's duty is
 *                                   ring.c's), 1..60000 ms
 * A theme pack: which design each animation shows, with a name and the rest; the designs are the library's.  On the
 * Echo (state/ring/packs/<id>):
 *     hassmic ring pack 2
 *     name <text>                   required, up to RD_NAME - 1 bytes; author the same, optional
 *     about <text>                  optional, up to 199 bytes
 *     shot <style>                  optional: how the page draws its packshot ([a-z] up to 15)
 *     has <design id>               a design in the pack: a folder of the library, a design in one at most
 *     use <slot> <design id> | stock   slot ids as ring.h's (others kept: another model's); the id one of its own
 * Shared (a .ringpack file, a copy to another Echo) a pack carries its designs: version 1, the same lines with
 * "design" + a design's text + "end" blocks and "use <slot> <n>" naming the n-th of them.  The page writes and reads
 * that; the Echo keeps version 2 only.  A design deleted leaves the packs naming it.
 * What is applied (state/ring-applied, read by root's hassmic -G as well) is one file of all assigned designs:
 *     hassmic ring applied 1
 *     slot <slot id> <design id>
 *     <the design's text>
 *     end
 */
#ifndef RINGDESIGN_H
#define RINGDESIGN_H
#include <stddef.h>

#define RD_MAX         100              /* designs in the library */
#define RD_FRAMES      1000             /* frames per design: stock's longest (call_connected) is 1758 lines, mostly repeats */
#define RD_LEDS_MAX    24
#define RD_NAME        48
#define RD_TEXT_MAX    (RD_FRAMES * (RD_LEDS_MAX * 7 + 8) + 256)     /* the longest design as text */
#define RD_LIBRARY_MAX (4 << 20)        /* all designs together on /data */
#define RD_APPLIED_MAX (8 << 20)
#define RD_PACKS_MAX   50               /* theme packs kept */
#define RD_PACK_MAX    8192             /* one pack as the Echo keeps it: names and a line per animation */

struct rd_frame { int ms; unsigned char px[RD_LEDS_MAX][3]; };
struct rd_design { char name[RD_NAME]; int leds, nframes, loop; struct rd_frame *f; };     /* loop -1: once */

/* Parses and checks a design (as above).  0, or -1 with the reason in err */
int   rd_parse(const char *text, size_t n, struct rd_design *d, char *err, size_t cap);
char *rd_format(const struct rd_design *d);          /* malloc'd text, as rd_parse reads it */
void  rd_free(struct rd_design *d);

/* What state/ring-applied (or TEXT) assigns: out[i] the design for ring_slots[i] (nframes 0: none), ids[i] its id.
 * 0, or -1 if the file is not ours */
int   rd_applied_parse(const char *text, size_t n, struct rd_design *out, int *ids);
/* state/ring-applied as ring.c reads it, for root too: a regular file only (O_NOFOLLOW: state/ is the daemon's, root
 * reads it), at most RD_APPLIED_MAX.  malloc'd, NULL if there is none */
char *rd_applied_read(size_t *n);
/* state/ring-installed: those of them root is known to have installed (ring.c confirms by the set's digest) */
char *rd_installed_read(size_t *n);
int   rd_installed_set(const char *text, size_t n);   /* 0, or -1 (logged) */

/* The settings page's side (web.c), each under the module's lock */
size_t rd_status_json(char *out, size_t cap);         /* slots, the library's names, what is on the ring */
char  *rd_library_text(size_t *n);                     /* every design, "design <id>\n" before each */
int   rd_save(int id, const char *text, size_t n, int *saved, char *err, size_t cap);    /* id 0: a new one */
int   rd_delete(int id, char *err, size_t cap);        /* and wherever it is assigned, stock's again */
int   rd_assign(const char *body, char *err, size_t cap);   /* "<slot> <id>" lines, 0: stock's */
int   rd_apply(char *err, size_t cap);                 /* the assignments to the ring: root installs them */
char *rd_stock(const char *slot, size_t *n);           /* stock's animation for a slot, as the file has it (volume:
                                                        * every step, "step NN" before each) */
char *rd_packs_text(size_t *n);                         /* every pack, "pack <id>\n" before each */
int   rd_pack_save(int id, const char *text, size_t n, int *saved, char *err, size_t cap);  /* version 2, its designs in the library; id 0: new */
int   rd_pack_delete(int id, char *err, size_t cap);
void  rd_start(void);                                   /* at a start, after ring_start: a request still out, a draft left */
void  rd_tick(void);                                    /* about once a second: root's answer to an apply */
#endif

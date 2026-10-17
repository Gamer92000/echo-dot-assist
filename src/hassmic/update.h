/* Online updates: the project's releases on GitHub, installed through root's installer like a push (update.c). */
#ifndef UPDATE_H
#define UPDATE_H

#include <stddef.h>

enum { UPDATE_OFF, UPDATE_BETA, UPDATE_RELEASE };
extern const char *const update_channels[3];    /* "off", "beta", "release": the select's options and the settings file */

struct update_state {
    int in_progress;                /* downloading or installing */
    float progress;                 /* percent of the download, while in_progress */
    char current[64];               /* what runs: VERSION (commit time, UTC) if CI published it, else VERSION+BUILD */
    char latest[64];                /* newest on the channel; = current while off or not looked yet */
    char summary[256];              /* release notes (cut short), or why the last look or install failed */
    char url[192];                  /* the release's page */
};

void update_start(void (*changed)(void));   /* thread; changed(): update_get() changed, called without locks */
int  update_channel(int ch);                /* UPDATE_*, or -1 to read; a new channel is looked at at once */
void update_check(void);                    /* look now */
void update_install(void);                  /* download and install the newest on the channel, if it is not what runs */
void update_get(struct update_state *s);
int  update_probe(char *err, size_t cap);         /* reaches GitHub? 0, or -1 with why; blocks up to 15 s, no locks held */
#endif

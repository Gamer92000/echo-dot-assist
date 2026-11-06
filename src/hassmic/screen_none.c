/* screen.h for models without a screen: nothing */
#include "screen.h"
#include <stdio.h>

int  screen_present(void) { return 0; }
void screen_start(void) {}
void screen_led(const char *op, const char *pattern) { (void)op; (void)pattern; }
void screen_level(int dbfs10) { (void)dbfs10; }
void screen_tz(const char *posix_tz) { (void)posix_tz; }
void screen_music_meta(const char *title, const char *artist, const char *album) { (void)title; (void)artist; (void)album; }
void screen_music_progress(int state, long long pos_ms, long long dur_ms, int speed) { (void)state; (void)pos_ms; (void)dur_ms; (void)speed; }
void screen_music_controller(int volume, int muted, const char *commands, const char *repeat, int shuffle) { (void)volume; (void)muted; (void)commands; (void)repeat; (void)shuffle; }
void screen_music_art(const void *image, size_t n) { (void)image; (void)n; }
void screen_music_gone(void) {}
int  screen_ui(void) { return 0; }
int  screen_width(void) { return 0; }
int  screen_band(const void *argb, int w) { (void)argb; (void)w; return -1; }
void screen_line(const char *line) { (void)line; }
void screen_bin(const char *line, const void *data, size_t n) { (void)line; (void)data; (void)n; }
size_t screen_status_json(char *o, size_t cap) { (void)o; (void)cap; return 0; }
/* no screen of hassmic's own (the Dots, the PC): the home app named by Echo Local is still kept, for kiosk.c (the PC's
 * tests of the contract, tests/ha_real_kiosk.py) */
static char home_[128];
int  screen_set_home(const char *pkg) { snprintf(home_, sizeof home_, "%s", pkg ? pkg : ""); return 0; }
int  screen_home(char *out, size_t cap) { snprintf(out, cap, "%s", home_); return out[0] ? 0 : -1; }
void screen_claim(const char *pkg, int mask) { (void)pkg; (void)mask; }
void screen_screens(const char *names) { (void)names; }

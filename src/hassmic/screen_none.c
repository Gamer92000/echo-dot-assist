/* screen.h for models without a screen: nothing */
#include "screen.h"

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
int  screen_set_home(const char *pkg) { (void)pkg; return -1; }
void screen_words(int what, const char *text) { (void)what; (void)text; }
int  screen_words_on(int set) { (void)set; return -1; }
int  screen_words_hold(int set) { (void)set; return -1; }
int  screen_timers_on(int set) { (void)set; return -1; }
void screen_timers_changed(void) {}
int  screen_keep_on(void) { return 0; }

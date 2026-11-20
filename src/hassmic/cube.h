#ifndef CUBE_H
#define CUBE_H
/* Root-only local IPC to the Android audio helper; never sent over the LAN. */
int cube_control(int command,int value);
int cube_indicator(const char *op,const char *pattern);
int cube_indicator_transition(const char *from,const char *to);
int cube_extra_duck(int value);
enum { CUBE_OUTPUT_MODE, CUBE_OUTPUT_VOLUME, CUBE_TV_VOLUME, CUBE_MUSIC_DUCK, CUBE_ROUTE_SETTINGS };
extern const char *const cube_route_names[CUBE_ROUTE_SETTINGS];
extern const char *const cube_output_modes[3];
int cube_route_setting(int field,int value);
void cube_route_text(int field,char *out,unsigned cap);
#endif

#ifndef CUBE_H
#define CUBE_H
/* Root-only local IPC to the Android audio helper; never sent over the LAN. */
int cube_control(int command,int value);
int cube_indicator(const char *op,const char *pattern);
int cube_indicator_transition(const char *from,const char *to);
int cube_extra_duck(int value);
#endif

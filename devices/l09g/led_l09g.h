#ifndef L09G_LED_H
#define L09G_LED_H
struct l09g_light { int on; float brightness, color_brightness, r, g, b; };
int l09g_led_start(void);
void l09g_led_event(const char *op, const char *pattern);
void l09g_light_get(struct l09g_light *light);
void l09g_light_set(const struct l09g_light *light);
#endif

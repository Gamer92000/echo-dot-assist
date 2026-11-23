/* Check visible state transitions and restoration without opening the LEDs. */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include "led_l09g.c"

static atomic_int frame_count, low = 255, high;
int ioctl(int fd, unsigned long request, ...)
{
    (void)fd; (void)request;
    va_list ap; va_start(ap, request); struct frame *f = va_arg(ap, struct frame *); va_end(ap);
    int green = f->pixels[1];
    if (green < atomic_load(&low)) atomic_store(&low, green);
    if (green > atomic_load(&high)) atomic_store(&high, green);
    atomic_fetch_add(&frame_count, 1);
    return 0;
}
static void frame(uint8_t *out, struct visual s, long long now)
{
    memset(out, 0, 72); render(out, &s, now);
}

int main(void)
{
    uint8_t a[72], b[72];
    struct visual s = { .volume = -1, .light = { 1, 0.5f, 1, 1, 0, 0 } };
    frame(a, s, 1000); assert(a[0] > 0 && a[1] == 0);
    s.call_state = 2; frame(a, s, 1000); frame(b, s, 1375);
    assert(a[1] > 0 && a[0] == 0 && memcmp(a, b, sizeof a));
    s.call_state = 3; frame(a, s, 2000); frame(b, s, 2500);
    assert(a[1] == 110 && !memcmp(a, b, sizeof a));
    s.muted = 1; frame(a, s, 2000); assert(a[0] == 100 && a[1] == 0);
    s.muted = 0; s.call_state = 0; s.activity = 1; s.wake_at = 1000;
    frame(a, s, 1000); frame(b, s, 1000 + WAKE_MS * 3 / 4);
    assert(a[1] > 0 && a[71] == 0 && b[46] > 0);
    s.activity = 0; s.wake_at = 0; s.end_at = 2000;
    frame(a, s, 2000); frame(b, s, 2000 + END_MS * 4 / 5); assert(a[2] > b[2] && b[2] > 0);
    frame(a, s, 2000 + END_MS + 100); assert(a[0] > 0 && a[1] == 0 && a[2] == 0);

    l09g_led_event("-s", "call_incoming"); assert(call_state == 2);
    l09g_led_event("-u", "call_incoming"); l09g_led_event("-s", "call_connected"); assert(call_state == 3);
    l09g_led_event("-u", "call_connected"); assert(call_state == 0);
    l09g_led_event("-s", "active-talking"); l09g_led_event("-u", "active-talking");
    l09g_led_event("-s", "active-end"); assert(!activity && end_at > 0);
    setenv("L09G_LED_DEVICE", "/dev/null", 1);
    assert(!l09g_led_start());
    l09g_led_event("-s", "call_incoming");
    usleep(2100000);
    assert(atomic_load(&frame_count) > 10 && atomic_load(&high) - atomic_load(&low) > 160);
    puts("LED transitions, animated worker and priority passed");
    return 0;
}

/* LX05 single RGB LED: kernel's packed colour order is G,R,B. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int muted, activity, call, volume, attention;

static void *worker(void *unused)
{
    (void)unused;
    unsigned previous = ~0u;
    for (;;) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        pthread_mutex_lock(&lock);
        int m=muted, a=activity, c=call, v=volume, auth=attention;
        pthread_mutex_unlock(&lock);
        unsigned r=0, g=0, b=0;
        if (m) r=55;
        else if (auth) { r=55; g=35; b=5; }
        else if (c) {
            /* Slow pulse for ringing, steady green while connected. */
            unsigned level = c == 3 ? 55 : ((now.tv_sec*1000 + now.tv_nsec/1000000) % 1200 < 600 ? 55 : 8);
            g=level;
        } else if (a == 1) { g=40; b=70; }
        else if (a == 2) { r=32; b=55; }
        else if (a == 3) b=60;
        else if (v) { r=25; g=25; b=25; }
        unsigned grb = (g<<16) | (r<<8) | b;
        if (grb != previous) {
            FILE *f=fopen("/sys/class/leds/sunxi_led0a/brightness", "w");
            if (f) { if (fprintf(f,"%u\n",grb)>0) previous=grb; fclose(f); }
        }
        usleep(80000);
    }
}
int lx05_led_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL)) return -1;
    pthread_detach(t); return 0;
}
void lx05_led_event(const char *op, const char *p)
{
    int on=!strcmp(op,"-s");
    pthread_mutex_lock(&lock);
    if (!strcmp(p,"mics-off_on")) muted=on;
    else if (!strcmp(p,"active-start")) { if(on) activity=1; else if(activity==1) activity=0; }
    else if (!strcmp(p,"active-thinking")) { if(on) activity=2; else if(activity==2) activity=0; }
    else if (!strcmp(p,"active-talking")) { if(on) activity=3; else if(activity==3) activity=0; }
    else if (!strcmp(p,"active-end") && on) activity=0;
    else if (!strcmp(p,"call_outbound_ringing")) { if(on) call=1; else if(call==1) call=0; }
    else if (!strcmp(p,"call_incoming")) { if(on) call=2; else if(call==2) call=0; }
    else if (!strcmp(p,"call_connected")) { if(on) call=3; else if(call==3) call=0; }
    else if (!strcmp(p,"authenticated_setup_mode")) attention=on;
    else if (!strncmp(p,"volume_step-",12)) volume=on;
    pthread_mutex_unlock(&lock);
}

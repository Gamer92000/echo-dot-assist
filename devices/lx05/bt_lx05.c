#include "duck.h"
#include "settings.h"
#include "statefile.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int lx05_bt_pair(int set)
{
    char path[320];
    const char *state=getenv("HASSMIC_STATE");
    snprintf(path,sizeof path,"%s/bluetooth.pairing",state ? state : "/data/lx05-assist/state");
    if (set >= 0) {
        const char *v=set ? "1\n" : "0\n";
        if (statefile_write(path,0600,v,2)) fprintf(stderr,"bluetooth: cannot save pairing setting\n");
    }
    FILE *f=fopen(path,"r");
    if (!f) return 0;
    int c=fgetc(f); fclose(f); return c=='1';
}

static void *duck_worker(void *unused)
{
    (void)unused;
    int previous=-1, refresh=0;
    for (;;) {
        float gain=duck_active() ? settings_duck_gain() : 1.0f;
        if (!isfinite(gain) || gain < 0.0f) gain=0.0f;
        if (gain > 1.0f) gain=1.0f;
        /* ALSA softvol: 255 is 0 dB, 0 is -51 dB. */
        int raw = gain <= 0.002818f ? 0 : (int)lroundf(255.0f + 100.0f*log10f(gain));
        if (raw < 0) raw=0; if (raw > 255) raw=255;
        /* Retry while ducked in case the Bluetooth stream was opened after
         * the request; an idle, unopened softvol defaults to full volume. */
        if (raw != previous || (raw < 255 && ++refresh >= 20)) {
            char cmd[150];
            snprintf(cmd,sizeof cmd,"amixer -q -D hw:0 cset name=lx05_bt_duck %d,%d >/dev/null 2>&1",raw,raw);
            (void)system(cmd); previous=raw; refresh=0;
        }
        usleep(100000);
    }
}
int lx05_bt_duck_start(void)
{
    pthread_t t;
    if (pthread_create(&t,NULL,duck_worker,NULL)) return -1;
    pthread_detach(t); return 0;
}

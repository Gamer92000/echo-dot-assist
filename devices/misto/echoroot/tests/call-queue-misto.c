#include "audio.h"
#include <stdio.h>
#include <unistd.h>
int misto_lipc_int(const char *s, const char *p, const int *in, int *out) { (void)s; (void)p; (void)in; (void)out; return -1; }
int main(void) {
    short pcm[320] = {0};
    if (voip_open(16000, 1)) return 1;
    for (int i=0; i<20; i++) {
        while (voip_queued_us()>40000) usleep(4000);
        if (voip_write(pcm, sizeof pcm)) return 2;
    }
    long long q=voip_queued_us();
    voip_close();
    if (q<0 || q>65000 || voip_queued_us()!=0) return 3;
    puts("call queue bounded and reset");
    return 0;
}

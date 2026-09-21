/*
 * mixcap - read a capture stream from Amazon's mixer daemon and write raw PCM.
 *
 *   mixcap [-t type] [-c channels] [-s seconds] [-l port]
 *
 * Default: micAsr (post-AEC/beamformer, 16 kHz mono s16le) to stdout, forever.
 * With -l, waits for one TCP client at a time and streams to it.
 * PuffinApp must be stopped first: micAsr has a single consumer.
 */
#include <signal.h>
#include <stdlib.h>
#include "mixer_api.h"
#include "netio.h"

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int stream(const char *type, unsigned ch, int fd, double seconds)
{
    MixerHandle h = MixerOpenRecCh(type, ch);
    if (!h) { fprintf(stderr, "mixcap: MixerOpenRecCh(%s,%u) failed\n", type, ch); return -1; }
    unsigned rate = MixerGetRate(h), bits = MixerGetSampleSizeBits(h), nch = MixerGetNumCh(h);
    fprintf(stderr, "mixcap: %s rate=%u ch=%u bits=%u name=%s\n", type, rate, nch, bits, MixerGetStreamName(h));
    /* On device the getters return -1 for record streams; micAsr was measured at 16 kHz mono pcm16. */
    if ((int)rate <= 0) rate = 16000;
    if ((int)bits <= 0) bits = 16;
    if ((int)nch <= 0) nch = ch ? ch : 1;
    unsigned long long limit = seconds > 0 ? (unsigned long long)(seconds * rate) * nch * (bits / 8) : 0, total = 0;
    int misses = 0, rc = 0;
    while (!stop && (!limit || total < limit)) {
        int status = 0; unsigned n = 0;
        void *buf = MixerGetBufRec(h, &status, &n);
        if (!buf) {
            fprintf(stderr, "mixcap: no data (status=%d)\n", status);
            if (++misses > 5) { rc = -1; break; }
            continue;
        }
        misses = 0;
        int w = write_all(fd, buf, n);
        MixerReleaseBufRec(h);
        if (w < 0) { rc = 1; break; }       /* peer went away */
        total += n;
    }
    MixerClose(h);
    fprintf(stderr, "mixcap: %llu bytes\n", total);
    return rc;
}

int main(int argc, char **argv)
{
    const char *type = MIXER_REC_ASR; unsigned ch = 1; double seconds = 0; int port = 0, o;
    while ((o = getopt(argc, argv, "t:c:s:l:")) != -1) switch (o) {
        case 't': type = optarg; break;
        case 'c': ch = atoi(optarg); break;
        case 's': seconds = atof(optarg); break;
        case 'l': port = atoi(optarg); break;
        default: fprintf(stderr, "usage: mixcap [-t type] [-c channels] [-s seconds] [-l port]\n"); return 2;
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    if (!port) return stream(type, ch, STDOUT_FILENO, seconds) ? 1 : 0;

    int ls = net_listen(port);
    if (ls < 0) { perror("mixcap: listen"); return 1; }
    while (!stop) {
        int c = net_accept(ls);
        if (c < 0) break;
        int rc = stream(type, ch, c, seconds);
        close(c);
        if (rc < 0) return 1;
    }
    return 0;
}

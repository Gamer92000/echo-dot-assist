/*
 * mixplay - play raw s16le PCM through Amazon's mixer daemon.
 *
 *   mixplay [-t type] [-r rate] [-c channels] [-l port]
 *
 * Default: stdin, 22050 Hz mono, stream type TTS.  Audio goes through the mixer,
 * so it is part of the AEC reference.  With -l, plays each TCP connection until it closes.
 */
#include <signal.h>
#include <stdlib.h>
#include "mixer_api.h"
#include "netio.h"

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int play(const char *type, unsigned rate, unsigned ch, int fd)
{
    MixerHandle h = MixerOpenPlay(rate, ch, 16, type);
    if (!h) { fprintf(stderr, "mixplay: MixerOpenPlay(%u,%u,16,%s) failed\n", rate, ch, type); return -1; }
    fprintf(stderr, "mixplay: %s rate=%u ch=%u name=%s\n", type, rate, ch, MixerGetStreamName(h));
    unsigned long long total = 0; int rc = 0;
    while (!stop) {
        int status = 0; unsigned cap = 0;
        void *buf = MixerGetBufPlay(h, &status, &cap);
        if (!buf) { fprintf(stderr, "mixplay: no buffer (status=%d)\n", status); rc = -1; break; }
        ssize_t n = read_full(fd, buf, cap);
        if (n <= 0) { MixerReleaseBufPlay(h, 0); break; }
        n -= n % (2 * ch);
        MixerReleaseBufPlay(h, n);
        total += n;
        if ((unsigned)n < cap) break;
    }
    MixerDrain(h);
    MixerClose(h);
    fprintf(stderr, "mixplay: %llu bytes\n", total);
    return rc;
}

int main(int argc, char **argv)
{
    const char *type = MIXER_PLAY_TTS; unsigned rate = 22050, ch = 1; int port = 0, o;
    while ((o = getopt(argc, argv, "t:r:c:l:")) != -1) switch (o) {
        case 't': type = optarg; break;
        case 'r': rate = atoi(optarg); break;
        case 'c': ch = atoi(optarg); break;
        case 'l': port = atoi(optarg); break;
        default: fprintf(stderr, "usage: mixplay [-t type] [-r rate] [-c channels] [-l port]\n"); return 2;
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    if (!port) return play(type, rate, ch, STDIN_FILENO) ? 1 : 0;

    int ls = net_listen(port);
    if (ls < 0) { perror("mixplay: listen"); return 1; }
    while (!stop) {
        int c = net_accept(ls);
        if (c < 0) break;
        int rc = play(type, rate, ch, c);
        close(c);
        if (rc < 0) return 1;
    }
    return 0;
}

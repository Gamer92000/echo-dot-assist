/*
 * Speaker gain (issue #19): louder than stock on the Echo's own speaker.
 *
 * The volume is digital (the mixer hands the HAL a stream_volume, 0.398 at 50 and 1.0 at 100), so at 100 the DAC is at
 * full scale and nothing digital goes higher without clipping.  On the Echo Dot 2 the DAC is a TI TLV320AIC32x4 whose
 * headphone driver feeds the external speaker amplifier; its "HP Driver Gain Volume" (0..35 = -6..+29 dB, 1 dB a step,
 * both channels) is what the audio HAL's ext_speaker_output path in /system/etc/audio_device.xml sets: 6, 0 dB.  Raising
 * it raises everything the speaker plays by as much, at every volume.  Users of the Dot 2 found +6..+7 dB loud and clean
 * at full volume, +9 and +14 fine on theirs; at +29 one unit was loud and clean at half volume, another of the same model
 * far too loud (EchoMuse issue 630: two speaker or amplifier variants, no known way to tell them apart).  So the page
 * offers up to +12 dB (SPKGAIN_MAX) and says what it risks: with more analog gain the amplifier clips on loud passages near
 * full volume, and a speaker fed clipped audio for long can be damaged.
 *
 * Who puts it back (Echo Dot 2, 2026-10-11): not the mixer's volume (MainVolume/TTSVolume set while a stream plays: the
 * gain stayed), not a stream starting or going to standby (the HAL logs mDacOn=1: Amazon keeps the DAC on, and the path is
 * only applied when the output device starts): the mixer starting (at boot, or restarted: the HAL's init sequence writes
 * 0, then ext_speaker_output 6), and by the XML a change of route (the 3.5 mm jack's ext_headphone_output sets 11).
 * With the watcher below, the mixer restarted: the HAL's 6 at .570 s, ours at .586.
 * hassmic runs as the stock client's user with group audio, and /dev/snd/controlC0 is system:audio 0660: no root needed.
 * A watcher subscribed to the card's control events sets it again as soon as the HAL writes stock, and leaves any other
 * value alone (the jack's level for a line-in on a stereo, or one set by hand); a poll every 10 s covers a write that
 * sends no event.  Started with the first gain above stock: an Echo left at +0 dB never writes to the card.
 */
#include "spkgain.h"
#include "board.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CTL_DEV "/dev/snd/controlC0"
#define POLL_MS 10000

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int present = -1, fd = -1, wanted, written = -1, started, hi, count;
static unsigned numid;
static const char *fake;                /* HASSMIC_FAKE_SPEAKER_GAIN: a file standing in for the control (tests) */

static int stock(void) { return board.speaker_gain ? board.speaker_gain_stock : 6; }

static int ctl_read(void)
{
    if (fake) { FILE *f = fopen(fake, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v; }
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v); v.id.numid = numid;
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0 ? -1 : (int)v.value.integer.value[0];
}

static int ctl_write(int x)
{
    if (fake) { FILE *f = fopen(fake, "w"); if (!f) return -1; fprintf(f, "%d\n", x); return fclose(f) ? -1 : 0; }
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v); v.id.numid = numid;
    for (int i = 0; i < count; i++) v.value.integer.value[i] = x;
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

static int probe(void)
{
    struct snd_ctl_elem_list l; struct snd_ctl_elem_id *ids; struct snd_ctl_elem_info in;
    if ((fake = getenv("HASSMIC_FAKE_SPEAKER_GAIN"))) { hi = 35; count = 1; return 1; }
    if (!board.speaker_gain) return 0;
    if ((fd = open(CTL_DEV, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) { fprintf(stderr, "speaker gain: %s: %s\n", CTL_DEV, strerror(errno)); return 0; }
    memset(&l, 0, sizeof l);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0 || !l.count || !(ids = calloc(l.count, sizeof *ids))) goto none;
    l.space = l.count; l.pids = ids;
    if (!ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l))
        for (unsigned i = 0; i < l.used && !numid; i++) if (!strcmp((const char *)ids[i].name, board.speaker_gain)) numid = ids[i].numid;
    free(ids);
    memset(&in, 0, sizeof in); in.id.numid = numid;
    if (!numid || ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &in) < 0 || in.type != SNDRV_CTL_ELEM_TYPE_INTEGER
        || in.value.integer.max <= stock() || in.count < 1 || in.count > 2) goto none;
    hi = (int)in.value.integer.max; count = (int)in.count;
    fprintf(stderr, "speaker gain: \"%s\" %d..%d, now %d\n", board.speaker_gain, (int)in.value.integer.min, hi, ctl_read());
    return 1;
none:
    fprintf(stderr, "speaker gain: no control \"%s\" on card 0\n", board.speaker_gain);
    close(fd); fd = -1;
    return 0;
}

/* mu held.  Stock or what we wrote: the HAL's speaker path, or ours: the setting's.  Anything else stays. */
static void apply(void)
{
    int cur = ctl_read(), target = stock() + wanted;
    if (cur < 0 || cur == target) { if (cur == target) written = target; return; }
    if (cur != stock() && cur != written) return;
    if (ctl_write(target) < 0) { fprintf(stderr, "speaker gain: cannot set %d: %s\n", target, strerror(errno)); return; }
    fprintf(stderr, "speaker gain: %+d dB (%s %d, was %d)\n", wanted, fake ? "fake" : board.speaker_gain, target, cur);
    written = target;
}

static void *watch(void *arg)
{
    (void)arg;
    for (;;) {
        if (fake) usleep(200000);
        else {
            struct pollfd p = { fd, POLLIN, 0 }; struct snd_ctl_event ev[8];
            if (poll(&p, 1, POLL_MS) > 0) while (read(fd, ev, sizeof ev) > 0) {}      /* which control: apply() reads ours */
        }
        pthread_mutex_lock(&mu); apply(); pthread_mutex_unlock(&mu);
    }
    return NULL;
}

int spkgain_present(void)
{
    pthread_mutex_lock(&mu);
    if (present < 0) present = probe();
    int r = present;
    pthread_mutex_unlock(&mu);
    return r;
}

int spkgain_get(void) { pthread_mutex_lock(&mu); int r = wanted; pthread_mutex_unlock(&mu); return r; }

void spkgain_set(int db)
{
    if (!spkgain_present()) return;
    pthread_mutex_lock(&mu);
    if (db > hi - stock()) db = hi - stock();
    wanted = db < 0 ? 0 : db > SPKGAIN_MAX ? SPKGAIN_MAX : db;
    if (wanted || started) apply();
    if (wanted && !started) {
        int one = 1; pthread_t t;
        if (!fake && ioctl(fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &one) < 0) fprintf(stderr, "speaker gain: no control events (%s), polled\n", strerror(errno));
        if (!pthread_create(&t, NULL, watch, NULL)) { pthread_detach(t); started = 1; }
    }
    pthread_mutex_unlock(&mu);
}

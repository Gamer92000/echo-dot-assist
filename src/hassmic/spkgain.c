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
 *
 * Echo Dot 3 (donut, EVT..HVT boards, 2026-10-11): the speaker amplifier is a TI TAS2770 at i2c 2-0044 on I2S; the
 * jack's path in mixer_paths.xml cuts that I2S and mutes it, and the jack plays from the MT8167's own codec.  The
 * driver's two controls, read from the kernel's soc_mixer_control and TLV tables: "PCM Playback Volume" (reg 0x05,
 * inverted, 0..255 = -127.5..0 dB), the amplifier's digital volume, 255 in the speaker path and 0 (muted) in the
 * jack's: no headroom.  "DAC Playback Volume" (reg 0x03, AMP_LEVEL, 0..20 = 11..21 dBV full-scale output, 0.5 dB a
 * step: board.speaker_gain_steps 2) is 16, 19 dBV, from boot on: no firmware file names it, and neither the HAL, a
 * mixer restart (it only reads it), nor a stream starting or stopping writes it (the driver logs every register write
 * to dmesg).  So +2 dB is all there is (spkgain_max()).  21 dBV is 15.9 V peak, above the 15 V adapter's: if that is the
 * amplifier's supply (not measured), samples near full scale clip by a fraction of a dB at the top step.
 *
 * Echo Show 5 (checkers, from the firmware only, 2026-10-13): the codec is a Realtek RT5616 whose line out feeds the
 * external speaker amplifier (/system/etc/audio_device.xml, the file the HAL reads: "LO out", ext_speaker_output switches
 * "OUT Playback Switch" and Ext_Speaker_Amp_Switch).  Its "OUT Playback Volume" (reg 0x03, both channels, inverted,
 * 0..39 = -46.5..+12 dB, 1.5 dB a step: the soc_mixer_control and TLV in checkers' own kernel) is set to 29, -3 dB, among
 * the XML's initial settings, which the HAL applies at its start; no path and no other file of the firmware names it.
 * Ten steps above stock: up to +15 dB, of which the page offers 12 (board.speaker_gain_steps 2 per speaker_gain_db 3).
 *
 * Echo Show 5 2nd gen (cronos, from the firmware only, 2026-10-26): one firmware for boards with three different
 * amplifiers (a device tree per revision; RT5616/RT5616E as checkers, TAS5805M, MAX98396), each with its own control.
 * board.speaker_gain_alt lists the others; the first name card 0 has is the one, the rest of a board_gain with it.
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
static int present = -1, fd = -1, wanted, written = -1, started, hi, count, stock, steps, per, lodb;
static unsigned numid;
static const char *ctl_name;           /* the control found: board.speaker_gain or one of speaker_gain_alt */
/* HASSMIC_FAKE_SPEAKER_GAIN=<file>[:<stock>:<steps>:<max>[:<per dB>[:<lowest dB, 0 or negative>]]]: a file standing in for the control (tests),
 * shaped as the Echo Dot 2's (6:1:35:1) unless told otherwise, whatever the board */
static const char *fake;
static char fake_path[256];

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
    const char *env = getenv("HASSMIC_FAKE_SPEAKER_GAIN");
    if (env) {
        const char *c = strchr(env, ':');
        snprintf(fake_path, sizeof fake_path, "%.*s", c ? (int)(c - env) : (int)strlen(env), env);
        stock = 6; steps = 1; hi = 35; per = 1; count = 1; lodb = 0; fake = fake_path;
        if (c && sscanf(c, ":%d:%d:%d:%d:%d", &stock, &steps, &hi, &per, &lodb) < 3) { stock = 6; steps = 1; hi = 35; per = 1; lodb = 0; }
        if (lodb > 0) lodb = 0;
        return steps > 0 && per > 0 && (hi - stock) * per >= steps;
    }
    if (!board.speaker_gain) return 0;
    /* the board's control, then the other amplifiers it may have (board.speaker_gain_alt): the first one on the card */
    struct board_gain g = { board.speaker_gain, board.speaker_gain_stock, board.speaker_gain_steps, board.speaker_gain_db,
                            board.speaker_gain_min_db, board.speaker_gain_limit };
    const struct board_gain *alt = board.speaker_gain_alt;
    if ((fd = open(CTL_DEV, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) { fprintf(stderr, "speaker gain: %s: %s\n", CTL_DEV, strerror(errno)); return 0; }
    memset(&l, 0, sizeof l);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0 || !l.count || !(ids = calloc(l.count, sizeof *ids))) goto none;
    l.space = l.count; l.pids = ids;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l)) l.used = 0;
    for (;;) {
        for (unsigned i = 0; i < l.used && !numid; i++) if (!strcmp((const char *)ids[i].name, g.name)) numid = ids[i].numid;
        if (numid || !alt || !alt->name) break;
        g = *alt++;
    }
    free(ids);
    ctl_name = g.name;
    stock = g.stock; steps = g.steps > 0 ? g.steps : 1; per = g.db > 0 ? g.db : 1;
    memset(&in, 0, sizeof in); in.id.numid = numid;
    if (!numid || ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &in) < 0 || in.type != SNDRV_CTL_ELEM_TYPE_INTEGER
        || (in.value.integer.max - stock) * per < steps || in.count < 1 || in.count > 2) goto none;
    hi = (int)in.value.integer.max; count = (int)in.count;
    lodb = g.min_db < 0 ? g.min_db : 0;
    int room = -((stock - (int)in.value.integer.min) * per / steps);       /* whole dB below stock the control has */
    if (lodb < room) lodb = room;
    if (g.limit > stock && g.limit < hi) hi = g.limit;
    fprintf(stderr, "speaker gain: \"%s\" %d..%d, stock %d, now %d\n", ctl_name, (int)in.value.integer.min, hi, stock, ctl_read());
    return 1;
none:
    fprintf(stderr, "speaker gain: no control \"%s\"%s on card 0\n", board.speaker_gain, board.speaker_gain_alt ? " (nor another of the board's)" : "");
    close(fd); fd = -1;
    return 0;
}

/* mu held.  Stock or what we wrote: the HAL's speaker path, or ours: the setting's.  Anything else stays.  The step
 * nearest the setting's whole dB where a step is not one (the Echo Show 5's 1.5 dB: +1 and +2 are both +1.5) */
static void apply(void)
{
    int cur = ctl_read(), target = stock + (wanted * steps + (wanted < 0 ? -per : per) / 2) / per;
    if (cur < 0 || cur == target) { if (cur == target) written = target; return; }
    if (cur != stock && cur != written) return;
    if (ctl_write(target) < 0) { fprintf(stderr, "speaker gain: cannot set %d: %s\n", target, strerror(errno)); return; }
    fprintf(stderr, "speaker gain: %+d dB (%s %d, was %d)\n", wanted, fake ? "fake" : ctl_name, target, cur);
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

/* whole dB below stock the page offers (0 or negative): board.speaker_gain_min_db, as far as the control goes */
int spkgain_min(void)
{
    if (!spkgain_present()) return 0;
    pthread_mutex_lock(&mu);
    int r = lodb;
    pthread_mutex_unlock(&mu);
    return r;
}

int spkgain_get(void) { pthread_mutex_lock(&mu); int r = wanted; pthread_mutex_unlock(&mu); return r; }

/* whole dB the control has above stock, at most SPKGAIN_MAX: the Dot 2's (35 - 6) / 1 -> 12, the Dot 3's (20 - 16) / 2 -> 2,
 * the Echo Show 5's (39 - 29) * 3 / 2 -> 15 */
int spkgain_max(void)
{
    if (!spkgain_present()) return 0;
    pthread_mutex_lock(&mu);
    int r = (hi - stock) * per / steps;
    pthread_mutex_unlock(&mu);
    return r < SPKGAIN_MAX ? r : SPKGAIN_MAX;
}

void spkgain_set(int db)
{
    int top = spkgain_max();
    if (!top) return;
    pthread_mutex_lock(&mu);
    wanted = db < lodb ? lodb : db > top ? top : db;
    if (wanted || started) apply();
    if (wanted && !started) {
        int one = 1; pthread_t t;
        if (!fake && ioctl(fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &one) < 0) fprintf(stderr, "speaker gain: no control events (%s), polled\n", strerror(errno));
        if (!pthread_create(&t, NULL, watch, NULL)) { pthread_detach(t); started = 1; }
    }
    pthread_mutex_unlock(&mu);
}

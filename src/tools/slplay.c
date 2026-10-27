/*
 * slplay - raw PCM through Android's AudioFlinger, as hassmic's players play it (audio_android.c: OpenSL ES, the music
 * stream): the Android models' mixplay.  The music stream's volume index applies, so what it plays at each index is what
 * hassmic's sounds do there (tools/vol-curve.py measures the curve with it).
 *
 *   slplay [-r rate] [-c channels] < pcm      s16le, default 48000 Hz mono; plays to the end of its input
 */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NBUF 4
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int busy;                                /* buffers with the player */

/* lock held: until busy < limit, 2 s at most (a buffer is 20 ms: the player stopped).  0, or -1 */
static int wait_busy(int limit)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
    while (busy >= limit) if (pthread_cond_timedwait(&cond, &lock, &ts) == ETIMEDOUT) { fprintf(stderr, "slplay: the player stopped taking buffers\n"); return -1; }
    return 0;
}

static void done(SLAndroidSimpleBufferQueueItf q, void *ctx)
{
    (void)q; (void)ctx;
    pthread_mutex_lock(&lock); busy--; pthread_cond_signal(&cond); pthread_mutex_unlock(&lock);
}

static size_t fill(char *b, size_t n)           /* whole reads: the last buffer of the input may be short */
{
    size_t got = 0;
    while (got < n) { ssize_t r = read(0, b + got, n - got); if (r <= 0) break; got += (size_t)r; }
    return got & ~(size_t)1;
}

int main(int argc, char **argv)
{
    unsigned rate = 48000, ch = 1; int o;
    while ((o = getopt(argc, argv, "r:c:")) != -1) {
        if (o == 'r') rate = (unsigned)atoi(optarg);
        else if (o == 'c') ch = (unsigned)atoi(optarg);
        else { fprintf(stderr, "usage: slplay [-r rate] [-c channels] < s16le\n"); return 2; }
    }
    if (!rate || ch < 1 || ch > 2) return 2;
    SLObjectItf eo, mo, po; SLEngineItf e; SLPlayItf play; SLAndroidSimpleBufferQueueItf q;
    if (slCreateEngine(&eo, 0, NULL, 0, NULL, NULL) || (*eo)->Realize(eo, SL_BOOLEAN_FALSE) || (*eo)->GetInterface(eo, SL_IID_ENGINE, &e) ||
        (*e)->CreateOutputMix(e, &mo, 0, NULL, NULL) || (*mo)->Realize(mo, SL_BOOLEAN_FALSE)) { fprintf(stderr, "slplay: no engine\n"); return 1; }
    SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NBUF };
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, ch, rate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             ch == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource src = { &loc, &fmt };
    SLDataLocator_OutputMix out = { SL_DATALOCATOR_OUTPUTMIX, mo };
    SLDataSink sink = { &out, NULL };
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    if ((*e)->CreateAudioPlayer(e, &po, &src, &sink, 2, ids, req)) { fprintf(stderr, "slplay: no player (%u Hz, %u ch)\n", rate, ch); return 1; }
    SLAndroidConfigurationItf cfg; SLint32 type = SL_ANDROID_STREAM_MEDIA;
    if (!(*po)->GetInterface(po, SL_IID_ANDROIDCONFIGURATION, &cfg)) (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_STREAM_TYPE, &type, sizeof type);
    if ((*po)->Realize(po, SL_BOOLEAN_FALSE) || (*po)->GetInterface(po, SL_IID_PLAY, &play) ||
        (*po)->GetInterface(po, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &q) || (*q)->RegisterCallback(q, done, NULL)) {
        fprintf(stderr, "slplay: player refused\n"); return 1;
    }
    size_t slot = rate * ch * 2 / 50 & ~(size_t)3;  /* 20 ms */
    char *buf = malloc(slot * NBUF);
    if (!buf) return 1;
    (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
    for (unsigned k = 0;; k = (k + 1) % NBUF) {
        pthread_mutex_lock(&lock);
        int stuck = wait_busy(NBUF);
        pthread_mutex_unlock(&lock);
        if (stuck) break;
        size_t n = fill(buf + k * slot, slot);
        if (!n) break;
        pthread_mutex_lock(&lock); busy++; pthread_mutex_unlock(&lock);
        if ((*q)->Enqueue(q, buf + k * slot, (SLuint32)n)) {      /* not with the player: the wait below would never end */
            fprintf(stderr, "slplay: enqueue failed\n");
            pthread_mutex_lock(&lock); busy--; pthread_mutex_unlock(&lock);
            break;
        }
    }
    pthread_mutex_lock(&lock);                  /* what is queued plays out */
    wait_busy(1);
    pthread_mutex_unlock(&lock);
    usleep(200000);                             /* the track's own buffer */
    (*po)->Destroy(po); (*mo)->Destroy(mo); (*eo)->Destroy(eo);
    return 0;
}

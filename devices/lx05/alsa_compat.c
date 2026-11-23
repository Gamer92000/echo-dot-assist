/* Small subset of the PulseAudio simple API, backed by the LX05 ALSA codec.
 * Capture uses the hardware; playback uses the device's dmix-based default. */
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

typedef struct snd_pcm_t snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
extern int snd_pcm_open(snd_pcm_t **, const char *, int, int);
extern int snd_pcm_set_params(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
extern int snd_pcm_close(snd_pcm_t *);
extern snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *, void *, snd_pcm_uframes_t);
extern snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
extern int snd_pcm_recover(snd_pcm_t *, int, int);
extern int snd_pcm_drain(snd_pcm_t *);
extern int snd_pcm_drop(snd_pcm_t *);
extern int snd_pcm_delay(snd_pcm_t *, snd_pcm_sframes_t *);
extern const char *snd_strerror(int);

typedef struct { int format; uint32_t rate; uint8_t channels; } spec_t;
typedef struct pa_simple { snd_pcm_t *pcm; unsigned rate, channels; } pa_simple;

pa_simple *pa_simple_new(const char *server, const char *client, int direction,
                         const char *device, const char *name, const spec_t *spec,
                         const void *map, const void *attr, int *error)
{
    (void)server; (void)client; (void)device; (void)name; (void)map; (void)attr;
    pa_simple *s = calloc(1, sizeof(*s));
    if (!s) { if (error) *error = -ENOMEM; return NULL; }
    int e = snd_pcm_open(&s->pcm, direction == 2 ? "plughw:0,0" : "default", direction == 2 ? 1 : 0, 0);
    if (e >= 0) e = snd_pcm_set_params(s->pcm, 2, 3, spec->channels, spec->rate, 1, 100000);
    if (e < 0) { if (error) *error = e; if (s->pcm) snd_pcm_close(s->pcm); free(s); return NULL; }
    s->rate = spec->rate; s->channels = spec->channels;
    return s;
}
void pa_simple_free(pa_simple *s) { if (s) { snd_pcm_close(s->pcm); free(s); } }
static int transfer(pa_simple *s, void *data, size_t bytes, int write, int *error)
{
    size_t frame = s->channels * 2;
    if (!frame || bytes % frame) { if (error) *error = -EINVAL; return -1; }
    unsigned char *p = data;
    while (bytes) {
        snd_pcm_sframes_t n = write ? snd_pcm_writei(s->pcm, p, bytes / frame)
                                    : snd_pcm_readi(s->pcm, p, bytes / frame);
        if (n < 0) { int e = snd_pcm_recover(s->pcm, (int)n, 1); if (e < 0) { if (error) *error = e; return -1; } continue; }
        if (!n) { if (error) *error = -EIO; return -1; }
        p += n * frame; bytes -= n * frame;
    }
    return 0;
}
int pa_simple_read(pa_simple *s, void *p, size_t n, int *e) { return transfer(s, p, n, 0, e); }
int pa_simple_write(pa_simple *s, const void *p, size_t n, int *e) { return transfer(s, (void *)p, n, 1, e); }
int pa_simple_drain(pa_simple *s, int *e) { int r = snd_pcm_drain(s->pcm); if (r < 0 && e) *e = r; return r; }
int pa_simple_flush(pa_simple *s, int *e) { int r = snd_pcm_drop(s->pcm); if (r < 0 && e) *e = r; return r; }
uint64_t pa_simple_get_latency(pa_simple *s, int *e) { snd_pcm_sframes_t d = 0; int r = snd_pcm_delay(s->pcm, &d); if (r < 0) { if (e) *e = r; return UINT64_MAX; } return d > 0 ? (uint64_t)d * 1000000 / s->rate : 0; }
const char *pa_strerror(int error) { return snd_strerror(error); }

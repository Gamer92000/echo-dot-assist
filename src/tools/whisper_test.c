/*
 * whisper_test - run Amazon's whisper detector (libpryon WhisperApi_*, docs/re-whisper.md) over raw 16 kHz mono s16le
 * audio.
 *
 *   whisper_test -m pryon_whisper.manifest [-u start:end]... [-x speed] [-l locale] [-v] [file.raw|file.wav]
 *
 * The model is DAVS's "whisper-static" (scripts/artifacts.sh, "Other artifacts"); /system has none. Each -u (seconds)
 * is one utterance; without -u the whole file is one. As AHE does: one detector per session, audio pushed with its
 * running sample index, end of utterance in 10 ms frames, then backlogWait, session end. -l picks the threshold that
 * whisper_confidence_config.json beside the manifest has for that locale ("default" otherwise), only to label the result.
 * The per-frame confidences come as a mean per second; -v prints every event in full.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pryon_api.h"

#define MAXU 64

static int results;

static void on_text(int code, const char *g, const char *text)
{
    (void)g;
    if (code <= 3) fprintf(stderr, "whisper[%d] %s\n", code, text ? text : "");
}

static void on_attr(int code, const char *g, const char *text) { (void)code; (void)g; fprintf(stderr, "attributes %s\n", text); }

static int threshold = -1;

static int verbose;

/* metadata carries one confidence per 10 ms frame ("frame_confidences"): a line per second (its mean) unless -v */
static void frames(const char *json)
{
    const char *p = strstr(json, "\"frame_confidences\":[");
    if (!p) return;
    p = strchr(p, '[') + 1;
    int n = 0, sum = 0, max = 0;
    printf("  per second:");
    while (*p && *p != ']') {
        int v = atoi(p); sum += v; if (v > max) max = v;
        if (++n % 100 == 0) { printf(" %d", sum / 100); sum = 0; }
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    if (n % 100) printf(" %d", sum / (n % 100));
    printf("  (%d frames, max %d)\n", n, max);
}

static void on_event(const char *det, const char *utt, const char *type, const char *json, uint32_t z0, uint32_t z1)
{
    if (verbose || !type || strcmp(type, "metadata")) {
        if (verbose || !type || strcmp(type, "metrics"))
            printf("%s %s utt=%s z=%u,%u %s\n", det, type, utt && *utt ? utt : "-", z0, z1, json ? json : "(null)");
    } else if (json) frames(json);
    if (type && !strcmp(type, "result")) {
        results++;
        const char *c = json ? strstr(json, "\"confidence\"") : NULL;
        if (c && (c = strchr(c, ':'))) {
            int v = atoi(c + 1);
            printf("  confidence %d: %s by AHE (> 500)", v, v > 500 ? "WHISPERED" : "normal");
            if (threshold >= 0) printf(", %s by the locale threshold %d", v > threshold ? "WHISPERED" : "normal", threshold);
            printf("\n");
        }
    }
    fflush(stdout);
}

/* "<locale>" : <n> or "default" : <n> in the confidence config next to the manifest */
static int read_threshold(const char *manifest, const char *locale)
{
    char path[512], buf[4096], pat[32];
    const char *slash = strrchr(manifest, '/');
    snprintf(path, sizeof path, "%.*swhisper_confidence_config.json", slash ? (int)(slash - manifest + 1) : 0, manifest);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    for (int pass = 0; pass < 2; pass++) {
        snprintf(pat, sizeof pat, "\"%s\"", pass ? "default" : locale);
        const char *p = strstr(buf, pat);
        if (p && (p = strchr(p + strlen(pat), ':'))) return atoi(p + 1);
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *manifest = NULL, *locale = "en-US";
    double speed = 1, us[MAXU], ue[MAXU];
    int nu = 0, o, rc;
    while ((o = getopt(argc, argv, "m:u:x:l:v")) != -1) switch (o) {
        case 'm': manifest = optarg; break;
        case 'u': if (nu < MAXU && sscanf(optarg, "%lf:%lf", &us[nu], &ue[nu]) == 2) nu++; break;
        case 'x': speed = atof(optarg); if (speed <= 0) speed = 1; break;
        case 'l': locale = optarg; break;
        case 'v': verbose = 1; break;
        default: goto usage;
    }
    if (!manifest) {
    usage:
        fprintf(stderr, "usage: whisper_test -m pryon_whisper.manifest [-u start:end]... [-x speed] [-l locale] [-v] [file]\n");
        return 2;
    }
    FILE *in = optind < argc ? fopen(argv[optind], "rb") : stdin;
    if (!in) { perror("open"); return 1; }
    if (optind < argc && strlen(argv[optind]) > 4 && !strcmp(argv[optind] + strlen(argv[optind]) - 4, ".wav"))
        fseek(in, 44, SEEK_SET);        /* canonical header only */
    threshold = read_threshold(manifest, locale);

    WhisperApi_setLogEventHandler(on_text);
    WhisperApi_getLibraryAttributes(on_attr);
    if ((rc = WhisperApi_setEventHandler(on_event))) { fprintf(stderr, "setEventHandler -> %d\n", rc); return 1; }
    if ((rc = WhisperApi_loadWhisperModelset("pryon", manifest))) { fprintf(stderr, "loadWhisperModelset -> %d\n", rc); return 1; }
    WhisperAudioFormat fmt = { 0, 16000, 16, 1 };
    if ((rc = WhisperApi_createWhisperDetector("whisper-detector-1", "pryon", "pryon", fmt))) {
        fprintf(stderr, "createWhisperDetector -> %d\n", rc); return 1;
    }

    int16_t buf[1600]; uint64_t idx = 0; size_t got; int next = 0;
    while ((got = fread(buf, sizeof *buf, 1600, in)) > 0) {
        if ((rc = WhisperApi_pushAudioEvent("whisper-detector-1", buf, got, idx))) {
            fprintf(stderr, "push @%llu -> %d\n", (unsigned long long)idx, rc); break;
        }
        idx += got;
        /* an utterance whose end has been pushed: its end of utterance now, as AHE sends it after the endpointer */
        while (next < nu && ue[next] * 16000 <= idx) {
            char id[32]; snprintf(id, sizeof id, "Utterance-%d", next + 1);
            uint64_t s = (uint64_t)(us[next] * 100), e = (uint64_t)(ue[next] * 100);
            if (e > s && (rc = WhisperApi_pushEndOfUtterance("whisper-detector-1", id, s, e)))
                fprintf(stderr, "pushEndOfUtterance %s -> %d\n", id, rc);
            next++;
        }
        usleep((useconds_t)(got * 1e6 / 16000 / speed));
    }
    if (!nu && idx >= 160 && (rc = WhisperApi_pushEndOfUtterance("whisper-detector-1", "Utterance-1", 0, idx / 160)))
        fprintf(stderr, "pushEndOfUtterance -> %d\n", rc);
    WhisperApi_backlogWait("whisper-detector-1", -1);
    sleep(1);
    WhisperApi_pushSessionEnd("whisper-detector-1");
    sleep(1);
    fprintf(stderr, "pushed %.1fs, %d result(s)\n", idx / 16000.0, results);
    WhisperApi_deleteWhisperDetector("whisper-detector-1");
    WhisperApi_deleteWhisperModelset("pryon");
    return 0;
}

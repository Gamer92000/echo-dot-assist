/* microWakeWord (github.com/kahrendt/microWakeWord, the wake word engine of ESPHome's voice satellites), run without
 * TensorFlow: mww_features.c is TFLite Micro's audio front end ("microfrontend": 30 ms windows every 10 ms, 40 mel
 * channels with noise reduction, PCAN gain control and a log scale), mww_model.c a small interpreter for the int8 .tflite
 * models microWakeWord trains (streaming: the model keeps its own state in resource variables).  Both are checked
 * against the reference libraries (TFLite Micro's frontend as pymicro-features builds it, TFLite's reference kernels) by
 * tests/unit/mww_ref.py.  wake_mww.c puts them together behind wake.h. */
#ifndef MWW_H
#define MWW_H
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- features (mww_features.c) */

#define MWW_CHANNELS 40
#define MWW_WINDOW   480                /* 30 ms at 16 kHz */
#define MWW_STEP     160                /* 10 ms */

struct mww_features {
    int16_t window[MWW_WINDOW], coef[MWW_WINDOW];
    int used;
    /* filterbank: per channel (and one more, the lower half of the first) where its bins start, its weights */
    int start_index, end_index;
    int16_t chan_freq_start[MWW_CHANNELS + 1], chan_weight_start[MWW_CHANNELS + 1], chan_width[MWW_CHANNELS + 1];
    int16_t weights[512], unweights[512];
    uint32_t estimate[MWW_CHANNELS];    /* noise reduction's noise estimate per channel */
    int16_t gain_lut[4 * 32 - 3];       /* PCAN's gain over the noise estimate, piecewise quadratic */
    int snr_shift;
    void *fft;                          /* the fixed point real FFT's twiddles */
};

int  mww_features_init(struct mww_features *f);     /* -1: no memory */
void mww_features_reset(struct mww_features *f);   /* forgets audio and noise estimates */
void mww_features_free(struct mww_features *f);
/* Takes up to n samples (returns how many); when that completes a window, writes its 40 features (0..~670) to out and
 * sets *done */
size_t mww_features_run(struct mww_features *f, const int16_t *s, size_t n, uint16_t out[MWW_CHANNELS], int *done);

/* ---------------------------------------------------------------- model (mww_model.c) */

struct mww_model;
/* Loads a .tflite from memory (copied: the caller may free it).  NULL with err filled: not a model this interpreter
 * runs (an operator it does not know, float tensors, a shape it does not expect) */
struct mww_model *mww_model_load(const void *data, size_t len, char *err, size_t errsz);
void mww_model_free(struct mww_model *m);
void mww_model_reset(struct mww_model *m);          /* the variables back to their start: as freshly loaded */
int  mww_model_stride(const struct mww_model *m);  /* feature frames per inference (the input's second dimension) */
/* Input: stride x 40 features as mww_features_run gives them.  The wake word's probability in 256ths (0..255), -1: the
 * model broke (a variable read before it was written) */
int  mww_model_run(struct mww_model *m, const uint16_t *features);
/* For the reference test: the input tensor's quantized values for those features */
void mww_model_quantize(const struct mww_model *m, const uint16_t *features, int8_t *out);

/* ---------------------------------------------------------------- installed models (mww_store.c)
 * <state>/mww/<id>/ holds manifest.json (microWakeWord's format, written anew by us) and model.tflite.  Added, edited and
 * deleted from the settings page (web.c), copied Echo to Echo as artifacts "mww:<id>" (artifacts.c).  Any thread. */

#define MWW_MODEL_MAX  (1 << 20)        /* the official models are 35-81 KB */
#define MWW_MODELS_MAX 16               /* main.c lists at most this many wake words */
#define MWW_CUTOFF_MIN 0.50f
#define MWW_CUTOFF_MAX 0.99f
#define MWW_WINDOW_MIN 1
#define MWW_WINDOW_MAX 20

struct mww_info {
    char id[48], name[64], langs[64], author[64], website[160];
    float cutoff;                       /* the probability the mean over the window must pass */
    int window;                         /* inferences averaged (30 ms each) */
    long size;                          /* of model.tflite */
};

const char *mww_dir(void);
int  mww_good_id(const char *id);                   /* a-z, 0-9, _ and -, at most 40: a folder name and an artifact id */
void mww_paths(const char *id, char *manifest, char *model, size_t cap);
/* microWakeWord's manifest JSON -> info (name, languages, cutoff, window; id and size are not in it).  0, or -1 with err */
int  mww_manifest_parse(const char *json, size_t n, struct mww_info *i, char *err, size_t errsz);
int  mww_info(const char *id, struct mww_info *i);  /* an installed model's: 0 if its manifest reads and its model is there */
int  mww_list(struct mww_info *out, int max);       /* installed ones, by id */
/* DIR holds a manifest that reads and a model this interpreter loads (a model arriving from another Echo).  0, or -1 */
int  mww_check_dir(const char *dir, char *err, size_t errsz);
/* a new model, or a new version of one: manifest JSON (n 0: made up from the id), the .tflite.  Checked: it must load */
int  mww_add(const char *id, const char *json, size_t jn, const void *model, size_t mn, char *err, size_t errsz);
/* "name=..", "cutoff=0.85", "window=5" lines: what changes */
int  mww_edit(const char *id, const char *text, char *err, size_t errsz);
int  mww_delete(const char *id, char *err, size_t errsz);
/* staged folder DIR (artifacts.c) becomes model ID */
int  mww_install_dir(const char *dir, const char *id, char *err, size_t errsz);
/* [{"id","name","langs","author","website","cutoff","window","size"}...] */
size_t mww_list_json(char *out, size_t cap);
#endif

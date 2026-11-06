/* The Echo's own voice on Fire OS's offline Ivona engine (speech.h, docs/re-ivona.md).
 *
 * The engine (libttsivonavoice.so, C API tts_*) comes with each voice pack; it wants a licence certificate that is
 * nowhere in plain text.  Amazon's Android TTS app has the glue that supplies it (IvonaTTS, lib/arm/libttsivona.so,
 * JNI): so the glue is what runs here, against a JNIEnv of our own that answers the few calls it makes, as Java's
 * IVONATextToSpeechService and IvonaPackagesInfo would.  The app is on the image (on an installed Echo moved to
 * /system/hassmic-removed with Amazon's other apps); the voice is downloaded from Amazon (tools/davs-fetch.py tts) and
 * put where the glue's built-in config looks for it, Amazon's own folder (VOICE_DIR): the glue scans only its config's
 * folders (another one: "Package iterator is invalid").
 *
 * The sequence, measured on the Show 5 (2026-11-02):
 *   JNI_OnLoad; NativeIVONAEngine.setEnvRuntime(<folder holding com.ivona.tts.oem/files/>, <sdcard>);
 *   ConfigXMLDataSet.getPackagesCfg() -> "com.ivona.tts.oem.voicebeta.eng.usa.nina" and
 *   IvonaPackagesInfo.getOEMpackages() -> "2;2;IVONA Nina;ivona_sdk-1.6.118.1;<voice dir>/;vox_en_us_nina22i-ta";
 *   Java then writes com.ivona.tts.oem/files/ivona_pkgs.csv (the time in ms, then "<cfg>;0;<OEM line>" per voice:
 *   without it "Voices vector is empty" and every call fails with -100);
 *   create(); onLoadLanguage("eng", "USA", "") 0 after 4.4 s (the voice loaded); getSampleRate() 22050;
 *   NativeIVONAEngineICS.synthesizeText(text, byte[] buf) fills buf and calls the object's audioAvailableClbk(bytes,
 *   status) per piece: status 1 while it speaks, 0 with no bytes at the end.  First audio after ~190 ms, ~6x real time.
 * It holds ~124 MB resident (the voice's 431 MB are mapped files).  Runs as the daemon's user (system).
 *
 * setContrast: Java sets 1 for the speaker and 0 for headphones (HeadsetPlugReceiver); the engine starts with it on.
 * On, it is a boost for small speakers (the sentence above peaks at -1.4 dBFS against -4.7 off) that the user heard as
 * popping on the Show 5 (2026-11-02), so it is off (CONTRAST).  Smoothing single-sample spikes (114 in 6.5 s) did not
 * help: those are the voice's own.
 *
 * Several voices may be installed side by side (one folder each in VOICE_DIR); the setting "voice" picks one by its
 * folder's name (speech_voices).  Nina (en-US) when she is there, else the first: she "hiccups" where the engine joins
 * two recorded pieces of very different level (the n of "now on", the m of "five minutes": the word before fades out,
 * the next starts at full level within one period; the same bytes as Amazon's own app makes, rate and pitch set as
 * Java sets them or not), but beside Evelyn (en-CA) the owner still found her the better voice (2026-11-02).  Another voice is loaded into the same engine
 * by onLoadLanguage (4.6 s, no restart; resident memory the same, 157 MB, 2026-11-02).  onLoadLanguage takes a
 * language and country only: two voices of one language and country cannot be told apart, the glue takes one. */
#include "speech.h"
#include "statefile.h"
#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

#define VOICE_DIR "/data/securedStorageLocation/com.amazon.providers.contentsupport/tts/ivona/"
#define BUF 8192
#define QUEUE 32                                           /* a streamed reply queues each sentence as its text comes
                                                            * (wyoming_tts.c): an LLM writes faster than the voice speaks */
#define CONTRAST 0

static const char *const glue_dirs[] = {                  /* the app's libraries: moved by sysinstall.sh, or stock's */
    "/system/hassmic-removed/priv-app/IvonaTTS/lib/arm/", "/system/priv-app/IvonaTTS/lib/arm/",
};

static const struct speech_out *out;
static atomic_int ready, cancel_now;
static char voice[32], language[16];
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char *queue[QUEUE]; static int tags[QUEUE], q_head, q_n;   /* lk */
static const struct speech_sink *sinks[QUEUE];            /* lk: NULL = played here (out) */
static int tag_now;                                        /* the speech thread: the sentence's tag */
static const struct speech_sink *sink_now;                 /* lk to write, the speech thread reads: its sink */
static int started;                                        /* the speech thread: the sentence's first piece came */
static unsigned rate_;

/* The voices installed, by folder (<lang>.<land>.<name>_<version>): their names are the setting's values */
#define VOICES 16
static char vnames[VOICES][24], vdirs[VOICES][96]; static const char *vlist[VOICES]; static int nvoices = -1;
static pthread_mutex_t scan_lk = PTHREAD_MUTEX_INITIALIZER;
static atomic_int want = -1;                               /* the voice asked for (index into vnames) */
static int loaded = -1;                                    /* the speech thread: the voice loaded */

/* ------------------------------------------------------------------ the JNIEnv the glue gets */

enum { O_STR = 0x48530001, O_BYTES, O_CLASS, O_OBJ, O_ARR };
struct obj { int type, len; char *s; void *data; struct obj **items; };

static struct obj *mk(int type) { struct obj *o = calloc(1, sizeof *o); if (!o) abort(); o->type = type; return o; }
static struct obj *jstr(const char *s) { struct obj *o = mk(O_STR); o->s = strdup(s ? s : ""); o->len = (int)strlen(o->s); return o; }
static void jfree(struct obj *o)
{
    if (!o) return;
    if (o->items) for (int i = 0; i < o->len; i++) jfree(o->items[i]);
    free(o->items); free(o->s); free(o);
}

static struct obj engine_obj, *buffer;
static long fields[32]; static char *field_names[32]; static int nfields;
static JNIEnv envp; static struct JNINativeInterface fn;

/* Every slot not answered below logs its number (JNINativeInterface order) and returns 0: the glue looks up classes
 * and a class loader in JNI_OnLoad (CallObjectMethodV, 35) that it never needs here. */
#define S(n) static void *stub##n(void *e, ...) { (void)e; fprintf(stderr, "speech: JNI call %d not answered\n", n); return NULL; }
#define S10(a) S(a##0) S(a##1) S(a##2) S(a##3) S(a##4) S(a##5) S(a##6) S(a##7) S(a##8) S(a##9)
S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9)
S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9) S10(10) S10(11) S10(12) S10(13) S10(14) S10(15) S10(16)
S10(17) S10(18) S10(19) S10(20) S10(21) S10(22) S(230) S(231) S(232) S(233) S(234) S(235)
#define P(n) (void *)stub##n
#define P10(a) P(a##0), P(a##1), P(a##2), P(a##3), P(a##4), P(a##5), P(a##6), P(a##7), P(a##8), P(a##9)
static void *const stubs[] = { P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9), P10(1), P10(2), P10(3),
    P10(4), P10(5), P10(6), P10(7), P10(8), P10(9), P10(10), P10(11), P10(12), P10(13), P10(14), P10(15), P10(16),
    P10(17), P10(18), P10(19), P10(20), P10(21), P10(22), P(230), P(231), P(232), P(233), P(234), P(235) };

static jclass FindClass(JNIEnv *e, const char *n) { (void)e; struct obj *o = mk(O_CLASS); o->s = strdup(n); return (jclass)o; }
static jclass GetObjectClass(JNIEnv *e, jobject o) { (void)o; return FindClass(e, "?"); }
static jmethodID GetMethodID(JNIEnv *e, jclass c, const char *n, const char *sig) { (void)e; (void)c; (void)sig; return (jmethodID)strdup(n); }
static jmethodID GetStaticMethodID(JNIEnv *e, jclass c, const char *n, const char *sig) { return GetMethodID(e, c, n, sig); }
static jfieldID GetFieldID(JNIEnv *e, jclass c, const char *n, const char *sig)
{
    (void)e; (void)c; (void)sig;
    for (int i = 0; i < nfields; i++) if (!strcmp(field_names[i], n)) return (jfieldID)(intptr_t)(i + 1);
    if (nfields + 1 >= (int)(sizeof fields / sizeof *fields)) return NULL;
    field_names[nfields] = strdup(n);
    return (jfieldID)(intptr_t)(++nfields);
}
static jlong GetLongField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; return fields[(intptr_t)f]; }
static void SetLongField(JNIEnv *e, jobject o, jfieldID f, jlong v) { (void)e; (void)o; fields[(intptr_t)f] = (long)v; }
static jint GetIntField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; return (jint)fields[(intptr_t)f]; }
static void SetIntField(JNIEnv *e, jobject o, jfieldID f, jint v) { (void)e; (void)o; fields[(intptr_t)f] = v; }
static jobject GetObjectField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)o; return (jobject)fields[(intptr_t)f]; }
static void SetObjectField(JNIEnv *e, jobject o, jfieldID f, jobject v) { (void)e; (void)o; fields[(intptr_t)f] = (long)v; }

static jint audio_cb(va_list ap)
{
    int n = va_arg(ap, int), status = va_arg(ap, int);
    if (atomic_load(&cancel_now)) return -1;                    /* the glue ends the sentence (Java's "no callback") */
    if (n > 0 && buffer && n <= buffer->len) {
        const struct speech_sink *k = sink_now;
        if (!started) { started = 1; if (k) k->begin(k->arg, rate_); else out->begin(rate_, tag_now); }
        if (k) k->data(k->arg, buffer->data, (unsigned)n & ~1u); else out->data(buffer->data, (unsigned)n & ~1u);
    }
    return status == 0 ? 1 : 0;                                 /* Java: 1 once done, else the callback's 0 (SUCCESS) */
}
static jint call_int(const char *m, va_list ap)
{
    if (!strcmp(m, "audioAvailableClbk")) return audio_cb(ap);
    fprintf(stderr, "speech: JNI CallIntMethod %s not answered\n", m);
    return 0;
}
static jint CallIntMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; va_list ap; va_start(ap, m); jint r = call_int((char *)m, ap); va_end(ap); return r; }
static jint CallIntMethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { (void)e; (void)o; return call_int((char *)m, ap); }
static void CallVoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; }
static void CallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, va_list ap) { (void)e; (void)o; (void)m; (void)ap; }
static jboolean CallBooleanMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; return 1; }
static jobject CallObjectMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; return NULL; }
static void CallStaticVoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e; (void)c; (void)m; }
static jobject CallStaticObjectMethod(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e; (void)c; (void)m; return NULL; }
static jint CallStaticIntMethod(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e; (void)c; (void)m; return 0; }
static jobject NewObject(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e; (void)c; (void)m; return (jobject)mk(O_OBJ); }

static jstring NewStringUTF(JNIEnv *e, const char *s) { (void)e; return (jstring)jstr(s); }
static const char *GetStringUTFChars(JNIEnv *e, jstring s, jboolean *c) { (void)e; if (c) *c = 0; return s ? ((struct obj *)s)->s : NULL; }
static void ReleaseStringUTFChars(JNIEnv *e, jstring s, const char *p) { (void)e; (void)s; (void)p; }
static jsize GetStringUTFLength(JNIEnv *e, jstring s) { (void)e; return ((struct obj *)s)->len; }
static jsize GetStringLength(JNIEnv *e, jstring s) { (void)e; return ((struct obj *)s)->len; }
static jsize GetArrayLength(JNIEnv *e, jarray a) { (void)e; return ((struct obj *)a)->len; }
static jbyte *GetByteArrayElements(JNIEnv *e, jbyteArray a, jboolean *c) { (void)e; if (c) *c = 0; return ((struct obj *)a)->data; }
static void ReleaseByteArrayElements(JNIEnv *e, jbyteArray a, jbyte *p, jint m) { (void)e; (void)a; (void)p; (void)m; }
static void *GetPrimitiveArrayCritical(JNIEnv *e, jarray a, jboolean *c) { (void)e; if (c) *c = 0; return ((struct obj *)a)->data; }
static void ReleasePrimitiveArrayCritical(JNIEnv *e, jarray a, void *p, jint m) { (void)e; (void)a; (void)p; (void)m; }
static void GetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize s, jsize l, jbyte *b) { (void)e; memcpy(b, (char *)((struct obj *)a)->data + s, (size_t)l); }
static void SetByteArrayRegion(JNIEnv *e, jbyteArray a, jsize s, jsize l, const jbyte *b) { (void)e; memcpy((char *)((struct obj *)a)->data + s, b, (size_t)l); }
static jobjectArray NewObjectArray(JNIEnv *e, jsize n, jclass c, jobject init)
{
    (void)e; (void)c; (void)init;
    struct obj *o = mk(O_ARR); o->len = n; o->items = calloc((size_t)n + 1, sizeof *o->items);
    if (!o->items) abort();
    return (jobjectArray)o;
}
static void SetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i, jobject v) { (void)e; ((struct obj *)a)->items[i] = (struct obj *)v; }
static jobject GetObjectArrayElement(JNIEnv *e, jobjectArray a, jsize i) { (void)e; return (jobject)((struct obj *)a)->items[i]; }
static jobject NewGlobalRef(JNIEnv *e, jobject o) { (void)e; return o; }
static void DeleteGlobalRef(JNIEnv *e, jobject o) { (void)e; (void)o; }
static void DeleteLocalRef(JNIEnv *e, jobject o) { (void)e; (void)o; }                 /* the strings stay: a few bytes */
static jobject NewLocalRef(JNIEnv *e, jobject o) { (void)e; return o; }
static jint PushLocalFrame(JNIEnv *e, jint n) { (void)e; (void)n; return 0; }
static jobject PopLocalFrame(JNIEnv *e, jobject o) { (void)e; return o; }
static jint EnsureLocalCapacity(JNIEnv *e, jint n) { (void)e; (void)n; return 0; }
static jboolean ExceptionCheck(JNIEnv *e) { (void)e; return 0; }
static jthrowable ExceptionOccurred(JNIEnv *e) { (void)e; return NULL; }
static void ExceptionClear(JNIEnv *e) { (void)e; }
static void ExceptionDescribe(JNIEnv *e) { (void)e; }
static jint ThrowNew(JNIEnv *e, jclass c, const char *m) { (void)e; fprintf(stderr, "speech: glue threw %s: %s\n", ((struct obj *)c)->s, m); return 0; }
static jboolean IsSameObject(JNIEnv *e, jobject a, jobject b) { (void)e; return a == b; }

static struct JNIInvokeInterface vmfn; static JavaVM vmp = &vmfn;
static jint vm_attach(JavaVM *vm, JNIEnv **e, void *a) { (void)vm; (void)a; *e = &envp; return 0; }
static jint vm_detach(JavaVM *vm) { (void)vm; return 0; }
static jint vm_getenv(JavaVM *vm, void **e, jint v) { (void)vm; (void)v; *e = &envp; return 0; }
static jint GetJavaVM(JNIEnv *e, JavaVM **vm) { (void)e; *vm = &vmp; return 0; }

static void env_init(void)
{
    memcpy(&fn, stubs, sizeof fn < sizeof stubs ? sizeof fn : sizeof stubs);
#define F(name) fn.name = name
    F(FindClass); F(GetObjectClass); F(GetMethodID); F(GetStaticMethodID); F(GetFieldID); F(GetLongField);
    F(SetLongField); F(GetIntField); F(SetIntField); F(GetObjectField); F(SetObjectField); F(CallIntMethod);
    F(CallIntMethodV); F(CallVoidMethod); F(CallVoidMethodV); F(CallBooleanMethod); F(CallObjectMethod);
    F(CallStaticVoidMethod); F(CallStaticObjectMethod); F(CallStaticIntMethod); F(NewObject); F(NewStringUTF);
    F(GetStringUTFChars); F(ReleaseStringUTFChars); F(GetStringUTFLength); F(GetStringLength); F(GetArrayLength);
    F(GetByteArrayElements); F(ReleaseByteArrayElements); F(GetPrimitiveArrayCritical); F(ReleasePrimitiveArrayCritical);
    F(GetByteArrayRegion); F(SetByteArrayRegion); F(NewObjectArray); F(SetObjectArrayElement); F(GetObjectArrayElement);
    F(NewGlobalRef); F(DeleteGlobalRef); F(DeleteLocalRef); F(NewLocalRef); F(PushLocalFrame); F(PopLocalFrame);
    F(EnsureLocalCapacity); F(ExceptionCheck); F(ExceptionOccurred); F(ExceptionClear); F(ExceptionDescribe);
    F(ThrowNew); F(IsSameObject); F(GetJavaVM);
#undef F
    envp = &fn;
    vmfn.AttachCurrentThread = vm_attach; vmfn.AttachCurrentThreadAsDaemon = vm_attach;
    vmfn.DetachCurrentThread = vm_detach; vmfn.GetEnv = vm_getenv;
    engine_obj.type = O_OBJ;
}

/* ------------------------------------------------------------------ the glue */

static jint (*j_rate)(JNIEnv *, jobject);
static jint (*j_load_lang)(JNIEnv *, jobject, jstring, jstring, jstring);
static jint (*j_contrast)(JNIEnv *, jobject, jint);
static struct obj *packages;                               /* the glue's package lines, one per voice */
static jint (*j_synth)(JNIEnv *, jobject, jstring, jbyteArray);

/* "eng.usa.nina_1512406685000" -> "eng", "USA" for onLoadLanguage and "en-US" for the rest */
static const char *const lang3[][2] = { {"eng","en"}, {"deu","de"}, {"fra","fr"}, {"ita","it"}, {"spa","es"}, {"por","pt"},
                                        {"jpn","ja"}, {"hin","hi"}, {"ara","ar"} };
static const char *const land3[][2] = { {"usa","US"}, {"gbr","GB"}, {"deu","DE"}, {"fra","FR"}, {"ita","IT"}, {"esp","ES"},
                                        {"mex","MX"}, {"can","CA"}, {"aus","AU"}, {"ind","IN"}, {"bra","BR"}, {"jpn","JP"}, {"sau","SA"} };
static const char *map3(const char *const t[][2], size_t n, const char *k)
{ for (size_t i = 0; i < n; i++) if (!strcasecmp(t[i][0], k)) return t[i][1]; return NULL; }

static void *glue_sym(void *h, const char *n)
{
    void *p = dlsym(h, n);
    if (!p) fprintf(stderr, "speech: the glue has no %s\n", n);
    return p;
}

static void scan(void)
{
    pthread_mutex_lock(&scan_lk);
    if (nvoices < 0) {
        nvoices = 0;
        DIR *d = opendir(VOICE_DIR);
        for (struct dirent *e; d && (e = readdir(d)) && nvoices < VOICES; ) {
            char lang[4], land[4], name[24];
            if (sscanf(e->d_name, "%3[a-z].%3[a-z].%23[a-z]_", lang, land, name) != 3) continue;
            int at = nvoices;                                   /* sorted by name: the same order on every start */
            while (at > 0 && strcmp(vnames[at - 1], name) > 0) { memcpy(vnames[at], vnames[at - 1], sizeof *vnames); memcpy(vdirs[at], vdirs[at - 1], sizeof *vdirs); at--; }
            snprintf(vnames[at], sizeof *vnames, "%s", name); snprintf(vdirs[at], sizeof *vdirs, "%s", e->d_name);
            nvoices++;
        }
        if (d) closedir(d);
        for (int i = 0; i < nvoices; i++) vlist[i] = vnames[i];
        int def = 0;
        for (int i = 0; i < nvoices; i++) if (!strcmp(vnames[i], "nina")) def = i;
        int none = -1;
        atomic_compare_exchange_strong(&want, &none, nvoices ? def : -1);
    }
    pthread_mutex_unlock(&scan_lk);
}

int speech_voices(const char *const **names) { scan(); *names = vlist; return nvoices; }
int speech_voice_index(void) { scan(); return atomic_load(&want); }
void speech_voice_select(int i)
{
    scan();
    if (i < 0 || i >= nvoices) return;
    atomic_store(&want, i);
    pthread_mutex_lock(&lk); pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);    /* the thread loads it */
}

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static int load(void)
{
    char p[300];
    struct stat st;
    if (stat(VOICE_DIR, &st)) return -1;                        /* no voice installed: quiet, the usual case */
    void *h = NULL;
    for (size_t i = 0; i < sizeof glue_dirs / sizeof *glue_dirs && !h; i++) {
        snprintf(p, sizeof p, "%slibc++_shared.so", glue_dirs[i]);
        if (stat(p, &st)) continue;
        if (!dlopen(p, RTLD_NOW | RTLD_GLOBAL)) { fprintf(stderr, "speech: %s: %s\n", p, dlerror()); continue; }
        snprintf(p, sizeof p, "%slibttsivona.so", glue_dirs[i]);
        if (!(h = dlopen(p, RTLD_NOW))) fprintf(stderr, "speech: %s: %s\n", p, dlerror());
    }
    if (!h) { fprintf(stderr, "speech: a voice is installed, but not the engine's app (IvonaTTS)\n"); return -1; }
    env_init();
    JNIEnv *env = &envp;
    jint (*onload)(JavaVM *, void *) = dlsym(h, "JNI_OnLoad");
    if (onload) onload(&vmp, NULL);

    void (*setenv_rt)(JNIEnv *, jclass, jstring, jstring) = glue_sym(h, "Java_com_ivona_ttslib_ics_NativeIVONAEngine_setEnvRuntime");
    jobjectArray (*cfgs)(JNIEnv *, jclass) = glue_sym(h, "Java_com_ivona_ttslib_config_ConfigXMLDataSet_getPackagesCfg");
    jobjectArray (*oems)(JNIEnv *, jclass) = glue_sym(h, "Java_com_ivona_ttslib_packages_IvonaPackagesInfo_getOEMpackages");
    void (*create)(JNIEnv *, jobject) = glue_sym(h, "Java_com_ivona_ttslib_ics_NativeIVONAEngine_create");
    jint (*load_lang)(JNIEnv *, jobject, jstring, jstring, jstring) = glue_sym(h, "Java_com_ivona_ttslib_ics_NativeIVONAEngine_onLoadLanguage");
    j_rate = glue_sym(h, "Java_com_ivona_ttslib_ics_NativeIVONAEngine_getSampleRate");
    j_synth = glue_sym(h, "Java_com_ivona_ttslib_ics_NativeIVONAEngineICS_synthesizeText__Ljava_lang_String_2_3B");
    if (!setenv_rt || !cfgs || !oems || !create || !load_lang || !j_rate || !j_synth) return -1;

    /* the glue's "data folder": ours, <state>/ivona/com.ivona.tts.oem/files/ivona_pkgs.csv */
    char parent[200];
    snprintf(parent, sizeof parent, "%s/ivona/", state_dir());
    mkdir(parent, 0700);
    snprintf(p, sizeof p, "%scom.ivona.tts.oem", parent); mkdir(p, 0700);
    snprintf(p, sizeof p, "%scom.ivona.tts.oem/files", parent); mkdir(p, 0700);
    jclass cls = FindClass(env, "com/ivona/ttslib/ics/NativeIVONAEngineICS");
    setenv_rt(env, cls, jstr(parent), jstr("/sdcard/"));
    struct obj *c = (struct obj *)cfgs(env, cls), *o = (struct obj *)oems(env, cls);
    if (!c || !o || !c->len || c->len != o->len) {
        fprintf(stderr, "speech: no voice found in %s (%d config entries, %d packages)\n", VOICE_DIR, c ? c->len : -1, o ? o->len : -1);
        return -1;
    }
    char csv[2048]; int k = snprintf(csv, sizeof csv, "%lld000\n", (long long)time(NULL));
    for (int i = 0; i < c->len && k < (int)sizeof csv; i++)
        k += snprintf(csv + k, sizeof csv - (size_t)k, "%s;0;%s\n", c->items[i]->s, o->items[i]->s);
    snprintf(p, sizeof p, "%scom.ivona.tts.oem/files/ivona_pkgs.csv", parent);
    if (k >= (int)sizeof csv || statefile_write(p, 0600, csv, (size_t)k)) { fprintf(stderr, "speech: cannot write %s\n", p); return -1; }

    packages = o; jfree(c);
    j_load_lang = load_lang;
    j_contrast = dlsym(h, "Java_com_ivona_ttslib_ics_NativeIVONAEngine_setContrast");
    create(env, (jobject)&engine_obj);
    buffer = mk(O_BYTES); buffer->len = BUF; buffer->data = calloc(1, BUF);
    return buffer->data ? 0 : -1;
}

/* Loads voice I (vnames) into the engine: its name from the glue's package line (field 3, "IVONA Evelyn"), its
 * language from the folder (<lang>.<land>.<name>_<version>).  0, or -1 with the voice before it gone too */
static int use_voice(int i)
{
    char lang[8] = "", land[8] = "", line[300] = "", nv[32], nl[16];
    for (int k = 0; packages && k < packages->len; k++)
        if (strstr(packages->items[k]->s, vdirs[i])) snprintf(line, sizeof line, "%s", packages->items[k]->s);
    if (!line[0]) { fprintf(stderr, "speech: the glue does not list %s\n", vdirs[i]); return -1; }
    char *f[8] = {0}; int nf = 0;
    for (char *s = line, *t; nf < 8 && (t = strsep(&s, ";")); ) f[nf++] = t;
    if (nf < 5) { fprintf(stderr, "speech: package line not understood: %s\n", vdirs[i]); return -1; }
    snprintf(nv, sizeof nv, "%s", !strncmp(f[2], "IVONA ", 6) ? f[2] + 6 : f[2]);
    sscanf(vdirs[i], "%3[a-z].%3[a-z]", lang, land);
    const char *l2 = map3(lang3, sizeof lang3 / sizeof *lang3, lang), *c2 = map3(land3, sizeof land3 / sizeof *land3, land);
    snprintf(nl, sizeof nl, "%s-%s", l2 ? l2 : lang, c2 ? c2 : land);
    for (char *s = land; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
    atomic_store(&ready, 0);                                    /* speech_voice/_language change: nobody reads them now */
    loaded = i;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    jint r = j_load_lang(&envp, (jobject)&engine_obj, jstr(lang), jstr(land), jstr(""));
    if (r < 0) { fprintf(stderr, "speech: loading %s (%s) failed: %d (logcat IVONATTS says why)\n", nv, nl, r); return -1; }
    if (j_contrast) j_contrast(&envp, (jobject)&engine_obj, CONTRAST);
    rate_ = (unsigned)j_rate(&envp, (jobject)&engine_obj);
    if (!rate_) return -1;
    snprintf(voice, sizeof voice, "%s", nv); snprintf(language, sizeof language, "%s", nl);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "speech: voice %s (%s, %u Hz) loaded in %ld ms\n", voice, language, rate_,
            (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    atomic_store(&ready, 1);
    return 0;
}

static void *thread(void *arg)
{
    (void)arg;
    struct timespec t0, t1;
    scan();
    if (atomic_load(&want) < 0 || load() || use_voice(atomic_load(&want))) return NULL;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!q_n && atomic_load(&want) == loaded) pthread_cond_wait(&cv, &lk);
        int w = atomic_load(&want);
        if (w != loaded) {                                      /* the setting changed: queued sentences wait for it */
            int was = loaded;
            pthread_mutex_unlock(&lk);
            if (use_voice(w) && (was < 0 || use_voice(was))) {  /* back to the one that spoke; failing that, none */
                fprintf(stderr, "speech: no voice now\n");
                atomic_store(&want, loaded);
                pthread_mutex_lock(&lk);                        /* what waits is ended: a reply's run ends with it */
                while (q_n) {
                    int i = q_head; q_head = (q_head + 1) % QUEUE; q_n--;
                    pthread_mutex_unlock(&lk);
                    if (sinks[i]) sinks[i]->end(sinks[i]->arg, 0); else out->end(tags[i]);
                    free(queue[i]);
                    pthread_mutex_lock(&lk);
                }
                pthread_mutex_unlock(&lk);
            } else if (loaded != w) atomic_store(&want, loaded);   /* the setting shows the voice that speaks */
            continue;
        }
        char *text = queue[q_head]; tag_now = tags[q_head]; sink_now = sinks[q_head]; q_head = (q_head + 1) % QUEUE; q_n--;
        atomic_store(&cancel_now, 0);
        pthread_mutex_unlock(&lk);
        started = 0;
        struct obj *s = jstr(text);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        jint r = j_synth(&envp, (jobject)&engine_obj, (jstring)s, (jbyteArray)buffer);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int cancelled = atomic_load(&cancel_now);
        if (sink_now) sink_now->end(sink_now->arg, r >= 0 && started && !cancelled); else out->end(tag_now);
        fprintf(stderr, "speech: \"%.60s%s\" %s in %ld ms\n", text, strlen(text) > 60 ? "..." : "",
                r < 0 ? (atomic_load(&cancel_now) ? "cancelled" : "failed") : "said",
                (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
        jfree(s); free(text);
    }
    return NULL;
}

void speech_start(const struct speech_out *o)
{
    pthread_t t;
    out = o;
    if (pthread_create(&t, NULL, thread, NULL) == 0) pthread_detach(t);
}

/* by the files, at once: the settings ask before the voice has loaded (settings.c present() is decided once) */
int speech_installed(void)
{
    struct stat st; char p[300];
    scan();
    if (!nvoices) return 0;
    for (size_t i = 0; i < sizeof glue_dirs / sizeof *glue_dirs; i++) {
        snprintf(p, sizeof p, "%slibttsivona.so", glue_dirs[i]);
        if (!stat(p, &st)) return 1;
    }
    return 0;
}
int  speech_ready(void) { return atomic_load(&ready); }
const char *speech_voice(void) { return atomic_load(&ready) ? voice : ""; }
const char *speech_language(void) { return atomic_load(&ready) ? language : ""; }

static int enqueue(const char *text, int tag, const struct speech_sink *sink)
{
    if (!atomic_load(&ready) || !text || !*text) return -1;
    char *t = strdup(text);
    if (!t) return -1;
    pthread_mutex_lock(&lk);
    if (q_n == QUEUE) { pthread_mutex_unlock(&lk); free(t); return -1; }
    int i = (q_head + q_n) % QUEUE;
    queue[i] = t; tags[i] = tag; sinks[i] = sink; q_n++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lk);
    return 0;
}

int speech_say(const char *text, int tag) { return enqueue(text, tag, NULL); }
int speech_render(const char *text, const struct speech_sink *sink) { return sink ? enqueue(text, 0, sink) : -1; }

void speech_cancel(void)
{
    pthread_mutex_lock(&lk);
    int dropped[QUEUE], n = 0, keep = 0;
    char *kq[QUEUE]; int kt[QUEUE]; const struct speech_sink *ks[QUEUE];
    while (q_n) {                                               /* ours go, renders for others stay, in order */
        int i = q_head; q_head = (q_head + 1) % QUEUE; q_n--;
        if (sinks[i]) { kq[keep] = queue[i]; kt[keep] = tags[i]; ks[keep++] = sinks[i]; }
        else { free(queue[i]); dropped[n++] = tags[i]; }
    }
    for (int i = 0; i < keep; i++) { int j = (q_head + q_n) % QUEUE; queue[j] = kq[i]; tags[j] = kt[i]; sinks[j] = ks[i]; q_n++; }
    if (!sink_now) atomic_store(&cancel_now, 1);                /* the sentence being made is ours */
    pthread_mutex_unlock(&lk);
    for (int i = 0; i < n; i++) out->end(dropped[i]);          /* never begun, but ended: see speech.h */
}

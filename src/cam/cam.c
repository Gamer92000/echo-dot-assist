/* The Echo Show's camera as H.264 (camera.h, docs/camera.md): a JNI library for app_process, loaded by screen.dex as
 * libhmui.so is, run by hassmic's camera.c as its child only while a stream runs:
 *
 *   CLASSPATH=<dir>/screen.dex HMCAM_W=1280 HMCAM_H=720 HMCAM_FPS=15 HMCAM_KBPS=1500 app_process / Screen <dir>/libhmcam.so
 *
 * stdout: records, each a 20 byte header (little-endian) and its bytes:
 *     "HMCV"  type u8 (1 codec config: SPS+PPS, 2 key frame, 3 frame)  3 zero bytes  length u32  pts u64 (us)
 * stdin:  nothing; its end (hassmic closed it, or went) stops the camera and ends the process.
 * stderr: lines for hassmic's log.  Exit status: 0 stopped, 2 no VM, 3 no camera (busy, refused), 4 no encoder, 5 no
 * frames within 5 s, 6 no dot (then nothing is sent).
 *
 * The camera: android.hardware.Camera (HAL1 on checkers: "MediaTek Camera Module", device version 0x100; camera2
 * there is the legacy shim).  Camera.open() takes the client's package from ActivityThread.currentOpPackageName(),
 * which is null in app_process, and native_setup's GetStringChars aborts on null; so the same steps by hand: the
 * package-private Camera() (field initializers: its locks), an EventHandler on a Looper of this thread (the HAL's
 * callbacks are dropped without one), native_setup(weak this, id, CAMERA_HAL_API_VERSION_NORMAL_CONNECT, "android").
 * "android" is uid system's own package, which this process runs as: AppOps' startOperation refuses a package of
 * another uid.  Measured on checkers (2026-11-04): native_setup 0.1-0.4 s, startPreview 0.2-0.4 s, the first H.264
 * 0.3-0.5 s after it; with the VM's start some 3 s to the first frame.
 *
 * The picture: preview straight into MediaCodec's input surface (Camera.setPreviewSurface, hidden, the legacy shim's
 * way in): no copy through this process, the encoder is the MT8163's own.  1280x720 Baseline at 15 fps, 1.5 Mbit/s,
 * a key frame a second (a viewer joining the integration's relay waits at most that); in a dark room the HAL's own
 * exposure took it to 10-11 fps.
 *
 * While pictures leave the Echo, a red dot in the screen's top right corner, over every app (a SurfaceFlinger layer of
 * this process, as the UI's): nobody can watch through the Echo without it showing, and it goes with the process.
 * Without the dot nothing is sent: a step of it that fails ends the process (an SDK with SurfaceControl.Builder only,
 * Android 9 on athena, would otherwise stream unseen).  The screen is woken with it and kept on (awake()).
 * HMCAM_DOT=0 would leave it off: camera.c always sets 1 and drops any HMCAM_DOT it inherited. */
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DOT       20                    /* px, the dot's layer (the circle inside it) */
#define DOT_LAYER 2000000001            /* over the band's (src/ui/android.c BAND_LAYER) */
#define STALL_MS  5000                  /* no output for that long: the camera is gone (server died, evicted) */

static JavaVM *vm;
static JNIEnv *e;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fprintf(stderr, "camera: %s\n", b); fflush(stderr);
}

/* an exception pending: logged, cleared, 1 (every call is checked before the next: one made with an exception
 * pending aborts under CheckJNI) */
static int failed(const char *what)
{
    if (!(*e)->ExceptionCheck(e)) return 0;
    jthrowable t = (*e)->ExceptionOccurred(e); (*e)->ExceptionClear(e);
    jclass oc = (*e)->FindClass(e, "java/lang/Object");
    jstring s = oc ? (*e)->CallObjectMethod(e, t, (*e)->GetMethodID(e, oc, "toString", "()Ljava/lang/String;")) : NULL;
    (*e)->ExceptionClear(e);
    const char *c = s ? (*e)->GetStringUTFChars(e, s, NULL) : NULL;
    say("%s: %s", what, c ? c : "?");
    return 1;
}

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int env_int(const char *n, int d) { const char *v = getenv(n); return v && atoi(v) > 0 ? atoi(v) : d; }

static int write_all(const void *p, size_t n)
{
    const uint8_t *c = p;
    while (n) { ssize_t w = write(1, c, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}

static int record(int type, long long pts_us, const uint8_t *p, size_t n)
{
    uint8_t h[20] = { 'H', 'M', 'C', 'V', (uint8_t)type };
    for (int i = 0; i < 4; i++) h[8 + i] = (uint8_t)(n >> (8 * i));
    for (int i = 0; i < 8; i++) h[12 + i] = (uint8_t)((unsigned long long)pts_us >> (8 * i));
    return write_all(h, sizeof h) || write_all(p, n) ? -1 : 0;
}

/* stdin ended: hassmic wants the camera off */
static int told_to_stop(void)
{
    struct pollfd p = { 0, POLLIN, 0 }; char b[64];
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLHUP | POLLERR)) return 1;
    ssize_t r = read(0, b, sizeof b);
    return r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN);
}

/* ---------------------------------------------------------------- the dot */

static jobject dot_sc;
/* on: 0 once it shows, -1 if any step failed (the caller then sends nothing: no picture leaves without the dot) */
static int dot(int on)
{
    jclass scc = (*e)->FindClass(e, "android/view/SurfaceControl");
    if (failed("SurfaceControl") || !scc) return -1;
    if (on && !dot_sc) {
        jclass ssc = (*e)->FindClass(e, "android/view/SurfaceSession"), sfc = (*e)->FindClass(e, "android/view/Surface");
        jclass dmc = (*e)->FindClass(e, "android/hardware/display/DisplayManagerGlobal"), dic = (*e)->FindClass(e, "android/view/DisplayInfo");
        if (failed("classes")) return -1;
        jobject dm = (*e)->CallStaticObjectMethod(e, dmc, (*e)->GetStaticMethodID(e, dmc, "getInstance", "()Landroid/hardware/display/DisplayManagerGlobal;"));
        jobject di = !failed("display") && dm ? (*e)->CallObjectMethod(e, dm, (*e)->GetMethodID(e, dmc, "getDisplayInfo", "(I)Landroid/view/DisplayInfo;"), 0) : NULL;
        if (failed("getDisplayInfo") || !di) return -1;
        int W = (*e)->GetIntField(e, di, (*e)->GetFieldID(e, dic, "logicalWidth", "I"));
        jobject ss = (*e)->NewObject(e, ssc, (*e)->GetMethodID(e, ssc, "<init>", "()V"));
        if (failed("SurfaceSession") || !ss) return -1;
        /* SDK 25 (checkers): the constructor; SurfaceControl.Builder from 26 (android.c layer_make has both) */
        jmethodID init = (*e)->GetMethodID(e, scc, "<init>", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIII)V");
        if (failed("SurfaceControl(): this SDK has the Builder only") || !init) return -1;
        jobject sc = (*e)->NewObject(e, scc, init, ss, (*e)->NewStringUTF(e, "hassmic-camera-dot"), DOT, DOT, -3 /* TRANSLUCENT */, 4 /* HIDDEN */);
        if (failed("SurfaceControl") || !sc) return -1;
        jobject surf = (*e)->NewObject(e, sfc, (*e)->GetMethodID(e, sfc, "<init>", "()V"));
        if (failed("Surface") || !surf) return -1;
        (*e)->CallVoidMethod(e, surf, (*e)->GetMethodID(e, sfc, "copyFrom", "(Landroid/view/SurfaceControl;)V"), sc);
        if (failed("copyFrom")) return -1;
        ANativeWindow *w = ANativeWindow_fromSurface(e, surf); ANativeWindow_Buffer b;
        if (!w || ANativeWindow_setBuffersGeometry(w, DOT, DOT, WINDOW_FORMAT_RGBA_8888) || ANativeWindow_lock(w, &b, NULL)) { say("no dot to draw on"); return -1; }
        for (int y = 0; y < DOT; y++) for (int x = 0; x < DOT; x++) {
            float dx = x + 0.5f - DOT / 2.0f, dy = y + 0.5f - DOT / 2.0f, r = DOT / 2.0f - 2, d2 = dx * dx + dy * dy;
            /* antialiased edge: coverage over the last pixel; premultiplied red */
            float a = d2 <= (r - 1) * (r - 1) ? 1 : d2 >= r * r ? 0 : (r * r - d2) / (r * r - (r - 1) * (r - 1));
            uint8_t A = (uint8_t)(a * 255);
            ((uint32_t *)b.bits)[y * b.stride + x] = (uint32_t)A << 24 | (uint32_t)(A * 0x30 / 255) << 16 | (uint32_t)(A * 0x30 / 255) << 8 | (uint32_t)(A * 0xe8 / 255);
        }
        ANativeWindow_unlockAndPost(w);
        dot_sc = (*e)->NewGlobalRef(e, sc);
        jmethodID op = (*e)->GetStaticMethodID(e, scc, "openTransaction", "()V"), cl = (*e)->GetStaticMethodID(e, scc, "closeTransaction", "()V");
        (*e)->CallStaticVoidMethod(e, scc, op);
        if (failed("openTransaction")) return -1;
        (*e)->CallVoidMethod(e, dot_sc, (*e)->GetMethodID(e, scc, "setLayer", "(I)V"), DOT_LAYER);
        int bad = failed("setLayer");
        if (!bad) { (*e)->CallVoidMethod(e, dot_sc, (*e)->GetMethodID(e, scc, "setPosition", "(FF)V"), (jfloat)(W - DOT - 6), 6.0f); bad = failed("setPosition"); }
        if (!bad) { (*e)->CallVoidMethod(e, dot_sc, (*e)->GetMethodID(e, scc, "show", "()V")); bad = failed("show"); }
        (*e)->CallStaticVoidMethod(e, scc, cl);
        if (failed("closeTransaction") || bad) return -1;
    } else if (!on && dot_sc) {
        jmethodID op = (*e)->GetStaticMethodID(e, scc, "openTransaction", "()V"), cl = (*e)->GetStaticMethodID(e, scc, "closeTransaction", "()V");
        (*e)->CallStaticVoidMethod(e, scc, op);
        if (!failed("openTransaction")) { (*e)->CallVoidMethod(e, dot_sc, (*e)->GetMethodID(e, scc, "hide", "()V")); failed("hide"); (*e)->CallStaticVoidMethod(e, scc, cl); failed("closeTransaction"); }
    }
    return 0;
}

/* The screen on while the dot shows: a dot on a dark screen tells nobody anything.  IPowerManager.wakeUp at the start
 * (as a touch wakes it, android.c power), then userActivity every 10 s so that the screen's timeout (5 min; 1 min
 * muted or in DND, ring_band.c) does not put it out under a running stream; Fire OS's own Home Monitoring lights the
 * screen too.  Failing here stops nothing: the dot is drawn either way. */
static jobject power_;
static void awake(int first)
{
    if (!power_) {
        jclass smc = (*e)->FindClass(e, "android/os/ServiceManager"), stc = (*e)->FindClass(e, "android/os/IPowerManager$Stub");
        if (failed("power classes")) return;
        jobject pb = (*e)->CallStaticObjectMethod(e, smc, (*e)->GetStaticMethodID(e, smc, "getService", "(Ljava/lang/String;)Landroid/os/IBinder;"), (*e)->NewStringUTF(e, "power"));
        jobject pw = !failed("power") && pb ? (*e)->CallStaticObjectMethod(e, stc, (*e)->GetStaticMethodID(e, stc, "asInterface", "(Landroid/os/IBinder;)Landroid/os/IPowerManager;"), pb) : NULL;
        if (failed("IPowerManager") || !pw) return;
        power_ = (*e)->NewGlobalRef(e, pw);
    }
    jclass pmc = (*e)->FindClass(e, "android/os/IPowerManager"), clk = (*e)->FindClass(e, "android/os/SystemClock");
    if (failed("power classes")) return;
    jlong t = (*e)->CallStaticLongMethod(e, clk, (*e)->GetStaticMethodID(e, clk, "uptimeMillis", "()J"));
    if (failed("uptimeMillis")) return;
    if (first) (*e)->CallVoidMethod(e, power_, (*e)->GetMethodID(e, pmc, "wakeUp", "(JLjava/lang/String;Ljava/lang/String;)V"), t, (*e)->NewStringUTF(e, "hassmic:camera"), (*e)->NewStringUTF(e, "android"));
    else (*e)->CallVoidMethod(e, power_, (*e)->GetMethodID(e, pmc, "userActivity", "(JII)V"), t, 0 /* USER_ACTIVITY_EVENT_OTHER */, 0);
    failed(first ? "wakeUp" : "userActivity");
}

/* ---------------------------------------------------------------- camera and encoder */

static void *run(void *arg)
{
    (void)arg; int rc = 3;
    jobject cam = NULL, codec = NULL;
    jclass cc = NULL, mcc = NULL;
    if ((*vm)->AttachCurrentThread(vm, &e, NULL)) exit(2);
    signal(SIGPIPE, SIG_IGN);                   /* hassmic gone: write() fails, the camera is let go below */
    int W = env_int("HMCAM_W", 1280), H = env_int("HMCAM_H", 720), fps = env_int("HMCAM_FPS", 15), kbps = env_int("HMCAM_KBPS", 1500);

    jclass lc = (*e)->FindClass(e, "android/os/Looper");
    if (failed("Looper")) exit(2);
    (*e)->CallStaticVoidMethod(e, lc, (*e)->GetStaticMethodID(e, lc, "prepare", "()V"));
    jobject looper = failed("Looper.prepare") ? NULL : (*e)->CallStaticObjectMethod(e, lc, (*e)->GetStaticMethodID(e, lc, "myLooper", "()Landroid/os/Looper;"));
    if (failed("myLooper") || !looper) exit(2);

    cc = (*e)->FindClass(e, "android/hardware/Camera");
    jclass ehc = (*e)->FindClass(e, "android/hardware/Camera$EventHandler"), wrc = (*e)->FindClass(e, "java/lang/ref/WeakReference");
    jclass pc = (*e)->FindClass(e, "android/hardware/Camera$Parameters");
    if (failed("camera classes")) goto out;
    cam = (*e)->NewObject(e, cc, (*e)->GetMethodID(e, cc, "<init>", "()V"));
    if (failed("Camera()") || !cam) { cam = NULL; goto out; }
    jobject eh = (*e)->NewObject(e, ehc, (*e)->GetMethodID(e, ehc, "<init>", "(Landroid/hardware/Camera;Landroid/hardware/Camera;Landroid/os/Looper;)V"), cam, cam, looper);
    if (failed("EventHandler")) { cam = NULL; goto out; }
    (*e)->SetObjectField(e, cam, (*e)->GetFieldID(e, cc, "mEventHandler", "Landroid/hardware/Camera$EventHandler;"), eh);
    jobject wr = failed("mEventHandler") ? NULL : (*e)->NewObject(e, wrc, (*e)->GetMethodID(e, wrc, "<init>", "(Ljava/lang/Object;)V"), cam);
    if (failed("WeakReference") || !wr) { cam = NULL; goto out; }
    long long t0 = now_ms();
    jint err = (*e)->CallIntMethod(e, cam, (*e)->GetMethodID(e, cc, "native_setup", "(Ljava/lang/Object;IILjava/lang/String;)I"), wr, 0, -2, (*e)->NewStringUTF(e, "android"));
    if (failed("native_setup") || err) { say("the camera did not open (%d): another app has it, or it is refused", err); cam = NULL; goto out; }
    say("open in %lld ms", now_ms() - t0);

    jobject par = (*e)->CallObjectMethod(e, cam, (*e)->GetMethodID(e, cc, "getParameters", "()Landroid/hardware/Camera$Parameters;"));
    if (failed("getParameters") || !par) goto out;
    (*e)->CallVoidMethod(e, par, (*e)->GetMethodID(e, pc, "setPreviewSize", "(II)V"), W, H);
    /* a fixed rate: (15000,15000) is in checkers' preview-fps-range-values; the default (5000,30000) let the HAL go to 30
     * in daylight, twice the encoder's and the Wi-Fi's work for nothing */
    if (!failed("setPreviewSize")) (*e)->CallVoidMethod(e, par, (*e)->GetMethodID(e, pc, "setPreviewFpsRange", "(II)V"), fps * 1000, fps * 1000);
    if (!failed("setPreviewFpsRange")) (*e)->CallVoidMethod(e, par, (*e)->GetMethodID(e, pc, "setRecordingHint", "(Z)V"), JNI_TRUE);   /* MTK's video pipeline */
    if (!failed("setRecordingHint")) (*e)->CallVoidMethod(e, cam, (*e)->GetMethodID(e, cc, "setParameters", "(Landroid/hardware/Camera$Parameters;)V"), par);
    if (failed("setParameters")) goto out;

    rc = 4;
    jclass mfc = (*e)->FindClass(e, "android/media/MediaFormat"); mcc = (*e)->FindClass(e, "android/media/MediaCodec");
    jclass bic = (*e)->FindClass(e, "android/media/MediaCodec$BufferInfo");
    if (failed("codec classes")) goto out;
    jstring avc = (*e)->NewStringUTF(e, "video/avc");
    jobject fmt = (*e)->CallStaticObjectMethod(e, mfc, (*e)->GetStaticMethodID(e, mfc, "createVideoFormat", "(Ljava/lang/String;II)Landroid/media/MediaFormat;"), avc, W, H);
    if (failed("createVideoFormat") || !fmt) goto out;
    jmethodID si = (*e)->GetMethodID(e, mfc, "setInteger", "(Ljava/lang/String;I)V");
    const struct { const char *k; int v; } kv[] = { { "color-format", 0x7F000789 /* COLOR_FormatSurface */ }, { "bitrate", kbps * 1000 },
                                                     { "frame-rate", fps }, { "i-frame-interval", 1 } };
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) { (*e)->CallVoidMethod(e, fmt, si, (*e)->NewStringUTF(e, kv[i].k), kv[i].v); if (failed(kv[i].k)) goto out; }
    codec = (*e)->CallStaticObjectMethod(e, mcc, (*e)->GetStaticMethodID(e, mcc, "createEncoderByType", "(Ljava/lang/String;)Landroid/media/MediaCodec;"), avc);
    if (failed("createEncoderByType") || !codec) { codec = NULL; goto out; }
    (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "configure", "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V"), fmt, NULL, NULL, 1 /* ENCODE */);
    jobject surf = failed("configure") ? NULL : (*e)->CallObjectMethod(e, codec, (*e)->GetMethodID(e, mcc, "createInputSurface", "()Landroid/view/Surface;"));
    if (failed("createInputSurface") || !surf) goto out;
    (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "start", "()V"));
    if (failed("MediaCodec.start")) goto out;
    (*e)->CallVoidMethod(e, cam, (*e)->GetMethodID(e, cc, "setPreviewSurface", "(Landroid/view/Surface;)V"), surf);
    if (failed("setPreviewSurface")) goto out;
    int shown = env_int("HMCAM_DOT", 1);
    if (shown && dot(1)) { say("no dot on the screen: not streaming without it"); rc = 6; goto out; }   /* before the first frame can leave */
    if (shown) awake(1);
    t0 = now_ms();
    (*e)->CallVoidMethod(e, cam, (*e)->GetMethodID(e, cc, "startPreview", "()V"));
    if (failed("startPreview")) goto out;
    say("streaming %dx%d at %d fps, %d kbit/s", W, H, fps, kbps);

    rc = 0;
    jobject bi = (*e)->NewObject(e, bic, (*e)->GetMethodID(e, bic, "<init>", "()V"));
    jfieldID f_off = (*e)->GetFieldID(e, bic, "offset", "I"), f_sz = (*e)->GetFieldID(e, bic, "size", "I"),
             f_fl = (*e)->GetFieldID(e, bic, "flags", "I"), f_pts = (*e)->GetFieldID(e, bic, "presentationTimeUs", "J");
    jmethodID deq = (*e)->GetMethodID(e, mcc, "dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I"),
              gob = (*e)->GetMethodID(e, mcc, "getOutputBuffer", "(I)Ljava/nio/ByteBuffer;"),
              rel = (*e)->GetMethodID(e, mcc, "releaseOutputBuffer", "(IZ)V");
    if (failed("BufferInfo") || !bi) { rc = 4; goto out; }
    long long last = now_ms(), kept = last; int first = 1;
    while (!told_to_stop()) {
        jint i = (*e)->CallIntMethod(e, codec, deq, bi, (jlong)100000);
        if (failed("dequeueOutputBuffer")) { rc = 5; break; }
        if (i < 0) {                                /* -1 try again, -2 format changed, -3 buffers changed */
            if (now_ms() - last > STALL_MS) { say("no frames for %d s: stopping", STALL_MS / 1000); rc = 5; break; }
            continue;
        }
        jobject bb = (*e)->CallObjectMethod(e, codec, gob, i);
        if (failed("getOutputBuffer")) { rc = 5; break; }
        int off = (*e)->GetIntField(e, bi, f_off), sz = (*e)->GetIntField(e, bi, f_sz), fl = (*e)->GetIntField(e, bi, f_fl);
        long long pts = (long long)(*e)->GetLongField(e, bi, f_pts);
        uint8_t *p = bb ? (*e)->GetDirectBufferAddress(e, bb) : NULL;
        int bad = p && sz > 0 && record(fl & 2 ? 1 : fl & 1 ? 2 : 3, pts, p + off, (size_t)sz);
        (*e)->CallVoidMethod(e, codec, rel, i, JNI_FALSE);
        if (bb) (*e)->DeleteLocalRef(e, bb);
        if (failed("releaseOutputBuffer") || bad) break;            /* bad: hassmic stopped reading */
        if (first) { first = 0; say("first frame %lld ms after startPreview", now_ms() - t0); }
        last = now_ms();
        if (shown && last - kept >= 10000) { awake(0); kept = last; }
    }
out:
    if (cam) {
        (*e)->CallVoidMethod(e, cam, (*e)->GetMethodID(e, cc, "stopPreview", "()V")); failed("stopPreview");
        (*e)->CallVoidMethod(e, cam, (*e)->GetMethodID(e, cc, "release", "()V")); failed("release");
    }
    if (codec) {
        (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "stop", "()V")); failed("MediaCodec.stop");
        (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "release", "()V")); failed("MediaCodec.release");
    }
    dot(0);
    say("stopped (%d)", rc);
    exit(rc);
}

JNIEXPORT jint JNI_OnLoad(JavaVM *v, void *reserved)
{
    (void)reserved; pthread_t t;
    vm = v;
    if (pthread_create(&t, NULL, run, NULL)) return JNI_ERR;
    pthread_detach(t);
    return JNI_VERSION_1_6;
}

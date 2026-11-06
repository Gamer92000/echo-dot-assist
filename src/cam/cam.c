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
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
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

/* ---------------------------------------------------------------- a call's picture (HMCAM_MODE=play) */

/* A Drop In's picture on this screen: hassmic's video.c runs this mode as its child while a call has video and writes
 * the same records to its stdin as the stream mode writes to stdout.  No camera is opened.  Two kinds (video.c):
 *   the other side's (an Echo Show's camera, or a browser's through Echo Local): a SurfaceFlinger layer of the
 *     screen's size, cropped to fill it (16:9 on the Show 5's 2:1), under the band (the call's animation stays on it)
 *     and over the UI's layer and every app; HMCAM_BLUR_MS > 0: frosted for that long from the first picture, the
 *     last second clearing (the caller's screen when the Echo called did not ring: stock's Drop In does the same);
 *   HMCAM_SELF=1, this Show's own (decoded from the records that leave it): a quarter of the width in the bottom
 *     right corner, over the other side's, mirrored as a mirror is.
 * The codec's config comes either as a record of its own (type 1, as MediaCodec's encoder gives it) or inline ahead of
 * a key frame (a browser's WebCodecs in Annex B); delta frames before the first key frame are dropped.
 *
 * Drawn through GLES 2: MediaCodec's H.264 decoder (the MT8163's own) into a SurfaceTexture, a quad on the layer's
 * EGL surface that crops to fill, mirrors and frosts (the picture drawn small into FROST_DIV times smaller texture,
 * 16 taps a texel, then up again through 9 taps: shapes and colours, no faces).  Without GL (EGL refused, an SDK
 * without these constructors): the decoder straight into the layer as before, and a frosted picture stays hidden for
 * its frost instead: never seen sharp before it would have cleared.
 *
 * The picture must never outlive the call (a frozen face on someone's Show): stdin's end, or PLAY_IDLE_MS without a
 * frame, hides the layer and ends the process; the layer is this process's, so a kill takes it too.  stdout: nothing.
 * Exit status: 0 stdin ended, 2 no VM, 4 no decoder or layer, 5 no frames for PLAY_IDLE_MS. */
#define VIDEO_LAYER  1999995000         /* between the UI's 1999990000 and the band's 2000000000 (src/ui/android.c) */
#define SELF_LAYER   (VIDEO_LAYER + 1)
#define PLAY_IDLE_MS 3000
#define PLAY_MAX     (4 << 20)          /* a record's largest: a key frame of 1080p at a high rate is some 300 KB */
#define FROST_DIV    24                 /* the frosted picture: 40x20 on the Show 5's 960x480 */
#define FROST_FADE   1000               /* ms: the frost's last, clearing */
#define SELF_MARGIN  16
#define SELF_BOTTOM  40                 /* px above the bottom: clear of the band */

static int read_full(uint8_t *p, size_t n, int wait_ms)
{
    while (n) {
        struct pollfd q = { 0, POLLIN, 0 };
        int k = poll(&q, 1, wait_ms);
        if (k == 0) return 1;                                  /* nothing for wait_ms */
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        ssize_t r = read(0, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r; wait_ms = PLAY_IDLE_MS;        /* a record begun is read to its end */
    }
    return 0;
}

/* ---- GL: decoder -> SurfaceTexture -> crop, mirror, frost -> the layer */
static const char VS[] =
    "attribute vec2 a; uniform vec2 sc; uniform float mir; varying vec2 t; varying vec2 u;\n"
    "void main() { u = a * 0.5 + 0.5; t = 0.5 + (u - 0.5) * sc; if (mir > 0.5) t.x = 1.0 - t.x; gl_Position = vec4(a, 0.0, 1.0); }\n";
static const char FS_DOWN[] =                   /* into the small texture: 16 taps over each of its texels */
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float; uniform samplerExternalOES s; uniform mat4 st; uniform vec2 d; varying vec2 t;\n"
    "void main() { vec4 c = vec4(0.0);\n"
    "  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)\n"
    "    c += texture2D(s, (st * vec4(t + (vec2(float(i), float(j)) - 1.5) * d, 0.0, 1.0)).xy);\n"
    "  gl_FragColor = c / 16.0; }\n";
static const char FS_SHOW[] =                   /* on the layer: sharp, or frosted by k (1 frosted .. 0 sharp) */
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float; uniform samplerExternalOES s; uniform sampler2D b; uniform mat4 st; uniform vec2 bp;\n"
    "uniform float k; varying vec2 t; varying vec2 u;\n"
    "void main() { vec3 c = texture2D(s, (st * vec4(t, 0.0, 1.0)).xy).rgb;\n"
    "  if (k > 0.0) { vec3 f = vec3(0.0);\n"
    "    for (int i = -1; i <= 1; i++) for (int j = -1; j <= 1; j++)\n"
    "      f += texture2D(b, u + vec2(float(i), float(j)) * bp).rgb * ((i == 0 ? 2.0 : 1.0) * (j == 0 ? 2.0 : 1.0));\n"
    "    f = mix(f / 16.0, vec3(0.85), 0.15);\n"
    "    c = mix(c, f, k); }\n"
    "  gl_FragColor = vec4(c, 1.0); }\n";

struct gl {
    EGLDisplay dpy; EGLSurface sf; EGLContext cx; ANativeWindow *win;
    GLuint oes, small, fbo, p_down, p_show; int fw, fh, W, H;
    jobject st, ssurf; jmethodID upd, mat, ts; jfloatArray m;
};

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind); GLint ok = 0;
    glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512] = ""; glGetShaderInfoLog(s, sizeof l, NULL, l); say("GL shader: %s", l); glDeleteShader(s); return 0; }
    return s;
}

static GLuint program(const char *fs)
{
    GLuint v = shader(GL_VERTEX_SHADER, VS), f = shader(GL_FRAGMENT_SHADER, fs), p; GLint ok = 0;
    if (!v || !f) return 0;
    p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glBindAttribLocation(p, 0, "a"); glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { say("GL program did not link"); return 0; }
    return p;
}

static void gl_end(struct gl *g)
{
    if (g->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->sf != EGL_NO_SURFACE) eglDestroySurface(g->dpy, g->sf);
        if (g->cx != EGL_NO_CONTEXT) eglDestroyContext(g->dpy, g->cx);
        eglTerminate(g->dpy);
    }
    if (g->win) ANativeWindow_release(g->win);
    g->dpy = EGL_NO_DISPLAY; g->sf = EGL_NO_SURFACE; g->cx = EGL_NO_CONTEXT; g->win = NULL;
}

/* 0 ready: g->ssurf is the decoder's surface */
static int gl_begin(struct gl *g, jobject surf, int W, int H)
{
    g->dpy = EGL_NO_DISPLAY; g->sf = EGL_NO_SURFACE; g->cx = EGL_NO_CONTEXT; g->W = W; g->H = H;
    if (!(g->win = ANativeWindow_fromSurface(e, surf))) { say("GL: no window"); return -1; }
    g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) { say("GL: no display"); g->dpy = EGL_NO_DISPLAY; return -1; }
    const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(g->dpy, ca, &cfg, 1, &n) || n < 1) { say("GL: no config"); return -1; }
    if ((g->cx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, xa)) == EGL_NO_CONTEXT) { say("GL: no context (0x%x)", eglGetError()); return -1; }
    if ((g->sf = eglCreateWindowSurface(g->dpy, cfg, g->win, NULL)) == EGL_NO_SURFACE) { say("GL: no surface (0x%x)", eglGetError()); return -1; }
    if (!eglMakeCurrent(g->dpy, g->sf, g->sf, g->cx)) { say("GL: not current (0x%x)", eglGetError()); return -1; }
    if (!(g->p_down = program(FS_DOWN)) || !(g->p_show = program(FS_SHOW))) return -1;
    g->fw = W / FROST_DIV > 8 ? W / FROST_DIV : 8; g->fh = H / FROST_DIV > 8 ? H / FROST_DIV : 8;
    glGenTextures(1, &g->oes); glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->oes);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &g->small); glBindTexture(GL_TEXTURE_2D, g->small);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g->fw, g->fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g->fbo); glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->small, 0);
    int fb_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!fb_ok || glGetError() != GL_NO_ERROR) { say("GL: no frost texture"); return -1; }

    jclass stc = (*e)->FindClass(e, "android/graphics/SurfaceTexture"), sfc = (*e)->FindClass(e, "android/view/Surface");
    if (failed("SurfaceTexture class")) return -1;
    jobject st = (*e)->NewObject(e, stc, (*e)->GetMethodID(e, stc, "<init>", "(I)V"), (jint)g->oes);
    if (failed("SurfaceTexture") || !st) return -1;
    jobject ss = (*e)->NewObject(e, sfc, (*e)->GetMethodID(e, sfc, "<init>", "(Landroid/graphics/SurfaceTexture;)V"), st);
    if (failed("Surface(SurfaceTexture)") || !ss) return -1;
    g->st = (*e)->NewGlobalRef(e, st); g->ssurf = (*e)->NewGlobalRef(e, ss);
    g->upd = (*e)->GetMethodID(e, stc, "updateTexImage", "()V"); g->mat = (*e)->GetMethodID(e, stc, "getTransformMatrix", "([F)V");
    g->ts = (*e)->GetMethodID(e, stc, "getTimestamp", "()J");
    jfloatArray m = (*e)->NewFloatArray(e, 16);
    if (failed("SurfaceTexture methods") || !m) return -1;
    g->m = (*e)->NewGlobalRef(e, m);
    return 0;
}

/* the decoder's newest frame (released for rendering just before) onto the layer; k 1 frosted .. 0 sharp.
 * 1 drawn, 0 no new frame within 40 ms, -1 failed */
static int gl_draw(struct gl *g, long long *last_ts, float sx, float sy, int mirror, float k)
{
    long long t0 = now_ms(); jlong ts = *last_ts;
    for (;;) {                                  /* the frame reaches the SurfaceTexture a moment after its release */
        (*e)->CallVoidMethod(e, g->st, g->upd);
        if (failed("updateTexImage")) return -1;
        ts = (*e)->CallLongMethod(e, g->st, g->ts);
        if (ts != *last_ts) break;
        if (now_ms() - t0 > 40) return 0;
        usleep(2000);
    }
    *last_ts = ts;
    float m[16];
    (*e)->CallVoidMethod(e, g->st, g->mat, g->m); if (failed("getTransformMatrix")) return -1;
    (*e)->GetFloatArrayRegion(e, g->m, 0, 16, m);
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad); glEnableVertexAttribArray(0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->oes);
    if (k > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo); glViewport(0, 0, g->fw, g->fh);
        glUseProgram(g->p_down);
        glUniform1i(glGetUniformLocation(g->p_down, "s"), 0);
        glUniformMatrix4fv(glGetUniformLocation(g->p_down, "st"), 1, GL_FALSE, m);
        glUniform2f(glGetUniformLocation(g->p_down, "sc"), sx, sy);
        glUniform1f(glGetUniformLocation(g->p_down, "mir"), (float)mirror);
        glUniform2f(glGetUniformLocation(g->p_down, "d"), sx / (float)g->fw / 4.0f, sy / (float)g->fh / 4.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glViewport(0, 0, g->W, g->H);
    glUseProgram(g->p_show);
    glUniform1i(glGetUniformLocation(g->p_show, "s"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g->small);
    glUniform1i(glGetUniformLocation(g->p_show, "b"), 1);
    glUniformMatrix4fv(glGetUniformLocation(g->p_show, "st"), 1, GL_FALSE, m);
    glUniform2f(glGetUniformLocation(g->p_show, "sc"), sx, sy);
    glUniform1f(glGetUniformLocation(g->p_show, "mir"), (float)mirror);
    glUniform2f(glGetUniformLocation(g->p_show, "bp"), 1.0f / (float)g->fw, 1.0f / (float)g->fh);
    glUniform1f(glGetUniformLocation(g->p_show, "k"), k);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!eglSwapBuffers(g->dpy, g->sf)) { say("GL: swap failed (0x%x)", eglGetError()); return -1; }
    return 1;
}

static int play(void)
{
    jclass scc = (*e)->FindClass(e, "android/view/SurfaceControl"), ssc = (*e)->FindClass(e, "android/view/SurfaceSession"),
           sfc = (*e)->FindClass(e, "android/view/Surface"), dmc = (*e)->FindClass(e, "android/hardware/display/DisplayManagerGlobal"),
           dic = (*e)->FindClass(e, "android/view/DisplayInfo"), mfc = (*e)->FindClass(e, "android/media/MediaFormat"),
           mcc = (*e)->FindClass(e, "android/media/MediaCodec"), bic = (*e)->FindClass(e, "android/media/MediaCodec$BufferInfo");
    if (failed("play classes")) return 4;
    int self = env_int("HMCAM_SELF", 0) == 1, blur_ms = self ? 0 : env_int("HMCAM_BLUR_MS", 0);
    jobject dm = (*e)->CallStaticObjectMethod(e, dmc, (*e)->GetStaticMethodID(e, dmc, "getInstance", "()Landroid/hardware/display/DisplayManagerGlobal;"));
    jobject di = !failed("display") && dm ? (*e)->CallObjectMethod(e, dm, (*e)->GetMethodID(e, dmc, "getDisplayInfo", "(I)Landroid/view/DisplayInfo;"), 0) : NULL;
    if (failed("getDisplayInfo") || !di) return 4;
    int SW = (*e)->GetIntField(e, di, (*e)->GetFieldID(e, dic, "logicalWidth", "I")), SH = (*e)->GetIntField(e, di, (*e)->GetFieldID(e, dic, "logicalHeight", "I"));
    int W = SW, H = SH, X = 0, Y = 0;
    if (self) { W = (SW / 4) & ~1; H = (W * 9 / 16) & ~1; X = SW - W - SELF_MARGIN; Y = SH - H - SELF_BOTTOM; }
    jobject ss = (*e)->NewObject(e, ssc, (*e)->GetMethodID(e, ssc, "<init>", "()V"));
    jmethodID init = failed("SurfaceSession") ? NULL : (*e)->GetMethodID(e, scc, "<init>", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIII)V");
    if (failed("SurfaceControl(): this SDK has the Builder only") || !init || !ss) return 4;
    jobject sc = (*e)->NewObject(e, scc, init, ss, (*e)->NewStringUTF(e, self ? "hassmic-call-self" : "hassmic-call-video"), W, H, -1 /* OPAQUE */, 4 /* HIDDEN */);
    if (failed("SurfaceControl") || !sc) return 4;
    jobject surf = (*e)->NewObject(e, sfc, (*e)->GetMethodID(e, sfc, "<init>", "()V"));
    if (failed("Surface") || !surf) return 4;
    (*e)->CallVoidMethod(e, surf, (*e)->GetMethodID(e, sfc, "copyFrom", "(Landroid/view/SurfaceControl;)V"), sc);
    if (failed("copyFrom")) return 4;
    jmethodID op = (*e)->GetStaticMethodID(e, scc, "openTransaction", "()V"), cl = (*e)->GetStaticMethodID(e, scc, "closeTransaction", "()V"),
              m_show = (*e)->GetMethodID(e, scc, "show", "()V"), m_hide = (*e)->GetMethodID(e, scc, "hide", "()V");
    (*e)->CallStaticVoidMethod(e, scc, op);
    if (!failed("openTransaction")) {
        (*e)->CallVoidMethod(e, sc, (*e)->GetMethodID(e, scc, "setLayer", "(I)V"), self ? SELF_LAYER : VIDEO_LAYER); failed("setLayer");
        (*e)->CallVoidMethod(e, sc, (*e)->GetMethodID(e, scc, "setPosition", "(FF)V"), (float)X, (float)Y); failed("setPosition");
        (*e)->CallStaticVoidMethod(e, scc, cl); failed("closeTransaction");
    }

    struct gl g; memset(&g, 0, sizeof g);
    int gl = getenv("HMCAM_GL") && !strcmp(getenv("HMCAM_GL"), "0") ? 0 : !gl_begin(&g, surf, W, H);
    if (!gl) { gl_end(&g); say("no GL: the picture straight from the decoder%s", blur_ms ? ", hidden while it would be frosted" : self ? ", not mirrored" : ""); }

    jstring avc = (*e)->NewStringUTF(e, "video/avc");
    jobject fmt = (*e)->CallStaticObjectMethod(e, mfc, (*e)->GetStaticMethodID(e, mfc, "createVideoFormat", "(Ljava/lang/String;II)Landroid/media/MediaFormat;"), avc, 1280, 720);
    if (failed("createVideoFormat") || !fmt) return 4;
    (*e)->CallVoidMethod(e, fmt, (*e)->GetMethodID(e, mfc, "setInteger", "(Ljava/lang/String;I)V"), (*e)->NewStringUTF(e, "max-input-size"), PLAY_MAX);
    if (failed("max-input-size")) return 4;
    jobject codec = (*e)->CallStaticObjectMethod(e, mcc, (*e)->GetStaticMethodID(e, mcc, "createDecoderByType", "(Ljava/lang/String;)Landroid/media/MediaCodec;"), avc);
    if (failed("createDecoderByType") || !codec) return 4;
    (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "configure", "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V"), fmt, gl ? g.ssurf : surf, NULL, 0);
    if (failed("configure")) return 4;
    if (!gl) { (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "setVideoScalingMode", "(I)V"), 2 /* SCALE_TO_FIT_WITH_CROPPING */);
               failed("setVideoScalingMode"); }                /* stretched then, not fatal */
    (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "start", "()V"));
    if (failed("MediaCodec.start")) return 4;
    jmethodID deqi = (*e)->GetMethodID(e, mcc, "dequeueInputBuffer", "(J)I"), gib = (*e)->GetMethodID(e, mcc, "getInputBuffer", "(I)Ljava/nio/ByteBuffer;"),
              qib = (*e)->GetMethodID(e, mcc, "queueInputBuffer", "(IIIJI)V"),
              deqo = (*e)->GetMethodID(e, mcc, "dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I"),
              rel = (*e)->GetMethodID(e, mcc, "releaseOutputBuffer", "(IZ)V"),
              gof = (*e)->GetMethodID(e, mcc, "getOutputFormat", "()Landroid/media/MediaFormat;"),
              gi = (*e)->GetMethodID(e, mfc, "getInteger", "(Ljava/lang/String;)I"), ck = (*e)->GetMethodID(e, mfc, "containsKey", "(Ljava/lang/String;)Z");
    jobject bi = (*e)->NewObject(e, bic, (*e)->GetMethodID(e, bic, "<init>", "()V"));
    if (failed("codec methods") || !bi) return 4;
    say("playing %s on %dx%d at %d,%d%s%s", self ? "this Show's own picture" : "the other side's picture", W, H, X, Y,
        gl ? " through GL" : "", blur_ms ? ", frosted at first" : "");

    static uint8_t buf[PLAY_MAX];
    int rc = 0, keyed = 0, shown = 0; long in = 0, out = 0, dropped = 0; long long t0 = now_ms(), kept = 0, first_at = 0, last_ts = -1;
    float sx = 1, sy = 1;                                       /* crop to fill, from the decoder's size */
    for (;;) {
        uint8_t h[20];
        int r = read_full(h, sizeof h, PLAY_IDLE_MS);
        if (r < 0) break;                                       /* stdin ended: the call's picture is over */
        if (r > 0) { say("no picture for %d s: hidden", PLAY_IDLE_MS / 1000); rc = 5; break; }
        uint32_t n = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
        long long pts = 0; for (int i = 0; i < 8; i++) pts |= (long long)h[12 + i] << (8 * i);
        if (memcmp(h, "HMCV", 4) || n > PLAY_MAX) { say("not a record"); rc = 4; break; }
        if (read_full(buf, n, PLAY_IDLE_MS)) break;
        int type = h[4];
        if (type == 2) keyed = 1;
        if (type == 3 && !keyed) { dropped++; continue; }       /* from a key frame on */
        jint i = (*e)->CallIntMethod(e, codec, deqi, (jlong)50000);
        if (failed("dequeueInputBuffer")) { rc = 4; break; }
        if (i < 0) { dropped++; if (type != 1) keyed = 0; continue; }   /* the decoder is behind: the next key frame */
        jobject bb = (*e)->CallObjectMethod(e, codec, gib, i);
        uint8_t *p = !failed("getInputBuffer") && bb ? (*e)->GetDirectBufferAddress(e, bb) : NULL;
        jlong cap = p ? (*e)->GetDirectBufferCapacity(e, bb) : 0;
        if (!p || cap < (jlong)n) { say("an input buffer of %lld for %u bytes", (long long)cap, n); rc = 4; break; }
        memcpy(p, buf, n);
        (*e)->DeleteLocalRef(e, bb);
        (*e)->CallVoidMethod(e, codec, qib, i, 0, (jint)n, (jlong)pts, type == 1 ? 2 /* BUFFER_FLAG_CODEC_CONFIG */ : 0);
        if (failed("queueInputBuffer")) { rc = 4; break; }
        in++;
        int drawn = 0;
        for (;;) {                                              /* what is decoded, on the screen at once */
            jint o = (*e)->CallIntMethod(e, codec, deqo, bi, (jlong)0);
            if (failed("dequeueOutputBuffer")) { rc = 4; goto done; }
            if (o == -2 && gl) {                                /* the decoder's size: crop to fill the layer */
                jobject of = (*e)->CallObjectMethod(e, codec, gof);
                if (!failed("getOutputFormat") && of) {
                    #define KEY(k) (*e)->NewStringUTF(e, k)
                    int vw = (*e)->CallIntMethod(e, of, gi, KEY("width")), vh = (*e)->CallIntMethod(e, of, gi, KEY("height"));
                    if (!failed("format size") && (*e)->CallBooleanMethod(e, of, ck, KEY("crop-right")) && !failed("crop")) {
                        vw = (*e)->CallIntMethod(e, of, gi, KEY("crop-right")) - (*e)->CallIntMethod(e, of, gi, KEY("crop-left")) + 1;
                        vh = (*e)->CallIntMethod(e, of, gi, KEY("crop-bottom")) - (*e)->CallIntMethod(e, of, gi, KEY("crop-top")) + 1;
                        failed("crop size");
                    }
                    #undef KEY
                    if (vw > 0 && vh > 0) {
                        float va = (float)vw / (float)vh, la = (float)W / (float)H;
                        if (va > la) { sx = la / va; sy = 1; } else { sx = 1; sy = va / la; }
                        say("the picture is %dx%d", vw, vh);
                    }
                }
                continue;
            }
            if (o < 0) break;                                   /* -1 none yet, -3 buffers */
            (*e)->CallVoidMethod(e, codec, rel, o, JNI_TRUE);
            if (failed("releaseOutputBuffer")) { rc = 4; goto done; }
            if (gl) {
                long long at = first_at ? now_ms() - first_at : 0;
                float k = blur_ms <= 0 ? 0 : at >= blur_ms ? 0 : at <= blur_ms - FROST_FADE ? 1 : (float)(blur_ms - at) / FROST_FADE;
                int d = gl_draw(&g, &last_ts, sx, sy, self, k);
                if (d < 0) { rc = 4; goto done; }
                drawn |= d;
            } else drawn = 1;
            if (!out++) { say("first picture %lld ms after the start", now_ms() - t0); first_at = now_ms(); }
        }
        /* shown once there is a picture: no black flash; without GL a frosted one only once its frost is over */
        if (out && drawn && !shown && (gl || !blur_ms || now_ms() - first_at >= blur_ms)) {
            (*e)->CallStaticVoidMethod(e, scc, op);
            if (!failed("openTransaction")) { (*e)->CallVoidMethod(e, sc, m_show); failed("show"); (*e)->CallStaticVoidMethod(e, scc, cl); failed("closeTransaction"); }
            shown = 1; awake(1); kept = now_ms();
        }
        if (shown && now_ms() - kept >= 10000) { awake(0); kept = now_ms(); }
    }
done:
    (*e)->CallStaticVoidMethod(e, scc, op);
    if (!failed("openTransaction")) { (*e)->CallVoidMethod(e, sc, m_hide); failed("hide"); (*e)->CallStaticVoidMethod(e, scc, cl); failed("closeTransaction"); }
    (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "stop", "()V")); failed("MediaCodec.stop");
    (*e)->CallVoidMethod(e, codec, (*e)->GetMethodID(e, mcc, "release", "()V")); failed("MediaCodec.release");
    if (gl) gl_end(&g);
    say("picture ended: %ld records in, %ld pictures shown, %ld dropped", in, out, dropped);
    return rc;
}

static void *run(void *arg)
{
    (void)arg; int rc = 3;
    jobject cam = NULL, codec = NULL;
    jclass cc = NULL, mcc = NULL;
    if ((*vm)->AttachCurrentThread(vm, &e, NULL)) exit(2);
    signal(SIGPIPE, SIG_IGN);                   /* hassmic gone: write() fails, the camera is let go below */
    int W = env_int("HMCAM_W", 1280), H = env_int("HMCAM_H", 720), fps = env_int("HMCAM_FPS", 15), kbps = env_int("HMCAM_KBPS", 1500);
    if (getenv("HMCAM_MODE") && !strcmp(getenv("HMCAM_MODE"), "play")) { rc = play(); say("stopped (%d)", rc); exit(rc); }

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

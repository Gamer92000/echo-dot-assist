/*
 * The screen UI's platform on Android (the Echo Show): libhmui.so, loaded by scripts/device/screen.dex into an
 * app_process that hassmic starts as its child (uid system, SELinux su; screen.c):
 *
 *   CLASSPATH=.../screen.dex app_process / Screen .../libhmui.so      (HMUI_BAND: the light band's rows)
 *
 * JNI_OnLoad starts the UI's thread; the dex's main thread waits on it for ever, and this thread ends the process.
 * What runs where:
 *   layer     a SurfaceFlinger layer of our own (SurfaceControl, as ringbar.dex makes the band's; SurfaceControl.Builder
 *             from SDK 26, Android 9 has no such constructor) over the screen above
 *             the band, under the band's layer, over every window; shown only while hassmic's home activity is in
 *             front (scene.h says why).  Drawn through ANativeWindow_lock: the gralloc buffer itself, the dirty
 *             rectangle only (Surface copies the rest from the last frame).  On the Echo Show 5 (2026-10-14): a
 *             200x200 change costs 0.5 ms to draw and 0.5 ms to post, a whole frame 3.8 ms; lock waits for vsync.
 *   text      Skia through TextPaint/Canvas onto an ALPHA_8 bitmap (the system font, every script it covers, an
 *             ellipsis where it is cut), copied out as a coverage mask; the scene caches them
 *   images    artwork from Sendspin (JPEG) through BitmapFactory, cut square and scaled to 400 px once; the lists' images
 *             (Music Assistant's imageproxy through hassmic, "img") decoded the same way on a thread of their own, cut
 *             square and scaled to the size the scene shows them at, so that scrolling never waits for a JPEG
 *   picture   what shows under the drawer pulled over an app: SurfaceControl.screenshot of the panel as it is mounted
 *             (natural orientation), turned here into the layer's, as touches are; the drawer's shade dims it
 *   holding   an app back in front (the drawer closed over it, the home app relaunched): our last frame stays up until
 *             the app has drawn, judged from a 48x48 screenshot of the layers under ours every 100 ms (like the one
 *             taken as the drawer opened, or simply not black), 2.5 s at most.  Kiosk Satellite (Flutter) is stopped
 *             under hassmic's home activity and shows black for about a second when it comes back (checkers,
 *             2026-11-02): without this, that second showed through
 *   power     the drawer's Screen off: IPowerManager.goToSleep as the power button; a touch while it is off wakes it
 *   apps      the launcher activities from IPackageManager, labels and icons from each APK's own resources, on a
 *             thread of their own (a few hundred ms with icons); launched through IActivityManager as `am start` does
 *   front     IActivityManager.getTasks(1) every 0.5 s while the layer is hidden, 1 s while shown (0.85 ms a call)
 *   touch     the touchscreen's evdev node, read beside Android, turned as InputReader turns it; grabbed (EVIOCGRAB)
 *             only while the words and timers cover an app (words.c), taken and given back with no finger down
 *   Back      a KEYCODE_BACK pair through InputManager.injectInputEvent (uid system has INJECT_EVENTS)
 *   band      the light band (ring_band.c): the window manager's overscan for its rows (every app is laid out above
 *             them; as ringbar.dex did, which this replaces where the UI runs), a layer of its own over everything,
 *             one row per frame from hassmic ("band <w>" + w ARGB pixels), repeated down the band
 *   words     what was said, the reply and the timers (words.c): a layer of its own the size of ours, TRANSLUCENT,
 *             over ours and every window, under the band; shown whatever is in front, hidden while the drawer is
 *             out.  Drawn only when what it shows changes; fades and a ringing timer's pulse are SurfaceControl.setAlpha
 *             (a transaction, no pixels), so a whole picture (1.8 MB on the Show 5) goes up a few times a request and
 *             once a second while timers count down
 *
 * Every class, method and field reached through JNI is in the table below (J, F, C; JV, CV those of some Android
 * versions only); tools/mkscreen.py --framework checks it against the firmware's boot class path, as it checks the dex.
 */
#include <jni.h>
#include <android/bitmap.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include "scene.h"
#include "touch.h"

#define LAYER      1999990000           /* under the band's 2000000000, over every window (type x 10000 + 1000) */
#define BAND_LAYER 2000000000           /* above every window and ours; the boot animation sits at 2^30 */
#define WORDS_LAYER 1999995000          /* over ours, under the band's */
#define ART_PX     400                  /* artwork kept at this size (the music view's), asked of Music Assistant too */
/* The grip: rows along the top that no app has (overscan, as the band's along the bottom), so that a pull to open the
 * drawer that starts there reaches no app: Kiosk Satellite took one from its own top as its pull to refresh
 * (checkers, 2026-11-02).  16 rows: 2 mm on the Show 5 and the Show 8 (195 and 189 ppi); a finger's swipe from the
 * bezel lands in them.  Ours to draw: black, a short handle in the middle */
#define GRIP       16
#define ICON_PX    96
#define ART_MAX    (4 << 20)

static JavaVM *vm;

/* ---------------------------------------------------------------- the JNI table */

static jclass c_rect, c_session, c_sc, c_surface, c_sm, c_pm_stub, c_amn, c_list, c_cn, c_intent, c_ipm_stub, c_plist,
              c_obj, c_am, c_res, c_bitmap, c_config, c_canvas, c_tpaint, c_typeface, c_textutils, c_truncate,
              c_bfactory, c_im, c_keyevent, c_clock, c_dmg, c_wm_stub, c_scb;
static jmethodID m_below, m_rect, m_sleep, m_wake, m_bmp_copy, m_session, m_sc_init, m_open, m_close, m_layer, m_pos, m_show, m_hide, m_alpha, m_surface, m_copy,
                 m_service, m_pm_as, m_interactive, m_amn_default, m_tasks, m_start, m_size, m_get, m_cn_pkg,
                 m_intent0, m_intent, m_cat, m_classname, m_package, m_flags, m_ipm_as, m_query, m_plist_get,
                 m_tostring, m_am_init, m_am_add, m_am_close, m_res_system, m_res_dm, m_res_cfg, m_res_init,
                 m_res_text, m_res_draw, m_dr_bounds, m_dr_draw, m_bmp_create, m_bmp_crop, m_bmp_scale, m_bmp_recycle,
                 m_bmp_w, m_bmp_h, m_canvas, m_drawtext, m_tpaint, m_textsize, m_settf, m_measure, m_metrics,
                 m_tf_create, m_ellipsize, m_decode, m_im_get, m_inject, m_key, m_uptime, m_dmg_get, m_dinfo,
                 m_wm_as, m_overscan, m_scb_init, m_scb_name, m_scb_size, m_scb_format, m_scb_flags, m_scb_build, m_tasks1;
static jfieldID f_base, f_ri_ai, f_ri_label, f_ri_labelres, f_ri_icon, f_pi_pkg, f_pi_name, f_pi_label, f_pi_labelres,
                f_pi_icon, f_ci_app, f_app_src, f_app_flags, f_fm_ascent, f_fm_descent, f_cfg_alpha8, f_cfg_argb, f_tf_default,
                f_trunc_end, f_di_w, f_di_h, f_di_rot;

/* JV, CV: only where from <= ro.build.version.sdk < to (0: no bound), resolved there alone (NULL elsewhere): what Android
 * 9 (athena, SDK 28) changed of Android 7's (25) */
static int sdk;
#define C(v, n)                         { &v, n, 0, 0 }
#define CV(v, n, from, to)              { &v, n, from, to }
#define J(v, c, n, s, st)               { &v, c, n, s, st, 0, 0 }
#define JV(v, c, n, s, st, from, to)    { &v, c, n, s, st, from, to }
#define F(v, c, n, s, st)               { &v, c, n, s, st }
static const struct { jclass *v; const char *n; int from, to; } classes[] = {
    C(c_rect, "android/graphics/Rect"), C(c_session, "android/view/SurfaceSession"), C(c_sc, "android/view/SurfaceControl"), C(c_surface, "android/view/Surface"),
    C(c_sm, "android/os/ServiceManager"), C(c_pm_stub, "android/os/IPowerManager$Stub"),
    C(c_amn, "android/app/ActivityManagerNative"), C(c_list, "java/util/List"), C(c_cn, "android/content/ComponentName"),
    C(c_intent, "android/content/Intent"), C(c_ipm_stub, "android/content/pm/IPackageManager$Stub"),
    C(c_plist, "android/content/pm/ParceledListSlice"), C(c_obj, "java/lang/Object"),
    C(c_am, "android/content/res/AssetManager"), C(c_res, "android/content/res/Resources"),
    C(c_bitmap, "android/graphics/Bitmap"), C(c_config, "android/graphics/Bitmap$Config"),
    C(c_canvas, "android/graphics/Canvas"), C(c_tpaint, "android/text/TextPaint"), C(c_typeface, "android/graphics/Typeface"),
    C(c_textutils, "android/text/TextUtils"), C(c_truncate, "android/text/TextUtils$TruncateAt"),
    C(c_bfactory, "android/graphics/BitmapFactory"), C(c_im, "android/hardware/input/InputManager"),
    C(c_keyevent, "android/view/KeyEvent"), C(c_clock, "android/os/SystemClock"),
    C(c_dmg, "android/hardware/display/DisplayManagerGlobal"), C(c_wm_stub, "android/view/IWindowManager$Stub"),
    CV(c_scb, "android/view/SurfaceControl$Builder", 26, 0),
};
static const struct { jmethodID *v; const char *c, *n, *s; int st, from, to; } methods[] = {
    J(m_session, "android/view/SurfaceSession", "<init>", "()V", 0),
    JV(m_sc_init, "android/view/SurfaceControl", "<init>", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIII)V", 0, 0, 26),
    JV(m_scb_init, "android/view/SurfaceControl$Builder", "<init>", "(Landroid/view/SurfaceSession;)V", 0, 26, 0),
    JV(m_scb_name, "android/view/SurfaceControl$Builder", "setName", "(Ljava/lang/String;)Landroid/view/SurfaceControl$Builder;", 0, 26, 0),
    JV(m_scb_size, "android/view/SurfaceControl$Builder", "setSize", "(II)Landroid/view/SurfaceControl$Builder;", 0, 26, 0),
    JV(m_scb_format, "android/view/SurfaceControl$Builder", "setFormat", "(I)Landroid/view/SurfaceControl$Builder;", 0, 26, 0),
    JV(m_scb_flags, "android/view/SurfaceControl$Builder", "setFlags", "(I)Landroid/view/SurfaceControl$Builder;", 0, 26, 0),
    JV(m_scb_build, "android/view/SurfaceControl$Builder", "build", "()Landroid/view/SurfaceControl;", 0, 26, 0),
    J(m_open, "android/view/SurfaceControl", "openTransaction", "()V", 1),
    J(m_close, "android/view/SurfaceControl", "closeTransaction", "()V", 1),
    J(m_layer, "android/view/SurfaceControl", "setLayer", "(I)V", 0),
    J(m_pos, "android/view/SurfaceControl", "setPosition", "(FF)V", 0),
    J(m_show, "android/view/SurfaceControl", "show", "()V", 0),
    J(m_hide, "android/view/SurfaceControl", "hide", "()V", 0),
    J(m_alpha, "android/view/SurfaceControl", "setAlpha", "(F)V", 0),
    J(m_surface, "android/view/Surface", "<init>", "()V", 0),
    J(m_copy, "android/view/Surface", "copyFrom", "(Landroid/view/SurfaceControl;)V", 0),
    J(m_service, "android/os/ServiceManager", "getService", "(Ljava/lang/String;)Landroid/os/IBinder;", 1),
    J(m_pm_as, "android/os/IPowerManager$Stub", "asInterface", "(Landroid/os/IBinder;)Landroid/os/IPowerManager;", 1),
    J(m_interactive, "android/os/IPowerManager", "isInteractive", "()Z", 0),
    J(m_amn_default, "android/app/ActivityManagerNative", "getDefault", "()Landroid/app/IActivityManager;", 1),
    JV(m_tasks, "android/app/IActivityManager", "getTasks", "(II)Ljava/util/List;", 0, 0, 28),
    JV(m_tasks1, "android/app/IActivityManager", "getTasks", "(I)Ljava/util/List;", 0, 28, 0),     /* (Android 9: no flags) */
    J(m_start, "android/app/IActivityManager", "startActivityAsUser", "(Landroid/app/IApplicationThread;Ljava/lang/String;Landroid/content/Intent;Ljava/lang/String;Landroid/os/IBinder;Ljava/lang/String;IILandroid/app/ProfilerInfo;Landroid/os/Bundle;I)I", 0),
    J(m_size, "java/util/List", "size", "()I", 0),
    J(m_get, "java/util/List", "get", "(I)Ljava/lang/Object;", 0),
    J(m_cn_pkg, "android/content/ComponentName", "getPackageName", "()Ljava/lang/String;", 0),
    J(m_intent0, "android/content/Intent", "<init>", "()V", 0),
    J(m_intent, "android/content/Intent", "<init>", "(Ljava/lang/String;)V", 0),
    J(m_cat, "android/content/Intent", "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", 0),
    J(m_classname, "android/content/Intent", "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", 0),
    J(m_package, "android/content/Intent", "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;", 0),
    J(m_flags, "android/content/Intent", "addFlags", "(I)Landroid/content/Intent;", 0),
    J(m_ipm_as, "android/content/pm/IPackageManager$Stub", "asInterface", "(Landroid/os/IBinder;)Landroid/content/pm/IPackageManager;", 1),
    J(m_query, "android/content/pm/IPackageManager", "queryIntentActivities", "(Landroid/content/Intent;Ljava/lang/String;II)Landroid/content/pm/ParceledListSlice;", 0),
    J(m_plist_get, "android/content/pm/ParceledListSlice", "getList", "()Ljava/util/List;", 0),
    J(m_tostring, "java/lang/Object", "toString", "()Ljava/lang/String;", 0),
    J(m_am_init, "android/content/res/AssetManager", "<init>", "()V", 0),
    J(m_am_add, "android/content/res/AssetManager", "addAssetPath", "(Ljava/lang/String;)I", 0),
    J(m_am_close, "android/content/res/AssetManager", "close", "()V", 0),
    J(m_res_system, "android/content/res/Resources", "getSystem", "()Landroid/content/res/Resources;", 1),
    J(m_res_dm, "android/content/res/Resources", "getDisplayMetrics", "()Landroid/util/DisplayMetrics;", 0),
    J(m_res_cfg, "android/content/res/Resources", "getConfiguration", "()Landroid/content/res/Configuration;", 0),
    J(m_res_init, "android/content/res/Resources", "<init>", "(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V", 0),
    J(m_res_text, "android/content/res/Resources", "getText", "(I)Ljava/lang/CharSequence;", 0),
    J(m_res_draw, "android/content/res/Resources", "getDrawableForDensity", "(IILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;", 0),
    J(m_dr_bounds, "android/graphics/drawable/Drawable", "setBounds", "(IIII)V", 0),
    J(m_dr_draw, "android/graphics/drawable/Drawable", "draw", "(Landroid/graphics/Canvas;)V", 0),
    J(m_bmp_create, "android/graphics/Bitmap", "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", 1),
    J(m_bmp_crop, "android/graphics/Bitmap", "createBitmap", "(Landroid/graphics/Bitmap;IIII)Landroid/graphics/Bitmap;", 1),
    J(m_bmp_scale, "android/graphics/Bitmap", "createScaledBitmap", "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;", 1),
    J(m_bmp_recycle, "android/graphics/Bitmap", "recycle", "()V", 0),
    J(m_bmp_w, "android/graphics/Bitmap", "getWidth", "()I", 0),
    J(m_bmp_h, "android/graphics/Bitmap", "getHeight", "()I", 0),
    J(m_canvas, "android/graphics/Canvas", "<init>", "(Landroid/graphics/Bitmap;)V", 0),
    J(m_drawtext, "android/graphics/Canvas", "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V", 0),
    J(m_tpaint, "android/text/TextPaint", "<init>", "(I)V", 0),
    J(m_textsize, "android/text/TextPaint", "setTextSize", "(F)V", 0),
    J(m_settf, "android/text/TextPaint", "setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;", 0),
    J(m_measure, "android/text/TextPaint", "measureText", "(Ljava/lang/String;)F", 0),
    J(m_metrics, "android/text/TextPaint", "getFontMetrics", "()Landroid/graphics/Paint$FontMetrics;", 0),
    J(m_tf_create, "android/graphics/Typeface", "create", "(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;", 1),
    J(m_ellipsize, "android/text/TextUtils", "ellipsize", "(Ljava/lang/CharSequence;Landroid/text/TextPaint;FLandroid/text/TextUtils$TruncateAt;)Ljava/lang/CharSequence;", 1),
    J(m_decode, "android/graphics/BitmapFactory", "decodeByteArray", "([BII)Landroid/graphics/Bitmap;", 1),
    J(m_im_get, "android/hardware/input/InputManager", "getInstance", "()Landroid/hardware/input/InputManager;", 1),
    J(m_inject, "android/hardware/input/InputManager", "injectInputEvent", "(Landroid/view/InputEvent;I)Z", 0),
    J(m_key, "android/view/KeyEvent", "<init>", "(JJIII)V", 0),
    J(m_uptime, "android/os/SystemClock", "uptimeMillis", "()J", 1),
    J(m_dmg_get, "android/hardware/display/DisplayManagerGlobal", "getInstance", "()Landroid/hardware/display/DisplayManagerGlobal;", 1),
    J(m_dinfo, "android/hardware/display/DisplayManagerGlobal", "getDisplayInfo", "(I)Landroid/view/DisplayInfo;", 0),
    J(m_wm_as, "android/view/IWindowManager$Stub", "asInterface", "(Landroid/os/IBinder;)Landroid/view/IWindowManager;", 1),
    J(m_overscan, "android/view/IWindowManager", "setOverscan", "(IIIII)V", 0),
    J(m_below, "android/view/SurfaceControl", "screenshot", "(Landroid/graphics/Rect;IIIIZI)Landroid/graphics/Bitmap;", 1),
    J(m_rect, "android/graphics/Rect", "<init>", "()V", 0),
    J(m_sleep, "android/os/IPowerManager", "goToSleep", "(JII)V", 0),
    J(m_wake, "android/os/IPowerManager", "wakeUp", "(JLjava/lang/String;Ljava/lang/String;)V", 0),
    J(m_bmp_copy, "android/graphics/Bitmap", "copy", "(Landroid/graphics/Bitmap$Config;Z)Landroid/graphics/Bitmap;", 0),
};
static const struct { jfieldID *v; const char *c, *n, *s; int st; } fields[] = {
    F(f_base, "android/app/ActivityManager$RunningTaskInfo", "baseActivity", "Landroid/content/ComponentName;", 0),
    F(f_ri_ai, "android/content/pm/ResolveInfo", "activityInfo", "Landroid/content/pm/ActivityInfo;", 0),
    F(f_ri_label, "android/content/pm/ResolveInfo", "nonLocalizedLabel", "Ljava/lang/CharSequence;", 0),
    F(f_ri_labelres, "android/content/pm/ResolveInfo", "labelRes", "I", 0),
    F(f_ri_icon, "android/content/pm/ResolveInfo", "icon", "I", 0),
    F(f_pi_pkg, "android/content/pm/PackageItemInfo", "packageName", "Ljava/lang/String;", 0),
    F(f_pi_name, "android/content/pm/PackageItemInfo", "name", "Ljava/lang/String;", 0),
    F(f_pi_label, "android/content/pm/PackageItemInfo", "nonLocalizedLabel", "Ljava/lang/CharSequence;", 0),
    F(f_pi_labelres, "android/content/pm/PackageItemInfo", "labelRes", "I", 0),
    F(f_pi_icon, "android/content/pm/PackageItemInfo", "icon", "I", 0),
    F(f_ci_app, "android/content/pm/ComponentInfo", "applicationInfo", "Landroid/content/pm/ApplicationInfo;", 0),
    F(f_app_src, "android/content/pm/ApplicationInfo", "sourceDir", "Ljava/lang/String;", 0),
    F(f_app_flags, "android/content/pm/ApplicationInfo", "flags", "I", 0),
    F(f_fm_ascent, "android/graphics/Paint$FontMetrics", "ascent", "F", 0),
    F(f_fm_descent, "android/graphics/Paint$FontMetrics", "descent", "F", 0),
    F(f_cfg_alpha8, "android/graphics/Bitmap$Config", "ALPHA_8", "Landroid/graphics/Bitmap$Config;", 1),
    F(f_cfg_argb, "android/graphics/Bitmap$Config", "ARGB_8888", "Landroid/graphics/Bitmap$Config;", 1),
    F(f_tf_default, "android/graphics/Typeface", "DEFAULT", "Landroid/graphics/Typeface;", 1),
    F(f_trunc_end, "android/text/TextUtils$TruncateAt", "END", "Landroid/text/TextUtils$TruncateAt;", 1),
    F(f_di_w, "android/view/DisplayInfo", "logicalWidth", "I", 0),
    F(f_di_h, "android/view/DisplayInfo", "logicalHeight", "I", 0),
    F(f_di_rot, "android/view/DisplayInfo", "rotation", "I", 0),
};

static void say(const char *fmt, ...)                         /* a line to hassmic: its log (one line: Java's words may have more) */
{
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    for (char *c = b; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    printf("log %s\n", b); fflush(stdout);
}

static int failed(JNIEnv *env, const char *what)
{
    if (!(*env)->ExceptionCheck(env)) return 0;
    jthrowable t = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    jstring s = t ? (*env)->CallObjectMethod(env, t, m_tostring) : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); s = NULL; }
    const char *u = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    say("%s: %s", what, u ? u : "exception");
    if (u) (*env)->ReleaseStringUTFChars(env, s, u);
    if (s) (*env)->DeleteLocalRef(env, s);
    if (t) (*env)->DeleteLocalRef(env, t);
    return 1;
}

static int applies(int from, int to) { return (!from || sdk >= from) && (!to || sdk < to); }

static int resolve(JNIEnv *env)
{
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.sdk", v);
    sdk = atoi(v);
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        if (!applies(classes[i].from, classes[i].to)) continue;
        jclass c = (*env)->FindClass(env, classes[i].n);
        if (!c || failed(env, classes[i].n)) { (*env)->ExceptionClear(env); say("no class %s", classes[i].n); return -1; }
        *classes[i].v = (*env)->NewGlobalRef(env, c); (*env)->DeleteLocalRef(env, c);
    }
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (!applies(methods[i].from, methods[i].to)) continue;
        jclass c = (*env)->FindClass(env, methods[i].c);
        if (c) *methods[i].v = methods[i].st ? (*env)->GetStaticMethodID(env, c, methods[i].n, methods[i].s)
                                              : (*env)->GetMethodID(env, c, methods[i].n, methods[i].s);
        if (!c || !*methods[i].v) { (*env)->ExceptionClear(env); say("no method %s.%s%s", methods[i].c, methods[i].n, methods[i].s); return -1; }
        (*env)->DeleteLocalRef(env, c);
    }
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        jclass c = (*env)->FindClass(env, fields[i].c);
        if (c) *fields[i].v = fields[i].st ? (*env)->GetStaticFieldID(env, c, fields[i].n, fields[i].s)
                                            : (*env)->GetFieldID(env, c, fields[i].n, fields[i].s);
        if (!c || !*fields[i].v) { (*env)->ExceptionClear(env); say("no field %s.%s", fields[i].c, fields[i].n); return -1; }
        (*env)->DeleteLocalRef(env, c);
    }
    return 0;
}

/* Java strings from UTF-8 as it comes (NewStringUTF wants modified UTF-8 and aborts on a 4-byte sequence: emoji in a
 * track title); bad bytes become U+FFFD */
static jstring jstr(JNIEnv *env, const char *s)
{
    size_t n = strlen(s); jchar *u = malloc((n + 1) * sizeof *u), k = 0; size_t o = 0;
    if (!u) return NULL;
    for (size_t i = 0; i < n; ) {
        unsigned c = (unsigned char)s[i], cp, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) { u[o++] = 0xFFFD; i++; continue; }
        cp = len == 1 ? c : c & (0x7F >> len);
        int ok = 1;
        for (unsigned j = 1; j < len; j++) { unsigned d = (unsigned char)s[i + j]; if ((d & 0xC0) != 0x80) { ok = 0; break; } cp = cp << 6 | (d & 0x3F); }
        if (!ok) { u[o++] = 0xFFFD; i++; continue; }
        i += len;
        if (cp >= 0x10000 && cp <= 0x10FFFF) { cp -= 0x10000; u[o++] = (jchar)(0xD800 + (cp >> 10)); u[o++] = (jchar)(0xDC00 + (cp & 0x3FF)); }
        else u[o++] = (jchar)(cp > 0xFFFF ? 0xFFFD : cp);
    }
    (void)k;
    jstring r = (*env)->NewString(env, u, (jsize)o);
    free(u);
    return r;
}

static void cstr(JNIEnv *env, jobject o, char *out, size_t n)    /* any CharSequence or String, as UTF-8 */
{
    out[0] = 0;
    if (!o) return;
    jstring s = (*env)->CallObjectMethod(env, o, m_tostring);
    if (failed(env, "toString") || !s) return;
    const char *u = (*env)->GetStringUTFChars(env, s, NULL);    /* modified UTF-8: fine for labels and packages */
    if (u) { snprintf(out, n, "%s", u); (*env)->ReleaseStringUTFChars(env, s, u); }
    (*env)->DeleteLocalRef(env, s);
}

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- the platform */

static JNIEnv *ui_env;                                          /* the UI thread's */
static jobject typeface[2], amgr, power_;
static int wake_pipe[2] = { -1, -1 };
static struct ui_app *loaded_apps; static int loaded_n; static pthread_mutex_t apps_lock = PTHREAD_MUTEX_INITIALIZER;
static int apps_busy;

static int p_text(void *ctx, const char *s, int px, int bold, int maxw, struct ui_text *out)
{
    (void)ctx; JNIEnv *env = ui_env; int rc = -1;
    if ((*env)->PushLocalFrame(env, 16)) return -1;
    jobject paint = (*env)->NewObject(env, c_tpaint, m_tpaint, 1 | 128);      /* ANTI_ALIAS | SUBPIXEL_TEXT */
    if (failed(env, "TextPaint")) goto out;
    (*env)->CallVoidMethod(env, paint, m_textsize, (jfloat)(px * 0.8f));
    (*env)->CallObjectMethod(env, paint, m_settf, typeface[bold ? 1 : 0]);
    jstring js = jstr(env, s);
    jobject cut = (*env)->CallStaticObjectMethod(env, c_textutils, m_ellipsize, js, paint, (jfloat)maxw,
                                                 (*env)->GetStaticObjectField(env, c_truncate, f_trunc_end));
    if (failed(env, "ellipsize") || !cut) goto out;
    jstring line = (*env)->CallObjectMethod(env, cut, m_tostring);
    jobject fm = (*env)->CallObjectMethod(env, paint, m_metrics);
    if (failed(env, "metrics")) goto out;
    float asc = (*env)->GetFloatField(env, fm, f_fm_ascent), desc = (*env)->GetFloatField(env, fm, f_fm_descent);
    int w = (int)((*env)->CallFloatMethod(env, paint, m_measure, line) + 1.5f), h = px;
    if (w <= 1 || w > 4096) goto out;
    jobject bmp = (*env)->CallStaticObjectMethod(env, c_bitmap, m_bmp_create, w, h, (*env)->GetStaticObjectField(env, c_config, f_cfg_alpha8));
    if (failed(env, "bitmap") || !bmp) goto out;
    jobject canvas = (*env)->NewObject(env, c_canvas, m_canvas, bmp);
    float base = (h - (desc - asc)) / 2 - asc;                  /* the line centred in px */
    (*env)->CallVoidMethod(env, canvas, m_drawtext, line, (jfloat)0.5f, (jfloat)base, paint);
    if (failed(env, "drawText")) goto out;
    AndroidBitmapInfo bi; void *pix;
    if (AndroidBitmap_getInfo(env, bmp, &bi) || bi.format != ANDROID_BITMAP_FORMAT_A_8 || AndroidBitmap_lockPixels(env, bmp, &pix)) goto out;
    out->m = malloc((size_t)w * h);
    if (out->m) {
        for (int y = 0; y < h; y++) memcpy(out->m + (size_t)y * w, (uint8_t *)pix + (size_t)y * bi.stride, (size_t)w);
        out->w = w; out->h = h; out->stride = w; rc = 0;
    }
    AndroidBitmap_unlockPixels(env, bmp);
    (*env)->CallVoidMethod(env, bmp, m_bmp_recycle);
out:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return rc;
}

/* the width text would have (p_text's paint, uncut): words.c wraps by it, a word at a time and at a few sizes, so the
 * paint of the last size is kept */
static int p_measure(void *ctx, const char *s, int px, int bold)
{
    (void)ctx; JNIEnv *env = ui_env; int w = 0;
    static jobject paint; static int p_px = -1, p_bold = -1;
    if ((*env)->PushLocalFrame(env, 8)) return 0;
    if (!paint) {
        jobject p = (*env)->NewObject(env, c_tpaint, m_tpaint, 1 | 128);   /* ANTI_ALIAS | SUBPIXEL_TEXT, as p_text's */
        if (!failed(env, "TextPaint") && p) paint = (*env)->NewGlobalRef(env, p);
    }
    if (paint && (px != p_px || bold != p_bold)) {
        (*env)->CallVoidMethod(env, paint, m_textsize, (jfloat)(px * 0.8f));
        (*env)->CallObjectMethod(env, paint, m_settf, typeface[bold ? 1 : 0]);
        if (failed(env, "typeface")) p_px = -1; else { p_px = px; p_bold = bold; }
    }
    jstring js = paint && p_px == px ? jstr(env, s) : NULL;
    if (js) {
        float f = (*env)->CallFloatMethod(env, paint, m_measure, js);
        if (!failed(env, "measureText")) w = (int)(f + 1.5f);
    }
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return w;
}

static jobject activity_manager(JNIEnv *env)
{
    jobject am = (*env)->CallStaticObjectMethod(env, c_amn, m_amn_default);
    return failed(env, "ActivityManagerNative.getDefault") ? NULL : am;
}

static void power_manager(JNIEnv *env)                         /* power_, once "power" is there */
{
    jobject pb = (*env)->CallStaticObjectMethod(env, c_sm, m_service, (*env)->NewStringUTF(env, "power"));
    jobject pw = pb ? (*env)->CallStaticObjectMethod(env, c_pm_stub, m_pm_as, pb) : NULL;
    if (!failed(env, "power") && pw) power_ = (*env)->NewGlobalRef(env, pw);
}

static jobject package_manager(JNIEnv *env)
{
    jstring n = (*env)->NewStringUTF(env, "package");
    jobject b = (*env)->CallStaticObjectMethod(env, c_sm, m_service, n);
    jobject pm = b ? (*env)->CallStaticObjectMethod(env, c_ipm_stub, m_ipm_as, b) : NULL;
    return failed(env, "package manager") ? NULL : pm;
}

/* the launcher activities, of one package or all: a List of ResolveInfo */
static jobject launchers(JNIEnv *env, jobject pm, const char *pkg)
{
    jobject i = (*env)->NewObject(env, c_intent, m_intent, (*env)->NewStringUTF(env, "android.intent.action.MAIN"));
    (*env)->CallObjectMethod(env, i, m_cat, (*env)->NewStringUTF(env, "android.intent.category.LAUNCHER"));
    if (pkg) (*env)->CallObjectMethod(env, i, m_package, (*env)->NewStringUTF(env, pkg));
    jobject sl = (*env)->CallObjectMethod(env, pm, m_query, i, NULL, 0, 0);
    jobject l = sl ? (*env)->CallObjectMethod(env, sl, m_plist_get) : NULL;
    return failed(env, "queryIntentActivities") ? NULL : l;
}

static struct scene *the_scene;
static int screen_on_ = 1;                                      /* poll_front's last look */

static void p_launch(void *ctx, const char *pkg, const char *cls)
{
    (void)ctx; JNIEnv *env = ui_env;
    if ((*env)->PushLocalFrame(env, 32)) return;
    char c[200] = "";
    if (pkg && (!cls || !cls[0])) {                              /* the package's own launcher activity */
        jobject pm = package_manager(env), l = pm ? launchers(env, pm, pkg) : NULL;
        if (l && (*env)->CallIntMethod(env, l, m_size) > 0) {
            jobject ri = (*env)->CallObjectMethod(env, l, m_get, 0), ai = (*env)->GetObjectField(env, ri, f_ri_ai);
            cstr(env, (*env)->GetObjectField(env, ai, f_pi_name), c, sizeof c);
        }
        cls = c;
        if (!c[0]) { say("launch %s: no launcher activity", pkg); scene_launch_failed(the_scene, pkg, mono_ms()); goto out; }
    }
    jobject am = activity_manager(env);
    if (!am) { say("launch %s: no activity manager yet", pkg ? pkg : UI_HOME_PKG); scene_launch_failed(the_scene, pkg, mono_ms()); goto out; }
    /* an app as launchers start it (MAIN, LAUNCHER: a task started so, by root's kiosk_tick through monkey or the
     * launcher, is brought back, not given a second copy of its activity); our home activity by its name */
    jobject i = pkg ? (*env)->NewObject(env, c_intent, m_intent, (*env)->NewStringUTF(env, "android.intent.action.MAIN")) : (*env)->NewObject(env, c_intent, m_intent0);
    if (pkg) (*env)->CallObjectMethod(env, i, m_cat, (*env)->NewStringUTF(env, "android.intent.category.LAUNCHER"));
    (*env)->CallObjectMethod(env, i, m_classname, (*env)->NewStringUTF(env, pkg ? pkg : UI_HOME_PKG),
                             (*env)->NewStringUTF(env, pkg ? cls : "android.app.Activity"));
    /* NEW_TASK, as from outside an activity; RESET_TASK_IF_NEEDED as launchers start apps (back where it was) */
    (*env)->CallObjectMethod(env, i, m_flags, (jint)(0x10000000 | 0x00200000));
    jint r = (*env)->CallIntMethod(env, am, m_start, NULL, NULL, i, NULL, NULL, NULL, 0, 0, NULL, NULL, 0);
    if (failed(env, "startActivityAsUser") || r < 0) {
        say("launch %s/%s: %d", pkg ? pkg : UI_HOME_PKG, pkg ? cls : "", r); scene_launch_failed(the_scene, pkg, mono_ms());
    }
out:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
}

static void p_back(void *ctx)
{
    (void)ctx; JNIEnv *env = ui_env;
    if ((*env)->PushLocalFrame(env, 8)) return;
    jobject im = (*env)->CallStaticObjectMethod(env, c_im, m_im_get);
    if (failed(env, "InputManager") || !im) { (*env)->PopLocalFrame(env, NULL); return; }
    jlong t = (*env)->CallStaticLongMethod(env, c_clock, m_uptime);
    for (int a = 0; a < 2 && !failed(env, "uptimeMillis"); a++) {   /* ACTION_DOWN, ACTION_UP; KEYCODE_BACK 4 */
        jobject k = (*env)->NewObject(env, c_keyevent, m_key, t, t, a, 4, 0);
        (*env)->CallBooleanMethod(env, im, m_inject, k, 0);
        failed(env, "injectInputEvent");
    }
    (*env)->PopLocalFrame(env, NULL);
}

static void p_send(void *ctx, const char *line) { (void)ctx; printf("%s\n", line); fflush(stdout); }

/* a drawable into ICON_PX x ICON_PX, premultiplied as the buffer wants it (RGBA bytes = our layout) */
static uint32_t *render_icon(JNIEnv *env, jobject res, int id)
{
    uint32_t *px = NULL;
    if (!id || (*env)->PushLocalFrame(env, 8)) return NULL;
    jobject d = (*env)->CallObjectMethod(env, res, m_res_draw, id, 320, NULL);    /* xhdpi: 96 px for a 48 dp icon */
    if (failed(env, "icon") || !d) goto out;
    jobject bmp = (*env)->CallStaticObjectMethod(env, c_bitmap, m_bmp_create, ICON_PX, ICON_PX, (*env)->GetStaticObjectField(env, c_config, f_cfg_argb));
    if (failed(env, "icon bitmap") || !bmp) goto out;
    jobject canvas = (*env)->NewObject(env, c_canvas, m_canvas, bmp);
    (*env)->CallVoidMethod(env, d, m_dr_bounds, 0, 0, ICON_PX, ICON_PX);
    (*env)->CallVoidMethod(env, d, m_dr_draw, canvas);
    if (failed(env, "icon draw")) goto out;
    AndroidBitmapInfo bi; void *p;
    if (!AndroidBitmap_getInfo(env, bmp, &bi) && bi.format == ANDROID_BITMAP_FORMAT_RGBA_8888 && !AndroidBitmap_lockPixels(env, bmp, &p)) {
        if ((px = malloc(ICON_PX * ICON_PX * 4)))
            for (int y = 0; y < ICON_PX; y++) memcpy(px + y * ICON_PX, (uint8_t *)p + (size_t)y * bi.stride, ICON_PX * 4);
        AndroidBitmap_unlockPixels(env, bmp);
    }
    (*env)->CallVoidMethod(env, bmp, m_bmp_recycle);
out:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return px;
}

static void *apps_thread(void *arg)
{
    (void)arg; JNIEnv *env; long long t0 = mono_ms();
    if ((*vm)->AttachCurrentThread(vm, &env, NULL)) return NULL;
    struct ui_app *apps = NULL; int n = 0;
    if ((*env)->PushLocalFrame(env, 64)) goto done;
    jobject pm = package_manager(env);
    for (int i = 0; !pm && i < 30; i++) { sleep(2); pm = package_manager(env); }   /* (system_server starting again) */
    jobject l = pm ? launchers(env, pm, NULL) : NULL;
    int count = l ? (*env)->CallIntMethod(env, l, m_size) : 0;
    jobject sysres = (*env)->CallStaticObjectMethod(env, c_res, m_res_system);
    jobject dm = (*env)->CallObjectMethod(env, sysres, m_res_dm), cfg = (*env)->CallObjectMethod(env, sysres, m_res_cfg);
    if (failed(env, "system resources") || count <= 0) goto pop;
    apps = calloc((size_t)count, sizeof *apps);
    for (int i = 0; apps && i < count; i++) {
        if ((*env)->PushLocalFrame(env, 32)) break;
        struct ui_app *a = &apps[n];
        jobject ri = (*env)->CallObjectMethod(env, l, m_get, i), ai = ri ? (*env)->GetObjectField(env, ri, f_ri_ai) : NULL;
        jobject app = ai ? (*env)->GetObjectField(env, ai, f_ci_app) : NULL;
        if (failed(env, "ResolveInfo") || !app) { (*env)->PopLocalFrame(env, NULL); continue; }
        cstr(env, (*env)->GetObjectField(env, ai, f_pi_pkg), a->pkg, sizeof a->pkg);
        cstr(env, (*env)->GetObjectField(env, ai, f_pi_name), a->cls, sizeof a->cls);
        char src[256]; cstr(env, (*env)->GetObjectField(env, app, f_app_src), src, sizeof src);
        a->system = ((*env)->GetIntField(env, app, f_app_flags) & (1 | 0x80)) != 0;   /* FLAG_SYSTEM, FLAG_UPDATED_SYSTEM_APP */
        jobject label = (*env)->GetObjectField(env, ri, f_ri_label);
        if (!label) label = (*env)->GetObjectField(env, ai, f_pi_label);
        if (!label) label = (*env)->GetObjectField(env, app, f_pi_label);
        int lres = (*env)->GetIntField(env, ri, f_ri_labelres), ires = (*env)->GetIntField(env, ri, f_ri_icon);
        if (!lres) lres = (*env)->GetIntField(env, ai, f_pi_labelres);
        if (!lres) lres = (*env)->GetIntField(env, app, f_pi_labelres);
        if (!ires) ires = (*env)->GetIntField(env, ai, f_pi_icon);
        if (!ires) ires = (*env)->GetIntField(env, app, f_pi_icon);
        jobject am = (*env)->NewObject(env, c_am, m_am_init);     /* the APK's own resources */
        jobject res = NULL;
        if (!failed(env, "AssetManager") && (*env)->CallIntMethod(env, am, m_am_add, (*env)->NewStringUTF(env, src)) > 0 && !failed(env, "addAssetPath"))
            res = (*env)->NewObject(env, c_res, m_res_init, am, dm, cfg);
        failed(env, "Resources");
        if (label) cstr(env, label, a->label, sizeof a->label);
        else if (res && lres) { jobject t = (*env)->CallObjectMethod(env, res, m_res_text, lres); if (!failed(env, "label")) cstr(env, t, a->label, sizeof a->label); }
        if (!a->label[0]) snprintf(a->label, sizeof a->label, "%s", a->pkg);
        if (res) { a->icon.px = render_icon(env, res, ires); a->icon.w = a->icon.h = a->icon.px ? ICON_PX : 0; }
        if (am) (*env)->CallVoidMethod(env, am, m_am_close);
        (*env)->ExceptionClear(env);
        (*env)->PopLocalFrame(env, NULL);
        if (a->pkg[0] && strcmp(a->pkg, UI_HOME_PKG)) n++;
        else { free(a->icon.px); memset(a, 0, sizeof *a); }
    }
pop:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
done:
    (*vm)->DetachCurrentThread(vm);
    say("apps: %d in %lld ms", n, mono_ms() - t0);
    pthread_mutex_lock(&apps_lock);
    if (loaded_apps) { for (int i = 0; i < loaded_n; i++) free(loaded_apps[i].icon.px); free(loaded_apps); }
    loaded_apps = apps; loaded_n = n; apps_busy = 0;
    pthread_mutex_unlock(&apps_lock);
    if (write(wake_pipe[1], "a", 1) < 0) {}
    return NULL;
}

static void p_apps(void *ctx)
{
    (void)ctx; pthread_t t;
    pthread_mutex_lock(&apps_lock);
    int go = !apps_busy; apps_busy = 1;
    pthread_mutex_unlock(&apps_lock);
    if (go && pthread_create(&t, NULL, apps_thread, NULL)) apps_busy = 0;
    else if (go) pthread_detach(t);
}


/* ---------------------------------------------------------------- the lists' images */


static int disp_w, disp_h, disp_rot, layer_h;                   /* the display (logical, the band's rows too) and our layer */

/* asked for by the scene: key at px, until hassmic's answer is decoded (UI thread only) */
static struct { char key[40]; int px, size; long long at; } wants[160]; static int nwants;

static int bucket(int px) { return px <= 80 ? 80 : px <= 160 ? 160 : px <= 256 ? 256 : 512; }   /* the imageproxy's sizes */

/* asked again (the scene does after 20 s, or after its cache let it go): asked of hassmic again, which may have dropped
 * the first (its queue keeps the newest 64) */
static void p_image(void *ctx, const char *key, int px)
{
    (void)ctx; int size = bucket(px), asked = 0; long long now = mono_ms();
    for (int i = 0; i < nwants; i++) {
        if (strcmp(wants[i].key, key) || wants[i].size != size) continue;
        if (now - wants[i].at < 3000) asked = 1;                /* that size on its way already */
        if (wants[i].px == px) { if (!asked) { wants[i].at = now; printf("img %s %d\n", key, size); fflush(stdout); } return; }
    }
    if (nwants == (int)(sizeof wants / sizeof wants[0])) { memmove(wants, wants + 1, sizeof wants - sizeof wants[0]); nwants--; }
    snprintf(wants[nwants].key, sizeof wants[0].key, "%s", key); wants[nwants].px = px; wants[nwants].size = size; wants[nwants].at = now; nwants++;
    if (!asked) { printf("img %s %d\n", key, size); fflush(stdout); }
}

/* decoding, on a thread of its own: jobs in, pictures out (both under img_lock), the UI woken through the pipe */
struct img_job { char key[40]; int px[8], n; uint8_t *data; size_t len; uint32_t *out[8]; struct img_job *next; };
static struct img_job *jobs_in, *jobs_out;
static pthread_mutex_t img_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t img_cv = PTHREAD_COND_INITIALIZER;

/* a decoded bitmap cut square from its middle and scaled to px: opaque pixels in the buffer's layout, or NULL */
static uint32_t *square(JNIEnv *env, jobject bmp, int px)
{
    uint32_t *out = NULL;
    if ((*env)->PushLocalFrame(env, 8)) return NULL;
    int w = (*env)->CallIntMethod(env, bmp, m_bmp_w), h = (*env)->CallIntMethod(env, bmp, m_bmp_h), m = w < h ? w : h;
    if (m <= 0 || px <= 0 || px > 1024) goto out;
    jobject sq = w != h ? (*env)->CallStaticObjectMethod(env, c_bitmap, m_bmp_crop, bmp, (w - m) / 2, (h - m) / 2, m, m) : bmp;
    jobject sc = sq && m != px ? (*env)->CallStaticObjectMethod(env, c_bitmap, m_bmp_scale, sq, px, px, JNI_TRUE) : sq;
    if (failed(env, "scale") || !sc) goto out;
    AndroidBitmapInfo bi; void *p;
    if (!AndroidBitmap_getInfo(env, sc, &bi) && bi.format == ANDROID_BITMAP_FORMAT_RGBA_8888 && (int)bi.width == px && (int)bi.height == px &&
        !AndroidBitmap_lockPixels(env, sc, &p)) {
        if ((out = malloc((size_t)px * px * 4)))
            for (int y = 0; y < px; y++) {
                uint32_t *d = out + (size_t)y * px; const uint32_t *r = (const uint32_t *)((uint8_t *)p + (size_t)y * bi.stride);
                for (int x = 0; x < px; x++) d[x] = r[x] | 0xFF000000u;
            }
        AndroidBitmap_unlockPixels(env, sc);
    }
out:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return out;
}

static void *decode_thread(void *arg)
{
    (void)arg; JNIEnv *env;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL)) return NULL;
    for (;;) {
        pthread_mutex_lock(&img_lock);
        while (!jobs_in) pthread_cond_wait(&img_cv, &img_lock);
        struct img_job *j = jobs_in; jobs_in = j->next;
        pthread_mutex_unlock(&img_lock);
        if (j->len && !(*env)->PushLocalFrame(env, 16)) {
            jbyteArray arr = (*env)->NewByteArray(env, (jsize)j->len);
            if (arr) {
                (*env)->SetByteArrayRegion(env, arr, 0, (jsize)j->len, (const jbyte *)j->data);
                jobject bmp = (*env)->CallStaticObjectMethod(env, c_bfactory, m_decode, arr, 0, (jint)j->len);
                if (!failed(env, "decode") && bmp) for (int i = 0; i < j->n; i++) j->out[i] = square(env, bmp, j->px[i]);
            }
            (*env)->ExceptionClear(env);
            (*env)->PopLocalFrame(env, NULL);
        }
        free(j->data); j->data = NULL;
        pthread_mutex_lock(&img_lock); j->next = jobs_out; jobs_out = j; pthread_mutex_unlock(&img_lock);
        if (write(wake_pipe[1], "i", 1) < 0) {}
    }
    return NULL;
}

/* hassmic's answer for key at size (0: not said): decoded for the scene's sizes of that bucket alone, never an 80 px
 * picture blown up (takes the bytes) */
static void image_came(const char *key, int size, uint8_t *data, size_t len)
{
    struct img_job *j = calloc(1, sizeof *j);
    if (!j) { free(data); return; }
    snprintf(j->key, sizeof j->key, "%s", key); j->data = data; j->len = len;
    for (int i = 0; i < nwants && j->n < 8; ) {
        if (!strcmp(wants[i].key, key) && (!size || wants[i].size == size)) { j->px[j->n++] = wants[i].px; memmove(&wants[i], &wants[i + 1], (size_t)(nwants - i - 1) * sizeof wants[0]); nwants--; }
        else i++;
    }
    if (!j->n) { free(data); free(j); return; }                  /* asked for and then no longer wanted */
    pthread_mutex_lock(&img_lock); j->next = jobs_in; jobs_in = j; pthread_cond_signal(&img_cv); pthread_mutex_unlock(&img_lock);
}

static void images_done(struct scene *s)                        /* UI thread: what the decoder made */
{
    pthread_mutex_lock(&img_lock);
    struct img_job *j = jobs_out; jobs_out = NULL;
    pthread_mutex_unlock(&img_lock);
    while (j) {
        struct img_job *nx = j->next;
        for (int i = 0; i < j->n; i++) scene_image(s, j->key, j->px[i], j->out[i], j->out[i] ? j->px[i] : 0, j->out[i] ? j->px[i] : 0);
        free(j); j = nx;
    }
}

/* ---------------------------------------------------------------- holding until an app has drawn */

#define TH 48                                                   /* the small screenshot's side */
static uint8_t ref_lum[TH * TH]; static int ref_ok; static char ref_pkg[96], front[96];

/* the layers under ours (the app's own pixels, never our layers or the band's), TH x TH, as luminance: 0 ok */
static int below(JNIEnv *env, uint8_t *lum)
{
    int rc = -1;
    if ((*env)->PushLocalFrame(env, 8)) return -1;
    jobject r = (*env)->NewObject(env, c_rect, m_rect);
    jobject bmp = r ? (*env)->CallStaticObjectMethod(env, c_sc, m_below, r, TH, TH, 0, LAYER - 1, JNI_FALSE, 0) : NULL;
    if (failed(env, "screenshot below") || !bmp) goto out;
    AndroidBitmapInfo bi; void *p;
    if (AndroidBitmap_getInfo(env, bmp, &bi)) goto out;
    if (bi.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        bmp = (*env)->CallObjectMethod(env, bmp, m_bmp_copy, (*env)->GetStaticObjectField(env, c_config, f_cfg_argb), JNI_FALSE);
        if (failed(env, "screenshot copy") || !bmp || AndroidBitmap_getInfo(env, bmp, &bi) || bi.format != ANDROID_BITMAP_FORMAT_RGBA_8888) goto out;
    }
    if ((int)bi.width != TH || (int)bi.height != TH || AndroidBitmap_lockPixels(env, bmp, &p)) goto out;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TH; x++) {
            uint32_t c = ((const uint32_t *)((uint8_t *)p + (size_t)y * bi.stride))[x];
            lum[y * TH + x] = (uint8_t)(((c & 255) * 54 + (c >> 8 & 255) * 183 + (c >> 16 & 255) * 19) >> 8);
        }
    AndroidBitmap_unlockPixels(env, bmp);
    rc = 0;
out:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return rc;
}

/* the app in front has drawn: its pixels like those it had when the drawer opened over it, or (no such picture, or
 * it changed since) bright where that picture was, or at least not black */
static int drawn(JNIEnv *env)
{
    uint8_t cur[TH * TH]; long sum = 0, ref = 0, diff = 0;
    if (below(env, cur)) return 1;                              /* cannot tell: no reason to keep it covered */
    for (int i = 0; i < TH * TH; i++) { sum += cur[i]; ref += ref_lum[i]; diff += abs((int)cur[i] - (int)ref_lum[i]); }
    long n = TH * TH;
    if (ref_ok && !strcmp(ref_pkg, front)) return diff / n < 14 || (ref / n > 30 && sum / n > ref / n / 2);
    return sum / n > 8;
}

/* the picture under the drawer pulled over an app: the panel as mounted, turned as touch_map turns touches; the layers
 * under ours only (as below()), since ours still shows while an app's first frames are held */
static void p_screenshot(void *ctx)
{
    (void)ctx; JNIEnv *env = ui_env;
    int nw = disp_rot & 1 ? disp_h : disp_w, nh = disp_rot & 1 ? disp_w : disp_h;
    if ((*env)->PushLocalFrame(env, 8)) return;
    jobject r = (*env)->NewObject(env, c_rect, m_rect);
    jobject bmp = r ? (*env)->CallStaticObjectMethod(env, c_sc, m_below, r, nw, nh, 0, LAYER - 1, JNI_FALSE, 0) : NULL;
    if (failed(env, "screenshot") || !bmp) goto out;
    AndroidBitmapInfo bi; void *p;
    if (AndroidBitmap_getInfo(env, bmp, &bi)) goto out;
    if (bi.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {          /* 565 and the like: as ARGB first */
        bmp = (*env)->CallObjectMethod(env, bmp, m_bmp_copy, (*env)->GetStaticObjectField(env, c_config, f_cfg_argb), JNI_FALSE);
        if (failed(env, "screenshot copy") || !bmp || AndroidBitmap_getInfo(env, bmp, &bi) || bi.format != ANDROID_BITMAP_FORMAT_RGBA_8888) goto out;
    }
    if ((int)bi.width != nw || (int)bi.height != nh || AndroidBitmap_lockPixels(env, bmp, &p)) goto out;
    uint32_t *px = malloc((size_t)disp_w * layer_h * 4);
    if (px) {
        for (int y = 0; y < layer_h; y++)
            for (int x = 0; x < disp_w; x++) {
                int ax, ay, ly = y + GRIP;                       /* the natural panel's pixel for (x, y) of the layer */
                switch (disp_rot) {
                case 1: ax = nw - 1 - ly; ay = x; break;
                case 2: ax = nw - 1 - x; ay = nh - 1 - ly; break;
                case 3: ax = ly; ay = nh - 1 - x; break;
                default: ax = x; ay = ly;
                }
                px[(size_t)y * disp_w + x] = ((const uint32_t *)((uint8_t *)p + (size_t)ay * bi.stride))[ax] | 0xFF000000u;
            }
        scene_backdrop(the_scene, px, disp_w, layer_h);
    }
    AndroidBitmap_unlockPixels(env, bmp);
    ref_ok = !below(env, ref_lum); snprintf(ref_pkg, sizeof ref_pkg, "%s", front);   /* what it looks like drawn */
out:
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
}

static void power(int on)                                       /* the screen off as the power button does, or on */
{
    JNIEnv *env = ui_env;
    if (!power_ || (*env)->PushLocalFrame(env, 8)) return;
    jlong t = (*env)->CallStaticLongMethod(env, c_clock, m_uptime);
    if (on) (*env)->CallVoidMethod(env, power_, m_wake, t, (*env)->NewStringUTF(env, "hassmic:touch"), (*env)->NewStringUTF(env, "hassmic"));
    else (*env)->CallVoidMethod(env, power_, m_sleep, t, 4 /* GO_TO_SLEEP_REASON_POWER_BUTTON */, 0);
    failed(env, on ? "wakeUp" : "goToSleep");
    (*env)->PopLocalFrame(env, NULL);
}

static void p_sleep(void *ctx)
{
    (void)ctx; power(0);
    screen_on_ = 0; scene_screen(the_scene, 0, mono_ms());      /* now, not at poll_front's next look: a touch wakes it */
}

static const struct ui_platform plat = { p_text, p_launch, p_back, p_send, p_apps, p_image, p_screenshot, p_sleep, p_measure };

/* artwork: JPEG/PNG/BMP bytes -> ART_PX square (the middle of it), the buffer's layout */
static void art(struct scene *s, const uint8_t *b, size_t n)
{
    JNIEnv *env = ui_env; uint32_t *px = NULL;
    if (!n) { scene_art(s, NULL, 0, 0); return; }
    if ((*env)->PushLocalFrame(env, 16)) { scene_art(s, NULL, 0, 0); return; }
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)n);
    if (!arr) goto out;
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)n, (const jbyte *)b);
    jobject bmp = (*env)->CallStaticObjectMethod(env, c_bfactory, m_decode, arr, 0, (jint)n);
    if (failed(env, "decode") || !bmp) { say("artwork: %zu bytes that do not decode", n); goto out; }
    int w = (*env)->CallIntMethod(env, bmp, m_bmp_w), h = (*env)->CallIntMethod(env, bmp, m_bmp_h), m = w < h ? w : h;
    if (m <= 0) goto out;
    jobject sq = (w != h) ? (*env)->CallStaticObjectMethod(env, c_bitmap, m_bmp_crop, bmp, (w - m) / 2, (h - m) / 2, m, m) : bmp;
    jobject sc = sq && m != ART_PX ? (*env)->CallStaticObjectMethod(env, c_bitmap, m_bmp_scale, sq, ART_PX, ART_PX, JNI_TRUE) : sq;
    if (failed(env, "scale") || !sc) goto out;
    AndroidBitmapInfo bi; void *p;
    if (!AndroidBitmap_getInfo(env, sc, &bi) && bi.format == ANDROID_BITMAP_FORMAT_RGBA_8888 && bi.width == ART_PX && bi.height == ART_PX &&
        !AndroidBitmap_lockPixels(env, sc, &p)) {
        if ((px = malloc(ART_PX * ART_PX * 4)))
            for (int y = 0; y < ART_PX; y++) {
                uint32_t *d = px + y * ART_PX; const uint32_t *r = (const uint32_t *)((uint8_t *)p + (size_t)y * bi.stride);
                for (int x = 0; x < ART_PX; x++) d[x] = r[x] | 0xFF000000u;
            }
        AndroidBitmap_unlockPixels(env, sc);
    }
out:
    scene_art(s, px, ART_PX, ART_PX);                           /* NULL (it did not decode): none, not the last track's */
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
}
/* ---------------------------------------------------------------- the layers */

struct layer { jobject sc; ANativeWindow *win; int w, h, shown; };
static struct layer screen_l, band_l, grip_l, words_l;
static jobject session;

/* (every call checked before the next: one made with an exception pending aborts under CheckJNI) */
static void layer_show(JNIEnv *env, struct layer *l, int on)
{
    (*env)->CallStaticVoidMethod(env, c_sc, m_open);
    if (failed(env, "openTransaction")) return;
    (*env)->CallVoidMethod(env, l->sc, on ? m_show : m_hide);
    int bad = failed(env, on ? "show" : "hide");
    (*env)->CallStaticVoidMethod(env, c_sc, m_close);
    if (!failed(env, "closeTransaction") && !bad) l->shown = on;
}

/* the words' layer: shown or hidden, at that opacity, in one transaction */
static void layer_alpha(JNIEnv *env, struct layer *l, int on, float a)
{
    (*env)->CallStaticVoidMethod(env, c_sc, m_open);
    if (failed(env, "openTransaction")) return;
    int bad = 0;
    if (on) { (*env)->CallVoidMethod(env, l->sc, m_alpha, (jfloat)a); bad = failed(env, "setAlpha"); }
    if (!bad && on != l->shown) { (*env)->CallVoidMethod(env, l->sc, on ? m_show : m_hide); bad = failed(env, on ? "show" : "hide"); }
    (*env)->CallStaticVoidMethod(env, c_sc, m_close);
    if (!failed(env, "closeTransaction") && !bad) l->shown = on;
}

/* opaque: PixelFormat.OPAQUE and RGBX_8888, else TRANSLUCENT and RGBA_8888 (premultiplied) */
static int layer_make(JNIEnv *env, struct layer *l, const char *name, int w, int h, int y, int z, int opaque)
{
    if (!session) { jobject s = (*env)->NewObject(env, c_session, m_session); if (failed(env, "SurfaceSession") || !s) return -1; session = (*env)->NewGlobalRef(env, s); }
    jstring nm = (*env)->NewStringUTF(env, name);
    if (failed(env, "name") || !nm) return -1;
    jobject c = NULL;
    if (c_scb) {                                                /* SDK 26 on: the Builder, the constructor's arguments */
        jobject b = (*env)->NewObject(env, c_scb, m_scb_init, session);
        if (!failed(env, "SurfaceControl.Builder") && b) {
            (*env)->CallObjectMethod(env, b, m_scb_name, nm);
            if (!failed(env, "setName")) (*env)->CallObjectMethod(env, b, m_scb_size, w, h);
            if (!failed(env, "setSize")) (*env)->CallObjectMethod(env, b, m_scb_format, opaque ? -1 /* OPAQUE */ : -3 /* TRANSLUCENT */);
            if (!failed(env, "setFormat")) (*env)->CallObjectMethod(env, b, m_scb_flags, 4 /* HIDDEN */);
            if (!failed(env, "setFlags")) c = (*env)->CallObjectMethod(env, b, m_scb_build);
        }
    } else c = (*env)->NewObject(env, c_sc, m_sc_init, session, nm, w, h, opaque ? -1 : -3, 4 /* HIDDEN */);
    if (failed(env, "SurfaceControl") || !c) return -1;
    l->sc = (*env)->NewGlobalRef(env, c); l->w = w; l->h = h;
    (*env)->CallStaticVoidMethod(env, c_sc, m_open);
    if (failed(env, "openTransaction")) return -1;
    (*env)->CallVoidMethod(env, l->sc, m_layer, z);
    int bad = failed(env, "setLayer");
    if (!bad) { (*env)->CallVoidMethod(env, l->sc, m_pos, 0.0f, (jfloat)y); bad = failed(env, "setPosition"); }   /* in the rotated frame */
    (*env)->CallStaticVoidMethod(env, c_sc, m_close);
    if (failed(env, "closeTransaction") || bad) return -1;
    jobject surf = (*env)->NewObject(env, c_surface, m_surface);
    if (failed(env, "Surface") || !surf) return -1;
    (*env)->CallVoidMethod(env, surf, m_copy, l->sc);
    if (failed(env, "Surface")) return -1;
    l->win = ANativeWindow_fromSurface(env, surf);
    if (!l->win || ANativeWindow_setBuffersGeometry(l->win, w, h, opaque ? WINDOW_FORMAT_RGBX_8888 : WINDOW_FORMAT_RGBA_8888)) return -1;
    return 0;
}

/* The band's rows and the grip's to nobody else: overscan on the panel's natural sides that are at the bottom and at
 * the top now (IWindowManager setOverscan(display, left, top, right, bottom) is given unrotated; AOSP 7
 * PhoneWindowManager turns it: the bottom is rotation 0 bottom, 1 left, 2 top, 3 right, the top the side opposite).
 * The window manager keeps it in /data/system/display_settings.xml */
static struct { int rot, rows, top, owed; } osc;                  /* the overscan wanted, and not set yet ("window" not
                                                                   up: hassmic starts before the boot completes) */
static void overscan(JNIEnv *env, int rot, int rows, int top)
{
    osc.rot = rot; osc.rows = rows; osc.top = top; osc.owed = 1;
    jobject b = (*env)->CallStaticObjectMethod(env, c_sm, m_service, (*env)->NewStringUTF(env, "window"));
    jobject wm = b ? (*env)->CallStaticObjectMethod(env, c_wm_stub, m_wm_as, b) : NULL;
    if (failed(env, "window manager") || !wm) return;            /* (poll_front asks again) */
    int side[5] = { 0 }, bottom = rot == 0 ? 4 : rot;
    side[bottom] = rows; side[bottom == 4 ? 2 : bottom == 2 ? 4 : bottom == 1 ? 3 : 1] = top;
    (*env)->CallVoidMethod(env, wm, m_overscan, 0, side[1], side[2], side[3], side[4]);
    if (!failed(env, "setOverscan")) osc.owed = 0;
}

/* the grip: black with a handle in its middle; with the drawer out, the drawer's top: its colour, no handle (the drawer
 * has its own at its bottom).  Drawn again only when that changes */
static void grip_draw(JNIEnv *env, int drawer)
{
    ANativeWindow_Buffer buf;
    if (!grip_l.win || ANativeWindow_lock(grip_l.win, &buf, NULL)) return;
    struct gfx g = { buf.bits, buf.width, buf.height, buf.stride, { 0, 0, buf.width, buf.height } };
    gfx_fill(&g, g.clip, drawer ? 0x15181D : 0x000000);         /* ui.h SURF, the drawer's panel */
    float cx = buf.width / 2.0f, cy = buf.height / 2.0f;
    if (!drawer) gfx_rrect(&g, cx - 22, cy - 2, cx + 22, cy + 2, 2, 0x3A3F47, 255);
    ANativeWindow_unlockAndPost(grip_l.win);
    if (!grip_l.shown) layer_show(env, &grip_l, 1);
}

/* a row of the band (0xAARRGGBB little-endian, as ringbar.dex took them) down all its rows */
static void band_draw(JNIEnv *env, const uint8_t *row, int w)
{
    ANativeWindow_Buffer buf;
    if (!band_l.win || ANativeWindow_lock(band_l.win, &buf, NULL)) return;
    if (w > buf.width) w = buf.width;
    uint32_t *d0 = buf.bits;
    for (int x = 0; x < w; x++) d0[x] = 0xFF000000u | (uint32_t)row[4 * x + 2] | (uint32_t)row[4 * x + 1] << 8 | (uint32_t)row[4 * x] << 16;
    for (int x = w; x < buf.width; x++) d0[x] = 0xFF000000u;
    for (int y = 1; y < buf.height; y++) memcpy(d0 + (size_t)y * buf.stride, d0, (size_t)buf.width * 4);
    ANativeWindow_unlockAndPost(band_l.win);
    if (!band_l.shown) layer_show(env, &band_l, 1);
}

/* ---------------------------------------------------------------- touch */

/* the screen on, asked now (it may have come on or gone off by itself since poll_front looked: a pattern's wakeUp, the
 * timeout); -1 unknown */
static int interactive(void)
{
    JNIEnv *env = ui_env;
    if (!power_) return -1;
    jboolean on = (*env)->CallBooleanMethod(env, power_, m_interactive);
    return failed(env, "isInteractive") ? -1 : on != 0;
}
static void power(int on);
static void touch_wake(void) { power(1); }

/* ---------------------------------------------------------------- the loop */

static void poll_front(JNIEnv *env, struct scene *s, jobject am)
{
    char pkg[96] = "";
    if ((*env)->PushLocalFrame(env, 16)) return;
    if (!am) {                                                  /* none at start: system_server starting again ("display" */
        jobject a = activity_manager(env);                      /* up, "activity" not yet): asked for until it is there */
        if (a) am = amgr = (*env)->NewGlobalRef(env, a);
    }
    if (!power_) power_manager(env);
    if (osc.owed) overscan(env, osc.rot, osc.rows, osc.top);
    jobject l = !am ? NULL : m_tasks1 ? (*env)->CallObjectMethod(env, am, m_tasks1, 1) : (*env)->CallObjectMethod(env, am, m_tasks, 1, 0);
    int bad = failed(env, "getTasks") || !am;
    /* failing for good: system_server started again (this process is hassmic's child, not zygote's, and outlives it;
     * the binder proxies it holds are dead).  Ended: hassmic starts a new one, which asks for the services anew */
    static long long bad_since;
    if (!bad) bad_since = 0; else if (!bad_since) bad_since = mono_ms();
    else if (mono_ms() - bad_since > 10000) { say("getTasks failing for 10 s (the system started again?): ending, to start anew"); exit(7); }
    if (!bad && l && (*env)->CallIntMethod(env, l, m_size) > 0) {
        /* the front task by what started it, not what is on top of it (a permission dialog, another package's activity
         * in the app's task): the app the drawer goes back to, by its launcher, which brings that task back */
        jobject t = (*env)->CallObjectMethod(env, l, m_get, 0), cn = t ? (*env)->GetObjectField(env, t, f_base) : NULL;
        if (cn) cstr(env, (*env)->CallObjectMethod(env, cn, m_cn_pkg), pkg, sizeof pkg);
    }
    if (pkg[0]) { scene_top(s, pkg, mono_ms()); snprintf(front, sizeof front, "%s", pkg); }
    if (power_) {
        jboolean on = (*env)->CallBooleanMethod(env, power_, m_interactive);
        if (!failed(env, "isInteractive")) { scene_screen(s, on, mono_ms()); screen_on_ = on; }
    }
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
}

static void *ui_main(void *arg)
{
    (void)arg; JNIEnv *env;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL)) exit(2);
    ui_env = env;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (resolve(env)) exit(3);
    const char *b = getenv("HMUI_BAND"); int band = b ? atoi(b) : 0;
    jobject dmg = (*env)->CallStaticObjectMethod(env, c_dmg, m_dmg_get);
    jobject di = dmg ? (*env)->CallObjectMethod(env, dmg, m_dinfo, 0) : NULL;
    if (failed(env, "display") || !di) exit(4);
    /* the display as it is at start, for good: the layers, touches and screenshots go by it.  The Show runs landscape;
     * an app that turns it (Kiosk Satellite can, docs/kiosk-satellite.md) is turned back by root's kiosk_tick within
     * 10 s, meanwhile the grip, the band's Back and the drawer map as in landscape */
    int W = (*env)->GetIntField(env, di, f_di_w), H = (*env)->GetIntField(env, di, f_di_h), rot = (*env)->GetIntField(env, di, f_di_rot);
    int h = H - band - GRIP;
    disp_w = W; disp_h = H; disp_rot = rot; layer_h = h;
    if (W <= 0 || h <= 0 || layer_make(env, &screen_l, "hassmic-screen", W, h, GRIP, LAYER, 1)) { say("no layer"); exit(5); }
    overscan(env, rot, band, GRIP);                             /* band 0: none along the bottom */
    if (band > 0 && layer_make(env, &band_l, "hassmic-band", W, band, GRIP + h, BAND_LAYER, 1)) say("no band layer");
    if (layer_make(env, &grip_l, "hassmic-grip", W, GRIP, 0, BAND_LAYER, 1)) say("no grip layer"); else grip_draw(env, 0);
    if (layer_make(env, &words_l, "hassmic-words", W, h, GRIP, WORDS_LAYER, 0)) { say("no words layer"); words_l.win = NULL; }
    jobject tf = (*env)->GetStaticObjectField(env, c_typeface, f_tf_default);
    typeface[0] = (*env)->NewGlobalRef(env, tf);
    typeface[1] = (*env)->NewGlobalRef(env, (*env)->CallStaticObjectMethod(env, c_typeface, m_tf_create, tf, 1));   /* BOLD */
    jobject a = activity_manager(env); if (a) amgr = (*env)->NewGlobalRef(env, a);
    power_manager(env);
    if (pipe(wake_pipe)) exit(6);
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);

    struct touch t = { .rot = rot, .W = W, .H = H, .grip = GRIP, .screen_on = &screen_on_, .interactive = interactive, .wake = touch_wake };
    char tn[160];
    if (touch_open(&t, tn, sizeof tn) >= 0) {
        say("touch: %s, x %d..%d, y %d..%d", tn, t.xmin, t.xmax, t.ymin, t.ymax);
        /* touch_map takes the ranges as the natural panel's, as InputReader does; a driver that turned them already
         * (MediaTek's TOUCHSCREEN_PHYSICAL_ROTATION_WITH_LCM, the Show 8 1st gen's LCM at 90) would land every tap
         * turned: said, for the log to tell (no model seen so yet) */
        int nw = rot & 1 ? H : W, nh = rot & 1 ? W : H;
        if ((t.xmax - t.xmin > t.ymax - t.ymin) != (nw > nh))
            say("touch: those ranges are not the shape of the panel as mounted (%dx%d): taps will land turned", nw, nh);
    }
    struct scene *s = scene_new(W, h, band, &plat, NULL, mono_ms());
    the_scene = s;
    scene_words_grab(s, t.fd >= 0 ? 0 : -1);
    { pthread_t dt; if (!pthread_create(&dt, NULL, decode_thread, NULL)) pthread_detach(dt); }
    char loc[PROP_VALUE_MAX] = "";                              /* 12-hour clock where the locale has it */
    __system_property_get("persist.sys.locale", loc);
    if (!strncmp(loc, "en-US", 5) || !strncmp(loc, "en-CA", 5) || !strncmp(loc, "en-AU", 5)) scene_msg(s, "h12 1", mono_ms());
    printf("ready %d %d\n", W, h); fflush(stdout);
    say("screen: %dx%d between a grip of %d and a band of %d, rotation %d", W, h, GRIP, band, rot);
    if (W < H)                                                  /* (taken for good, above: until the UI starts again) */
        say("screen: portrait at start: the layout is made for landscape and stays small until the UI starts again");
    p_apps(NULL);                                               /* the page's home app choice: the apps at once */

    static uint8_t in[ART_MAX + 4096]; size_t have = 0, art_need = 0, band_need = 0, img_need = 0; int shown = -1, img_size = 0; char img_key[40] = "";
    long long next_front = 0, wake_watch = 0, frame_at = 0, hold_t0 = 0, next_hold = 0, next_touch = 0; int holding = 0, grip_as = 0;
    float words_alpha = -1;
    for (;;) {
        long long now = mono_ms();
        /* the activity in front: often while one asked for is on its way (our last frame stands in for it till then) */
        if (now >= next_front) { poll_front(env, s, amgr); next_front = now + (scene_waiting(s) ? 150 : !screen_on_ && now < wake_watch ? 100 : shown == 1 ? 1000 : 500); }
        int vis = scene_visible(s);
        int next = -1;
        struct rect d = scene_prepare(s, now, &next);
        if (!rect_empty(d)) {
            ANativeWindow_Buffer buf; ARect r = { d.x0, d.y0, d.x1, d.y1 };
            if (ANativeWindow_lock(screen_l.win, &buf, &r)) scene_invalidate(s);   /* not drawn: all of it next time */
            else {
                struct gfx g = { buf.bits, buf.width, buf.height, buf.stride, { r.left, r.top, r.right, r.bottom } };
                /* larger (the buffer did not keep the last frame): all of that; smaller (only ever d cut to the buffer):
                 * the buffer's part of d, and everything next time */
                if (g.clip.x0 > d.x0 || g.clip.y0 > d.y0 || g.clip.x1 < d.x1 || g.clip.y1 < d.y1) {
                    g.clip = rect_and(d, (struct rect){ 0, 0, buf.width, buf.height }); scene_invalidate(s);
                }
                scene_draw(s, &g);
                ANativeWindow_unlockAndPost(screen_l.win);
            }
            frame_at = now;
        }
        int wnext = -1; float wa = 0;                           /* the words' layer: drawn on a change, its alpha set */
        int wk = words_l.win ? scene_words(s, now, &wa, &wnext) : 0;
        if (wk == 2) {
            ANativeWindow_Buffer buf;
            if (ANativeWindow_lock(words_l.win, &buf, NULL)) { wk = 0; scene_words_lost(s); }   /* (owed: drawn next time) */
            else {
                struct gfx g = { buf.bits, buf.width, buf.height, buf.stride, { 0, 0, buf.width, buf.height } };
                scene_words_draw(s, &g);
                ANativeWindow_unlockAndPost(words_l.win);
            }
        }
        if (words_l.win && (wk > 0) != words_l.shown) layer_alpha(env, &words_l, wk > 0, wa);
        else if (wk > 0 && fabsf(wa - words_alpha) > 0.004f) layer_alpha(env, &words_l, 1, wa);
        if (wk > 0) words_alpha = wa;
        if (wnext >= 0 && (next < 0 || wnext < next)) next = wnext;
        /* the panel to the words while they cover an app (scene.h), back to Android once they go: never mid-touch */
        if (t.fd >= 0 && !touch_fingers(&t) && scene_words_wants_grab(s) != t.grabbed) {
            int want = scene_words_wants_grab(s);
            if (touch_grab(&t, want)) {
                say("touch: %s refused (%s): a ring over an app brings our screen up instead", want ? "grab" : "let go", strerror(errno));
                if (!want) { touch_gone(&t, s, now); next_touch = now; }   /* (closed: the kernel lets go of it) */
                scene_words_grab(s, -1);
            } else scene_words_grab(s, want);
        }
        int dr = scene_drawer(s);                                /* the grip as the drawer's top while it is out */
        if (dr != grip_as) { grip_draw(env, dr); grip_as = dr; }
        if (vis && holding) holding = 0;                         /* ours again before the app had drawn */
        if (!vis && shown == 1 && !holding) { holding = 1; hold_t0 = now; next_hold = now; }   /* an app came: hold */
        if (holding && now >= next_hold) {
            next_hold = now + 100;
            if (drawn(env) || now - hold_t0 > 2500) { holding = 0; layer_show(env, &screen_l, 0); shown = 0; }
        } else if (!holding && vis != shown && (frame_at || !vis)) { layer_show(env, &screen_l, vis); shown = vis; }
        int wait = next < 0 ? (int)(next_front - now) : next;
        if (wait > next_front - now) wait = (int)(next_front - now);
        if (holding && wait > next_hold - now) wait = (int)(next_hold - now);
        if (wait < 0) wait = 0;
        struct pollfd p[3] = { { 0, POLLIN, 0 }, { wake_pipe[0], POLLIN, 0 }, { t.fd, POLLIN, 0 } };
        if (poll(p, t.fd >= 0 ? 3 : 2, wait) < 0 && errno != EINTR) break;
        if (p[1].revents) {
            char c[16]; while (read(wake_pipe[0], c, sizeof c) > 0) {}
            pthread_mutex_lock(&apps_lock);
            if (loaded_apps) { scene_apps(s, loaded_apps, loaded_n); loaded_apps = NULL; loaded_n = 0; }
            pthread_mutex_unlock(&apps_lock);
            images_done(s);
        }
        if (t.fd >= 0 && (p[2].revents & (POLLERR | POLLHUP | POLLNVAL))) {   /* gone: poll would not sleep */
            say("touch: the device went away"); touch_gone(&t, s, now); next_touch = now + 5000; scene_words_grab(s, -1);
        } else if (t.fd >= 0 && p[2].revents) touch_read(&t, s);
        if (t.fd < 0 && now >= next_touch) { if (touch_open(&t, NULL, 0) >= 0) scene_words_grab(s, 0); next_touch = now + 5000; }
        if (p[0].revents) {
            ssize_t r = read(0, in + have, sizeof in - have);
            if (r <= 0) break;                                   /* hassmic is gone: so is the layer */
            have += (size_t)r;
            size_t at = 0;                                       /* what is used of in[]; moved down once a read */
            for (;;) {
                size_t left = have - at;
                if (band_need) {                                 /* a band row after "band <w>" */
                    if (left < band_need) break;
                    band_draw(env, in + at, (int)(band_need / 4));
                    at += band_need; band_need = 0;
                    continue;
                }
                if (art_need) {                                  /* an image after "art <n>" */
                    if (left < art_need) break;
                    art(s, in + at, art_need);
                    at += art_need; art_need = 0;
                    continue;
                }
                if (img_need) {                                  /* a list's image after "img <key> <n>" */
                    if (left < img_need) break;
                    uint8_t *data = malloc(img_need);
                    if (data) memcpy(data, in + at, img_need);
                    image_came(img_key, img_size, data, data ? img_need : 0);
                    at += img_need; img_need = 0;
                    continue;
                }
                uint8_t *nl = memchr(in + at, '\n', left);
                if (!nl) { if (at == 0 && have == sizeof in) have = 0; break; }
                *nl = 0;
                char *l = (char *)in + at;
                at = (size_t)(nl - in) + 1;
                if (!strncmp(l, "band ", 5)) {
                    long w = atol(l + 5);
                    if (w > 0 && w <= 4096) band_need = (size_t)w * 4;
                } else if (!strncmp(l, "art ", 4)) {
                    long n = atol(l + 4);
                    if (n > 0 && n <= ART_MAX) art_need = (size_t)n;
                    else if (!n) art(s, NULL, 0);
                } else if (!strncmp(l, "img ", 4)) {
                    long n = -1; img_size = 0;
                    if (sscanf(l + 4, "%39s %ld %d", img_key, &n, &img_size) >= 2 && n > 0 && n <= ART_MAX) img_need = (size_t)n;
                    else if (n == 0) image_came(img_key, img_size, NULL, 0);
                } else {
                    scene_msg(s, l, mono_ms());
                    /* a pattern with the screen off: hassmic's band wakes it (ring_band.c), without a word to us; looked at
                     * soon and often for a while, not the last frame from before it slept up for a second */
                    if (!screen_on_ && !strncmp(l, "p+ ", 3)) { wake_watch = mono_ms() + 3000; next_front = mono_ms() + 50; }
                }
            }
            memmove(in, in + at, have - at); have -= at;
        }
    }
    exit(0);
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *v, void *reserved)
{
    (void)reserved; pthread_t t;
    vm = v;
    if (pthread_create(&t, NULL, ui_main, NULL)) return JNI_ERR;
    pthread_detach(t);
    return JNI_VERSION_1_6;
}

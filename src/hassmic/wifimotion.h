/* Wi-Fi motion (experimental, off by default): someone moving between the Echo and the access point changes the level
 * of what the radio receives.  Polls the Wi-Fi driver's receive statistics 10 times a second while switched on and
 * reports motion like a PIR sensor: on while it moves, off 30 s after.  Not presence: someone keeping still does not
 * show.  Only where our kernel module can be loaded (device.conf KMOD) or the driver answers MediaTek's private command
 * RX_STAT (donut's MT7668); elsewhere absent. */
#ifndef WIFIMOTION_H
#define WIFIMOTION_H

#define WIFIMOTION_SENS_MIN 1
#define WIFIMOTION_SENS_MAX 10
#define WIFIMOTION_SENS_DEFAULT 5

int  wifimotion_present(void);                      /* module or RX_STAT; probed at the first call */
void wifimotion_start(void (*changed)(void));       /* poller thread, once; each call adds its changed() (4 at most):
                                                     * wifimotion_motion() changed, called with no locks held */
int  wifimotion_enable(int set);                    /* -1: query */
/* A person switched it on (the page, an import, Home Assistant; never the start's load of state/config): state/
 * wifi-motion-on is written, which root's main.sh kmod() takes as the word to try the module again after a load the
 * kernel fell in (its mark, KMOD_TRY, older than this).  An off and on within a second is seen so */
void wifimotion_switched_on(void);
int  wifimotion_sensitivity(int set);               /* WIFIMOTION_SENS_MIN..MAX, clamped; -1: query */
int  wifimotion_motion(void);                       /* 1 moving, 0 still, -1 off (state unknown) */

/* The detector on its own (tests/unit/wifimotion_test.c): one RCPI reading per call, 10 a second. */
#define WM_WINDOW 20                                /* 2 s */
struct wm_det { float raw[3], med[WM_WINDOW]; unsigned char above[WM_WINDOW]; int n; long long last_ms; };
void wm_reset(struct wm_det *d);
int  wm_feed(struct wm_det *d, float rcpi, long long now_ms, int sensitivity, int hold_ms);   /* 1: motion */
int  wm_parse_rcpi(const char *rx_stat);
/* Before the detector: each reading as its distance from the level of frames of the same kind, i.e. sent at the same
 * power (key: the module's KIND, 0 where there is none).  0 while that kind's level is still being set (its first 5
 * readings). */
#define WM_KINDS 16
struct wm_kind { unsigned key; int n; float level; long long last_ms; };
struct wm_kinds { struct wm_kind k[WM_KINDS]; };
int  wm_kind_norm(struct wm_kinds *w, int rcpi, unsigned key, long long now_ms, float *out);            /* "RCPI RX0 = 112" of RX_STAT's answer; -1 if none or invalid */
#endif

# Kiosk Satellite on an Echo Show with hassmic

[Kiosk Satellite](https://kiosksatellite.com/) (KS, `me.jxl.kiosk_satellite`, Flutter + WebView) is the optional dashboard
on the Shows (`crown` Show 8 1st gen, `checkers` Show 5, `athena` Show 8 2nd gen): the screen shows a Home Assistant
dashboard, hassmic is the voice satellite. The two share the Echo and nothing else; this page is how they are kept
apart, and what hassmic does for the app so that it works without anyone at a keyboard. The code is in
`scripts/system/main.sh` (`kiosk_helper`, `kiosk_grants`, `rotation_lock`), `scripts/device/lockdown.sh` (`kiosk_state`,
`keep`), `devices/<codename>/device.conf` (`KIOSK_PACKAGE`), `devices/athena/sepolicy.rules`. Findings per model:
`re-crown.md` "Kiosk Satellite", `re-athena.md`.

KS itself is not part of `scripts/setup.sh` (the user picks the app; VACA and others exist). Install it by hand
(`adb install kiosk-satellite-<version>.armeabi-v7a.apk`, minSdk 24, 32-bit ARM) and, if the screen needs one, the
keyboard step of setup (Simple Keyboard 6.7: the Echo has no input method of its own). Needs a WebView provider
(crown: `app/AmazonWebView` must stay on the partition). Turn KS's own voice assistant off: hassmic is the satellite.

## Who does what

| | hassmic | Kiosk Satellite |
|---|---|---|
| Microphone, wake word, TTS, Assist pipeline | yes | no (denied the microphone) |
| Screen: home activity, overscan, light band layer | `org.hassmic.home` stays HOME | opened over it once per boot (`monkey`) |
| Dashboard, HA web UI | no | yes |
| Rotation | `rotation_lock` puts the settings back every 10 s (the Show has no accelerometer; KS flips it) | would write them itself |
| Wake word arbitration with tablets | `arbitration_mode` "kiosk" (UDP 2330, `kiosk-arbitration.md`) | speaks the same protocol |
| Own updates | not hassmic's business; the helper below | installs its updates without a dialog |

KS is **not** made the home app (it declares no HOME category; the home activity keeps the overscan and the band). If
the app crashes the plain home screen shows until the next boot or `monkey -p me.jxl.kiosk_satellite -c
android.intent.category.LAUNCHER 1`.

## Permissions

`main.sh` `kiosk_grants` applies these once per boot (before the app is opened; idempotent), as root:

| Permission | Why |
|---|---|
| `CAMERA`, `ACCESS_COARSE_LOCATION`, `ACCESS_FINE_LOCATION`, `READ/WRITE_EXTERNAL_STORAGE` (runtime, `pm grant`) | dashboards that use them, the app's settings and downloads; nobody is there to answer a dialog |
| `SYSTEM_ALERT_WINDOW`, `WRITE_SETTINGS`, `GET_USAGE_STATS`, `REQUEST_INSTALL_PACKAGES` (app ops, `appops set ... allow`) | overlays, screen brightness and timeout, the foreground check, installing its own updates |
| `dumpsys deviceidle whitelist +<pkg>`, `dpm set-active-admin <pkg>/.KioskAdminReceiver` | no doze for the dashboard and its ESPHome server; the device admin is what lets it turn the screen off (force-lock; it has no other admin power here) |
| **`RECORD_AUDIO` denied** (`pm revoke` + `appops set ... deny`) | hassmic owns the microphone: the mixer hands the stream to one client, and a second recorder would also hear the wake sound and replies |

Granted by installing: `INTERNET`, network and Wi-Fi state, multicast, wake lock, vibrate, Bluetooth, boot receiver,
foreground service, battery-optimization exemption, audio settings.  Not available on Android 9 and so skipped without
complaint: notifications, `BLUETOOTH_SCAN/CONNECT`, `READ_MEDIA_*` (the grants fail with "Unknown permission");
`MANAGE_EXTERNAL_STORAGE`, `SCHEDULE_EXACT_ALARM`, `PACKAGE_USAGE_STATS` have no app op of that name (usage stats is
`GET_USAGE_STATS`). `READ_LOGS` and `UPDATE_PACKAGES_WITHOUT_USER_ACTION` are signature level: only the update helper's
path installs without a dialog.

## The update helper

The app's content provider `content://me.jxl.kiosk_satellite.update-helper/start` returns a script (`CLASSPATH` = the
APK, a per-uid token) that starts `app_process ... UpdateHelper serve <uid> <token>` under the name
`ks-update-helper-<uid>`: the app's own code listening on a local TCP socket, through which the app installs its updates
(Home Assistant's update entity, or its own page) without a dialog. Its documentation has it piped into `sh` from adb
after every boot; `main.sh` `kiosk_helper` does it instead:

- 10 s after boot: as the shell user (what adb is on a locked device: `pm install` is allowed to it) with groups
  `log,inet` (`inet`: its `ServerSocket` fails with "Permission denied" without it), never as root; through `runas`
  (no `su` on the image).
- again every 5 minutes: the script is idempotent ("already running"), an update of the app ends the process, so the
  next round starts the new one.
- boot.log: `kiosk update helper: started` once, silence while it is up, otherwise its last line.

The script's `status` check is a client connecting to the helper's socket from **su**: it is why the helper looked dead on
the Show 8 2nd gen (below).

## Internet access

The firewall invariant stays (`lockdown.sh`): every process on the Echo may reach local addresses only, hassmic's group
(3990) anything, Amazon's daemons nothing. KS needs more than the LAN when HA is reached by an external or hosted
URL, for its updates (GitHub) and for the pages a dashboard shows. Setting **`kiosk_internet`** (settings page,
System; screen models only; default on; not exported): `kiosk_state` reads it from `state/config` and `hassmic_out`
gets `-m owner --uid-owner <uid of KIOSK_PACKAGE, from /data/system/packages.list> -j RETURN` before the group rule:
that one uid, nothing else. Off removes the rule.  Also admitted: INPUT TCP 2324 (KS's web page and API) and 6053 (its ESPHome API, the one Home Assistant's ESPHome integration connects to; hassmic's own is 26053), both outside
16384-32767) from private addresses only (`keep`), and UDP 2330 in `arbitration_mode=kiosk`.

Checked on the Show 8 2nd gen (2026-10-15): KS's uid reaches `api.github.com:443`; root, system and shell time out;
`iptables -L hassmic_out -v` counts the uid rule's packets. (A resolver of the user's network may block some public
addresses regardless: 1.1.1.1:443 timed out from the same uid while GitHub connected.)

## Show 8 2nd gen: `connect()` failed for everything but apps

On Android 9 the libc's `connect()`, `accept4()` and `sendto()` ask netd's fwmarkd (libnetd_client) to mark the socket.
netd then reads the caller's socket, and stock policy lets it do that for the app domains but not for `su` (every
hassmic service, root's scripts, `runas`, adb): `avc: denied { read write } ... scontext=u:r:netd:s0
tcontext=u:r:su:s0 tclass=tcp_socket permissive=0` and the call fails with **EBADF**. Symptoms: `toybox nc` and `ping`
failing, hassmic's HTTP fetches (before `net_connect_fd`), libcurl, and the update helper's status check, so the script
said "Could not start the update helper" every 5 minutes while the helper was in fact listening.  Fix: three rules in
`devices/athena/sepolicy.rules` (`allow netd su {tcp,udp,rawip}_socket { read write getattr getopt setopt ioctl }`),
installed with the policy by `scripts/install-system.sh` (a reboot loads it); the raw system calls of `netio.h` stay as
a second line. The app itself runs as `untrusted_app` and was never affected. Not seen on crown or checkers (Android 7).

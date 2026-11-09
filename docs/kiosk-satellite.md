# Kiosk Satellite on an Echo Show with hassmic

[Kiosk Satellite](https://kiosksatellite.com/) (KS, `me.jxl.kiosk_satellite`, Flutter + WebView) is the optional dashboard
on the Shows (`crown` Show 8 1st gen, `athena` Show 8 2nd gen, `checkers` Show 5, `cronos` Show 5 2nd gen): the screen shows a Home Assistant
dashboard, hassmic is the voice satellite. The two share the Echo and nothing else; this page is how they are kept
apart, and what hassmic does for the app so that it works without anyone at a keyboard. The code is in
`scripts/system/main.sh` (`kiosk_tick`, `kiosk_grants`), `scripts/device/lockdown.sh` (`kiosk_state`,
`keep`), `devices/<codename>/device.conf` (`KIOSK_PACKAGE`), `devices/athena/sepolicy.rules`. Findings per model:
`re-crown.md` "Kiosk Satellite", `re-checkers.md`.

The guided setup offers KS (default no; `install_screen_apps`, VACA and others stay possible by hand), and
`scripts/kiosk-install.sh [echo-ip]` does it on an installed Echo: newest release (armeabi-v7a, minSdk 24), checked against GitHub's
sha256 and, on the PC before anything reaches the Echo, against the signing certificate pinned in `scripts/lib/kiosk.sh` (`scripts/lib/apkcert.py`: only a zip with no comment and no zip64, so that it and Android read the same block; Android then verifies that block),, plus Simple Keyboard 6.7 (the Echo has no input method). Needs a WebView provider
(crown, checkers, cronos: `app/AmazonWebView` is the only one, so it stays on the partition; an Echo that lost it gets it back with the update that brings this). Turn KS's own voice assistant off: hassmic is the satellite.

## Who does what

| | hassmic | Kiosk Satellite |
|---|---|---|
| Microphone, wake word, TTS, Assist pipeline | yes | no (denied the microphone) |
| Screen: home activity, overscan, light band layer | `org.hassmic.home` stays HOME | opened over it whenever the home screen shows (`monkey`) |
| Dashboard, HA web UI | no | yes |
| Wake word arbitration with tablets | `arbitration_mode` "kiosk" (UDP 2330, `kiosk-arbitration.md`) | speaks the same protocol |
| Own updates | not hassmic's business; the helper below | installs its updates without a dialog |

KS is **not** made the home app (it declares no HOME category; the home activity keeps the overscan and the band). If
the app ends (a crash, its own update, a permission taken away) or was only just installed, `kiosk_tick` sees our home
screen on top within 10 s and opens it, half a minute after the last open at the soonest and a minute apart while it does not stay; it takes 15-30 s to come up (checkers: on screen 34 s after boot_completed).

Where hassmic's own screen runs (`screen.c`, every Show), its UI opens the app instead: KS is the UI's home
app until the settings page's Screen page picks another (`state/screen_home`), launched at start and again when it leaves (three
times a minute at most), but not while the face, the music or the drawer were asked for. While a UI is meant to run
(from hassmic's start, through the UI's restarts, until a UI that never stays up is given up) `screen.c` keeps
hassmic's pid in the name of an empty file, `state/screen-ui.<pid>` (root lists the folder: it opens no file the daemon
could swap; a name of more than digits, statefile.c's temp file, is none); `kiosk_tick` sees a live one and leaves the
opening alone (grants, rotation and the update helper go on), so it never opens the app over the face picked while a UI
starts again. hassmic gone (its pid with it), or the UI given up: `kiosk_tick` opens the app again as above.

## Permissions

`main.sh` `kiosk_grants` applies these as root when `kiosk_tick` first sees an install or update of the app, within 10 s (before the app is opened; idempotent):

| Permission | Why |
|---|---|
| `ACCESS_COARSE_LOCATION`, `ACCESS_FINE_LOCATION`, `READ/WRITE_EXTERNAL_STORAGE` (runtime, `pm grant`) | dashboards that use them, the app's settings and downloads; nobody is there to answer a dialog |
| `SYSTEM_ALERT_WINDOW`, `WRITE_SETTINGS`, `GET_USAGE_STATS`, `REQUEST_INSTALL_PACKAGES` (app ops, `appops set ... allow`) | overlays, screen brightness and timeout (its own adaptive brightness is switched off by root's `main.sh` `kiosk_brightness`: the Echo's brightness settings set the screen, issue #126), the foreground check, installing its own updates |
| `dumpsys deviceidle whitelist +<pkg>`, `dpm set-active-admin <pkg>/.KioskAdminReceiver` | no doze for the dashboard and its ESPHome server; the device admin is what lets it turn the screen off (force-lock: the only policy `dumpsys device_policy` lists on 2026.10.45, checkers; a later version could ask for more) |
| **`RECORD_AUDIO` denied** (`pm revoke` + `appops set ... deny`) unless the setting `kiosk_mic` is on (below) | hassmic owns the microphone and the wake word; the app would hear the wake sound and replies, and its own voice assistant is off |
| **`CAMERA` denied** the same way unless the setting `kiosk_camera` is on | a camera is not something to hand out because a dashboard might want it |

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
(Home Assistant's update entity, or its own page) without a dialog. The provider is exported with no permission in the manifest (2026.10.45), so only the app's own code can tell who may read the token; not checked. Another app on the Echo might get a shell-uid helper that way: install nothing else you do not trust. Its documentation has it piped into `sh` from adb
after every boot; `main.sh` `kiosk_ask_helper` does it instead:

- within 10 s of seeing the app: as the shell user (what adb is on a locked device: `pm install` is allowed to it) with groups
  `shell,log,inet` (`inet`: its `ServerSocket` fails with "Permission denied" without it), never as root; through `runas`
  (no `su` on the image).
- again about every 5 minutes, and at once after an update: the script is idempotent ("already running"), an update of the app ends the process, so the
  next round starts the new one.
- boot.log: `kiosk update helper: started` once, silence while it is up, otherwise its last line.

The script's `status` check is a client connecting to the helper's socket from **su**: it is why the helper looked dead on
the Show 8 2nd gen (below).

What that costs: the helper is the app's own code as the shell user, so the app can do what an adb shell can, `pm grant`
included. The settings above are Android's permissions, no wall against an app that wants past them. docs/SECURITY.md
says the same to users.

## Uninstalling

`scripts/kiosk-install.sh --uninstall [echo-ip]` (`kiosk_remove`, `scripts/lib/kiosk.sh`). A plain `adb uninstall` fails:
`kiosk_grants` makes the app a device admin, and Android refuses to uninstall an active admin
(`DELETE_FAILED_DEVICE_POLICY_MANAGER`). The Shows have no Settings app to turn it off, and `dpm remove-active-admin` is
`forceRemoveActiveAdmin`, which takes testOnly admins only, root or not. `removeActiveAdmin` would let root through
(`MANAGE_DEVICE_ADMINS` is granted to uid 0), but only by `service call` with a transaction number of each firmware's own.

So the script disables the admin's receiver, `pm disable <package>/<receiver>` for every admin of the package that
`dumpsys device_policy` lists. The device policy service checks its admins on every `PACKAGE_CHANGED` and drops one whose
receiver no longer resolves (`handlePackagesChanged`: `getReceiverInfo` without `MATCH_DISABLED_COMPONENTS`).
`set-active-admin` refuses a disabled receiver ("Unknown admin", `findAdmin`), so a `kiosk_grants` meanwhile cannot bring
it back. Then `pm uninstall`, once dumpsys no longer lists the admin; after 15 s without that, the receivers are enabled
again and the script stops. Read in Amazon's `services.jar` of checkers, cronos (the same dex) and crown (2026-11-03);
athena's is compact dex and was not read, AOSP 9's code is the same. Run on a Show 5 (checkers, Android 7.1.2, KS
2026.10.45, 2026-11-03): dumpsys no longer listed the admin at the first look after `pm disable`, `Success` 5 s after the
start; package, data, admin, doze entry, ports and uid rule gone, hassmic's home on top. Not yet run on athena.

Within 10 s `kiosk_tick` sees the folder gone. It ends the update helper (`kiosk_helper_end`: that is the app's code as
the shell user, started by us, so Android's uninstall does not end it; on that Show 5 `ks-update-helper-10002`, uid
2000, was still up after the uninstall, and `kiosk_helper_end` ended it), removes the helper's folder and log in
`/data/local/tmp` (empty, named by the uid), forgets the app, and the firewall closes 2324 and 6053 (`lockdown.sh` reads
packages.list: within about 3 s on athena, 2026-11-01). Simple Keyboard stays (`adb uninstall
rkr.simplekeyboard.inputmethod`).

The uninstall deletes the app's data, its setup with the Home Assistant login included. Installed again it gets another
uid (10002 -> 10154 on that Show 5); `kiosk_tick` grants again, the firewall's uid rule follows from packages.list. To
keep its setup over a reinstall, as done there: `tar` of `/data/data/<package>` (not `lib`, `code_cache`) before,
afterwards the app stopped, the folders put back, `chown -R` to the new uid and `chcon -R` to the folder's own context
(`app_data_file:s0:c512,c768`: toybox tar keeps no labels), with hassmic stopped meanwhile so nothing opens the app.

By hand, as done on athena before the script (2026-11-01): `stop hassmic`, delete the app's `<admin>` block from
`/data/system/device_policies.xml` as root (the service reads that file only at boot), reboot, `stop hassmic` again as
soon as boot completes (a new `kiosk_tick` loop grants the admin on its first look), `adb uninstall`, `start hassmic`.
The app itself offers no way to give up its admin (2026.10.45).

## Microphone and camera

Settings `kiosk_mic`, `kiosk_camera` (settings page, System, shown once the app is installed; default off; not exported).
`main.sh` `kiosk_grants` grants or revokes the permission and its app op from `state/config`, and `kiosk_perm_sync` looks
again every 2 s, so a change reaches the app at once. Revoking ends the app's process; `kiosk_tick` opens it again.
The page learns the app is there from `/data/local/hassmic/kiosk-app`, root's: the daemon's user cannot read packages.list.

Measured on a Show 8 2nd gen (athena, Android 9, 2026-10-26) with a doorbell plugin talking while hassmic listened:
hassmic's `VOICE_RECOGNITION` capture never stopped or reopened, "Alexa" answered during and after every call, and
the app's own recorders opened for all three sources it offered, but only `VOICE_COMMUNICATION` reached the doorbell
(no echo, which is the HAL's call pipeline with its echo canceller); `VOICE_RECOGNITION` and `CAMCORDER` (what
"Automatic" took) delivered nothing. Why they are silent is inferred from docs/re-checkers.md (one consumer of the
ASR pipeline, no pipeline for the others), not measured.

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

## Rotation

The Echo has no accelerometer. checkers' panel is 480x960, portrait by nature, measured 2026-11-01; cronos and crown get the same settings, `ANDROID_SETTINGS` (`accelerometer_rotation=0`, `user_rotation=1`) and `KIOSK_ROTATION=1`, cronos as the same design and crown from its 2026-10-14 notes, neither measured with this code. athena has neither: `wm size` on one has not been looked at, and its fresh setup reported no problem, which says it is landscape by nature without showing it. The app itself writes `accelerometer_rotation=1`, `user_rotation=0` about 5 s after it opens (checkers 2026-11-01); with no sensor Android keeps the rotation it has until something makes it look again, which a reboot or a killed app did not, and a reinstall did: portrait. So where device.conf has `KIOSK_ROTATION`, `kiosk_tick` looks at the rotation itself every 10 s and puts both settings back: after a restart of the app the screen can show portrait for up to that long (checkers: once in three forced stops).

## Screen timeout

hassmic sets `screen_off_timeout` (5 min, 1 min muted or in Do Not Disturb, `ring_band.c`) and the app may set it too (`WRITE_SETTINGS`), so whichever wrote last holds. Not measured together.

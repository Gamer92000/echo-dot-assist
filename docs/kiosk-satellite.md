# Kiosk Satellite on an Echo Show with hassmic

[Kiosk Satellite](https://kiosksatellite.com/) (`me.jxl.kiosk_satellite`) can show a Home Assistant dashboard on the
screen models that name it in `device.conf` (`KIOSK_PACKAGE`: `crown`, `athena`). hassmic stays the voice satellite; the
two share the Echo and nothing else. Install the app yourself (`adb install`, 32-bit ARM, minSdk 24) and switch off its
own voice assistant. Do not make it the home app: hassmic's home screen keeps the overscan and the light band.

What hassmic does for it (`scripts/system/main.sh` `kiosk_start`, `scripts/device/lockdown.sh` `keep`):

- Once per boot, when the app is installed: runtime permissions and app ops it would otherwise ask a person for
  (camera, location, storage, overlay, settings, usage stats), no doze, device admin (it turns the screen off through it).
  **`RECORD_AUDIO` is revoked and denied**: the mixer hands the microphone to one client, and a second recorder would
  also hear the wake sound and replies.
- Then it opens the app over the home screen (it has no HOME category, so nothing else starts it).
- Firewall: inbound TCP 2324 (the app's web page and API) and 6053 (its ESPHome API, which Home Assistant's ESPHome
  integration connects to; hassmic's own is 26053) from private addresses only. Nothing else changes: the app, like
  every program but hassmic, reaches local addresses only, so it shows Home Assistant but cannot fetch outside pages or
  update itself.

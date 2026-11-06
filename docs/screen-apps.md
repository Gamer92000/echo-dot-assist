# Home apps: what the Echo Show's screen shows

hassmic's own screen (`src/ui/`, `screen.c`: the face, the drawer, Music Assistant) stays small: the screen is the
**home app's**, an Android app that the Echo starts at boot and brings back when it ends. Any installed app can be
it (Kiosk Satellite, Echo-Wall, Home Assistant's own app, a browser); the settings page's Screen page or Echo Local
in Home Assistant picks it (`state/screen_home`; the last choice made counts). An app made for hassmic speaks the
contract below and gets more: Home Assistant's address and a login, a way to be asked for one of its screens, and
the screen left to it for what it draws itself. [Nt-f/Echo-Wall](https://github.com/Nt-f/Echo-Wall) speaks it; a
template that does nothing else is [Nt-f/Echo-Screen](https://github.com/Nt-f/Echo-Screen).

This is the whole contract between hassmic and a home app.

## Being started

The app's main activity declares

```xml
<intent-filter>
    <action android:name="io.github.gamer92000.echodotassist.action.SCREEN" />
    <category android:name="android.intent.category.DEFAULT" />
</intent-filter>
```

and the UI starts it by that action (`src/ui/android.c` `launch_contract`) whenever it starts the home app, with an
extra that says what to show: `home` (true: the drawer's Home) or `screen` (one of its screens by name, picked in the
drawer's row); no extra when it only comes back after it ended. An app without the action is started as Android's
launcher starts it. `singleTask` keeps one copy.

## The config, and saying it is in front

```
GET http://127.0.0.1:28931/kiosk/config/front                  while the activity is resumed: every 5 s
GET http://127.0.0.1:28931/kiosk/config/front/draws/<list>     the same, claiming what it draws itself (below)
GET http://127.0.0.1:28931/kiosk/config/back                   once when it is paused, then every 30 s while paused
GET http://127.0.0.1:28931/kiosk/config                        the config alone
```

hassmic answers only connections from 127.0.0.1 whose owner (the uid in `/proc/net/tcp6`) is the home app's, the
owner of `/data/data/<package>`; anyone else gets a 404. No CORS headers: pages the app shows cannot read it.

```json
{
  "enabled": true,
  "ha": "http://192.168.1.211:8123",
  "has_token": true,
  "token": "<a long-lived access token>",
  "settings": {"immich.url": "http://192.168.1.9:2283"},
  "app": "io.github.nt_f.echowall",
  "default": 0,
  "return": 120,
  "screens": [
    {"name": "Home", "url": "hassmic:home"},
    {"name": "Home Assistant", "url": "http://192.168.1.211:8123/lovelace"}
  ]
}
```

- `ha`, `token`: Home Assistant's address as the Echo reaches it (local addresses only: the Echo's firewall) and a
  long-lived token of a Home Assistant user made for this Echo by Echo Local (local only, not an administrator unless
  the owner chose so). For Home Assistant's own frontend, put it into the origin's `localStorage` as `hassTokens`
  (`{"access_token", "token_type": "Bearer", "expires_in", "hassUrl", "clientId", "expires": far future,
  "refresh_token": ""}`) before its first page; for the websocket API, `{"type": "auth", "access_token"}`.
- `screens`, `default`, `return`: what to offer, where to start, and after how many seconds untouched on another
  screen to go back to the default one (0: never). The drawer's row offers their names (not the `hassmic:` ones, which
  are the app's own: `hassmic:home` is the drawer's Home); a tap starts the app with the `screen` extra.
- `settings`: the app's own, as the owner typed them into Echo Local ("Screen app settings", one `key=value` a line:
  keys of a-z 0-9 . _ -, 16 at most, values up to 400 characters). They travel sealed with the token and are only in
  this answer, never in the signed API's.

Echo Local sends it (`POST /api/kiosk`, signed, the token sealed) when it is set up and whenever its options change;
its `app` line makes that app the home app.

## What an app claims: the words, the ring

hassmic's own layer shows only over hassmic's home: over an app, nothing of hassmic's is drawn, except that a ringing
timer or alarm brings the face with its **Stop** over the app until nothing rings (then the app is started again).
An app that draws a ring itself says so in the path it polls: `/kiosk/config/front/draws/<list>`, the list comma- or
plus-separated of `words` (what was heard and the reply) and `ring` (a ringing alarm or timer); `/front/draws` alone
means both. Words not known are ignored, so an app may name more. A claim holds while the app polls `/front` (12 s),
and only for that app. What to draw comes from

```
GET http://127.0.0.1:28931/kiosk/state           now
GET http://127.0.0.1:28931/kiosk/state/<v>       a long poll: answers once the version is not <v> any more, or after 25 s
```

(the same rule: 127.0.0.1 and the home app's uid only)

```json
{
  "v": 42,
  "running": true,
  "words": true,
  "heard": "turn on the kitchen lights",
  "reply": "Turned on the kitchen lights.",
  "ringing": "Alarm 7:30 AM",
  "ringing_alarm": true,
  "h24": false,
  "clock_set": true,
  "next_alarm": {"at": 1793689800, "text": "Alarm 7:30 AM on weekdays", "label": "Gym"},
  "display": "always"
}
```

- `running`: a request is under way (listening, thinking, answering); `words`: show `heard` (what speech to text
  understood) and `reply` now (for 20 s after the request ended).
- `ringing`: what rings, empty when nothing does; `ringing_alarm`: an alarm (snoozes) rather than a timer. Draw what
  stops it, and hand the finger on: `GET /kiosk/ring/snooze` on a tap (an alarm snoozes, a timer stops) and
  `GET /kiosk/ring/stop` on a hold (the same rule: 127.0.0.1 and the home app's uid).
- `h24`, `next_alarm`, `clock_set` (the Echo's clock came from Home Assistant): for a clock of the app's own.

## The drawer and the touch screen

The drawer (`src/ui/drawer.c`) is pulled down from the middle of the top edge over any app: Home, Face, Music, Do not
disturb, Screen off, the volume, and the app switcher (the home app's screens, then every app the owner installed).
No rows are taken from the apps for it. The UI process stands between the touch screen and Android
(`src/ui/touchgate.c`, run by `touch.c` in a thread of its own): it grabs the panel's evdev device and writes every
touch on to a uinput copy of it, which Android takes for the touch screen. A touch that starts in the middle 80 % of
the screen's top edge (within 10 % of it: a fingertip below the edge) while an app is in front is held back until it shows what it is. If it comes
down 5 % of the screen, mostly straight, it is a pull: the drawer's alone, and since the app never saw its start
nothing has to be cancelled. If it lifts (a tap), goes 3 % sideways, gets a second finger, or rests 300 ms (a long
press), it goes on to the app from where it began; nothing is held past 600 ms. Every other touch goes on at once,
from that thread, whatever the UI's frames are doing. The copy's node in `/dev/input` is checked before the panel is
grabbed; without a copy the UI reads beside Android and keeps the grip (16 rows of overscan along the top) for the
pull. One cost: Amazon's `touchlogger` (touch telemetry, which also resets the panel when it sees a storm of ghost
touches) reads the grabbed panel, so it sees nothing.

## A Home of the app's own

The layout an app draws on its Home is the app's own business: hassmic and Echo Local only say which app is the home
app and give it Home Assistant's address and a login. Echo-Wall keeps its Home in Home Assistant through an
integration of its own (Nt-f/Echo-Wall, `custom_components/echo_wall`, its `docs/integration.md`).

## What the Echo lacks

- **No keyboard of Fire OS's.** The guided setup installs Simple Keyboard (open source, no network access); a home
  app may bring its own (Echo-Wall does), which root's `main.sh` makes the one in use where the Echo has none that
  works.
- **No navigation bar, no system UI.** The drawer is the way between apps; over an app, a swipe along the light band
  is Back.
- **Memory.** The Echo Show 5 has 1 GB for Android, hassmic and the app; a Flutter UI takes about 62 MB, a WebView
  with Home Assistant's frontend about 90 MB (measured, 2026-11-02). One heavy page at a time.
- **GPU.** Mali-T720, OpenGL ES 3.1, no Vulkan: Flutter runs on Skia (GLES) at 60 fps; a full-screen live blur
  costs 25-43 fps.
- **Android 7.1 (API 25) on the Show 5, 32-bit ARM only** (`armeabi-v7a`).

## Installing one

From the settings page's Screen page (Add an app) or Echo Local's Apps tab, or `adb install my-app.apk`; then pick it
as the home app on the Screen page or in Echo Local's Screen tab.

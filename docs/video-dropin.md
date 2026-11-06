# Video Drop In from Home Assistant

Status (2026-11-04): steps 1-4 done, and video between two Echo Shows. From a dashboard: the Echo's side with sound
and picture (`tests/fake_web_call.py`), Echo Local's relay (`call.py`) and the card, tried in a real Home Assistant
and headless Chrome (`tests/ha_real_camera.py`), and on an Echo Show 5 with a person on each end; the browser's camera
on the Echo's screen: Chrome's encoder's own output played by the Show 5's decoder (28 of 37 records shown, fed
faster than real time), the rest PC-tested.  Between Echo Shows: PC-tested only (`tests/fake_ha_dropin_video.py`),
nobody has two Shows here yet. A call from a Home Assistant dashboard to an Echo Show: the Echo's camera and microphone to
the browser, the browser's microphone to the Echo's speaker, answered and ended like a Drop In between Echos. It
replaces watching the camera entity for live use (HLS: seconds behind, and no clear start or end).

## How a call goes

- **Start**: a dashboard card (Echo Local's `echo-local-call-card`) or the action `echo_local.drop_in` (device). The
  Echo rings or connects at once by its own Drop In settings (`drop_in`, `drop_in_answer`); Do not disturb and the
  microphone button refuse it, as they refuse a call from another Echo.
- **On the Echo**: Drop In's call face and sounds; the red camera dot while the camera sends; the action button or
  "stop" / "hang up" ends it; the lens cover cuts the picture (the call goes on with sound).
- **In the browser**: the Echo's picture within ~0.3 s, its sound, a microphone button, Hang up.

## The path

```
 browser (card)  <-- HA websocket view, binary both ways -->  Echo Local  <-- sealed channel -->  Echo
   WebCodecs VideoDecoder (H.264 Annex B)                         relay          web.c GET /api/call (signed)
   AudioWorklet: mic 16 kHz PCM up, Echo's PCM down                              dropin.c: an "ha" far end
```

- **Echo <-> Echo Local**: one signed request on the settings page's port (`/api/call`), then both directions as
  records sealed with a key of the call's (ChaCha20-Poly1305, as the camera stream): video (camera.c's records),
  audio 16 kHz PCM 20 ms frames each way, control (state, hang up). No new port.
- **Echo side**: dropin.c gets a second kind of far end beside the UDP one of another Echo: frames go to and come from
  the channel instead of the socket, PCM instead of Opus (the LAN has the room; the browser then needs no Opus).
  The rest is Drop In's own: the mic gate and echo estimate, the Voip stream, call states, LEDs and face, sounds,
  hang-up words, refusals, results.  The picture: camera.c's stream child (one viewer: the call takes it).
- **Echo Local**: a `HomeAssistantView` with a websocket at `/api/echo_local/call/<entry>`, opened with a signed path
  (`auth/sign_path`), so only a logged-in user of Home Assistant reaches it; it opens `/api/call` on the Echo and
  passes frames both ways.
- **Card**: H.264 through WebCodecs (Chrome, Edge, Safari 16.4+, the Companion app's WebView), PCM through an
  AudioWorklet; getUserMedia needs HTTPS or localhost (Home Assistant's frontend usually is: Nabu Casa, a proxy,
  or the app).

## The browser's camera on the Echo's screen (step 4)

The card's **Camera** button (off at the start of every call, as the microphone; shown only where the camera entity
says `shows_video`): getUserMedia, then WebCodecs' VideoEncoder, H.264 Baseline in Annex B (its key frames carry
SPS/PPS inline), 640x360 at 15 fps, ~500 kbit/s, a key frame a second; frames skipped while the encoder or the
websocket is behind.  Up as 'V' records through `call.py` and `api.py` `EchoCall.video()` to web.c (`CALL_UP_MAX`
512 KB a record), into `video.c`.  The card shows this side's own picture small, mirrored, while it sends.

**On the Echo** (`video.c`, `src/cam/cam.c` `HMCAM_MODE=play`): a child of hassmic while a call has a picture, as the
camera's: MediaCodec's H.264 decoder (the MT8163's) straight into a SurfaceFlinger layer of the screen's size, cropped
to fill it, between the UI's layer and the band's (the call's animation stays on top, as does the other Show's red
dot over its own).  The config either as a record of its own or inline: both measured on checkers (first picture
60-75 ms after the start).  The picture must never outlive the call: every end of a call stops the player
(dropin.c `end_call` -> `video_stop`), a new call's records end the old one's, and cam.c hides the layer and exits
after 3 s without a frame; the layer is the process's, so a kill takes it (checked with `kill -9`).

## Between two Echo Shows

Both send their camera (while each one's setting "camera" is on and its lens open, the red dot on) and show the
other's.  Over the call's own UDP port and key, no new port:

```
"HMV1" call_id[8] dir vseq[4]  enc(frame u32, frag u8, nfrag u8, type u8, pts u64, data <= 1150)  mac[16]
nonce = call_id dir vseq 'V'    (audio's nonces have byte 13 zero: the two never meet)
```

- **Who gets a picture**: invite and accept carry a capability byte after the reason (`CAP_SHOWS`, `CAP_SENDS`); an
  older build sends none and ignores it, so it is never sent a picture, and a Dot (no player) says it shows none.
- **Sender** (`video_sender`, a thread of dropin.c): the camera's stream at `CALL_KBPS` (1000, the integration's
  1500) while the call is live, the other side shows pictures; tried again every 2 s (the setting switched on, the
  lens opened, a stream that had the camera).  The codec config goes inline ahead of every key frame.
- **Receiver** (`on_video`): one frame put together at a time; a fragment of a newer frame drops the one unfinished,
  and after any loss only a key frame starts the picture again (no broken pictures on the screen).  The call's end
  logs `pictures: sent, shown, lost on the way`.
- **Frosted start** (stock's "frosted glass"): the caller's screen blurs an Echo that did not ring, `FROST_MS` 4000
  from its first picture (3 s frosted, 1 s clearing): dropin.c `go_live` -> `video_blur` -> cam.c `HMCAM_BLUR_MS`.
  The card does the same by its states (no "ringing" before "connected").  Done by the receiver, both ours: Home
  Assistant can watch the camera anyway while the setting is on, so this is courtesy, not a lock.  Without GL the
  player keeps such a picture hidden for its frost rather than show it sharp.
- **Self-view**: the records that leave go to a second player too (`video_self`, `HMCAM_SELF=1`: a quarter of the
  width, bottom right, mirrored), from Show to Show and to Home Assistant alike.
- **Unmeasured**: two Shows sending 720p at 1 Mbit/s each on one Wi-Fi; a key frame's 30-60 fragments must all
  arrive.  `CALL_KBPS` and the key frame interval are what to tune once two Shows can be tried.

## Steps

1. Echo: `/api/call` and dropin.c's "ha" far end, audio only; a PC test with a stand-in Echo Local.
2. Echo Local: the view and the relay; `echo_local.drop_in`; a test in a real HA.
3. The card: audio, then video through WebCodecs; hang up both ways.
4. The browser's camera on the Echo's screen: done (above).
5. Video between two Echo Shows: done, PC-tested (above).

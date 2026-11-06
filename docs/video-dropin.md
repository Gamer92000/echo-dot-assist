# Video Drop In from Home Assistant

Status: steps 1-3 done (2026-11-04): the Echo's side with sound and picture (`tests/fake_web_call.py`), Echo Local's
relay (`call.py`) and the card, tried in a real Home Assistant and headless Chrome (`tests/ha_real_camera.py`); not
yet on a device with a person on each end. A call from a Home Assistant dashboard to an Echo Show: the Echo's camera and microphone to
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

## Steps

1. Echo: `/api/call` and dropin.c's "ha" far end, audio only; a PC test with a stand-in Echo Local.
2. Echo Local: the view and the relay; `echo_local.drop_in`; a test in a real HA.
3. The card: audio, then video through WebCodecs; hang up both ways.
4. The browser's camera on the Echo's screen (later, if wanted).

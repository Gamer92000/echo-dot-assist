# The Echo Show's camera in Home Assistant

Status: works on checkers (Echo Show 5, 2026-11-04). The other Shows have `board.camera` 0 until someone tries them;
the Dots and the Echo Input have none. User side: [GUIDE.md](GUIDE.md#camera-echo-show); security: [SECURITY.md](SECURITY.md#camera).

## The route: Echo Local's camera entity, H.264 from the Echo's hardware encoder

```
  Echo Show                                                Home Assistant (Echo Local)
  camera HAL --preview--> MediaCodec H.264 (hardware)        api.py camera_stream: checks, decrypts
     src/cam/cam.c (libhmcam.so, app_process,                  |
     only while streaming; the red dot)                       camera.py relay: MPEG-TS (tsmux.py), 127.0.0.1
        | stdout: records                                      |
  camera.c (one viewer) -> web.c GET /api/camera/stream  ----> stream_source -> go2rtc (WebRTC), stream (HLS,
        settings page port 28931, sealed with the                recordings); stills through HA's ffmpeg
        integration's key
```

- **Video**: the Echo encodes H.264 in the MT8163's hardware encoder: the camera previews straight into MediaCodec's
  input surface, no copy through any process of ours. The records go to the integration, which hands them to Home
  Assistant as the entity's `stream_source`: HA's `stream` makes low-latency HLS and recordings of it (tested:
  `tests/ha_real_camera.py`, fMP4 parts of 0.93 s), its ffmpeg the stills; HA's go2rtc (built in since 2024.11)
  should make WebRTC of it without transcoding, not seen yet.
- **No new port**: the stream is a long signed `GET /api/camera/stream` on the settings page's port (web.c runs a thread
  per connection), so the firewall needs nothing new.
- **Encryption**: the request is signed with the key K Echo Local shares with the Echo (its approved X25519 key,
  `state/web_clients`) and a fresh counter; the answer's head is signed over a server nonce; every record is sealed with
  ChaCha20-Poly1305 (IETF, noise.c's nonce layout) under SK = BLAKE2b-256 keyed with K over
  `"hassmic camera 1\n" counter "\n" nonce`. Only the relay inside Home Assistant sees plain video, on 127.0.0.1.
- **MPEG-TS, not raw H.264**: raw Annex B carries no timing; ffmpeg then assumes 25 fps, and the camera's 10-15 would
  have played at nearly twice the speed in recordings. `tsmux.py` keeps the Echo's own pts, puts PAT/PMT and the codec
  config (MediaTek's encoder sends SPS/PPS once, in a buffer of its own) before every key frame, one a second, where a
  viewer may join.
- **Only while someone looks**: the camera process starts for a stream and ends with it; the relay keeps one stream
  from the Echo for all of Home Assistant's viewers and lets it go 10 s after the last one.
- **Off by default**: setting `camera` (Features; not exported). The lens cover (`SW_CAMERA_LENS_COVER` on gpio-keys,
  which hassmic grabs anyway) stops it at once. The mic-mute button does **not** cover the camera.
- **The dot means "pictures are leaving"**: a red dot in the screen's top right corner, a SurfaceFlinger layer of the
  camera process over every app, there before the first frame leaves and gone with the process (hassmic always
  starts it with `HMCAM_DOT=1`).

## Why not ESPHome's camera entity

The ESPHome native API has a camera entity (`ListEntitiesCameraResponse`, `CameraImageRequest` with `single` or
`stream`, `CameraImageResponse` in pieces; aioesphomeapi `request_single_image`, `request_image_stream`), and HA turns
it into a camera with live view. It would have needed no integration code and no new endpoint, and it rides the Noise
link that already exists. We chose against it for how it streams:

- **It is MJPEG**: every frame a whole JPEG. At 640x480 that is ~30-60 kB a frame, 2.5-5 Mbit/s at 10 fps, where the
  H.264 here holds 1280x720 at ~1 Mbit/s. The Echo shares one radio with Sendspin, the Bluetooth proxy and the other
  Echos' arbitration claims (which already lose 10-30 % of broadcasts to BLE scanning).
- **Every frame goes through Home Assistant's Python**: the ESPHome integration takes each JPEG and serves it as
  multipart MJPEG per viewer. go2rtc cannot take it without re-encoding, so there is no WebRTC, and a recording is a
  transcode on the HA host.
- **It costs the Echo software JPEG** on the A53s that also run the wake word, the front end's readers and the screen UI;
  MediaCodec's H.264 is the hardware's (measured: hassmic 26.5 -> 28 % of a core while streaming, the camera process 1 %).
- **One link with the voice assistant**: a stream of JPEGs on the ESPHome connection delays the voice audio and events
  behind them.

## Why not plain RTSP from the Echo

The usual way to get H.264 into HA is an RTSP URL. It would spare the relay, but RTSP has no encryption that HA's
stream and go2rtc both take from a device we write ourselves (RTSPS needs a TLS server; the firmware's BoringSSL is
only reachable through libcurl, a client). The video and the URL's token would be readable by anything on the LAN,
against the rule of this project that nothing on the network can listen in or take control. The relay costs Home
Assistant one ChaCha20-Poly1305 decryption at ~1 Mbit/s: nothing.

## What the device has (checkers, Fire OS on Android 7.1.2, SDK 25)

- `dumpsys media.camera`: one camera, front facing, orientation 270, "MediaTek Camera Module", HAL module API 0x204,
  device version 0x100 (HAL1: Camera1 semantics; camera2 on it is the legacy shim).
- Parameters: `preview-size-values` up to 1280x720; `preview-fps-range-values` (5000,30000), (6000,6000), (10000,10000),
  (15000,15000), (15000,24000), (15000,30000), (20000,20000), (30000,30000); `max-num-detected-faces-hw=15`, `-sw=0`;
  `video-size-values` up to 1280x720.
- Vendor tags in `com.mediatek.facefeature`: gesture, smile detection, ASD (scene detection); no blink.
- `/system/vendor/lib/libcamalgo.so` has MediaTek's face detector (`FD_CORE`, `GlobalFaceDetectionEngine`);
  `libcam.client.so` wires it to Camera1's `startFaceDetection`. `libcam.device1.so` writes Amazon's
  `amazon-vq-metadata` (lux, `num-of-faces`, `distance-in-cm-1`, ...) into the parameters.
- `getevent -lp`: gpio-keys has `SW_CAMERA_LENS_COVER`.

## Opening the camera from app_process

`Camera.open()` takes the client's package from `ActivityThread.currentOpPackageName()`, which is null in app_process,
and `native_setup`'s `GetStringChars` aborts on null. cam.c does the constructor's steps itself through JNI: the
package-private `Camera()` (its field initializers make the locks), an `EventHandler` on a Looper of the calling thread
(the HAL's callbacks are dropped without one), `native_setup(new WeakReference(cam), 0,
CAMERA_HAL_API_VERSION_NORMAL_CONNECT, "android")`. "android" is uid system's own package, which is the uid the
process runs as: AppOps' `startOperation` refuses a package of another uid. Then `setPreviewSize`,
`setPreviewFpsRange`, `setRecordingHint(true)` (MediaTek's video pipeline), the hidden `setPreviewSurface` with
MediaCodec's `createInputSurface()`, `startPreview`. Measured: native_setup 0.1-0.4 s, startPreview 0.2-0.4 s, first
H.264 0.5 s after it, 1.9-2.0 s from the request to the first record with the VM's start.

## Later

- Two-way video for Drop In.
- The other Shows (cronos, crown, athena): Android 7 / 9, other HALs; `board.camera` once tried.

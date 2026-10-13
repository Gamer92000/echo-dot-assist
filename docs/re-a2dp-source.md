# Playing to a Bluetooth speaker: the mixer's A2DP route

Static analysis of NS65741 (donut; biscuit's `libaudioCtrl.so`, `audio.a2dp.default.so` and `libace_aipc.so` are
byte-identical), checked on an Echo Dot 2 with `src/tools/a2dpprobe.c` (2026-10-01). Used by `src/hassmic/btout.c`
(the mixer's side) and `a2dp.c` (the radio side). **UNVERIFIED** marks what was not seen on a device.

`bin/mixer` is **ARM** code, not Thumb (even function addresses in its dynsym): `firmware/donut/re/mixer.asm`, made as
Thumb, is garbage for the mixer's own functions. Addresses below are ARM for the mixer, Thumb for the libraries.

## How stock does it

btmanagerd (Fluoride `bluetooth.default.so` inside, through `libacehal_bt.so`) connects the speaker and tells the mixer
with one LIPC string property. Nothing else passes between them: no system property, no mixer API call.

- `libacehal_bt.so` `acebthal_a2dp_src_co_set_conn_state` (0x105ec): `LipcOpen("com.lab126.ace.hal")`, then
  `LipcSetStringProperty(h, "com.doppler.audiod", "A2DPSourceConnect", v)`, only for "connected" and "disconnected".
  `v` from `utils_convert_to_a2dp_evt_string` (0x1058c): `"%c:%02x%02x%02x%02x%02x%02x"`, `1` connected / `0` not, the
  address in display order, lowercase. `set_audio_state` is an empty function: stream start and stop never reach the
  mixer this way, only through the HAL's sockets.
- The mixer serves `com.doppler.audiod` with libaudioCtrl: `lipcSetStrProp` (0x1dc2c) calls the mixer's
  `mixSetA2DPRoute` (0x9a8d4); its parser (0x9cadc) takes the number before `:` (non-zero = connect) and copies 6 bytes
  of address **without a bound check**: never send more than 12 hex digits.
- Connect: playback mode BT_A2DP, `ChangeOutput` (0x73d6c): `connect=128` to the primary HAL, the speaker's output
  stream closed, `open_output_stream(devices 0x80, 44100 Hz, stereo, 16 bit)` on the A2DP HAL, `routing=128`, LIPC
  event `OutputDeviceChanged` `"BT##<name>"`. One output only: **the Echo's speaker is silent** while routed.
  Disconnect: back to the primary HAL at 48 kHz (or line out when `/sys/class/switch/h2w/state` says plugged).
- Every set calls `getBtDeviceName` (0x9ceb4): `aceBT_openSession`, `aceBT_getName`, `aceBT_closeSession` over AIPC to
  btmanagerd. The name only feeds the log, the `BT##` event and the keep-alive table. Failure is logged and ignored,
  but with btmanagerd stopped the connect polls until its timeout: **each `lipc-set-prop` timed out after 10 s and the
  switch came ~20 s after the set** (measured). See AIPC below.
- BT keep-alive (0x733e4): a table compiled into the mixer maps speaker names (UE Boom, Tailgater, ~30 others) to a
  strategy; any other name plays continuously, silence included (`AudioOutIsSilence` to the HAL instead of standby).
- Line out plugged while routed: the mixer sets the LIPC int `BTUnpair` itself, which calls
  `aceBT_a2dpSource_disconnectProfile` (AIPC) on the stored address.
- `audio_manager_set_prop OutputDevice …` does nothing on this build (`setPreferredPlaybackDevice` returns -38).
  `PLAYBACK_MODE_*` in libmixerAPI are unused name tables; no client API selects the output.

## The A2DP HAL (`audio.a2dp.default.so`)

AOSP N `system/bt/audio_a2dp_hw` plus Amazon additions.

- Two **abstract** UNIX stream sockets, `\0/data/misc/bluedroid/.a2dp_ctrl` and `.a2dp_data` (address length 2 + 1 +
  strlen, no trailing NUL). No file, no owner: only SELinux decides, and `allow domain su unix_stream_socket connectto`
  in the boot-root policy lets the mixer reach hassmic (domain su).
- Control: one command byte, one ack byte (0 done, 2 in call, else failure). 1 CHECK_READY (on opening the output, 3
  tries 250 ms apart: no ack 0, no output, and the mixer is left with none), 2 START (then the data socket connects),
  3 STOP, 4 SUSPEND, 5 GET_AUDIO_CONFIG (input side only), 7 / 8 ACL priority up / down (Amazon: sent for
  `AudioOutIsSilence=0/1`, i.e. sound starts / stops).
- Data: 44.1 kHz stereo s16, written with MSG_DONTWAIT; the HAL gives up after 2 s without room. **The reader sets the
  pace**: unpaced it took 234 kB/s, paced at 176.4 kB/s it plays exactly that (measured). Order seen: CHECK_READY, 8,
  START, data, then 7 / 8 with the sound, SUSPEND on disconnect.
- Measured with a 48 kHz stereo tone through `mixplay`: 1 kHz left and 3 kHz right arrive as 44.1 kHz, the whole 15 s,
  at the Echo's volume (the mixer applies it: 8000 in, 1268 out at one volume; ~full scale at 100, -38 dB at 30).

## AIPC: answering as btmanagerd

`libace_aipc.so` has a server API; hassmic loads it with `dlopen` and registers service uuid 0 (`src/include/aipc_api.h`).

- `aceAipc_start(&handle, cfg)` (0x3dd8): config of 0x4a0 bytes as btmanagerd fills it (uuid 0, handler, max payload
  0x2800, +0x10 = 10, +0x14 = 1). **`thread_option` (+0x20) 0 joins the server thread**, i.e. never returns
  (btmanagerd calls it on a thread of its own); 1 returns. Refuses uid 0. Requests run on a worker thread; the handler
  writes the answer into `task->data` and the library sends it (synchronous calls).
- The socket is `/dev/aipc/0/ss` (SOCK_SEQPACKET). Start removes an existing `/dev/aipc/0` first and fails if it
  cannot: btmanagerd's is `bluetooth:aipc 0710`, so root removes it when stopping btmanagerd (`alexa-off.sh`).
- SELinux: created by domain su, dir and socket would be `aipcd_tmpfs`, which the mixer may not write to; btmanagerd's
  type transition makes them `btmanagerd_aipc_tmpfs`, which it may. hassmic writes that context to
  `/proc/self/task/<tid>/attr/fscreate` around the start (su is permissive): checked, the mixer connects.
- What the mixer asks (packed, little-endian): fid 0 session open (len 0x1b; answer u32 session at 1, s32 status at 7),
  0x13 get name (len 0x103; bdaddr at 4; answer status at 0, name[249] at 0x0a), 1 session close (len 9), and on
  BTUnpair 0xca A2DP source disconnect (len 12, bdaddr at 0, status at 8).
- With the service up: `lipc-set-prop` returns in 0.08 s, the switch follows at once (measured).
- `ace_blemesh_service` connects too and asks fid 0x0a every 2 s; it gets an error status.

## Notes

- Fluoride acks START only once the speaker has started (AVDTP START accepted) and CHECK_READY only with a stream
  configured; hassmic does the same.
- Stock sets `persist.bluetooth.disableabsvol=true`: the mixer's volume, never the speaker's. hassmic uses the speaker's
  absolute volume when it has one and plays the mixer at full scale then (README).
- Echo cancellation while the sound comes out of a speaker elsewhere in the room: asp.cfg has a 44.1 kHz "A2DP"
  pipeline and AFE.cfg "…BT" parameter sets, so the front end seems to know the case. **UNVERIFIED**, not measured.

## Android (checkers)

The Echo Show 5 has the same HAL protocol (its own build of `audio.a2dp.default.so`) behind AudioFlinger instead of the
mixer; the route there is AudioPolicy's device connection state (`media.audio_policy` transactions 1 and 2, see
`docs/re-checkers.md` "Bluetooth speaker"). btout.c's sockets and encoder are shared; the backends (`audio.h`
`audio_a2dp_*`) hold the route. No AIPC there.

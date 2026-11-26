# Call audio over Bluetooth (HFP's SCO/eSCO) on the Echos

Issue #233 (the Echo as a phone's hands-free speakerphone).  Measured 2026-11-25 on biscuit, donut, cannoli and
checkers with `src/tools/scoprobe.c` (synchronous links on raw HCI) and `src/tools/ebcprobe.c` (biscuit's BTCVSD),
the stock firmware read where the measurements left a question.

## Link setup: plain HCI, as stock

Every controller has SCO and eSCO, CVSD, transparent data, and lists the codecs CVSD, transparent and mSBC
(`scoprobe info`).  An eSCO link between two Echos (S3: EV3 and 2-EV3, latency 10, retransmission 1; or SCO HV3)
comes up with `Setup_Synchronous_Connection`/`Accept_Synchronous_Connection_Request`, voice setting 0x0060.  Stock
does nothing more: Fluoride (`bluetooth.default.so`) sends no vendor command around SCO (its only `0xfc61` is
`BTM_ReadRSSI`), and MediaTek's vendor libraries leave `BT_VND_OP_SCO_CFG` empty (cannoli's `libbt-vendor.so` logs
and returns; biscuit's `GORM_SCO_Init_Thread` runs `bt_init_postload_script_consys`, an empty list).

## The audio never goes over HCI

SCO data written to `/dev/stpbt` (H4 type 3) is passed to the chip by `btmtksdio` (its log shows `type(3)`) and never
taken: no Number Of Completed Packets, nothing received, in every variant tried (SCO/eSCO, CVSD/transparent,
Enhanced Setup with data path 0 = HCI, `0xfc72` variants).  The audio goes where the chip's integration sends it:

| Model | Bluetooth | Echo -> phone | Phone -> Echo |
|---|---|---|---|
| cannoli (Dot 5) | MT7663, PCM pins to the SoC's PCM1 | DL6 (`pcmC0D3p`) -> `O00 I00`, `O01 I01` -> PCM1 | PCM1 -> `O02 I02`, `O03 I03` -> UL5 (`pcmC0D7c`) |
| donut (Dot 3) | MT7668, on the SoC's 2nd I2S | DL2 (`pcmC0D8p`) -> `O00 I07`, `O01 I08` -> 2ND I2S | 2ND I2S -> `O13 I00`, `O14 I01` -> HW GAIN1 -> `O11 I10` -> DAI (`pcmC0D9c`) |
| checkers (Show 5) | MT7668 | `VOIP_Call_BT_Playback` (`pcmC0D10p`) | `VOIP_Call_BT_Capture` (`pcmC0D11c`) |
| biscuit (Dot 2) | MediaTek CONSYS (SoC-integrated, WMT, chip id 0x8163) | the air bitstream written to `/dev/ebc` | the air bitstream read from `/dev/ebc` |

Sources: cannoli's routes from its kernel's DAPM table (`mt8519-afe-pcm.c`; `mendocino.c` links `PCM1 BE` with the
widgets `HFP Out`/`HFP In`); donut's from its kernel's table and stock's `mixer_paths.xml` (`normal-playback bt-sco`,
`normal-capture bt-sco`); checkers' from its MediaTek HAL (`audio.primary.mt8163.so`, `AudioALSA*BTSCO`); biscuit's
from its HAL (`audio.primary.mt8163_headless.so`: `AudioBTCVSDControl` on `/dev/ebc`, `libcvsd_mtk.so`,
`libmsbc_mtk.so`).  The DAI/MRG route on donut (`O11 I02`, `DAIBT Mux`) has no back end ("no BE found for MRG BT
Capture"), its BTCVSD platform points at a connsys the SDIO chip does not have.

### The chip's PCM side (MT7668)

donut's and checkers' `btmtksdio.ko` (v0.0.1.00_2022121401) configure nothing for audio: Echo -> phone works, phone ->
Echo reads an idle 0/1.  cannoli's newer driver (v0.0.1.13_2023062701) sends after every HCI reset
`btmtk_sdio_set_audio_slave` = `0xfc72` with `03 10 00 4a` for the MT7668 and `49 00 80 00` for the MT7663 ("set mode
0x13"), then the pin mux (`0xfcd0`/`0xfcd1` at 0x81023054 / 0x78005054).  With `0xfc72 03 10 00 4a` sent after the
reset, donut and checkers receive too.  The MT7668's pins are already muxed (`0x22220000`).

### biscuit's /dev/ebc

MediaTek's AudDrv_BTCVSD, misc 10:59, `media:media` 0660.  ioctls magic `'C'`: `0xe0` buffer (0 TX, 1 free TX, 2 RX,
3 free RX), `0xe1` state (TX 0x00..0x04, RX 0x10..0x14: idle, init, ready, running, ending).  `read()` gives units of
32 bytes: 30 bytes of the air bitstream and 2 bytes `01 00` (valid), 8 kB/s for CVSD (64 kb/s); idle CVSD is `0x55`.
`write()` takes the bitstream alone.  CVSD bits go LSB first (Bluetooth's air order); the spec's CVSD (h = 1 - 1/32,
beta = 1 - 1/1024, J = K = 4, delta 10..1280) decodes and encodes it.

## Measured (Echo to Echo, tone in, tone out, eSCO S3, CVSD)

| Sender -> receiver | Received |
|---|---|
| cannoli -> PC (Intel AX200, BlueZ SCO socket) | 500 Hz, 100 %, -15.4 dBFS |
| checkers -> PC | 500 Hz, 100 %, -15.4 dBFS |
| donut -> cannoli | 1 kHz, 96 %, -16.8 dBFS |
| checkers -> cannoli | 1 kHz, 96 %, -16.8 dBFS |
| cannoli -> donut (`0xfc72` set) | 500 Hz, 100 %, -16.9 dBFS |
| cannoli -> checkers (`0xfc72` set) | 500 Hz, 100 %, -16.9 dBFS |
| cannoli -> biscuit (`/dev/ebc`, decoded on the PC) | 500 Hz, 100 %, -15.2 dBFS |
| biscuit (`/dev/ebc`, encoded on the PC) -> cannoli | 1 kHz, 96 %, -15.6 dBFS |

Notes for an implementation:
- The PC's AX200 through a BlueZ SCO socket sent silence: every "phone -> Echo" test with the PC as sender failed
  for that reason alone.  Test against an Echo or a phone.
- cannoli's UL5 delivers two channels at 8 kHz whatever is asked, and only while DL6 plays (PCM1's clock).
- donut's DL2 and DAI, cannoli's DL6 and UL5 are not the mixer's streams (DL1/DLM, TDM/ETDM1): the call can play on
  the speaker through the mixer's Voip stream with micHfp (AEC) to the phone, beside it.
- Not tried yet: mSBC (wideband) on the PCM paths (the controller's own mSBC coding, or transparent with the
  host's) and on biscuit (transparent: the HAL's `btsco_process_RX_MSBC`).

## The other models, from their firmware (2026-11-25)

Read from each model's boot image (kernel, device tree) and system image; none of them run here yet.

| Model | Path | Why |
|---|---|---|
| Echo 2nd gen (radar) | ebc, as biscuit | MT8163 with the integrated Bluetooth behind WMT (`WMT_SOC.cfg`), the same `audio.primary.mt8163_headless.so`, `AudDrv_BTCVSD` in the kernel, `mediatek,audio_bt_cvsd` in the device tree, `/dev/ebc` media:media |
| Echo Show 5 2nd gen (cronos), Echo Show 8 1st gen (crown) | pcm, as checkers | the same `audio.primary.mt8163.so`; the machine driver's links read out of each kernel (by the pointers to their stream names: the method gives checkers' `/proc/asound/pcm` exactly) are checkers', `VOIP_Call_BT_Playback` / `_Capture` at 10 / 11; MT7668 behind btmtksdio v0.0.1.00: the `0xfc72` setup is ours |
| Echo Show 8 2nd gen (athena) | pcm, by the legacy front ends' names | MediaTek's MT8183 HAL with its BTSCO path; the kernel has `VOIP_Call_BT_Playback` / `_Capture` (mtk-soc-pcm-bt-dai.c, DAI BT) and the BTCVSD block of an integrated core this board does not have; MT7663 behind btmtksdio v0.0.1.13, which sets the chip's PCM side itself (as cannoli's).  Its link table is in a form the pointer search did not read (a relocatable kernel), so its numbers are not known: the device is found by name |
| Echo Input (cupcake) | pcm, as donut | donut's SoC and chip, donut's `bt-sco` paths, its own kernel's `2ND EXT Codec` link (2ND I2S, `linux_bt_sco_codec`), the same links at 8 / 9.  Its device tree does not mux the 2nd I2S's pins (donut's does), but its bootloader does: LK's `mt_gpio_set_default` tables (decoded from both LKs: the routine's GOT, the mode block 0x10005300..0x10005490, five pins of 3 bits a halfword; donut's gives its device tree's values exactly) set GPIO55-57 to mode 4 and GPIO25 to 6 on both.  It plays the call on its jack or a Bluetooth speaker |

The PCM devices are named on the boards and found in `/proc/asound/pcm` at run time, as MediaTek's HAL does
(AudioALSADeviceParser): ASoC's id is the stream's name and " (*)" (a front end) or " <codec dai>-<n>".  The names
of the measured Echos resolve to the numbers the probes used (`make unit-scoaudio`, their listings in testdata/hfp/).

## As built (2026-11-25)

`src/hassmic/hfp.c` (the hands-free unit), `scoaudio.c` (the audio per `board.sco`), `cvsd.c` (biscuit's coder),
hooks in `a2dp.c` (SDP record, PSM 3, the synchronous link's events, class of device and EIR) and `ble.c` (H4 type
3 for the PC's tests).  Setting `bluetooth_calls` (Features, off).  CVSD everywhere, mSBC where the host codes the
link (below).

- **SDP**: Hands-Free 1.7, RFCOMM channel 1, features EC/NR, three-way calling, CLI, remote volume (0x0017) and
  wideband speech (0x0020) where mSBC is ours; listed only while the setting is on, the class of device's telephony
  bit and the EIR's 0x111E with it.
- **RFCOMM**: the phone opens it; credit based flow (PN), MSC.  The Echo answers PN with 7 credits and gives them back
  as they are used.
- **AT**: BRSF (0x37: EC/NR, three-way, CLI, remote volume, enhanced call status; 0xb7 with codec negotiation),
  BAC=1,2 (both negotiate codecs, mSBC ours), CIND=?, CIND?, CMER, CHLD=? (both with three-way), then CLIP, CCWA=1,
  CMEE and NREC=0 (the phone's echo cancelling off where it has it).  Calls from `+CIEV` call / callsetup / callheld,
  `RING`, `+CLIP`, `+CCWA`, `+CLCC`.  ATA, AT+CHUP, AT+CHLD=1/2, ATD<number>; and, for "take the call here", AT+BCC
  where codecs are negotiated, else Setup Synchronous Connection (CVSD, S3's parameters) on the link.
- **Codec**: the phone's `+BCS: <id>` is answered `AT+BCS=<id>` (1 CVSD, 2 mSBC where ours; anything else AT+BAC
  again).  The link it then opens is accepted with voice setting 0x0063 (transparent) and EV3 or 2-EV3 (T1, T2) for
  mSBC, 0x0060 and any packet type for CVSD.  What the call's audio is coded as comes from Synchronous Connection
  Complete's air mode (3 transparent: mSBC), not from the last +BCS.  An mSBC link that fails twice in a row (a chip that
  does not take it: a failed Synchronous Connection Complete, or the Enhanced Accept refused outright; only links
  answered as mSBC count, a good one starts anew; a command the chip leaves unanswered is a lost controller, ble.c's
  restart) turns that connection to CVSD: AT+BAC=1 at once (HFP 1.7 lets the hands-free unit update its codecs any time), so the phone's next try is
  narrowband.
- **mSBC** (msbc.c, sbc.c's `msbc_*`): 16 kHz, mono, 15 blocks, 8 subbands, loudness, bitpool 26: 57 bytes behind
  the H2 header (0x01, 0x08/0x38/0xc8/0xf8) and a padding byte, 60 bytes a 7.5 ms.  Received frames are found by
  header and syncword wherever they start, their CRC checked (it covers header and scale factors only); every 60
  bytes without a frame, and every frame that fails, is a lost one (the last, 6 dB down).  ebc: 64 kbit/s as CVSD,
  read in its 30-byte pieces (a piece marked lost given as 30 zeros), written three packets at a time; hci: one
  60-byte packet each 7.5 ms.  The PCM paths stay CVSD: they carry 8 kHz linear audio the chip codes, and whether
  a chip codes mSBC onto a 16 kHz PCM was never measured.  Not run on hardware yet (the Dot 2's /dev/ebc with a
  2-EV3 link): only the PC's HCI path against BlueZ's sbcenc/sbcdec -m.
- **Volume** (remote volume control): `+VGS: n` (0..15) is the call's volume (`n * 100 / 15`, the volume or Call
  volume: `core_call_volume`) while its audio is on the Echo; one outside a call is noted and not taken (it would move
  the music).  The Echo's call volume goes to the phone as `AT+VGS=` at the start of the audio and whenever it moves.
  `+VGM` (the microphone's gain) is not taken: call.c's gain sets the level.
- **A second call**: callsetup 1 beside a call is one waiting (+CCWA names it), callheld 1 two calls (one held), 2
  every call held.  The one shown and the other change places at callheld 1; +CLCC (where the phone has enhanced
  call status, AG bit 6) says which is which after every move.  The action button with a call waiting and HA's
  "Switch phone call" send AT+CHLD=2; ending the active call beside a held one is AT+CHLD=1 (the held one back),
  else AT+CHUP.
- **The call**: call.c's claim `CALL_HFP`; ringing (the phone's callsetup 1, not with do not disturb or the mics off),
  answered here (6 s for the phone to open the audio link), connected (the link up: call.c's media in
  `CALL_CODEC_PCM`, its mic gate and the Voip stream), on the phone (nothing shown here).
- **The phone decides where a call talks** (the owner's decision, 2026-11-25, over a review that wanted an Echo-side
  action first): a link the phone opens during a call is accepted, also for one answered on the phone itself;
  docs/SECURITY.md says what a paired phone can do with that.
- **No call, no microphone**: an audio link the phone opens while it has no call (its voice assistant, an app) is
  refused, and the Echo's microphone goes to the phone only while there is one (the phone's call or callsetup, or a
  call dialled or answered here).  An AT command unanswered for 8 s closes the DLC (a late answer would have been
  taken for the next command's); the phone opens it again.
- **/dev/ebc teardown**: the driver frees what another thread's blocked `write()` uses.  `sco_stop` freeing it while
  the writer waited rebooted the Echo Dot 2 (oops: NULL at 0x1e4 in `AudDrv_btcvsd_write+0x50`, then panic).  The
  device is now closed by the last of its two threads to leave it, and the reader rides out the driver's timeouts
  (`read()` fails after them while nothing comes).

## Wideband on the PCM paths (2026-11-25, evening)

`scoprobe` with air `msbc` (Enhanced Setup/Accept: mSBC on the air, 16 kHz s16 linear on data path 1, transport unit 0,
T2's latency 13, packets 0x0380, retransmission 2: Android's `esco_parameters` for mSBC T2), tinyplay/tinycap on the
model's call PCM devices at 16 kHz, a tone above 4 kHz (narrowband cannot carry it), the Dot 2's `/dev/ebc` as the
other end where the air's bytes are to be read or written (`ebcprobe`), decoded or encoded on the PC by BlueZ's
sbcdec/sbcenc -m.

| Test | Result |
|---|---|
| cannoli sets up mSBC (DL6 5 kHz at 16 kHz) -> biscuit accepts transparent, reads /dev/ebc | 2412 H2-framed mSBC frames in 18 s, decoded: 5 kHz 100 %, -15.3 dBFS |
| biscuit sets up transparent, writes H2 mSBC frames (6 kHz) to /dev/ebc -> cannoli accepts mSBC (Enhanced Accept) | UL5 at 16 kHz: 6 kHz 99 %, -17.3 dBFS, both channels |
| cannoli sets up mSBC -> donut accepts mSBC (Enhanced Accept, data path 1) | Synchronous Connection Complete 0x10 (accept timeout), both sides |
| donut sets up mSBC (Enhanced Setup, data path 1, after `0xfc72 03 10 00 4a`) -> biscuit listens transparent | the command is taken (status 0), then nothing: no link asked of biscuit, no completion |
| cannoli sets up CVSD by Enhanced Setup on data path 1 (8 kHz) | status 0x20 (unsupported parameter value) |
| transparent link, legacy commands: biscuit's pattern into /dev/ebc -> cannoli's UL5 | zeros; the other way cannoli's chip sends 0x55 filler whatever DL6 plays: the PCM pins carry no transparent bytes |
| donut sets up mSBC with transport unit 16 (Enhanced Setup, data path 1, DL2 5 kHz at 16 kHz) -> biscuit transparent | 2492 frames, 5 kHz 100 %, -15.3 dBFS |
| biscuit sets up transparent, writes 6 kHz mSBC -> donut accepts mSBC, transport unit 16 | the 2nd I2S at 16 kHz: 6 kHz 100 %, -14.2 dBFS |
| checkers, the same both ways (VOIP_Call_BT_Playback/Capture at 16 kHz) | 2599 frames, 5 kHz 100 %; 6 kHz 100 %, -14.2 dBFS |
| cannoli accepts with transport unit 16 as well | 6 kHz 100 %, -14.2 dBFS |
| donut, data path 2 or 3 | no link |
| donut, a transparent link: its pattern on DL2 -> biscuit; biscuit's -> donut's bus | the MT7668 does pass transparent bytes (unlike the MT7663): a byte per 8-bit slot, two slots a frame at 8 kHz, so twice the link's rate; the SoC runs the bus mono (Linux's bt-sco codec: one channel), so half of them never reach the host either way.  Not a usable path; the chip's own coding is |

So both chips code mSBC themselves from and to 16 kHz on their PCM pins, set up either way, with Enhanced
Setup/Accept on data path 1 and **transport unit 16**: the MT7668 (donut, checkers: HCI rev 0x1007, LMP subversion
0x0707) takes the command with transport unit 0 and then sets up nothing (the rows above), the MT7663 works with
either.  Board `sco_msbc_chip` (donut, cannoli, checkers), scoaudio.c `sco_accept_params` (exactly these parameters),
the PCM devices at 16 kHz then; `0xfc72` unchanged (`03 10 00 4a` on the MT7668: the chip follows the SoC's clocks).
Not measured, and on by their measured siblings (the maintainer's call, 2026-11-25): cupcake (donut's chip and
paths), cronos and crown (checkers' chip and links), athena (an MT7663, cannoli's chip); radar is ebc as biscuit,
mSBC coded by hassmic.  The Dot 2 with its whole hassmic stack against PipeWire 1.6's gateway (paired as a
phone pairs, the Hands-Free record found): AT+BRSF, AT+BAC=1,2, the service level connection, `+BCS: 2` answered
AT+BCS=2 (PipeWire then offers "Headset Head Unit, codec mSBC"), AT+VGS=5 and its `+VGS: 5` back.  PipeWire's gateway
reports no call (its Telephony API is the hands-free side's), so its audio link stays refused by design.

### What stock's firmware says about wideband (static, 2026-11-25)

Stock never was a phone's hands-free unit (Alexa: A2DP/AVRCP, calls over the internet), so no stock code sets up a
wideband call on these chips.  What there is:
- MediaTek's `libbluetooth_mtk_pure.so` (every model with a MediaTek chip, identical) has `audio_conf_map`, 18
  entries of {chip id, interface, sample rate, sync format, bit length}: the CONSYS chips (8163, 8127, 8167, 6582...)
  interface 3 (CVSD removal: the Dot 2's /dev/ebc), MT6628/6630/6632 0 (PCM), **MT7668 1 (I2S), rate 0 (8 kHz)**; no
  MT7663 entry.  It only answers `BT_MTK_OP_AUDIO_GET_CONFIG` for the HAL and sends nothing to the chip.
- The Show 5's HAL (`audio.primary.mt8163.so`): `WCNChipController::BTChipSamplingRateNumber()` is 16000 where that
  table's rate is nonzero, else 8000: the MT7668's call bus fixed at 8 kHz.  Its `bt_wbs`, `BT_SCO_isWideBand` and
  libmsbc_mtk.so serve the CONSYS path (`AudioBTCVSDControl`, software mSBC over BTCVSD).  The Dot 5's
  `audio.primary.mt8519.so` the same code (`bt_wbs` -> "wideband_on").
- The Dot 5's btmtksdio.ko `btmtk_sdio_set_audio_slave`: `0xfc72` with `03 10 00 4a` for chip 0x7668 and for any
  other, `49 00 80 00` for 0x7663; nothing depends on a rate.  "Slave": the chip follows the SoC's clocks, which fits
  the MT7663 running unchanged at 8 and 16 kHz.  donut's and checkers' older btmtksdio.ko send none.
- Fluoride (`bluetooth.default.so`, Android 7 generation): legacy setup with voice setting 0x0063 for mSBC and a
  chip-specific hook through the vendor library; MediaTek's `libbt-vendor.so` implements no audio op but an empty
  `BT_VND_OP_SCO_CFG`.

So nothing in stock says how (or whether) the MT7668 codes mSBC from its I2S bus; measured (the table above): it
does, with transport unit 16.

## On the Echos (2026-11-25)

A PC as the phone: BlueZ 5 with PipeWire 1.6's HFP audio gateway (its `headset-head-unit` profile opens the eSCO
link), an Intel AX200, the link key seeded on both sides.  The build in place of the installed one for the test.  PipeWire's gateway opens the link with no call: the build tested took it; since the rule above the same setup is
refused (a phone's call, or the PC test's, is needed).

| Echo | Service level connection | Audio from the phone | Audio to the phone |
|---|---|---|---|
| Echo Dot 2 (biscuit, ebc) | ok (BRSF … CMEE, beside A2DP aptX HD and AVRCP) | 1752 packets in 6.6 s, 8 lost; 327 frames played | the room (-48..-43 dBFS, 120-200 Hz), the Echo's own 1 kHz cancelled |
| Echo Dot 5 (cannoli, pcm) | ok | 401 frames in 8 s, 398 played, 0 lost | a constant -67 dBFS: the Echo was muted (its microphones off), and a muted call sends zeros, as it should |

The Echo Dot 3 and the Echo Show 5 ran their PCM paths Echo to Echo (above), not yet hassmic's HFP.

### The Echo Dot 5's Bluetooth address

cannoli's btmtksdio leaves the MT7663 at 00:0c:43:29:50:01 after every HCI reset: every Echo Dot 5 had that BR/EDR
address under hassmic, and the PC could not page the idme's (`br-connection-page-timeout`).  Stock's vendor library
sets it (`GORMcmd_HCC_Set_Local_BD_Addr`, 0xfc1a).  0xfc1a with the idme's six bytes (HCI order), then Read BD_ADDR:
the idme's.  `ble.c` now does that after every reset where the controller says something else; the other models'
drivers set it already.

## A real phone (2026-11-25/26, a user's Echo Dot 5)

A Redmi 12C (Android) against a Dot 5, cellular and WhatsApp calls: service level connection (AG features 0xf67),
mSBC, the downlink clean (0 lost, 0 late).  The uplink from the assistant's micAsr through call.c's gain and gate was
heard but quiet from across the room, clearly better at 30 cm; the talker's level in micAsr there, -50..-58 dBFS,
is at the gate's floor (`NEAR_MIN_DB` -58).  The Dot 5's micHfp (stock's own calls record it: `libpjmedia-audiodev.so`)
was offered then (board `call_mic` 2), and the user measured it: speech -10..-30 dBFS against micAsr's -50..-58,
the same level at 30 cm, 1.5 m and 3 m, the far end "a huge difference": `call_mic` 1.  call.c's own echo suppressor,
made for micAsr, then ducked quick replies (micHfp's AGC puts speech at the playback's level, which its echo estimate
took for echo): capped on micHfp since (PLAN.md).

Two gateways: an Android TV box (Mi Box, Android 14) paired as a music source connects for calls as well, first, and
its settings cannot stop it.  hfp.c takes one gateway at a time; a phone (class of device, major class Phone) now
takes the place from a gateway that is no phone and has no call (its DLC and multiplexer closed with DISC).


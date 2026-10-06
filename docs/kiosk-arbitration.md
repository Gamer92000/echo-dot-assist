# Kiosk Satellite wake word arbitration, against ours

Question (2026-10-05): can hassmic adopt Kiosk Satellite's wake word arbitration
(<https://kiosksatellite.com/docs/voice-satellite/#wake-word-arbitration>)? Kiosk Satellite is Xavier Larrea's Android
kiosk + Home Assistant voice satellite app (<https://github.com/jxlarrea/kiosk-satellite>). The feature came in its
2026.10.1 release. Read from the source at commit `382621d` (2026-10-04), cited as `wake_arbitration.dart:LINE` (in
`app/lib/managers/wake_word/`) and `wake_word_manager.dart:LINE`. Nothing was run; no kiosk was on the network.

Licence: CC BY-NC-ND 4.0 (plus a plugin exception that does not cover the app's code). No code may be copied; a
reimplementation of the wire format, written from this description, is interoperability, not a derived work.

Short answer: do not replace `arb.c` with it (it is a subset of ours without authentication). An opt-in bridge, so that
Echos and kiosks listening for the same phrase settle a wake word together, is possible; see the last section.

Done (2026-10-06), as a second mode instead of a bridge: the setting `arbitration_mode` switches an Echo between ours
(default) and theirs, as they designed it (no priority, no "answers" message, always the window). The settings page
compares the two. How the problems below were met: 1. off by default, and the page and README say what it costs;
2. `lockdown.sh` admits 2330 only while `state/config` has `arbitration_mode=kiosk`; 3. their formula on micAsr's last
3 s, frames lifted by 30 dB first (`main.c` `kiosk_score`), not yet calibrated; 4. the Echo waits their window (its
own setting, `arbitration_window`, 400 ms); 5. no priority at all, as asked; 6. `id` = hex of our public key's first 8
bytes. Echos in the two modes do not settle wake words with each other (beacons carry the mode, the page warns).

## Their protocol

- UDP broadcast to 255.255.255.255, port **2330**, fixed (`wake_arbitration.dart:137`), bound with `reuseAddress`. No
  discovery, no roster, no beacons: a kiosk only exists for the others while it claims.
- One JSON datagram per claim (`wake_arbitration.dart:241`):
  `{"ks":"wake","v":1,"id":"<16 hex>","n":<random 0..2^30>,"p":"<phrase>","e":<dB>}`.
  `id` is random per run (only there for the tie break), `n` random per claim (dedup key `id:n`), `p` trimmed, lower
  case, whitespace collapsed (`:182`), `e` rounded to 0.1 dB (`:186`) so both sides compare the numbers on the wire.
  Receivers drop anything that does not match these types, `id` longer than 64, a non-finite `e`, and their own `id`.
- Sent three times, at 0, 15 and 30 ms (`:253`). Claims are kept 2 s (`:141`).
- Round: claim, wait the window, then every claim with the same phrase whose arrival is within ±window of its own
  detection competes (`:262`). Highest `e` wins, exact tie: lower `id` string (`:269`, `:276`). One round, no
  "I answer" message: a claim that does not arrive leaves both answering, and Home Assistant's 2 s duplicate cooldown
  then lets the first one through.
- Window: setting `voice.wake_arbitration_window_ms`, 100-500 ms in steps of 50, default 400 (`definitions.dart:6097`).
- A muted kiosk does not claim (`wake_word_manager.dart:348`). A kiosk with too little audio does not claim and answers.
  Nothing for a kiosk already in a conversation: it claims like any other.
- Phrase: the wake word model's `wakeWord`, else its id (`wake_word_manager.dart:352`), so microWakeWord's "Alexa"
  model claims `"alexa"`, the same string ours compares (as a BLAKE2b hash) for the stock keyword.
- Score (`wakeEnergy`, `wake_arbitration.dart:21`), on the app's own 16 kHz mic audio of the last 3 s: 20 ms frames,
  dBFS each; speech = mean of the 10 loudest frames among the last 75 (1.5 s); floor = the frame at the 20th percentile
  of the whole 3 s, but never below -75 dBFS; `e` = speech - floor.

## Against `arb.c`

| | Kiosk Satellite | hassmic `arb.c` |
|---|---|---|
| Who may take part | anyone on the subnet | holders of the network key, handed over only through Home Assistant |
| Packets | plain JSON | MAC with a key from K, counter against replays |
| Score | dB of the loudest 200 ms over the quietest fifth of 3 s of mic audio | the front end's voiceEnergy / ambientEnergy (stock's own number), 1000 * log10, i.e. dB * 100; mic-stream SNR as fallback |
| Busy device | competes normally | in a conversation or ringing: claims with priority |
| Late detection | both answer (HA cooldown decides) | 1 s lookback, and the winner broadcasts that it answers |
| Window | ±400 ms (configurable 100-500) | 200 ms after our detection, fixed; no wait with no peers known |
| Copies | 3 (0/15/30 ms) | 2 |
| Port | UDP 2330 | UDP 28930 (inside the firewall's inbound 16384-32767) |

Ideas worth taking for ourselves: a third copy of each claim (cheap). A configurable window only if the not-yet-done
measurement of claim delay over Wi-Fi with power save (PLAN.md, wake word arbitration) shows 200 ms is too short.

## An opt-in bridge, if wanted

Only matters where a kiosk listens for the same phrase as an Echo ("alexa"); with different wake words they never
compete. Without a bridge Home Assistant's first-come rule already covers mixed setups, and an Echo that comes second
goes quiet (`duplicate_wake_up_detected` is a quiet finish).

Sketch: a second socket in `arb.c` on UDP 2330, behind an HA switch (settings field, off by default). On our
detection, also send a kiosk claim; take kiosk claims with our keyword into the decision.

Problems to solve:

1. **Security.** Their claims cannot be authenticated: with the bridge on, anyone on the LAN can silence an Echo by
   claiming `e: 999` for "alexa". That breaks README "Arbitration between Echos" ("nobody else on the network can
   join or silence them"), so it must be off by default and say so where it is switched on.
2. **Firewall.** 2330 is outside the inbound range 16384-32767, against the invariant in CLAUDE.md; we cannot move
   their port. Needs an INPUT rule in `lockdown.sh` (next to the 16384:32767 ones), only while the switch is on, and
   the invariant reworded to name the exception. Outbound broadcast is already allowed for hassmic.
3. **Comparable scores.** Our front-end ratio is a different quantity from their dB. For kiosk claims compute their
   formula on our 4 s micAsr ring instead. But micAsr runs about 30 dB below the level STT expects, so its floor will
   often sit at their -75 dBFS clamp, which shortens our margin: Echos would lose too often. Compute it after
   `micgain.c`, or add a fixed offset first; either way calibrate on the device next to a real kiosk.
4. **Window.** A kiosk waits 400 ms (±); we decide after 200 ms. A kiosk claim later than that is missed and both
   answer (HA's cooldown decides, quietly on our side). With no beacons we only learn of a kiosk from its first
   claim; after that, wait 400 ms for some hours.
5. **Priority.** An Echo in a conversation can claim a large `e` (their rule is only "highest wins"). A hack, but it
   gives the same outcome as our prio.
6. **Ties and ids.** Their tie break compares `id` strings: send a hex id derived from our public key, so it is stable.
7. **Stability.** `v:1`, documented only in their source, a few days old: it may change. Worth asking the author to
   write it down and to consider an optional shared key.

Effort: roughly 150-200 lines in `arb.c` and `main.c`, the switch in `proto_esphome.c`, the firewall rule, and a kiosk
stand-in in `tests/fake_ha_arbitration.py` (a socket sending their JSON). Then a device test with a kiosk.

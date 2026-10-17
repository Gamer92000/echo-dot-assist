# Known issues and notes for the maintainer

Found while testing the Echo Show 8 (crown, 1st gen) and 2nd gen (athena) in October 2026. Not fixed in the base
branches unless it says so.

## Wake sound removal shortens the listening window on the Show 8 2nd gen (athena)

Setting `wake_sound_removal` (experimental, off by default) holds the mic audio sent to the pipeline until the echo of the
wake sound has passed ("2.08 s held" in the log). On the athena, with it on, Home Assistant often ended the command
before the speaker had finished it (2026-10-15, "Alexa stops listening before I can say a command"). With it off the
window is longer and the problem is gone. The athena's wake sound starts late anyway (see below), so the held audio
comes on top of that. Leave it off on the Shows until the hold is measured and shortened.

## Wake sound and earcons start late on the Show 8 2nd gen

The first OpenSL ES player after idle takes about 575 ms before the HAL pulls its first buffer (TAS5805m amplifier
unmuting; the kernel log shows `tas5805m_mute(): triggered unmute mode`), plus the 200 ms lead-in of
`audio_android.c` (`LEAD_MS`). The sound is heard about 0.8 s after it was asked for. Keeping a player warm would
shorten it at some idle power cost; not done. The user found the delay on par with the Echo Dot 3 (donut).

## Mute button on the Show 8 2nd gen: the driver reports 1 s late

Amazon's `gpio-privacy` driver holds the enable line for `enable-gpio-toggle-duration` (1000 ms in the device tree)
before it changes `state` and sends its input event: 1.2-1.3 s after the press when muting, 0.3 s when unmuting. The
red light under the button is wired to the button and comes on at once. Fix on the athena branch: `board.privacy_button`
(the button's own input device) and `board.privacy_settle_ms`; `buttons.c` `early_press()` reports the predicted state
at the press and `latch_check()` lets the latch confirm or correct it. Other boards leave the fields empty.

## Wake word arbitration window starts at 500 ms

With other Echos on the network the window is "learning" (500 ms) until 10 claims from other members have been
sampled; an Echo whose neighbours are out of earshot never learns, and each wake word waits the full 500 ms. It
shrinks by itself once rounds with neighbours happen. (An athena in a vlan with four Dots, none near it, 2026-10-15.)

## Show 8 1st gen (crown): speaker gain

Stock gain is loud at the Sendspin maximum. `board.speaker_gain_min_db` (-6 for crown) lets the settings page go
below stock; the owner liked -2 dB. The athena (TAS5805m digital volume 0..158, stock 100, limit 118) needs no negative
range: stock is usable but without bass, +6 dB is loud and clean (owner's setting), 130 distorts.

## The Show 8 has no action button to hold

`board.action_combo`: a quick tap of both volume keys (within 400 ms of each other, released within 1 s) is the action
button, which approves a settings page login, an adb opening and Improv on an Echo that is set up and offline. A hold of
both keys is the arbitration pairing (2 s), and the 5 s warning and 10 s factory reset of a held action button do not
exist there. Reset from Home Assistant's "Factory reset" button or the settings page. (Improv on an Echo that is not set
up needs no button: tested on an athena, 2026-10-15, advertising 20 s after the reset, Wi-Fi from a phone worked.)

## Restart after a push update (fixed on crown)

After `ota-push.sh` the `hassmic` service once stayed stopped for 3 min: `init` drops a `start` issued while the
service is still stopping. `main.sh` `restart_hassmic()` waits for `stopped` before `start`.

## Tests

- `tests/otatool_test.sh` fails here ("install: a file starting with whitespace"; the C otatool says "malformed
  bundle"); not looked into, unrelated to the changes above. `tests/ota_push_test.sh` needs `nc` on the PC.

## Not testable by the owner

- Media over HTTPS (issue #30) needs a Home Assistant with `ssl_certificate`.
- The GitHub update channels (beta/release) are the last thing left on each Show.

# Security

What protects an Echo running hassmic, and where that ends. Nobody has audited any of it; see the caution in the
[README](../README.md).

## Network

| Direction | What gets through |
|---|---|
| **Out** | Amazon's daemons: local addresses only, plus DNS to the servers DHCP hands out. `otad` and `ace_otad` (firmware updates): never. hassmic itself: anywhere, to fetch replies and music from where Home Assistant or Music Assistant point it. On the screen models the Kiosk Satellite app too, while "Kiosk Satellite internet access" is on (default; one rule for that app's uid, nothing else of Amazon's or the system's gets out through it). Any other app you installed on an Echo Show: local addresses only, until you switch its **Internet** on (settings page or Echo Local's Apps tab); Amazon's apps never. |
| **In, TCP** | 16384–32767 only: 26053 ESPHome, 16700 Wyoming, 16710 the Echo's own voice (Wyoming text to speech, where one is installed), 28928 Sendspin, 28929 updates, 28931 settings page; on the screen models that name a Kiosk Satellite package (`KIOSK_PACKAGE`), also 2324 (the app's remote administration) and 6053 (its ESPHome API), from private addresses only; and the ports an app you installed listens on while its **Open ports** switch is on, from private and link-local addresses only (issue #109) |
| **In, UDP** | 16384–32767 only (28930: arbitration between Echos, 28932: Drop In audio), and 2330 only while arbitration is in Kiosk Satellite mode (their fixed port) |
| **In, an app's own ports** | Echo Shows: none, until you switch an app's **Open ports** on; then the TCP ports it listens on and the UDP ports it holds (a DLNA renderer's, its SSDP 1900), from private and link-local addresses only, found by the app's uid in the kernel's socket tables and followed when they move (issue #109). Never below 1024, never an Amazon app's. |
| **In, ICMP** | ping (echo request) from private and link-local addresses only; answers to the Echo's own traffic |

**Kiosk Satellite**, where you install it, is an app you trust like adb. Its update helper runs the app's own code as the shell user, every few minutes, so the app and each update of it can do what an adb shell can, `pm grant` included. It also gets storage and location without asking. The "microphone" and "camera" switches are Android's permissions, no wall against the app itself. Setup and `scripts/kiosk-install.sh` check who signed the APK against a pinned certificate on the PC, before it reaches the Echo; a copy you install by hand and the app's own updates are not checked by this project. The helper's provider is exported with no manifest permission, so another app on the Echo might reach it: install nothing else you do not trust. Ports 2324 and 6053 are open only while the app is installed. Its page on 2324 asks for a login once the app's remote admin password is set (seen on 2026.10.45): set one. Beyond that its pages are the app's own, and this project has not reviewed them.

Put the Echo on a network without internet as a second layer.

## Links

- **ESPHome**: encrypted like an ESPHome device with `api: encryption` but no key in its YAML. Home Assistant generates
  the key when you add the Echo, sets it over an encrypted connection, and clears it when you delete the device.
  **Until then anyone on the network can connect**, or set a key first (then see
  [When something is wrong](../README.md#when-something-is-wrong)). The key lives in `/data/local/hassmic/state/api_key`.
- **Wyoming**: unencrypted and unauthenticated, like every Wyoming satellite.

## Settings page

Plain HTTP: an Echo has no certificate a browser accepts. So:

- It never shows a secret (API key, Sendspin token, network keys).
- A browser gets in only by a press of the Echo's action button while its login waits; two browsers asking at once are
  both refused.
- Every request after that is signed with a key only that browser and that Echo share (X25519), with a counter against
  replays; so are the answers.
- The one secret the page sends, a Wi-Fi password, is encrypted with that key; the Echo keeps only the network key
  derived from it, as `wpa_supplicant.conf` does.
- Someone who can change traffic on your network (not only read it) could change the page itself, as with any plain
  HTTP page.

The light ring's designs go the other way: hassmic writes them (`state/ring-applied`), and root makes the ring's
animations from them with its own hassmic and writes those to the system partition. Root reads that one file only if
it is a regular file and not a link, at most 8 MB, and takes nothing from it but colours and times; the request
to install is moved into root's own folder before root looks at it, so it cannot be swapped for a link or a pipe
meanwhile, and a design's large upload is read only from an approved browser; the designs can
take at most 3 MB of the partition, and root checks the room before it writes.

## adb

A root shell without authentication: the unlock turns adbd's key check off. Over Wi-Fi it is closed: adbd runs without
its network listener and the firewall drops port 5555. The [ways to open it](GUIDE.md#adb-over-wi-fi) need an
approved browser plus a press of the action button for that one request, a fresh challenge signed with your update key
(a recorded exchange does not work twice), or `ADB_WIFI=1` in root's `hassmic.conf`.

While it is open, anyone on the network has root. USB always works: physical access is root access anyway. Without
`hassmic.conf` (stock behaviour, or before the install) it is open, as stock leaves it.

## Drop In

A call opens the called Echo's microphone, at once by default. Only a member of the Echo network can ask for one (the
calls travel signed with the network's key), Home Assistant only over its encrypted link, the settings page only from
an approved browser. Each call gets its own key from a fresh key exchange between the two Echos, so a recording stays
closed even to someone who gets the network key later. The audio goes straight between the two Echos and nowhere else.
Do not disturb and the microphone switches refuse calls; "Drop In answers: after the action button" makes every call
wait for a press on the Echo called; "Drop In" off stops calls both ways.

## Camera

The Echo Show's camera ([guide](GUIDE.md#camera-echo-show), [design](camera.md)) is off by default, and only an
approved client of the settings page can switch it on or watch: in practice the Echo Local integration, approved with
the action button like a browser. A stream is a signed request with a fresh counter, so a recorded one does not start
the camera again; the video is encrypted and authenticated (ChaCha20-Poly1305) with a key of that stream's, derived
from the key that client shares with the Echo, so others on the network see only its size and timing. One viewer at a
time: a second client asking meanwhile is refused rather than handed the camera. While pictures leave the Echo a red
dot shows in the screen's corner, drawn by the process that drives the camera, over every app. The lens cover stops
the camera at once (the Echo reads its switch, the image would be dark anyway); the microphone button does not.

Inside Home Assistant, Echo Local hands the video on in the clear on 127.0.0.1, at a URL with a random token made at
each start: other programs on the Home Assistant machine that learn that URL (its add-ons with host networking, say)
can watch while it runs. Any approved browser of the settings page can switch the camera on as well: revoke the
logins you no longer use (System page).

Drop In from Home Assistant (Echo Show, Echo Local's call card): the call is a signed request of the Echo Local
integration with a fresh counter, then sealed both ways with keys of that call and direction; the picture only with the
camera setting on, under the red dot. In Home Assistant the card reaches the call only through a signed path, as a
logged-in user who is not one of the Echos' kiosk users.

## Arbitration between Echos

An Echo takes the network key only from an Echo Home Assistant vouches for, from Home Assistant over its encrypted API
link, or from the button pairing.

- **Vouched for**: each Echo reports the tag `hassmic_<its public key>` as scanned. The key goes on the network,
  encrypted to the receiver, only between two Echos that each saw Home Assistant record a new scan of the other's tag:
  so only devices you adopted, and only while they are. A user of your Home Assistant could scan such a tag too, from
  the companion app.
- **Action**: where that fails, a member asks Home Assistant to run the newcomer's action
  `esphome.<node>_arbitration_key` (needs "Allow the device to perform Home Assistant actions").
- **Buttons**: a member hands the key only if exactly one Echo asked, from 2 minutes before its own buttons were held.
  Someone on the network who asks as well only makes it fail, and the newcomer's buttons go first.

The key travels encrypted to the receiving Echo, so it is not readable in Home Assistant's states, traces or logbook.
Rounds are authenticated with the key and cannot be replayed. Keys: `state/arb_key`, `state/arbitration`.

**Kiosk Satellite mode** has none of this: its claims carry no key, so any device on the network can silence the Echo
while that mode is on.

## Updates

Installed only when signed with your `secrets/update.key` (pushed from your PC) or with the project's release key
(`keys/release.pub`; downloaded by hassmic itself, only once a channel is picked under "Online updates"). Root checks
the signature, with the tool and keys from the system partition or the installed copy, before anything is unpacked.
Your key also opens adb over Wi-Fi; the release key does not.

Online updates mean trusting the project's releases: GitHub Actions builds and signs them
(`.github/workflows/build.yml`) in a job that runs only for `main` and `release`, and its secret is the only copy of the
release key besides the maintainer's. Only a browser approved on the settings page (with the action button) may pick
the channel, and only Home Assistant's encrypted link may install.

## Bluetooth

- Keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker), under `/data/local/hassmic/`.
- **Setup from a phone (Improv)**: offered only without a network: to anyone in range on an Echo that is not set up
  (nothing to lose there), only after a press of the action button on one that is. The Wi-Fi password crosses
  Bluetooth unencrypted, as with every Improv device; the Echo keeps only the network key derived from it.

## Amazon downloads

The settings page's "Download from Amazon" registers the Echo to your Amazon account. Its tokens stay on the Echo
(`state/davs`, readable by hassmic only) and never reach the page. To be let in, the Echo signs with its attestation
key, which lives in the Echo's secure hardware: hassmic can ask it for a signature but never read it. For that the
daemon carries the group that opens the secure hardware (`drmrpc`), on the Echo Dot 3 also the one of its certificate
file (`keystore`). "Sign out" on the page removes the registration again.

## Factory reset

From Home Assistant only over its encrypted link (once it set a key), from the settings page only from an approved
browser, else by holding the button.

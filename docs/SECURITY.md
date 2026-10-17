# Security

What protects an Echo running hassmic, and where that ends. Nobody has audited any of it; see the caution in the
[README](../README.md).

## Network

| Direction | What gets through |
|---|---|
| **Out** | Amazon's daemons: local addresses only, plus DNS to the servers DHCP hands out. `otad` and `ace_otad` (firmware updates): never. hassmic itself: anywhere, to fetch replies and music from where Home Assistant or Music Assistant point it. |
| **In, TCP** | 16384–32767 only: 26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates, 28931 settings page |
| **In, UDP** | 16384–32767 only (28930: arbitration between Echos, 28932: Drop In audio), and 2330 only while arbitration is in Kiosk Satellite mode (their fixed port) |

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
release key besides the maintainer's. Only Home Assistant's encrypted link may switch the channel or install.

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

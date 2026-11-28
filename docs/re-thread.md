# Thread and Zigbee on the Echo 4 (laser)

What the Echo 4's 802.15.4 radio is, what stock runs on it, and what runs in its place (`src/hassmic/radio.c`,
`src/tools/rcpbridge.c`, `scripts/otbr-build.sh`, `scripts/gprcp-build.sh`, `src/zigbee/`, `scripts/system/main.sh`
`radio_watch`).  Measured on one unit, Fire OS NS6575 / 8349, 2026-11-27 (CSL on a second unit, 2026-11-28).  What
stands today: the radio program is ours by default (gprcp, below and docs/re-gp712.md), Amazon's ot-rcp on request;
on it our border router or router (Thread) or zigbeed (Zigbee, docs/zigbee.md), or the radio for a host elsewhere.

## The hardware

- **Qorvo GP712** (GreenPeak's multi-protocol 802.15.4 transceiver).  The host side of the chip's stacks runs on the
  SoC: `gpCom`, `gpMacDispatcher`, `gpRxArbiter` and friends are RPC clients ("wrapper_client") of the chip.
- Kernel: `gp712_osal.ko`, `gp712_osaloem.ko`, `gp712_mac.ko` (`/system/vendor/etc/init/init.zigbee.rc` loads them at
  post-fs), character device `/dev/gp` (major 242, `zigbee:fifteen_four 0660`).  `gp712_regaccess.ko` is in the image
  and not loaded.
- The chip takes Zigbee and Thread at once: stock runs both stacks side by side, each its own "stack id" on
  `/dev/gp`, arbitrated by `libradio802154mngr.so` and `libcoex_mgr.so` (Wi-Fi/BLE coexistence, PTA).

## What stock runs

| Service | Program | Rc | What it is |
|---|---|---|---|
| `otservice` | `otbr-agent -I ot0 -B wlan0 -B br0 spinel+hdlc+forkpty:///system/bin/ot-rcp` | `init.openthread.rc` | OpenThread border router (Qorvo's fork), user `openthread`, NET_RAW NET_ADMIN, D-Bus, avahi |
| `connthreadsvc` | `ace_conn_thread_service` | `init.connthreadservice.rc` | Amazon's Thread service over otbr's D-Bus (Thread credentials for the Alexa app, Matter) |
| `zigbeeservice` | `ace_zigbee_service` | `init.zigbeeservice.rc` | Amazon's Zigbee hub: `libacehal_zigbee.so`, Qorvo's ZigbeeAdapter (`CZigBee7bfx...`), state in `/data/misc/zigbee/c7b.*.bin` |
| `smarthomed` | `SmartHomed` | `init.smarthomed.rc` | Alexa's smart home (Zigbee, Matter, BLE mesh); started with the Alexa client |

All three of the first are started `on property:wlan.driver.status=ok`: at every Wi-Fi driver start, not once.
`device.conf` `RADIO_SERVICES` names them; root's `radio_watch` stops whichever runs at every look (2 s), whatever
the radio is set to.  `smarthomed` is in `ALEXA_SERVICES`.

## Amazon's RCP

`/system/bin/ot-rcp` (99952 bytes, links `libpwr_timer.so`, `libpwrsvc.so`): OpenThread's RCP app on Qorvo's GP712
platform layer (`vendor/qorvo/zigbee/user/OpenThread/OpenThread_Lib_GP712/...`, `amz/src/region_settings.c`), Spinel
in HDLC-lite on its standard input and output.  Run by hand with a socket for both (toybox `nc -L`, then
`rcpbridge`), as root and as `openthread`, it answers like any OpenThread RCP:

| Property | Answer |
|---|---|
| `PROTOCOL_VERSION` | 4.3 |
| `NCP_VERSION` | `OPENTHREAD/404cfe2ff ;GP712 ; Tue Oct 27 01:25:34 UTC 2026` (404cfe2ff is not in OpenThread's public history: Qorvo's fork) |
| `INTERFACE_TYPE` | 3 (Thread) |
| `CAPS` | 5 (counters), 12 (unsolicited update filter), 24, 34 (config: radio), 513, 64 (RCP API version), 65 (min host API version), 521 |
| `RCP_API_VERSION` | 11 |
| `RCP_MIN_HOST_API_VERSION` | 4 |
| `RADIO_CAPS` | 0x1f: ack timeout, energy scan, transmit retries, CSMA backoff, sleep to transmit |
| `PHY_CHAN_SUPPORTED` | 11–25 |
| `PHY_RX_SENSITIVITY` | −100 dBm |
| `HWADDR` | `00:15:5f:00:3c:2c:6a:6d` (Qorvo's OUI) |

The radio caps lack **transmit security** and **transmit timing** (`OT_RADIO_CAPS_TRANSMIT_SEC`,
`OT_RADIO_CAPS_TRANSMIT_TIMING`), which every OpenThread host built for Thread 1.2 or later requires
(`src/posix/platform/radio.hpp` `kRequiredRadioCaps`): current otbr-agent refuses it, "RCP is missing required
capabilities: tx-security tx-timing".  So does Home Assistant's OpenThread Border Router add-on, which has no way to
pass the radio URL parameter that skips the check.  Our own radio program, gprcp (scripts/gprcp-build.sh: OpenThread's
RCP on ot-qorvo's GP712 platform and Qorvo's public library, docs/re-gp712.md), has both in software and is the
default; Amazon's stays as the setting `radio_program`'s other choice and the fallback after three short-lived
clients.

## Our border router

Upstream's ot-br-posix (pinned: main of 2026-11-26, `ot-br-posix-7d52ce8`, OpenThread `27ded13`), built with the NDK
r21e for armv7 / API 24 by `scripts/otbr-build.sh`: 3.2 MB stripped, libc only.  What differs from a plain build:

- `skip-rcp-compatibility-check` on the radio URL (main.sh) with Amazon's RCP only (gprcp needs none), which drops
  only the capability check above.  The host then does the MAC security itself where the radio does not
  (`SubMac::ProcessTransmitSecurity`, `ShouldHandle(kCapTransmitSec)`), as a Thread 1.1 RCP always had it: the RCP's
  retries resend the same secured bytes.  The link metrics subject (enhanced-ACK probing) is off: the chip's acks carry
  no Link Metrics IE.  The CSL transmitter (frames timed to a CSL child's window) is off in the plain `otbr-agent`
  (`OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE=0`) and on in `otbr-agent-csl` (`OTBR_CSL=1`), which root runs only
  with the setting `thread_csl` (experimental, off), gprcp and our module `hassmic_gptime` hooked (the chip's clock,
  its MAC filter for 802.15.4-2015 frames and Enhanced ACKs, its ack wait; docs/re-gp712.md).  Without it a
  synchronized sleepy end device (CSL, Thread 1.2) does not take this Echo as its parent; ordinary sleepy end devices
  and routers do.  With it, on a second unit (2026-11-28): a sleepy XIAO nRF52840 (NCS's OpenThread) a CSL child within
  1 s, 20 pings of 20.
- Android: the tun device is `/dev/tun` (`system:vpn`), the daemon's socket and settings in
  `/data/local/hassmic/thread`, the build date from `ro.build.date`, `-lutil`/`-lpthread` empty (bionic has both in
  libc), `__FILE__` the name alone (clang 9 has no prefix map: reproducible builds).
- OpenThread's own mDNS (ot-br-posix's default): no avahi, no D-Bus.  It runs beside the stock avahi-daemon on 5353
  (both answer: ESPHome's `_esphomelib._tcp` and the border router's `_meshcop._udp` both seen from a PC).
- REST on, on a unix socket (`--rest-listen-address`), never a TCP port of its own: `GET /node/dataset/active` gives
  the network key.  hassmic forwards it (below).  No web GUI.  NAT64, TREL, the backbone router and the rest of
  Thread 1.3 and 1.4's border router: the last section.

On the unit (2026-11-27), under root's `radio_watch`: rcpbridge holds the RCP, otbr-agent connects to it
(`spinel+hdlc+unix://`), both as `openthread` (otbr with `CapEff` 0x3000: NET_ADMIN and NET_RAW, nothing else; runas
`-c`, ambient capabilities).  A new network by REST (`DELETE /node`, `PUT /node/dataset/active`
`{"networkName":...}`, `PUT /node/state "enable"`): leader within 12 s, border routing running, an OMR prefix
(`fd1d:d3db:29ff:1::/64`) published, the border agent active, the SRP server running a few seconds later (auto
mode), `_meshcop._udp` on the LAN.  `/node`, `/node/state`, `/node/dataset/active` (JSON and TLV), `/node/ba-id` and
`/node/coprocessor/version` answer as python-otbr-api (Home Assistant's OTBR integration) reads them.  A router
(no `-B`): "Backbone interface is not specified", border routing stopped, SRP server and border agent off.

On gprcp (2026-11-27), without `skip-rcp-compatibility-check`: leader within 20 s, border routing, the border
agent; through main.sh as shipped.  Home Assistant's OTBR integration on a real Home Assistant (Docker), added with
the URL (port 28933): it found no network and formed its own, the Echo leader within 6 s, `_meshcop._udp` with the
border agent's port 28937 seen from a PC.  Every call the integration makes (python-otbr-api 3.0.0, the version Home
Assistant pins) answers; `/diagnostics` is 404 at this pin of upstream's REST (Home Assistant does not ask).

Not tried yet: a Thread device on it, Matter commissioning through it, a second border router beside it, router mode
on another border router's network, Home Assistant's OTBR add-on with the Echo as its remote radio (it should take
gprcp, which has the capabilities it asks for).

## The radio for a host elsewhere

rcpbridge serves the RCP (gprcp, or Amazon's) on `/data/local/hassmic/thread/rcp.sock`, a fresh process for each
client, ended when the client goes (`PR_SET_PDEATHSIG`: also when rcpbridge is killed).  hassmic forwards TCP 28934 to
it for Home Assistant's address and one the owner names.  With Amazon's RCP, zigbee-herdsman 11.2.5's `zoh` adapter
(Zigbee2MQTT's library; zigbee-on-host 0.3.2's stack) over `tcp://<echo>:28934`, from a PC on the LAN over the Echo's
Wi-Fi (2026-11-27): the driver took the RCP (API 11, min host 4), the network started on channel 25 with PAN ID 0x1a62
in 441 ms, permit join on and off.  No Zigbee device was at hand to join.

## Qorvo's Zigbee stack

`libacehal_zigbee.so` (780 kB) is Amazon's HAL over Qorvo's ZigbeeAdapter, a coordinator's API: `aceZbHal_init`,
`_enable`, `_formNetwork`, `_formNetworkAuto`, `_permitJoinNetwork`, `_permitJoinZts`, `_addInstallCode`,
`_sendZclUnicast`, `_sendZclBroadcast`, `_sendUnicastEui64Addr`, `_bindRequest`, `_unbindRequest`,
`_matchDescriptorRequest`, `_lqiNeighbourTableRequest`, `_getNeighbors`, `_getChildren`, `_getRouteTableEntries`,
`_getKey`, `_setToken`/`_getToken`, `_setRadioChannel`, `_setRadioPower`, `_leaveNetwork`, `_factoryReset` and
debug calls.  There is no call to join a network: it forms one or leaves it.  So it could stand behind a coordinator
for Home Assistant (an emulated EZSP or deCONZ coordinator for ZHA), never behind a router of someone else's network.
Not reversed further yet.

## Done since, and open

- Zigbee: zigbeed, a stack of our own over this RCP (docs/zigbee.md), as the coordinator (the deCONZ protocol for
  ZHA and Zigbee2MQTT) or a router (with a Green Power proxy); Qorvo's HAL stays unused: it cannot join a network.
  On the unit: a coordinator network formed by zigpy-deconz over Wi-Fi, and a router that joined a real ZHA network
  (an EmberZNet coordinator) with its own link key confirmed.  Open: Zigbee devices of other manufacturers on it.
- Our own RCP on Qorvo's driver: gprcp, the default (docs/re-gp712.md: the driver's RPC; Qorvo's public library
  works with one byte changed; no enhanced-ack probing in the driver, CSL through our module's patch of the chip's
  MAC filter).  PLAN.md "Thread 1.4".
- Home Assistant's OTBR add-on with this Echo as its remote radio: gprcp has the transmit security and timing it
  asks for; not tried (the owner's Home Assistant runs in Docker, without add-ons).

## Thread 1.3 and 1.4 on our border router (2026-11-27)

The border router's features of 1.3 and 1.4 (scripts/otbr-build.sh, main.sh `radio_watch`), tried on the unit:
NAT64 (OpenThread's translator, the Echo's MASQUERADE while `thread_nat64` is on: from the Thread side 8.8.8.8
answered), TREL (port 28936), the backbone router (Primary; the kernel has IPv6 multicast routing), the border agent
on 28937 (it was on an ephemeral port above the range the Echo admits) and its ephemeral key on 28938, DHCPv6 prefix
delegation (`thread_pd`), upstream DNS (the Wi-Fi's local DNS servers in a file of root's).  Not tried for lack of a
Thread device or another border router: a device's DNS through it, TREL to another border router, a delegated prefix,
a phone's app taking the network by the ephemeral key.

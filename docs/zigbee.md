# zigbeed: a Zigbee stack for the Echo 4

The Echo 4's radio (a Qorvo GP712, docs/re-thread.md) runs Amazon's OpenThread RCP: a plain 802.15.4 radio over
Spinel. Amazon's own Zigbee stack (`libacehal_zigbee.so`) can form a network but has no call to join one, and speaks
nothing Home Assistant does. So the Zigbee roles run a stack of our own, `zigbeed` (`src/zigbee/`, C, ~4800 lines),
as a client of the RCP like our Thread border router.

## Roles

- **Coordinator** (`-r coordinator -H <socket>`): the network's coordinator and trust center. Its host is Home
  Assistant's ZHA or Zigbee2MQTT, through the **deCONZ serial protocol** (the ConBee's), which both speak
  (`deconz.c`; zigpy-deconz's `COMMAND_SCHEMAS` and `NETWORK_PARAMETER_TYPES` are the reference). It answers as a
  ConBee II, firmware 0x26780700, protocol 0x010C. hassmic forwards TCP 28935 to it for Home Assistant's address
  (`radio.c`) and announces `_zigbee-coordinator._tcp` with `radio_type=deconz`, so ZHA discovers it. The network
  runs on after the first start whether a host is connected or not.
- **Router** (`-r router`): joins the network the owner chose on the page from those it heard (`prefer`), or the
  first that lets it (an open permit joining, Zigbee PRO, router capacity), and extends it: relays, takes children,
  holds broadcasts for its sleepy children, answers its ZDO and a ZCL Basic cluster ("hassmic", "Echo 4 Zigbee
  router"), so ZHA and Zigbee2MQTT interview it as any router; and a Green Power basic proxy (below).
- **End device** (`-r enddevice`, tests only): a sleepy end device that polls every second (at once again while its
  parent says more waits), and rejoins (secured) through another router when its parent stops acking its polls.

## What is in it

| Layer | |
|---|---|
| RCP (`spinel.c`) | Spinel 4.3 in HDLC-lite, OpenThread's formats (`radio_spinel.cpp`); one transmission at a time; energy scan one channel a request (Amazon's RCP refuses more); what the RCP says unasked is queued and acted on from the main loop only |
| MAC (`mac.c`) | data, command and beacon frames (2003), beacons with Zigbee's payload, association both sides, data requests, indirect frames for sleepy children (7.68 s, the RCP's source match tables set frame pending) |
| NWK (`nwk.c`) | security (network key, CCM* level 5, re-secured at every hop; a frame counter per sender, kept over restarts; the key before a switch taken 30 min with counters of its own; a frame under the pending key switches to it), address map, route discovery (request, reply hop by hop), many-to-one routes and route records (the coordinator as concentrator, every minute), source routes (the host's relays too), broadcasts (transaction table, relayed with jitter, repeated twice, held for sleepy children), link status every 15 s (26 entries a frame), end device timeout, leave, rejoin (secured and trust center rejoin), network status, address conflicts (ours moves, a sleepy child's is changed by its parent, else network status 0x0D), PAN ID conflicts (network report, network update, the switch after the broadcast's time) |
| APS (`aps.c`) | data (unicast, broadcast, group), acks and three tries, duplicates; fragmentation (up to 1024 bytes, blocks of 64, a window of one) both ways; link-key security (data, key-transport, key-load keys: HMAC-MMO); the trust center: transport key straight or tunnelled through a router, update device, request key (a key of its own per device, Zigbee 3.0), verify and confirm key, remove device, a new network key to all and the switch; as a joiner the same from the other side |
| Green Power (`gp.c`) | the router's basic proxy: commissioning mode from the sink, commissioning notifications, the proxy table from GP Pairing (kept in `zb.state`), GPDFs of paired switches checked (CCM*, security levels 2 and 3, their key) and their counter, tunnelled as GP Notifications to the sink's group; unidirectional (no transmission to a GPD), SrcID switches only |
| ZDO (`zdo.c`) | NWK/IEEE address (ours and our sleeping children's), node, power, simple descriptors, active endpoints (a router's 1 and 242), match descriptor (router), device announce, system server discovery, Mgmt_Lqi, Mgmt_Rtg, Mgmt_Leave, Mgmt_Permit_Joining (0xFF taken as 254 s, R21), Mgmt_NWK_Update (energy scan from timers, unicast only, the network's once per 10 s; channel change) |
| ZCL (`zdo.c`) | a router's Basic cluster (read, discover attributes) and Identify, default responses |
| Host (`deconz.c`) | parameters, network state, APS data request/confirm/indication queues (indications up to 1024 bytes), endpoint slots (four), permit joining, install-code keys (link key parameter), neighbor restore, Green Power frames passed on, MAC poll indications (a child's poll, once a minute each), the watchdog (run out: the host's connection ends) |
| State (`store.c`) | `zb.state` in the role's folder: the network, keys, every node (its key, the frame counter last heard), the NWK frame counter saved 4096 ahead, the router's choice of network and proxy table; a joiner that never proved the network key is not kept, and forgotten after 60 s |

## Security

Who can do what, and what keeps them from more (two reviews, 2026-11-27; their findings are fixed as listed):

- **Anyone in radio range** (no key): sees EUI-64s, which every device sends.
  - An unsecured rejoin (trust center rejoin) in a known device's name changes nothing of that device's entry
    (address, capability, child, its keys): the device is told the address it has, gets the network key only
    sealed with a key of its own that is proven (verified) or never was on the air (an install code's), and takes
    its place as our child only once a secured frame of its own comes. A stranger's rejoin counts only while
    joining is open, as an association does.
  - Frames to joiners and rejoiners not yet proven (association and rejoin responses, their keys) take at most four
    of the indirect queue's places, one per EUI-64; at most 16 such joiners are known at once, each forgotten after
    60 s unless it proves the network key, and none saved.
  - A frame that fails its MIC keeps no neighbor alive. A beacon moves the PAN ID only after three over 30 s, once
    an hour at most.
  - Green Power: a paired switch's frames count only at its pairing's security level and kind of key, checked
    with its key before anything of them is read or remembered (a level 0 frame in a protected switch's name goes
    nowhere); what goes out as GP notifications is bounded (commissioning frames 4 a second, all 20). In
    commissioning mode an unprotected frame of anyone's becomes a commissioning notification: the sink decides.
- **Replays**: a frame counter per sender, always (an unknown sender gets an entry; none left, the frame is
  refused), never a frame of our own EUI-64, on the flash every 10 min and at a stop (frames heard since the last
  save can be replayed once after a power cut); only growing until the key changes.
- **A network member** (has the network key, as every device):
  - cannot ask in another device's name for its key (the NWK source must be that EUI-64's address here), cannot
    use the well-known key for a device whose own key is proven, cannot send update device without APS security
    while joining is closed;
  - cannot make our router take a key, a link key, a confirm key, a remove device or a tunnelled key from anyone
    but the trust center (sealed as it; a confirm with the very key it confirms);
  - cannot move the channel (only the network manager: the coordinator's host, or 0x0000), cannot hold our radio
    with energy scans (unicast only, a pause five times as long as each scan, a minute at least), cannot flip our
    key's number (learnt once a start, and never once ours was heard).
  - What it still can: forge frames as any NWK source (Zigbee's network key is shared: true of every Zigbee
    network), including the trust center's unsecured switch key and broadcast transport key of a rotation (Ember's
    and deCONZ's way), so it can cut our router off; and race the trust center with a link key of its own while our
    router has only the well-known one (Zigbee 3.0's request key under the global key). That is the network key's
    model, not ours to close.

## Changing the network key

The coordinator's page button ("New network key…", control socket `rotate`): a new random key, its sequence number
one up, broadcast to all (NWK-secured under the current key) and then unicast to every device with a key of its own
(sealed with it, one every 100 ms), the switch key 20 s after the start or 5 s after the last unicast, whichever
later; every device counts its frames anew from 0 under the new key.  Sleepy
devices get both at their next polls (broadcasts held for them); one that missed them switches at the first frame
under the new key, or rejoins.  ZHA keeps a backup with the key: after a rotation, make a new one.  The deCONZ
protocol has no key sequence number, so a coordinator restored from an older backup starts with the old key's
number: the first frame that holds under its key with another number corrects it, and until then the other devices
take its frames under the current key whatever old number they carry.


## How it was tested

On the PC (`tests/zbair.py`: a medium of simulated OpenThread RCPs, the RCP's address filter, acks with frame
pending, energy scans one channel at a time as Amazon's, a pcap of everything):

- `tests/fake_zigbee.py`: zigpy-deconz (ZHA's library) forms the network on zigbeed's coordinator, a zigbeed router
  joins (network key, its own link key requested, verified, confirmed) and is interviewed, a sleepy end device out
  of the coordinator's reach joins through the router (update device, the key tunnelled) and is interviewed and
  read through two hops, Mgmt_Lqi, an energy scan, the coordinator restarted (same network, devices reached), a leave;
  APS fragmentation both ways (a read answer of eleven attributes, a write of 110 bytes); a Green Power switch only
  the router hears, commissioned through its proxy into zigpy's sink (its key in the commissioning frame sealed with
  the GP link key), a protected toggle tunnelled, a replay and a forged frame dropped; an address conflict (the
  router moves and announces itself; its sleepy child rejoins); a new network key (router and sleepy end device
  switch, both answer); ZHA's backup restored onto a coordinator that lost its state; the deCONZ watchdog run out;
  a PAN ID conflict (the coordinator moves, the router follows).
- `tests/fake_zigbee_z2m.py`: Zigbee2MQTT's library (zigbee-herdsman 11.2.5): its deconz adapter runs zigbeed's
  coordinator (a router joins and is interviewed and read); and its own stack, zigbee-on-host (written by others),
  as the trust center that zigbeed's router joins (its own link key confirmed), with an end device that hears only
  our router joining through it, interviewed and read through it.
- `tests/fake_radio.py`: the whole chain through hassmic: the page's settings, the port forwarded to Home
  Assistant's address only, zigpy forming the network through it, the mDNS service, permit joining from the page.
- `make unit-zb_crypto`: CCM* against Python's AES-CCM (300 cases), HMAC-MMO against an MMO of its own, the install
  code example of the spec (its link key 66B6900981E1EE3CA4206B6B861C02BB).
- `make unit-zb_fuzz` (clang, not in `make unit`): libFuzzer with ASan and UBSan over every byte from the air in each
  role and state and every byte from the host; the MIC check passes in that build only. 4 million inputs clean
  (2026-11-27, with fragmentation and Green Power).

On the Echo 4 (2026-11-27): zigbeed on the GP712 through rcpbridge; zigpy-deconz on a PC over Wi-Fi formed a network
through hassmic's port (channel 25), the energy scan per channel, permit joining; the network back by itself after an
update before any host connected; the router role hearing 2 to 6 beacons of networks nearby (none open: it waits).
Then, with joining open, the router joined the owner's ZHA network (an EmberZNet coordinator) at once, its own link
key confirmed (before that fix every association response was dropped: zigbeed gave the RCP its EUI-64 in air order,
Spinel's is big-endian).  The radio program under zigbeed is now gprcp by default, our own (docs/re-thread.md;
on it zigbeed read the EUI-64 and heard 4 networks nearby), whose energy scan answers in dBm (-53 to -79 on the
unit; Amazon's RCP reports the driver's raw levels).

## What it does not do (yet)

- No Zigbee device of a manufacturer has joined it yet: every join above is zigbeed's own or zigbee-on-host's, and
  the Green Power switch is zigpy's own frames. Real devices (a bulb, a sensor, a Friends of Hue switch) are the next
  test.
- Green Power: no bidirectional commissioning through the proxy (it never transmits to a GPD: a GPD that needs a
  Commissioning Reply from a proxy gets none), IEEE-addressed GPDs, the sink's translation table; the coordinator's
  GPDFs go to the host as a ConBee's do.
- APS fragmentation with a window of one only (EmberZNet's default); a sender with a larger window gets each block
  acked alone.
- deCONZ's beacon indication is not sent (no host reads it).
- A key rotation or a PAN ID change makes ZHA report changed radio settings at its next restart until it took a new
  backup (the page says so).

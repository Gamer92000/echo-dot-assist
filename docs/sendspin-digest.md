# Sendspin client implementer's digest (target: Music Assistant 2.10.4 / aiosendspin 9.1.1)

Sources: spec `sendspin.md` (1868 lines, cited as `spec:LINE`) and the unpacked wheel `aiosendspin 9.1.1`
(cited as `path.py:LINE`, relative to `aiosendspin/`). `noiseprotocol 0.3.1` and `cpace 0.1.0` were unpacked and read
(cited as `noise-pkg/...`, `cpace-pkg/...`). Music Assistant's own source was NOT available: everything about how MA
configures or drives the library is marked **UNVERIFIED**. Nothing here was run against a live MA. The only executed
code is the Noise test vector in Appendix A (run against the real `noiseprotocol` package).

Conventions: "LIB" = what aiosendspin 9.1.1 does (= what you must interoperate with). "SPEC" = the spec text.
b64url = RFC 4648 URL-safe alphabet, **no `=` padding**. All timestamps are microseconds, int64.

## 0. READ FIRST: aiosendspin 9.1.1 implements an older draft than the spec
The library's wire format differs from the supplied spec in many places. A client written purely from the spec will
be disconnected by the library. Where they differ, follow the LIB column.

| # | Topic | SPEC | LIB 9.1.1 (follow this) | Ref |
|---|---|---|---|---|
| D1 | Binary audio header | 13 bytes: type, int64 ts, **uint32 send_ahead** | **9 bytes**: type(1) + int64 BE ts(8). No `send_ahead`. Audio starts at offset 9 | `models/__init__.py:69-71`, `server/roles/base.py:87-91` |
| D2 | Fragmentation | type `1`, `[1][flags][orig_type][data]`, bit1=first, bit0=last | type **`2`** = more, type **`3`** = end. First: `[2][orig_type][data]`, middle: `[2][data]`, last: `[3][data]`. No flags byte | `noise/constants.py:24-25`, `noise/wire.py:160-187,201-215` |
| D3 | Noise msg-1 payload | `{"psk_id","psk_category"}` | `{"psk_id"}` only. No `psk_category` | `noise/models.py:68-73`, `noise/driver.py:284` |
| D4 | Sentinel fallback | client falls back to Sentinel on psk_id miss; server re-verifies | **Not implemented** on either side: miss = silent close | `noise/driver.py:310-312` |
| D5 | `server/error` | sent on bad `client/init` | **Never sent**; server just closes | `noise/driver.py:328-336,382-391` |
| D6 | Re-handshake | hellos NOT re-sent; `server/activate` first | After the 2 noise msgs the server **re-sends `server/hello`, expects `client/hello` again**, then `server/activate` | `server/connection.py:1490-1505`, `client/connection.py:1186-1191` |
| D7 | `player@v1_support` | `supported_formats`, `buffer_capacity` | additionally **required** `supported_commands` (subset of `volume`,`mute`). Missing => hello rejected | `models/player.py:43-65` |
| D8 | `client/state.player.supported_commands` | `volume`/`mute`/`set_output_delay` | may contain **only `set_static_delay`**. Anything else => exception => connection dropped | `models/player.py:105-124`, `models/types.py:159-164` |
| D9 | Output delay naming | `output_delay_ms`, command `set_output_delay` | **`static_delay_ms`**, command **`set_static_delay`** | `models/player.py:87,146` |
| D10 | Format preference | `client/state.player.format` | separate client message **`stream/request-format`**; `format` key is ignored | `models/core.py:731-754`, `server/roles/player/v1.py:724` |
| D11 | `supported_pair_methods` | object keyed by method | **list** of `{"method":..., ...}`; optional (may be omitted). Spec's object form => hello parse error => disconnect | `models/core.py:113-129,169` |
| D12 | Pair method names | `dynamic_pairing_code`, `static_pairing_code` | **`dynamic_pin`**, **`static_pin`**, `pairing_psk` | `models/types.py:251-256` |
| D13 | Pair abort reasons | `pairing_code_mismatch` | **`pin_mismatch`**, plus `pin_length_unacceptable` | `models/types.py:259-267` |
| D14 | Dynamic code | label `sendspin-pairing-code-derive-v1`, fixed 6 digits, `format`, rounds/`client/pair-retry`, `wrapped_nonce_B`, sid has `round` | label **`sendspin-pin-derive-v1`**, negotiated `pin_length` 4-12, no rounds/retry, **`nonce_B` sent in clear b64url**, sid = label‖h‖pairing_index (no `round`) | `noise/pin.py:10,42-49`, `noise/pairing.py:243-248,700-702` |
| D15 | Pairing-PSK flow | `client/pair-init` then `client/pair-finalize` | client sends **only `client/pair-finalize`**; a `client/pair-init` here is a protocol error | `noise/pairing.py:154-181,472-495` |
| D16 | Unknown client message types | MUST be ignored | unknown `type` (e.g. `client/leave`, `client/pair-retry`) or any field validation error **raises and drops the connection** | `server/connection.py:1722-1730,1748-1749` |
| D17 | `client/hello` extras | - | optional `trust_level` (`"none"`/`"user"`) | `models/core.py:148` |
| D18 | Activities | `playback`,`pairing` | also `management` (paired sessions only) + `management/*` messages | `models/types.py:219-227` |
| D19 | `stream/end` | `roles?` | also carries `server_transmitted` | `models/core.py:759-765` |
| D20 | `server/hello` | `name`, `languages?`, `source@v1_support?` | `name` only | `models/core.py:393-398` |
| D21 | Max Noise plaintext | 65518 payload + type byte | `MAX_TRANSPORT_PLAINTEXT = 65519` **including** the type byte (same thing) | `noise/constants.py:28` |
| D22 | source message types | `client-stream/start` | `client_stream/start` (underscore) - irrelevant for a player | `models/source.py:103` |

Forward-compat strategy: the LIB ignores unknown JSON *keys* (mashumaro default; `models/base.py:48-61` sets no
`forbid_extra_keys`), so you MAY send both `static_delay_ms` and `output_delay_ms`. You can NOT send unknown enum
values or unknown message types (D8, D16). Make the dialect a build-time switch: `SENDSPIN_DIALECT_AIOSENDSPIN_9`.

## 1. Connection establishment
Transport is always plain `ws://` (spec:165); security is Noise inside the WebSocket.

### 1.1 Server-initiated (device is the WebSocket *server*) - spec-RECOMMENDED (spec:161)
- Device advertises mDNS `_sendspin._tcp.local.`, port recommended `8928`, TXT `path` REQUIRED (recommended
  `/sendspin`), TXT `name` optional (spec:169-173). Reference client: `client/listener.py:11-13,146-160`
  (instance name `<client_id>._sendspin._tcp.local.`; the server only uses the instance name as a dict key).
- LIB server browses that type (`server/server.py:1071-1079`), IPv4 only (`IPVersion.V4Only`, `:959`), takes the
  first non-link-local address, **ignores the service if TXT `path` is missing or does not start with `/`**
  (`:1124-1127`), builds `ws://{addr}:{port}{path}` and dials it (`:1129,1148`).
- Dial options: `ws_connect(url, heartbeat=30, timeout=ClientWSTimeout(ws_close=10, ws_receive=60))`
  (`server/server.py:823-827`). No subprotocol, no compression requested.
- Reconnect: after a session the server re-dials unless `client/goodbye` said otherwise; with no goodbye it re-dials
  if activities were empty or contained `playback` (`server/connection.py:354-368`). Only goodbye reasons `restart`
  and `concurrent_attempt` trigger a re-dial. Exponential backoff from 1 s, ceiling 300 s
  (`server/server.py:48`), reset after a session lasting >= 10 s (`:51`).
- **Identity pinning:** once a URL has been associated with a `client_id`, later dials to that URL pass
  `expected_client_id`; a different `client_id` in `client/init` aborts the handshake
  (`server/server.py:835-842`, `noise/driver.py:95-98`). Your keypair MUST be persistent.
- Device MUST NOT also dial servers while advertising (spec:179).

### 1.2 Client-initiated (device is the WebSocket *client*)
- Server advertises `_sendspin-server._tcp.local.`, default port `8927`, TXT `path=/sendspin`, TXT `name`
  (`server/server.py:1045-1069`; `API_PATH = "/sendspin"` `:145`; `start_server(port=8927, ...)` `:931`).
  Instance name is `<server_id>._sendspin-server._tcp.local.`.
- Server side socket: `web.WebSocketResponse(heartbeat=30, compress=False)` (`server/connection.py:270`).
- Device MUST NOT advertise `_sendspin._tcp` in this mode (spec:214). Multi-server policy is implementation-defined
  (spec:218).

### 1.3 Which one does Music Assistant use?
LIB `start_server(..., discover_clients=True)` does **both at once**: advertises itself and dials every discovered
client. Whether MA 2.10.4 changes port/`discover_clients` is **UNVERIFIED** (MA source not available). Either
direction works against the library; after the WebSocket is up the protocol is identical (spec:220).

### 1.4 Multiple servers (server-initiated; spec:181-200)
Every Sendspin server on the LAN will dial you. Rules: hold at most one admitted connection. Let each new connection
complete the handshake up to its first `server/activate` (provisional, drop after 30 s without activate). Rank by
highest declared activity: `playback` > `pairing` > empty. Incoming >= current wins; loser gets `client/goodbye`
`another_server` (displaced) or `concurrent_attempt` (rejected incoming), then close. Both-empty tie: admit incoming
only if its `server_id` equals the persisted "last-playback server" (the server that last held the admitted connection
with `playback` declared; MUST be persisted). Later `server/activate` updates do not re-arbitrate. Note the LIB declares
`playback` only while the group is non-stopped (`server/connection.py:1196-1218`), so an idle MA connection ranks lowest.

### 1.5 WebSocket specifics
- Cleartext handshake messages = one **text** message each; everything after the Noise handshake = **binary**
  messages, one Noise transport message per WebSocket message (spec:332,381). A text frame after the handshake is
  an error (`noise/wire.py:158`). A binary frame during the handshake aborts it (`noise/driver.py:271-272`).
- Ping/Pong: the LIB enables aiohttp `heartbeat=30` on both directions. You MUST answer Ping with Pong promptly
  (aiohttp closes if no Pong within half the heartbeat - aiohttp behaviour, **UNVERIFIED** in local sources).
- Masking: as WS *server* (1.1) you receive **masked** frames and send unmasked; as WS *client* (1.2) you must mask.
- As WS server you must compute `Sec-WebSocket-Accept` = base64(SHA-1(key ‖ GUID)): **SHA-1 and standard base64 are
  needed** in addition to your SHA-256 code.
- Sizes: a Noise message is <= 65535 bytes, so every post-handshake WS message is <= 65535 bytes (use the 16-bit
  extended length; 64-bit lengths only needed defensively). 25 ms PCM 48k/16/2 chunk = 9 + 4800 + 16 = 4825 bytes.
  The LIB reassembles up to 64 MiB of Sendspin fragments (`noise/wire.py:21`), irrelevant for a player.
- WS-level fragmentation (continuation frames) is legal (spec:334); aiohttp normally sends whole frames (**UNVERIFIED**).
- JSON produced by the LIB is compact (orjson) and, because of dataclass field order, serialises as
  `{"payload":{...},"type":"..."}` - **`type` comes after `payload`** (derived from `noise/models.py:26-31`; not
  executed). Never depend on key order. Explicit `null` values occur (`"controller":null`, metadata fields).

## 2. Cleartext init exchange
Envelope for every JSON message: `{"type": "<str>", "payload": {<object>}}` (spec:340).

Sequence (spec:321-328; `noise/driver.py:80-186`):

1. C->S text `client/init`
2. S->C text `server/init`
3. S->C text `noise/handshake` (Noise msg 1) - sent back-to-back with 2
4. C->S text `noise/handshake` (Noise msg 2)
5. both switch to transport mode (binary only)
```json
{"type":"client/init","payload":{"client_id":"<43 b64url>","version":1,"suite":"25519_ChaChaPoly_SHA256"}}
{"type":"server/init","payload":{"server_id":"<43 b64url>","version":1}}
{"type":"noise/handshake","payload":{"data":"<b64url, no padding>"}}
```
- `client_id` / `server_id` = b64url(32-byte X25519 public key) = exactly 43 chars; LIB checks length 43 and decoded
  length 32 (`noise/driver.py:394-407`, `noise/keys.py:22,25-33`). `version` integer, must equal 1 both ways (`:389-391`).
- `suite` strings: `25519_ChaChaPoly_SHA256`, `25519_AESGCM_SHA256` (`noise/session.py:18-19`). The client picks; the
  server accepts either, no negotiation (spec:241-243, `noise/driver.py:382-386`). **Offering only ChaChaPoly is fine.**
- `data`: b64url without padding (`noise/keys.py:25-27`); the LIB decoder tolerates missing padding (`:30-33`).
- The server resolves the PSK *before* sending anything; with the LIB `_psk_provider` that always succeeds (falls back
  to Sentinel) (`server/connection.py:923-943`).
- `server/error` `{"reason":"unsupported_version"|"unsupported_suite"|"malformed"}` exists in SPEC (spec:495-502);
  the LIB never sends it (D5). Handle it if received (log, close), expect a bare close instead.
- Timeout: 30 s per expected message (`DEFAULT_HANDSHAKE_TIMEOUT_S = 30.0`, `noise/driver.py:40`). Every failure in
  this phase is a silent close (spec:301).
- Legacy: if the server runs with `allow_unencrypted=True` a first frame of type `client/hello` is accepted without
  Noise (`server/connection.py:907-912`). Off by default; MA's setting **UNVERIFIED**. Do not rely on it.

## 3. Noise layer
### 3.1 Parameters
- Protocol name (exact ASCII): `Noise_KKpsk2_25519_ChaChaPoly_SHA256` (`noise/session.py:21-24`). 36 bytes > 32, so
  initial `h = SHA-256(name)`, `ck = h` (`noise-pkg/state.py` initialize_symmetric).
- **Server = initiator, client = responder**, regardless of who opened the WebSocket (spec:230).
- Static keys are pre-known: each side takes the peer's from `client_id`/`server_id` in the init messages.
- Prologue = raw bytes of the `client/init` text message ‖ raw bytes of the `server/init` text message, exactly as
  sent/received (spec:291-293; `noise/driver.py:114,164`). Keep the received buffer; never re-serialise.
- HKDF: Noise standard, HMAC-SHA-256: `temp = HMAC(ck, ikm)`, `o1 = HMAC(temp, 0x01)`, `o2 = HMAC(temp, o1‖0x02)`,
  `o3 = HMAC(temp, o2‖0x03)` (`noise-pkg/functions/hash.py:25-43`).
- ChaChaPoly nonce (12 bytes) = `00 00 00 00` ‖ 64-bit **little-endian** counter `n`
  (`noise-pkg/backends/default/ciphers.py:34-35`). (AESGCM: 4 zero bytes ‖ big-endian counter.)
- Handshake AEAD AD = current `h`; transport AEAD AD = empty. Tag = 16 bytes appended.

### 3.2 KKpsk2 token flow (pre-messages: `-> s`, `<- s`)
```
msg1 (server -> client): e, es, ss            payload = {"psk_id":"..."}  (encrypted, decryptable WITHOUT the PSK)
msg2 (client -> server): e, ee, se, psk       payload = {}                 (literal 2 bytes "{}")
```
Responder (client) algorithm, verified by Appendix A:
```
h = SHA256(name); ck = h
h = SHA256(h ‖ prologue)
h = SHA256(h ‖ server_static_pub)            # initiator's pre-message first
h = SHA256(h ‖ client_static_pub)
-- read msg1 (bytes M) --
re = M[0:32];  h = SHA256(h ‖ re);  (ck,k) = HKDF2(ck, re); n=0      # psk handshakes also MixKey(e.pub)
(ck,k) = HKDF2(ck, X25519(client_static_priv, re)); n=0              # es
(ck,k) = HKDF2(ck, X25519(client_static_priv, server_static_pub)); n=0  # ss
payload1 = AEAD_decrypt(k, n=0, ad=h, M[32:]);  h = SHA256(h ‖ M[32:])
-- select PSK from payload1.psk_id (3.3) --
-- write msg2 --
e = fresh X25519 keypair;  out = e.pub;  h = SHA256(h ‖ e.pub);  (ck,k) = HKDF2(ck, e.pub)
(ck,k) = HKDF2(ck, X25519(e.priv, re))                               # ee
(ck,k) = HKDF2(ck, X25519(e.priv, server_static_pub))                # se (responder side)
(ck,temp_h,k) = HKDF3(ck, psk);  h = SHA256(h ‖ temp_h);  n=0        # psk
c = AEAD_encrypt(k, n=0, ad=h, "{}");  out ‖= c;  h = SHA256(h ‖ c)
(k1,k2) = HKDF2(ck, "")      # k1: server->client (you DECRYPT), k2: client->server (you ENCRYPT); n=0 each
handshake_hash = h           # keep: pairing sid + re-handshake prologue
```
msg1 length = 32 + len(payload1) + 16 (= 104 with the LIB payload); msg2 = 32 + 2 + 16 = 50.

### 3.3 PSK selection
- `psk_id = b64url(SHA-256("sendspin-psk-id-v1" ‖ PSK))` (`noise/keys.py:36-42`, `noise/constants.py:13`).
- Sentinel PSK = `SHA-256("sendspin-sentinel-psk-v1")` =
  `1b5e24dbc1aed95fc2a5a338a90c05df44bd10f5ec1f4cd66cbf86272767b9d3`; its psk_id =
  `GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo` (spec:263-275; recomputed locally, matches).
- Client candidates: Sentinel, its own pairing PSK (always, spec:881), every stored long-term PSK
  (`client/connection.py:381-385`, `noise/trust_store.py:666-673`).
- Long-term record match: additionally require stored `server_id` == `server/init.server_id`, else fail
  (`noise/driver.py:313-319`).
- `psk_category` (`lt`/`pr`/`sn`) is SPEC-only (D3). Treat a missing key as "any category"; if present, enforce it.
- Which PSK the LIB server references (`server/connection.py:923-943`): operator pairing attempt => pairing PSK or
  Sentinel; else stored long-term record for this `client_id`; else staged pairing PSK; else **Sentinel**.
- Session category (derived, never on the wire): long-term => paired; pairing/Sentinel => unpaired.
- Miss: SPEC says complete msg2 with the Sentinel PSK (initial handshake only). LIB client just closes and LIB server
  never retries under Sentinel (D4), so either way the connection fails until the operator deletes the stale record
  in MA. Implementing the SPEC fallback is harmless.

### 3.4 Transport framing
- Each WS binary message = one Noise transport message = AEAD(`[type byte][body]`). Plaintext must be non-empty
  (`noise/wire.py:146-147`). Separate send/receive keys and counters, both starting at 0; any AEAD failure, replay or
  reorder => close (spec:232,301).
- Type `0`: body = UTF-8 JSON envelope (`noise/wire.py:99-101,188-192`). Types 4-7 player, 8-11 artwork, 12-15
  source, 16-23 visualizer (spec:389-399). Ignore unknown types after the first `server/activate` (spec:346).
- Max plaintext incl. type byte: 65519 (`noise/constants.py:28`). Larger messages are fragmented, LIB format (D2):
  first `[2][orig_type][<=65517 data]`, then `[2][<=65518 data]`..., final `[3][data]`
  (`noise/wire.py:201-215`). Receiver: on `2` with no buffer start one (byte 1 = orig_type); on `2` with buffer
  append `plaintext[1:]`; on `3` append and dispatch `[orig_type]‖buffer`; a non-fragment while a buffer is open, or
  `3` with no buffer, is an error => close (`noise/wire.py:139-187`). A player/controller client is unlikely to ever
  receive a fragment (only artwork/huge JSON exceed 64 KiB) and never needs to send one. Optionally also accept the
  SPEC type-`1` format for future servers.

### 3.5 Rekey / re-handshake
- No Noise `REKEY` is used. Nonce exhaustion (2^64-1) is unreachable.
- Re-handshake (spec:305-315): server-initiated, in band. Both `noise/handshake` messages travel as **type-0 encrypted
  JSON** under the *old* keys; msg2 is still sent under old keys; then both directions switch. No init messages;
  `client_id`, `server_id`, suite carry over; **prologue = previous handshake hash `h`** (`noise/driver.py:189-253`).
  Reset `pairing_index` to 0 (`client/connection.py:736-752`).
- LIB then redoes `server/hello` -> `client/hello` -> `server/activate` under the new keys (D6); SPEC sends only
  `server/activate`. Robust client: after a re-handshake, if the next message is `server/hello`, answer with
  `client/hello`; if it is `server/activate`, proceed.
- Between msg1 and the new `server/activate` send nothing else (spec:311; LIB client suppresses sends,
  `client/connection.py:1031-1036`). Streams, buffers and the time filter persist (spec:315).
- The LIB only re-handshakes around pairing (`server/connection.py:1465-1505`); no periodic key rotation. A
  miss during re-handshake is a failure, not a fallback (spec:283).

## 4. After the handshake
Order (spec:326-330; `server/connection.py:1010-1016,1507-1524`): S `server/hello` -> C `client/hello` -> S
`server/activate`. All as binary type 0. The client sends nothing else before the first activate (except optionally
`client/goodbye`). LIB client waits 10 s for `server/hello` (`client/connection.py:413-416`); the whole bring-up to the
first activate is bounded at 30 s (`:149`).
```json
{"type":"server/hello","payload":{"name":"Music Assistant"}}
```
`client/hello` for LIB 9.1.1 (D7, D11, D17):
```json
{"type":"client/hello","payload":{
  "name":"Kitchen Dot",
  "device_info":{"product_name":"...","manufacturer":"...","software_version":"...","mac_address":"aa:bb:cc:dd:ee:ff"},
  "supported_roles":["player@v1","controller@v1"],
  "player@v1_support":{
    "supported_formats":[{"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":16}],
    "buffer_capacity":1048576,
    "supported_commands":["volume","mute"]},
  "supported_pair_methods":[{"method":"pairing_psk","locations":["device"]}],
  "unpaired_access":{"enabled":true},
  "trust_level":"none"}}
```
- `name` str and `supported_roles` str[] required. `device_info` and all its members optional. Do NOT send
  `client_id`/`version` here (legacy-only fields, `models/core.py:152-156`).
- Exact key `player@v1_support` (`models/core.py:157`). Required iff `player@v1` is listed, else hello rejected
  (`:215-220`). `supported_formats` non-empty; every entry needs all four ints > 0 (`models/player.py:19-39`);
  `buffer_capacity` > 0. `supported_commands` required list, values only `volume`/`mute` (`:50,61-65`).
- `supported_pair_methods`: LIB = list of `{"method","out_channels"?,"min_pin_length"?,"locations"?}` or omitted.
  `dynamic_pin` needs `min_pin_length` 4..12 or the server raises when it tries that method
  (`server/connection.py:1444-1463`). `locations` values: `device`/`leaflet`/`operator`; `out_channels`:
  `display`/`speaker` (spec:1107-1115).
- `unpaired_access.enabled` defaults to **false** if omitted (`models/core.py:132-136`).
- Role IDs known to the LIB: `player@v1 controller@v1 metadata@v1 artwork@v1 visualizer@v1 color@v1 source@v1`
  (`models/types.py:62-85`). First listed registered version per family wins (`server/roles/negotiation.py:36-56`).
- A malformed hello => server disconnects without retry (`server/connection.py:1042-1051`).

`server/activate` (`models/core.py:447-489`):
```json
{"type":"server/activate","payload":{"activities":[],"active_roles":["player@v1","controller@v1"]}}
{"type":"server/activate","payload":{"activities":["playback"]}}
{"type":"server/activate","payload":{"activities":["pairing"],"active_roles":[],"pairing":{"method":"dynamic_pin","pin_length":6,"languages":["en"]}}}
```
- `activities`: subset of `playback`,`pairing` (LIB also `management`; tolerate unknown strings). `active_roles`
  optional and **sticky**: omitted = keep previous; omitted on the first activate = empty. The LIB omits it on
  activity-only refreshes (`server/connection.py:1220-1232`) and sends it (possibly `[]`) otherwise (`:1507-1524`).
- Allowed sets per matched PSK (spec:553-561): long-term `[]`|`[playback]`; pairing/Sentinel `[]`, `[pairing]`, and -
  only if your unpaired access is enabled - `[playback]`, `[playback,pairing]`. Non-empty `active_roles` only on a
  playback-capable connection.
- Client duties on receipt (spec:565-588; reference `client/connection.py:445-483,1193-1208`):
  1. Not admissible and enabling unpaired access would fix it => `client/goodbye` `pairing_required`, close.
  2. Otherwise not admissible => `client/goodbye` `unauthorized`, close.
  3. `pairing` with a method you do not offer / wrong for the PSK => reply `pair/abort` `method_not_supported`, stay
     connected. (`pairing_psk` iff matched PSK is the pairing PSK.)
  4. Apply roles. For every removed stream role: stop output, flush buffers. For removed `controller`/`metadata`:
     discard state. For every newly active role with a state object (player): send `client/state` including it.
  5. If `pairing` is declared, run the pairing flow (5.x); pause `client/time` during it (`:519-521`).
  6. Persist last-playback `server_id` when `playback` is declared on the admitted connection.
- LIB schedules a 5 s timer for the initial `client/state` after the first activate with a player role
  (`server/connection.py:1000-1003,702-717`): lenient mode logs and continues, strict mode disconnects.
  **No audio is sent until that first `client/state` arrives** (`:459-478`).

## 5. Unpaired access and pairing
### 5.1 Unpaired access (the no-pairing path)
Client side: send `"unpaired_access":{"enabled":true}` in `client/hello` and accept Sentinel-keyed activates that
declare `playback`/roles. Nothing else. Privacy note: do not enable for a `source` (mic) role (spec:1406).

Server side (LIB): roles are activated on a Sentinel session only if
`client_info.unpaired_access.enabled and trusted_unpaired(client_id)` (`server/connection.py:1156-1164`). Approval =
`SendspinServer.trust_unpaired(client_id)` (`server/server.py:597-600`) which stores a `TrustedUnpairedClient` and
immediately re-sends `server/activate` with roles on a live connection (`server/connection.py:1526-1545`). Until then
the connection idles at `activities: []`, `active_roles: []`. How MA exposes that approval in its UI (explicit button
vs. implied by starting playback, spec:849) is **UNVERIFIED**. Approval is per `client_id`, persists in the server's
pairing store, and is deleted on a successful pairing (`noise/pairing.py:529-531`).

Expected operator steps in MA (**UNVERIFIED**): enable the Sendspin provider; the device appears as an
unpaired/unapproved player; approve it (or start playback on it).

Security: Sentinel sessions are MITM-able on the LAN (spec:853).
### 5.2 Is pairing optional?
SPEC: clients MUST implement **Pairing PSK** and MUST list it in `supported_pair_methods`; the code methods are
optional (spec:453,535,803). LIB: `supported_pair_methods` may be omitted entirely, and the server does not gate on
the advertised list - the client arbitrates with `pair/abort method_not_supported` (`server/connection.py:1360-1362`).
Minimum that works with MA: unpaired access + answering any pairing activate with `pair/abort`
`{"reason":"method_not_supported"}`. Minimum spec-compliant: that plus the Pairing PSK flow.

**Recommendation for a headless speaker (LED ring, buttons, speaker, no display):**
1. Ship with unpaired access enabled (zero extra crypto).
2. Implement **Pairing PSK**: needs only a CSPRNG 32-byte PSK, base32 and the Noise code you already have. The token
   must reach the operator out of band (print on serial console / setup log / companion page / sticker).
3. Dynamic PIN via spoken digits is the spec-preferred method for a speaker (spec:817) but needs CPace:
   SHA-512, HMAC-SHA-512 and **Elligator2 over GF(2^255-19)** (field mul/inverse/Legendre), which an
   "X25519 + ChaCha20-Poly1305" primitive library does not expose, plus bundled digit audio. Do it last.
4. Static PIN needs a printed/known 8-digit secret plus a button gesture; pointless without a label.
### 5.3 Pairing records
Record = 32-byte long-term PSK + `server_id`; hold >= 5, evict on overflow (never one backing an open connection)
(spec:819-827). After any successful pairing the server re-handshakes to the new PSK (3.5). `server/unpair` (no
payload): on a paired session delete the record, send `client/goodbye` `unpaired`, close; ignore when unpaired
(spec:764-773). `pairing_index` = number of pairing activates received since the last Noise handshake, first = 1
(`client/connection.py:553-554`).
### 5.4 Pairing PSK flow (LIB variant, D15)
Token (spec:884-899, `noise/pairing_token.py:29-43`): `"SP:0" + base32(client_pub(32) ‖ pairing_psk(32))` with `=`
stripped and every `2` replaced by `9`. Vector (client_pub `00..1f`, psk `e0..ff`; recomputed locally, matches spec):
`SP:0AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFYYDENBWHA5DYP6BYPC4PSOLZXH5DU6V97M5XXO74HR6LZ7J5PW674PT6X37T6757Y`

1. Operator pastes the token into the server. Server dials (or re-handshakes) with `psk_id` of the pairing PSK.
2. S: `server/hello`; C: `client/hello`; S: `server/activate {activities:["pairing"],active_roles:[],pairing:{method:"pairing_psk"}}`.
3. C verifies the matched PSK is its pairing PSK (else `pair/abort method_not_supported`), generates a fresh 32-byte
   `long_term_psk` and sends (LIB: **without** a preceding `client/pair-init`):
   `{"type":"client/pair-finalize","payload":{"long_term_psk":"<43 b64url>"}}`
4. S persists, replies `{"type":"server/pair-finalize","payload":{}}`. Only now C persists `(psk, server_id)`,
   replacing any older record for that server (`noise/pairing.py:483-495`).
5. S re-handshakes with `psk_id(long_term_psk)`, (LIB) hellos again, then `server/activate`.
   If instead of step 4 a `server/activate` arrives, persist nothing (spec:837).
   Client attempt timeout 120 s => `pair/abort attempt_timeout` (`noise/pairing.py:57,566-572`).
### 5.5 CPace (both PIN methods)
CPACE-X25519-SHA512, initiator/responder mode with explicit mutual confirmation; server = A/initiator, client =
B/responder (spec:1070; `cpace-pkg/cpace/__init__.py`). Inputs:

- `PRS` = ASCII digits of the PIN (`pin.encode("ascii")`, `noise/pairing.py:216,363`).
- `sid` = `"sendspin-pair-pake-v1"` ‖ `h`(32) ‖ `pairing_index` as uint32 BE (`noise/pairing.py:700-702`). (SPEC
  appends `round` uint32 BE; the LIB does not - D14.)
- `CI` = empty; `ADa` = `"server"`, `ADb` = `"client"` (`noise/pairing.py:51-52`).

Computation (`cpace-pkg/cpace/__init__.py:51-224`), `lv(x)` = LEB128 length ‖ x:
```
gen_str = lv("CPace255") ‖ lv(PRS) ‖ lv(zero_pad) ‖ lv(CI) ‖ lv(sid)
          zero_pad length = max(0, 128 - 1 - len(lv(PRS)) - len(lv("CPace255")))
g  = Elligator2( decodeLE255( SHA512(gen_str)[0:32] ) )          # u-coordinate, Z = 2, A = 486662
yb = 32 random bytes;  Yb = X25519(yb, g)                         # abort if all-zero
K  = X25519(yb, Ya)                                               # abort if all-zero (low-order point)
ISK     = SHA512( lv("CPace255_ISK") ‖ lv(sid) ‖ lv(K) ‖ lv(Ya)‖lv(ADa) ‖ lv(Yb)‖lv(ADb) )      # 64 bytes
mac_key = SHA512( "CPaceMac" ‖ sid ‖ ISK )
Ta = HMAC-SHA512(mac_key, lv(Ya)‖lv(ADa))      # server_kc, client verifies (constant time)
Tb = HMAC-SHA512(mac_key, lv(Yb)‖lv(ADb))      # client_kc, client sends
```
Wire fields, all b64url: `pake_msg_1` = Ya (32 B, 43 chars), `pake_msg_2` = Yb, `server_kc` = Ta (64 B, 86 chars),
`client_kc` = Tb. Wrapping: `K_wrap = SHA-256("sendspin-pair-psk-wrap-v1" ‖ sid ‖ ISK)`; `wrapped_psk` =
AEAD of the negotiated suite (ChaCha20-Poly1305), key `K_wrap`, 12 zero-byte nonce, empty AD, over the 32-byte PSK =
48 bytes = 64 chars (`noise/pairing.py:55-56,705-714`).
### 5.6 Dynamic PIN (LIB `dynamic_pin`)
Hello descriptor: `{"method":"dynamic_pin","out_channels":["speaker"],"min_pin_length":6}`.
1. Sentinel session. S: activate `pairing:{method:"dynamic_pin","pin_length":N,"languages":[...]?}` with
   `N = max(client_min, server_min)`, server default 6 (`server/connection.py:1444-1463`). Reject N outside
   `[your_min,12]` with `pair/abort pin_length_unacceptable` (`client/connection.py:703-708`).
2. C: 32 random bytes `nonce_B`; `{"type":"client/pair-init","payload":{"pairing_index":1,"commit_B":"<b64url SHA-256(\"sendspin-pair-commit-v1\"‖nonce_B)>"}}`
   (optionally preceded by `client/pair-pending {"pairing_index":1}` while waiting for a button gesture; reference
   client gates when N < 6 or after repeated failures, `client/connection.py:580-585`).
3. S: `{"type":"server/pair-init","payload":{"nonce_A":"<43>"}}`.
4. C: `digest = SHA-256("sendspin-pin-derive-v1" ‖ h ‖ nonce_A ‖ nonce_B)`;
   `pin = decimal(uint256_be(digest) mod 10^N)` zero-padded to N (`noise/pin.py:42-49`). Speak it (groups of 3,
   spec:959,1040), start CPace with PRS = pin.
5. S: `server/pair-auth {"pake_msg_1"}`; C: `client/pair-auth {"pake_msg_2"}`; S: `server/pair-confirm {"server_kc"}`.
6. C verifies Ta. Fail => `pair/abort pin_mismatch` (LIB has no retry round). OK =>
   `{"type":"client/pair-confirm","payload":{"client_kc":"<86>","nonce_B":"<43>"}}` immediately followed by
   `{"type":"client/pair-finalize","payload":{"wrapped_psk":"<64>"}}`.
7. S checks commit, Tb and that the derived PIN equals the entered one (`noise/pairing.py:315-327`), persists, sends
   `server/pair-finalize {}`; C persists; re-handshake as in 5.4 step 5.
Server-side timeouts: 60 s for the first client message, 360 s after `pair-pending`, 180 s for the rest
(`noise/pairing.py:62-64`); client attempt timeout 120 s.
### 5.7 Static PIN (LIB `static_pin`)
Fixed per-device random 8 ASCII digits (`noise/pin.py:18,21-23`). Every attempt is gesture-gated by a pairing window
(open by button; 5 min; closes after 5 failed `server_kc` checks) (spec:1026-1036). Flow: activate
`pairing:{method:"static_pin"}` -> [`client/pair-pending`] -> `client/pair-init {"pairing_index"}` (no `commit_B`) ->
`server/pair-auth` -> `client/pair-auth` -> `server/pair-confirm` -> verify -> `client/pair-confirm {"client_kc"}`
(no `nonce_B`) + `client/pair-finalize {"wrapped_psk"}` -> `server/pair-finalize` (`noise/pairing.py:340-400`).
### 5.8 `pair/abort`
`{"type":"pair/abort","payload":{"reason":...}}`, either direction. LIB reasons: `attempt_timeout`,
`concurrent_attempt` (sender closes afterwards), `method_not_supported`, `pin_length_unacceptable`, `pin_mismatch`,
`user_cancelled` (`models/types.py:259-271`). After an abort, silently discard in-flight pairing messages until the
next `server/activate`. Malformed pairing data = protocol error = close without any message (spec:1099-1101).

## 6. Clock synchronisation
```json
{"type":"client/time","payload":{"client_transmitted":123456789}}
{"type":"server/time","payload":{"client_transmitted":123456789,"server_received":987654321,"server_transmitted":987654400}}
```
The server stamps `server_received` on receipt and `server_transmitted` at actual send, and sends `server/time` from a
priority queue ahead of audio (`server/connection.py:1781-1792,1964-1972`). Server clock = `CLOCK_MONOTONIC_RAW` µs
(`clock.py`), not epoch. Use a raw/unslewed monotonic clock on the client too.

Measurement, T4 = local receive time taken after decryption (`client/connection.py:1222-1232`):
```
offset    = ((server_received - client_transmitted) + (server_transmitted - T4)) / 2
max_error = ((T4 - client_transmitted) - (server_transmitted - server_received)) / 2
filter.update(round(offset), round(max_error), T4)
```
Cadence in the reference client (`client/connection.py:1506-1527`): send, sleep `interval`, repeat;
`interval` = 0.2 s while not synchronised; afterwards by `error = sqrt(offset_covariance)` µs: `<1000` -> 3.0 s,
`<2000` -> 1.0 s, `<5000` -> 0.5 s, else 0.2 s. One extra `client/time` on every new `stream/start` (`:1296`).
Paused during pairing/re-handshake.

Filter (`client/time_sync.py`, constructed with defaults `SendspinTimeFilter()` at `client/connection.py:271`):
```
const ADAPTIVE_FORGETTING_CUTOFF = 3.0; MAX_ERROR_SCALE = 0.5; DRIFT_SIGNIFICANCE_THRESHOLD_SQUARED = 4.0
param process_var = 0.0^2; drift_process_var = (1e-11)^2; forget_var_factor = 2.0^2 = 4.0
state last_update=0, count=0, offset=0.0, drift=0.0, P_oo=+inf, P_od=0.0, P_dd=0.0      # doubles
      elem = {last_update, offset, drift, use_drift=false}                              # snapshot used by conversions

update(z, max_error, t):
  if t <= last_update: return
  dt = t - last_update; last_update = t
  R = (max_error * 0.5)^2
  if count == 0: count=1; offset=z; P_oo=R; drift=0; elem={t,offset,0,false}; return
  if count == 1: count=2; drift=(z-offset)/dt; offset=z; P_dd=(P_oo+R)/dt^2; P_oo=R
                 elem={t,offset,drift,false}; return
  pred  = offset + drift*dt
  nP_dd = P_dd + dt*drift_process_var
  nP_od = P_od + P_dd*dt
  nP_oo = P_oo + 2*P_od*dt + P_dd*dt^2 + dt*process_var
  resid = z - pred
  if count < 100: count += 1
  elif |resid| > 3.0*max_error: nP_dd*=4.0; nP_od*=4.0; nP_oo*=4.0
  u  = 1 / max(nP_oo + R, 1e-9)
  Ko = nP_oo*u;  Kd = nP_od*u
  offset = pred + Ko*resid;  drift += Kd*resid
  P_dd = nP_dd - Kd*nP_od;  P_od = nP_od - Kd*nP_oo;  P_oo = nP_oo - Ko*nP_oo
  use_drift = drift^2 > 4.0 * P_dd
  elem = {t, offset, drift, use_drift}

d = elem.use_drift ? elem.drift : 0
compute_server_time(tc) = tc + round(elem.offset + d*(tc - elem.last_update))
compute_client_time(ts) = round((ts - elem.offset + d*elem.last_update) / (1 + d))
is_synchronized = count >= 2 and P_oo is finite        # time_sync.py:298-305
error_us        = round(sqrt(P_oo))
```
Convergence before `available: true`: SPEC only says "converged enough" (spec:438). The LIB's criterion is
`count >= 2` (~0.4 s). Suggested stricter gate: `count >= 2 && error_us < 5000`, with a 3 s cap so the server's 5 s
initial-state timer never fires. (The reference client does not gate at all: it sends `client/state` immediately and
schedules at `now + 500 ms` until synced, `client/connection.py:156,1490-1495`.)

## 7. State, commands, group, goodbye
### 7.1 `client/state` (C->S) - LIB dialect
```json
{"type":"client/state","payload":{"available":true,"player":{
  "volume":40,"muted":false,"static_delay_ms":0,"required_lead_time_ms":250,"min_buffer_ms":250,
  "supported_commands":["set_static_delay"]}}}
```
- `available` bool: always send it. `false` = taken over by something that will not yield; the server then moves the
  client to a solo stopped group and ends streams, and never auto-rejoins (spec:629-659, `server/client.py:285-312`).
- Player fields (`models/player.py:70-129`): `volume` 0-100, `muted`, `static_delay_ms` 0-5000,
  `required_lead_time_ms` 0-30000, `min_buffer_ms` 0-30000, `supported_commands` subset of `["set_static_delay"]`
  (omit or `[]` if unsupported). **Out-of-range values or other command names throw and kill the connection** (D8, D16).
- The first state after player activation must carry all three timing fields plus `volume`/`muted` if those commands
  were declared in hello, else it is flagged non-compliant (`server/roles/player/v1.py:636-654`). Send the full
  object every time (SPEC requires full state; LIB treats omitted fields as unchanged).
- Do not send `player.state` (deprecated string) or top-level `state` (legacy).
- Server-side defaults until your state arrives: volume 100, unmuted, delay 0, lead 250 ms, min buffer 1000 ms
  (`server/roles/player/v1.py:62-76`).
- Send again whenever anything changes, including after executing a `server/command` (spec:1249). Persist volume,
  mute and the delay across reboots (spec:1266-1268).
- Volume law: `amplitude = (volume/100)^1.5`, ramp changes; mute and volume are independent (spec:1211-1213).
### 7.2 `server/command` (S->C)
```json
{"type":"server/command","payload":{"player":{"command":"volume","volume":30}}}
{"type":"server/command","payload":{"player":{"command":"mute","mute":true}}}
{"type":"server/command","payload":{"player":{"command":"set_static_delay","static_delay_ms":120}}}
```
(`models/player.py:134-177`.) `volume`/`mute` are only sent if declared in hello `supported_commands`;
`set_static_delay` only if listed in the latest state (`server/roles/player/v1.py:489-503,600-632`). Apply, persist,
echo with `client/state`. Note the key is `mute` in the command but `muted` in state. A `source` object may exist;
ignore it.
### 7.3 `group/update` (S->C)
`{"type":"group/update","payload":{"playback_state":"playing"|"stopped","group_id":"...","group_name":"..."}}`.
Sent after the client is marked connected (i.e. after the initial `client/state`) and on change
(`server/group.py:191-229`). LIB fields are individually optional (`omit_none`, `models/core.py:549-563`) and the enum
also contains `paused` although this server code never sets it; tolerate all three.
### 7.4 `server/state` (S->C)
- `controller`: `{"supported_commands":[...],"volume":0-100,"muted":bool,"repeat":"off"|"one"|"all","shuffle":bool,"seek_max_ms"?}`
  (`models/controller.py:81-98`). LIB always includes `volume`,`mute`,`switch` plus whatever the application enabled
  (`server/roles/controller/group.py` `_get_supported_commands`); which transport commands MA enables is **UNVERIFIED**.
  `volume` = average of player volumes, `muted` = all muted (spec:1595-1597). May be `null` on deactivation
  (`server/roles/controller/v1.py:67-70`).
- `metadata`: `timestamp` (server µs; future = scheduled update), optional/nullable `title`, `artist`,
  `album_artist`, `album`, `artwork_url`, `year`, `track`, `progress{track_progress ms, track_duration ms,
  playback_speed x1000}`; LIB also emits deprecated `repeat`/`shuffle` here (`models/metadata.py:50-69`).
  Position = `track_progress + (now_server - timestamp) * playback_speed / 1e6`, clamped (spec:1633-1644).
- Omitted role object = unchanged. Consecutive updates may be merged by the server before sending.
### 7.5 `client/command` (C->S, controller role active)
```json
{"type":"client/command","payload":{"controller":{"command":"next"}}}
{"type":"client/command","payload":{"controller":{"command":"volume","volume":55}}}
{"type":"client/command","payload":{"controller":{"command":"mute","mute":true}}}
```
Commands: `play pause stop next previous volume mute repeat_off repeat_one repeat_all shuffle unshuffle switch seek
seek_relative`; `volume` needs `volume`, `mute` needs `mute`, `seek` needs `position_ms` >= 0, `seek_relative` needs
`offset_ms`; **any other combination of extra fields throws** (`models/controller.py:18-76`). Only send commands listed
in the last `controller.supported_commands`; others are logged and ignored. Controller `volume`/`mute` act on the
*group*: the server redistributes and sends each player a `server/command` (spec:1550-1568), so a button press comes
back to you as a player command which you apply and echo.

Buttons on a speaker: volume +/- => either change local volume and send `client/state` (affects only this player) or
send controller `volume` (affects the group). Play/pause/next => controller commands.
### 7.6 `client/goodbye`, `client/leave`
`{"type":"client/goodbye","payload":{"reason":R}}`, R in `another_server shutdown restart user_request unauthorized
pairing_required concurrent_attempt unpaired` (`models/types.py:230-248`). The LIB disconnects on receipt and re-dials
only for `restart` (and `concurrent_attempt`) (`server/connection.py:1838-1846,354-368`). Closing without goodbye is
treated as `restart` when activities were empty or `playback`.

`client/leave` (SPEC, no payload) **does not exist in LIB 9.1.1 - sending it drops the connection** (D16). Use
`available:false`/`true` instead if you need to step out of a group.

## 8. Streams
### 8.1 `stream/start`
```json
{"type":"stream/start","payload":{"server_transmitted":1234567890,"player":{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}}}
```
`player`: `codec` `pcm|flac|opus`, `sample_rate`, `channels`, `bit_depth`, `codec_header?` (standard base64 **with**
padding; FLAC only: `fLaC` + STREAMINFO) (`models/core.py:643-681`, `models/player.py:207-224`; spec:1241).
The LIB sends it lazily, immediately before the first audio chunk (`server/roles/player/v1.py:272-340`), through the
same ordered per-role queue as the audio. A `stream/start` without `player` concerns other roles.

`stream/start` on an already active stream = format change: keep buffered chunks, decode each chunk with the format
in force when it was *received*, timeline continues (spec:1303-1305). LIB detail: on a format change the server drops
its own queued old-format chunks and resets its buffer accounting (`v1.py:832-850`) and comments that "the client
flushes its invalidated buffer only on a stream/start"; with a single-format client you never see this.
### 8.2 `stream/clear`, `stream/end`
`{"type":"stream/clear","payload":{"server_transmitted":N,"roles":["player"]}}` - flush buffered audio, keep the
stream and decoder, continue with later chunks (seek / track jump). `roles` omitted = player + visualizer.

`{"type":"stream/end","payload":{"server_transmitted":N,"roles":["player"]}}` - stop output, flush, stream inactive;
`roles` omitted = all. Process start/clear/end strictly in arrival order, even when `available:false` (spec:455,633).
Gapless track transitions send neither message (spec:740). The LIB drops its own queued binary for the role whenever
it sends clear/end (`server/connection.py:595-596`). Audio received with no active stream: discard.
### 8.3 Binary audio chunk (LIB layout, D1)
```
byte 0      : 0x04
bytes 1..8  : int64 big-endian, server-clock µs at which the FIRST frame of this chunk must leave the speaker
bytes 9..   : codec data
```
(`models/__init__.py:69-71`). PCM: signed little-endian two's complement, interleaved by channel, whole frames; 24-bit
packed as 3 bytes (spec:1233-1237); LIB can also emit 32-bit. SPEC inserts a 4-byte `send_ahead` at bytes 9..12 (µs,
0 and 0xFFFFFFFF = "no measurement"); it is absent in 9.1.1. The two layouts cannot be told apart by length parity
(9 and 13 are both 1 mod 4), so select by dialect switch.

Chunk duration: LIB fixed at 25 ms: `chunk_samples = int(sample_rate * 0.025)` = 1200 frames at 48 kHz, 1102 at
44.1 kHz; 4800 payload bytes at 48k/16/2 (`server/roles/player/v1.py:952`, `audio/codecs.py:35-60`). SPEC bounds:
<= 150 ms, normally >= 15 ms (spec:1356). Timestamps advance by exact sample counts, but the whole timeline is
**shifted forward** if the source stalls (`server/push_stream.py:1225-1239`): treat a timestamp jump as a gap
(insert silence / resync), never as an error.
### 8.4 How far ahead, `buffer_capacity`
- First chunk timestamp = now + max over grouped players of `max(min_buffer_ms, required_lead_time_ms) +
  static_delay_ms` for buffered sources, `min_buffer_ms + static_delay_ms` for live sources; 250 ms if no player is
  known yet (`server/push_stream.py:45,827-847`).
- Afterwards the server pushes as fast as the application feeds it, limited per client by (a) `buffer_capacity`
  bytes and (b) a 30 s horizon (`max_duration_us = 30_000_000`, `v1.py:69`); the writer blocks that role until space
  frees (`server/audio.py` `time_until_ready`, `server/connection.py:2228-2250`). The application may throttle earlier
  (`PushStream.sleep_to_limit_buffer`); MA's value is **UNVERIFIED**.
- LIB accounting: counts **payload bytes only** (no 9-byte header; `server/push_stream.py:1703-1705`); a chunk
  stops counting when server clock >= `timestamp + duration - static_delay` (`v1.py:353-366`). SPEC counts header +
  payload (spec:1371). Size `buffer_capacity` to RAM you really have: PCM 48k/16/2 = 192 000 B/s, so 1 MiB is about
  5.4 s. You must be able to hold that much *undecoded* data.
- Server-side late drop: chunks whose `timestamp - static_delay` has already passed are discarded before sending,
  except during the first 2 s of a stream (`v1.py:179-186`, `server/connection.py:1903-1956`). A client that is too slow
  to read gets disconnected when 4096 messages queue up (`MAX_PENDING_MSG`, `server/connection.py:149`).
### 8.5 Codec choice
LIB server can produce `pcm` (16/24/32-bit), `flac` (16/24), `opus` (16-bit, 1-2 ch, 8/12/16/24/48 kHz); channels
1-8 or 10 (`server/roles/player/capabilities.py:13-52`). Selection = **first entry of your `supported_formats` that
the server can encode**, exactly as listed; the server resamples to it (`v1.py:883-934`). An application-side
override (`set_preferred_format`) is accepted only if it is one of your listed formats (`v1.py:560-575`).
**Listing a single PCM entry works and guarantees that exact format.** Listing one rate/channel layout is also what
the spec recommends for devices that cannot switch output format gaplessly (spec:1305).

## 9. Playback synchronisation requirements
- Output instant for a chunk: `t_local = compute_client_time(timestamp) - static_delay_ms*1000`, additionally
  compensating your own DAC/driver latency (spec:1322,1266). `static_delay_ms`/`output_delay_ms` covers only delay
  *after* the audio port (external amp/speaker); 0-5000, persisted, never negative.
- Accuracy: steady-state |error| <= 1 ms (MUST), target 0.5 ms; measured against the time filter's prediction, not
  the true server clock (spec:1337-1346).
- Correction quality: inaudible; effective speed within +/-0.5 % averaged over 150 ms. One-shot resyncs are exempt
  but must be rare (spec:1332-1335). No warble at startup (spec:1350).
- Late chunks (timestamp already passed): SHOULD drop (spec:1346). Partially late: drop the late prefix.
- Startup / after `stream/start` from empty / after `stream/clear` / after underrun: hard-snap - if early, pad
  with silence until `t_local`; if late, drop the leading frames (spec:1395). `required_lead_time_ms` = your startup
  latency measured from `server_transmitted` of the trigger to the first fully playable chunk; `min_buffer_ms` =
  steady-state jitter reserve; neither includes the static delay (spec:1255-1256,1354). Report the smallest values
  that work; the reference client uses 250/250.
- Suggested steady-state strategy (spec:1377-1395), per decoded chunk: `err` = scheduled local time minus the time the
  renderer will actually reach it. `|err| < ~100 µs` => nothing. Else drop (late) or duplicate (early) `N` whole frames
  at the chunk boundary, `N = max(1, round(21e-6 * sample_rate))` (1 at 44.1/48 kHz, 2 at 96 kHz), capped at
  `floor(0.005 * frames_in_chunk)` (= 6 for a 1200-frame chunk); carry the rest to the next chunk. `|err|` > 1 ms =>
  one-shot snap. Alternative: ASRC.
- Lowering the delay can transiently leave more audio buffered than `buffer_capacity`; stay operational (spec:1266).

## 10. Test plan (aiosendspin 9.1.1 server as the peer)
Setup (network needed once): `python -m venv v && v/bin/pip install "aiosendspin[server]==9.1.1"` (pulls PyAV, numpy,
zeroconf, noiseprotocol, cpace). The outline below uses only names that exist in the source, but it was **not executed**
(dependencies are not installed on this machine) - treat as UNVERIFIED until run.
```python
import asyncio, logging, math, struct
from aiosendspin.noise import Identity, InMemoryServerPairingStore          # noise/__init__.py
from aiosendspin.server import SendspinServer, AudioFormat, ClientAddedEvent   # server/__init__.py
CLIENT_URL = "ws://127.0.0.1:8928/sendspin"   # server-initiated; set to None to let the client dial :8927/sendspin
async def main():
    logging.basicConfig(level=logging.DEBUG)          # logs non-compliance warnings and parse errors
    loop = asyncio.get_running_loop()
    server = SendspinServer(loop, Identity.generate(), "digest-test",
                            pairing_store=InMemoryServerPairingStore())   # server/server.py:149-161
    added: asyncio.Queue[str] = asyncio.Queue()
    def on_event(_srv, ev):                           # Callable[[SendspinServer, SendspinEvent], None]
        print("SERVER EVENT", ev)
        if isinstance(ev, ClientAddedEvent):
            added.put_nowait(ev.client_id)
    server.add_event_listener(on_event)
    await server.start_server(port=8927, discover_clients=False)          # (a) listens on /sendspin
    if CLIENT_URL:
        server.connect_to_client(CLIENT_URL, retry_initial_connection=True, retry_indefinitely=True)
    cid = await added.get()                           # fires once hello + first activate are done
    client = server.get_client(cid)
    print("HELLO", client.info)                       # ClientHelloPayload
    print("SECURITY", client.connection_security)     # psk_category should be 'sentinel'
    client.add_event_listener(lambda c, e: print("CLIENT EVENT", e))      # VolumeChangedEvent, StaticDelayChangedEvent...
    await server.trust_unpaired(cid)                  # (b) -> server/activate with active_roles
    await asyncio.sleep(3)                            # client syncs clock, sends initial client/state
    player = client.role("player@v1")                 # PlayerV1Role or None
    assert player is not None, client.active_role_ids
    print("STATE", player.volume, player.muted, player.static_delay_ms, player.required_lead_time_ms, player.min_buffer_ms)
    fmt = AudioFormat(sample_rate=48000, bit_depth=16, channels=2)
    stream = client.group.start_stream()              # (c) PushStream; group/update -> playing
    n, phase = 4800, 0                                # 100 ms blocks
    for i in range(60):                               # 6 s of 440 Hz
        pcm = b"".join(struct.pack("<hh", s, s) for s in
                       (int(8000 * math.sin(2 * math.pi * 440 * (phase + k) / 48000)) for k in range(n)))
        phase += n
        stream.prepare_audio(pcm, fmt)
        await stream.commit_audio()
        await stream.sleep_to_limit_buffer(1_000_000)
        if i == 20:
            player.set_volume(30)                     # (d) server/command volume -> expect VolumeChangedEvent echo
    await asyncio.sleep(2)
    await client.group.stop()                         # stream/end + group/update stopped
    print("FINAL", player.volume, player.muted)
    await server.close()
asyncio.run(main())
```
Notes: approve on `ClientAddedEvent`, **not** `ClientConnectedEvent` - the latter fires inside `attach_connection`
before the first `server/activate` is built, and approving there races with `_activate()`
(`server/connection.py:1101-1129,1507-1545`). `ClientAddedEvent` fires once per client lifetime. To pre-approve a known
id instead: `await store.add_trusted_unpaired(TrustedUnpairedClient(client_id=cid))` (`noise/trust_store.py:285-290`)
before connecting. Pairing tests: `server.initiate_pairing(cid, PairingAttempt(method=PairMethod.PAIRING_PSK,
pairing_psk=decode_token(tok).pairing_psk))` (`noise/pairing.py:99-115`, `server/server.py:553`). Whether
`start_server` behaves with `host="127.0.0.1"` for zeroconf was not determined; the default `0.0.0.0` is used above.

Bring-up order for the C client: (1) Appendix A vector offline; (2) handshake + hello against this script - expect
`activities:[]`, `active_roles:[]`, then the approved activate; (3) `client/time` loop; (4) `client/state` - watch the
server log for "non-compliant client" lines; (5) audio; (6) commands; (7) kill/restart the script to test reconnect
and `client/goodbye`.

## Appendix A. Noise KKpsk2 test vector (generated with noiseprotocol 0.3.1, Sentinel PSK, fixed ephemerals)
```
client static priv  0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
client_id           B6N8vBQgk8i3VdwbEOhstCY3StFqqFPtC9_AsrhtHHw
server static priv  2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40
server_id           WGmv9FBUlzLLqu1eXfmzCm2jHLDldCutWtShp2jxpns
client eph priv     4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60
server eph priv     6162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80
client/init bytes   {"payload":{"client_id":"B6N8vBQgk8i3VdwbEOhstCY3StFqqFPtC9_AsrhtHHw","version":1,"suite":"25519_ChaChaPoly_SHA256"},"type":"client/init"}
server/init bytes   {"payload":{"server_id":"WGmv9FBUlzLLqu1eXfmzCm2jHLDldCutWtShp2jxpns","version":1},"type":"server/init"}
prologue            client/init bytes ‖ server/init bytes (no separator, no newline)
SHA256(name)        5795ccf5ca3351f091043ef6826d4ae15c94afd8972574c94131e94c79a79cd0
msg1 payload        {"psk_id":"GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo"}
msg1 (104 B)        244fe3b963e899dd295baffce248d3530f3a9a7479ba063002680ebfe7adad49ef18399c61e15c21405c68f1c8ed034a
                    204be1459469b9b1a623c78f797a44ef992ceec263b71b14beab339adbcc74cd568e94f8d00e0c9aa7f384a0f00ea9a8
                    de4baa2893a7cbe6
msg2 payload        {}
msg2 (50 B)         64b101b1d0be5a8704bd078f9895001fc03e8e9f9522f188dd128d9846d48466cbc5a25cb576c6aa46691ba388e98ce2206d
handshake hash h    4303e5dfe73ec7ca9e5e1a708fa9aee005807246192a8145056920266e7efbb7
S->C transport #0   plaintext 00 ‖ {"type":"server/hello","payload":{"name":"x"}}
                    1d8be2dc1c58672e6183a1a82b24f64105ed759a64830d9a533d0ca71be90a6be5cddf698812bfe15b6b352b52292189
                    03d79c2b9b0ee4a68c2d77924640a0
C->S transport #0   plaintext 00 ‖ {}      ->  ceca79568ab6c25849ac80bd81cdaa751456ec
```
msg2 depends on the client ephemeral above, so a C implementation with an injectable ephemeral must reproduce msg2,
`h` and both transport ciphertexts byte for byte.

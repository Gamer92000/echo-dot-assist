# A DAVS token without stock Alexa: registration from the PC

Goal: download Amazon's artifacts (wake words, sound detection, whisper) from the settings page, without
`MODE=stock-online`, the reboots and the Alexa app of `scripts/artifacts.sh`. All a download needs is an access token DAVS
takes (`tools/davs-fetch.py`); stock Alexa only serves to get one. Probe: `tools/davs-login.py` (registers a device from
the PC, asks DAVS, deregisters; registration kept in `device-logs/davs-login.json` between steps). Run 2026-10-06
against an Echo Dot 2 (biscuit, NS65741, device type `A3S5BH2HU6VAYF`) on USB. Static analysis of `libace_map.so`
(MAP, donut and biscuit) and `libace_connectivity_manager.so`.

**Result (2026-10-06, later that day): built.** The Echo does the login itself now: `src/hassmic/davs.c` (login, DAVS,
download, staging) with `src/hassmic/dha.c` for the device attestation token whose absence failed the PC probe. What the
firmware builds was verified against `libace_map.so` of biscuit and radar instruction by instruction (see dha.c); the
whole flow runs against a fake Amazon in `tests/fake_web_davs.py`.

**donut followed (2026-10-09): its drvV3 attestation reversed.** The Echo Dot 3 proves itself with a certificate its
EC key in the TEE had Amazon sign; dha.c builds that token too now, and the "Download from Amazon" page works on it.
Details at the attestation section below. Against the real Amazon the drvV3 path is not yet tried (no device at hand);
the drvV1 path is (below).

**Against the real Amazon (2026-10-06, Echo Dot 2, amazon.de): it works.** The attestation token is what was missing;
the device secret is not needed (hassmic as `puffin` cannot read idme `mac_sec`, and none was sent). What the device
run showed:

- Until the code is entered, `/auth/register` answers `401 Unauthorized` "The request is not authorized." (not the PC
  probe's `400 InvalidDevice`: with the token, Amazon knows the device and waits for the user). The fake answers the same.
- Amazon names the device itself after the account ("<first name>'s Echo Dot"), whatever `device_name` says.
- The Echo's clock was a day behind (no NTP through the egress lock) and the token's `dat` passed all the same.
- The firmware's attestation library prints "Could not open /dev/block/mmcblk0boot1! Can't read the idme." four times
  per signing (it tries the raw idme first, which `puffin` may not open) and signs anyway.
- Downloads: wake words, the sound detection model and whisper arrived. Whisper (`whisper_components/`) and alexa-de-DE
  (`BDPGeneratedFiles/`) carry one level of folders, which artifacts.c and root's installer keep since.

## Two ways in

- **cbl** (code-based linking): what the Echo's MAP does itself (`map_registration_calculate_code_pair`,
  `map_registration_register_device`). The device asks for a code pair; a user enters the public code with their
  account; the device then polls `/auth/register` with the pair. On stock the Alexa app enters the code for the user.
  This is what davs.c does: the page shows a 6-character code and a link, nothing to paste back.
- **app**: the Alexa app's login (as alexapy / audible do): PKCE login on Amazon's page, which lands on
  `https://www.amazon.<domain>/ap/maplanding?...openid.oa2.authorization_code=...`. That page shows "not a functioning
  page"; that is expected, and the code is in the address. Then `/auth/register` as device type `A2IVLV5VM2W81`. Login
  tried once, never registered: the user turned the route down (pasting the address back). Whether DAVS hands
  artifacts to a token of an app device type is **not known**. Not built.

## What was found (cbl)

- `POST https://api.amazon.<domain>/auth/create/codepair` with
  `{"code_data":{"domain":"Device","device_serial":...,"device_type":...}}`: HTTP 200 on `.com` and `.de`, also for a
  made-up serial. Answer: `public_code` (6 characters), `private_code` (36), `secureSessionToken`,
  `polling_interval_in_seconds` (3 on .com, 6 on .de), `expires_in` 600. Creates nothing until the code is entered.
- `amazon.de/code` accepts the public code. With the serial and device type sent right, the page names the device
  ("Echo Dot 2"); with them wrong, it still says "connected".
- `/proc/idme/*` values end in a NUL byte (and `device_type_id` in a space before it): read them stripped, or every
  field goes out with `\0` attached. Readable: `serial`, `device_type_id`, `mac_addr` (0444). `mac_sec` is `system` only
  (0400): hassmic (user `puffin`) cannot read it; davs.c sends it only when a root-run hassmic saw it.
- MAP's `/auth/register` body (`map_reg_json_create_register_device_payload`, same on donut and biscuit):
  - `requested_token_type` `["bearer","mac_dms"]`, `requested_extensions` `["device_info","customer_info"]` (`actor_info`
    in some cases);
  - `auth_data`: `{"code_pair":{"public_code","private_code"},"use_global_authentication":"true"}` (or a preauthorized
    `code`, or email/password, which MAP also supports);
  - `registration_data`: `domain` "Device", `device_type`, `device_serial`, then if set: `device_name`, `device_secret`
    (only when exactly 20 characters: idme `mac_sec`), `device_authentication_token` (below),
    `device_model`, `app_version`, `app_name`, `os_version`, `software_version`. A registered Echo's `map.db` says it
    used `MYAPP` / `0.1` / `1.1` / `EchoDevice`;
  - `device_metadata`: `{"mac_address": <idme mac_addr>}`, at the top level.
- Headers (`libace_connectivity_manager.so`, secure-stream policy `map_1p_post`): `accept-language`, `content-type`,
  `authorization`, the `x-adp-*` signing headers, and `x-amzn-identity-secure-session-token`, which carries the
  `secureSessionToken` from the code pair answer. MAP stores the newest one from each answer (`map.sst` in `map.db`).
  davs.c skips the `x-adp-*` signing (the probe's plain calls were answered, and stock's own token is bound to the
  device's TLS stack); if Amazon insists on it, that is the next thing to reverse.
- The probe's `/auth/register` answered `400 InvalidDevice` "The device information is invalid." every time: before the
  code was entered (so that answer cannot tell "not entered yet" from "refused"), and after it, with the device secret,
  the MAC and the session token header all sent. What it did not send is `device_authentication_token` — the device
  attestation, now dha.c. The `index` field of the error differs in every answer.

## The attestation token (dha.c)

What MAP builds in `map_registration_create_dha_jwt` (libace_map.so), read out of the firmware of biscuit and radar and
matched by dha.c (`tests/unit/dha_jwt_test.c` pins it byte for byte against the same disassembly):

- header `{"typ":"drvV1","alg":"PS256","jwk":{"kty":"RSA","e":"65537","n":"<b64url of the 256-byte modulus>",`
  `"mac":"<b64url of HAL field 0x204>"}}` — MAP assembles this text and b64url-encodes it piecewise, every piece cut to
  a multiple of 3 bytes with the rest carried over, which equals one unpadded encode of the whole string
  (`map_base64_url_encode`: alphabet `A-Za-z0-9-_`, never `=`).
- payload `{"dev":{"dt":<device type>,"cpuid":<field 1>,"dsn":<field 0x101>,"typ":"v1"},"dat":<date>,"cust":{"typ":"v1"}}`,
  the date `strftime("%Y-%m-%dT%H:%MZ")` of the caller's time (the Echo's local time; they run on GMT).
- signature: `aceDhaHal_signData` over SHA-256 of `b64url(header) "." b64url(payload)` — the PSS happens in the HAL
  (Amazon's keymaster in the TEE; the private key never leaves it). The session needs group `drmrpc`: as `puffin`
  without it, "Failed to open DHA session: Non-specific cause". biscuit's and radar's `DAEMON_GROUPS` (`device.conf`)
  carry it, as the stock puffin service does.
- **donut builds a drvV3 token instead** (`_registration_create_jwt_internal`, inlined; reversed 2026-10-09, read out
  of `firmware/donut/rootfs/system/lib/libace_map.so` instruction by instruction like the rest, dha.c `jwt_v3` and
  `tests/unit/dha_jwt_test.c` pin it byte for byte):
  - header `{"typ":"drvV3","alg":"ES256","x5c":["<the certificate's PEM body>"]}` — the certificate is HAL field
    0x203 (`aceDhaHal_getField`), `/persist/dha_certificate.pem`: what the Echo's dhav2 provisioning had Amazon sign
    over its EC key in the TEE. MAP `strstr`s the BEGIN marker (skipping its 27 bytes) and the END marker and copies
    what stands between, CR and LF dropped — the base64 body as one line. (The HAL wrapper `libacehal_dha.so` itself
    cuts the value at the END marker first; the body is what carries, so the token runs ~1.9 kB against drvV1's ~0.7.)
  - payload `{"dev":{"dt":<device type>,"cpuid":"dfae219fe47947c7","dsn":<serial>,"typ":"v1"},"dat":<date>}` — no
    "cust" part, and the cpuid is a literal of donut's MAP (`.rodata` 0x161f9), the same for every Echo Dot 3. dt and
    dsn come from the registration info (the register payload's own `device_type` at struct offset 64, `device_serial`
    at 60 — settled by the shared `{"code_data":...,"device_serial":"%s","device_type":"%s"}` format), not from HAL
    fields.
  - signature: SHA-256 of `b64url(header) "." b64url(payload)`, `aceDhaHal_signData` over the digest — but donut's
    HAL is the dhav2 one (`/system/lib/hw/amzn_dha.mt8167.so`): an EC key behind an OpenSSL ENGINE method whose
    private half lives in the TEE (`UREE_TeeServiceCall`). The session opens `/dev/trustzone` (0660 system:drmrpc,
    gid 1026 on the device) and reads the certificate from `/persist/dha_certificate.pem` (0660 keystore:keystore,
    gid 1017) — hence `drmrpc` *and* `keystore` in donut's `DAEMON_GROUPS`, as puffinmrmd's group list has them
    (the HAL cannot add groups itself once hassmic runs as puffin; without keystore, field 0x203 answers -1,
    measured). The HAL answers in DER, which MAP splits into R and S (`mbedtls_asn1_get_tag` 0x30, two
    `mbedtls_asn1_get_mpi`) and writes left-padded into 32 bytes each — the JWS raw form — before the final b64url.
    All of this checked on a real Dot 3 with `src/tools/dha_test.c` as puffin plus those groups: the EC SPKI, the
    CRLF PEM at 0x203 (682 bytes, cut at the END marker as the wrapper does), a 71-byte DER signature.
  - how dha.c tells the shapes apart: the HAL's public key. An RSA keymaster modulus (biscuit, radar) means drvV1;
    donut's EC SubjectPublicKeyInfo fails that parse, and nothing but the EC key can sign a drvV3 token anyway.
    biscuit's and radar's HALs *have* the dhav2 functions and certificate file too (same NS65741 family), so the
    certificate's presence would not tell them apart — their MAPs just never ask for it.
  - **The token itself now runs on a real Dot 3** (2026-10-09): the Echo builds it at start (its log no longer says
    "no device attestation"). **The sign-in against the real Amazon is the part still untried** — if Amazon refuses,
    that is where to look.

## Endpoints, for reference

- `/auth/register`, `/auth/token` (refresh: `{"requested_token_type":"access_token","app_name","app_version",
  "source_token_type":"refresh_token","source_token"}`), `/auth/deregister` (`{"request_metadata":{"app_version",
  "app_name"}}` with the access token as bearer), all on `api.amazon.<domain>`; MAP picks the domain from the
  registration's country code.
- DAVS: `GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=...` with a bearer token
  (`tools/davs-fetch.py`, PLAN.md "DAVS route"). The answer's `downloadUrl` is a signed CloudFront URL; the artifact is
  a gzipped tar of a model folder (the sound detection model under `AED/`; whisper and some wake word sets with one
  level of folders inside, kept as they are).

## Left in the tree

`tools/davs-login.py` (the PC probe; nothing uses it now that davs.c does the login on the Echo) and
`src/tools/dha_test.c` (what the HAL answers on an Echo; `make DEVICE=<model> build/<model>/dha_test`).

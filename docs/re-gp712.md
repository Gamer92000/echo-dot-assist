# The GP712's driver on the Echo 4 (laser): `/dev/gp`, gpVersion, gpMacDispatcher

Static analysis, 2026-11-27. Sources:
- Amazon's kernel module `gp712_mac.ko` ("Pxxx_GpDriver-DrvMacKernel_GP712_QPF4230A_FEM_COEX_P1102_Laser (2,10,3,2/197438)",
  GCC 4.9.4, ARM). It has a **full symbol table and full DWARF (`.debug_info`)**, so every server function, struct and
  prototype is named. `llvm-dwarfdump --name=<sym> -c` gives the prototypes quoted below.
- Amazon's `/system/bin/ot-rcp` (Thumb-2, stripped; analysed by pc-relative literal xrefs).
- Qorvo's public `libQorvoGP712_ftd.a` (ARM, unstripped objects).
Tried on the unit (2026-11-27): Qorvo's public library with the one byte of §0 patched (2.8 to 2.7), linked into a probe
(static, musl, hard float: the library is the Raspberry Pi's armhf) in place of Amazon's ot-rcp: init, the EUI-64, a
beacon request sent on each channel 11 to 26 (no transmit error), the beacons and traffic of networks nearby heard
(channels 11, 20, 25), with the driver's timestamps.  Its energy scan answered values that cannot be dBm (+112, +127
on some channels): the 802.15.4 ED level read as dBm, "Energy scan" below.  (PLAN.md "Thread 1.4", phase 0.)

## Built on it (2026-11-27)

- **gprcp** (scripts/gprcp-build.sh, src/gprcp/): OpenThread's RCP on ot-qorvo's GP712 platform and this library
  (patched), transmit security and timed transmit in software.  On the unit: zigbeed and otbr-agent on it, the latter
  without `skip-rcp-compatibility-check`, leader.  The default radio program (setting `radio_program`).
- **hassmic_gptime** (src/kmod/hassmic_gptime.c): the chip clock beside the host's, caught at the driver's own calls
  to gpHal_GetTime (§5's missing piece for timed transmit), /proc/hassmic_gptime.
- **Timed transmit on the chip's clock** (src/gprcp/timed.c, tools/gp712/gptimed.c): the frame staged (txOptions |
  0x10, CSMA mode 0 around it: with CSMA the driver asserts at gpHal_MAC.c:1799 and restarts its thread), then
  0x8E [pbm handle, chip time u32 LE, stackId] through `gpCom_DataRequestAndWaitForAck(0x72, 7, buf, 0x80000040,
  ack_flag, 10000000, 0xFE)`, the dispatcher's ack buffer and flag read from gpMacDispatcher_SetPanId's literal pool
  (+0xdc, +0xec).  The chip's TX timestamp comes +255 us after the time given (255 to 256 over 20 frames); a time
  already past goes at once; staging takes 0.33 to 0.99 ms, 0x8E 0.26 to 1.19 ms (more when the driver's thread is
  late: 4.7 ms seen).  The chip's clock begins again whenever the library opens the driver.  Through gprcp over
  Spinel (tools/gp712/timedtx.py, 30 ms ahead): mean +1 us, -9 to +15 against the target (the pairs' own jitter).

## The chip's own programs (2026-11-27)

The GP712 runs programs from its own RAM (0x2000-0x3fff, at the host's SPI register addresses; 8 KB): an 8051 core
and a sequence processor.  Static (gp712_mac.ko), then on the unit:
- `gpRamProgram_RamInfo_t` {u16 size; const u8 *data; u16 start}.  Coex (Wi-Fi/Bluetooth arbitration):
  `gpHal_k7c_CoexDrvOnChip_RamDataInfo` (.data+0x33c) {0x900, .rodata+0x8ec, 0x3600}, 8051 code (SDCC output:
  vectors at the base, +0 reset, +3 INT0, +6 main; SP 35h; no call outside the blob, so no ROM code), uploaded and
  started by `gpHal_CoexInitGpioInt` (0x335e0) from every `gpHal_Init`.  The MAC filter: `gpHal_MacFilter_RamData`
  {0x4ac, .rodata+0x41c, 0x3000}, uploaded by `gpHal_Init` itself (0x31990-0x31a10) and started by writing 0x3000 to
  0x08a2 (block "seqproc"): sequencer bytecode, not 8051 (reads MACFILT 0x6a2a-0x6a34 and rib 0x430-0x43b, writes
  0x432/0x434/0x435: the receive decision, likely the ack's too).  Its instruction set: not decoded.
- `gpRamProgram_Upload` (0x1afc0): 0x806 bit 1 holds the 8051, 16-byte writes, start in [0x2000, 0x4000).
  `gpRamProgram_Run(vector)` (0x1b100): vector >> 7 into 0x680 (the vector base: 128-byte aligned), 0x806 bit 1
  released.  One core: a program of ours runs in coex's place, or merged with it.
- Register blocks (the driver's "layoutlist", .data+0x58): rib 0x400-0x43b, qta 0x480-0x48c, int_ctrl 0x540-0x573,
  es 0x580-0x5d3 (time 0x586, 2 us units), uc8051 0x680-0x6a7, pbm_adm 0x780-0x784, seqproc 0x8a2-0x8ac, macfilt
  0x6a00-0x6a37, ssp (the AES engine) 0x260-0x274; packet buffers n = 0..6 at (n + 0x62) << 8.  Coex's mailbox
  0x3fe0-0x3ff8; free: 0x2000-0x2fff (but the 0x2bxx page), 0x34ac-0x35ff, 0x3f00-0x3fdf.
- **A program of ours runs** (src/kmod/hassmic_gp8051.c, a test: `make DEVICE=laser KMOD_RADIO="hassmic_gptime
  hassmic_gp8051" build/laser/hassmic_gp8051.ko`, with a client holding the driver open; every step on the driver's
  thread through gpSched_ScheduleEvent): 0x77 bytes at 0x3f00 wrote its marker (0xA5 at 0x3fd0), counted, and coex
  was uploaded and started again (its first bytes read back, 0x680 = 0x6c).  The 8051 ran 181 loops of 10
  instructions (17 machine cycles of a classic 8051, two MOVX to the time register among them) per 512 us: **about
  3.5 instructions a microsecond** (a fast core: 6 M classic machine cycles a second), some 670 in 192 us.
- Never a stream read of the uc8051 block (0x680-0x6a7): the chip answers it with another event (0x0f for 0x07),
  the read fails and the driver's assert resets the chip (hal_spi_registeraccess.c 359, gpHal_MSI_linux.c 177,
  "Thread re-init"); single register reads (0x680, 0x806) are fine.
- tools/gp712/d51.py: the 8051 disassembler used (table-driven, recursive).

## The MAC filter program (2026-11-27)

The sequencer's instruction set (tools/gp712/dseq.py disassembles it; one opcode byte, high nibble the operation, low
the register 0..7): `0r` end, `1r vv` load immediate (`17 nn` a trace marker), `2r mm aaaa` r = [a] & m,
`3r vv aaaa` [a] = r | v (r0 is zero: a store of an immediate), `4r vv tttt`/`5r vv tttt` jump if r ==/!= v
(`40 00 tttt` always), `4(8+r)`/`5(8+r)` the same through the pointer r2:r3, `6r vv` add, `88 nn` copy n+1 bytes
between the pages in 0x883 (source) and 0x885 (destination); `7r`, `cr`, `dr`: compare, one-hot test, table match
(inferred).  Registers: r1 A, r2:r3 P, r4 I, r5 J, r6 C (reason), r7 marker.
- Phase 1 (event 0x438 = 2, the frame control field received): the ACK template reset (length 3 at 0x60e9, FCF 0x02
  at 0x6900), the FCF copied to its scratch (0x6d1d), the header's length to 0x431, 0x432 = 1.  Phase 2 (the header
  in): a copy of it at 0x6d00-0x6d23 (+02 the frame version one-hot: 1 2003, 2 2006, 4 2015; +03 seq; +06 dst PAN;
  +08 dst address; +12 src PAN, or the dst PAN when compressed; +14 src address; +1d/+1e FCF), the checks: frame
  type against 0x6a2f, **frame version against 0x6a30 at 0x320a** (2015 rejected, reason 2 into 0x435), PANs and
  addresses against the stacks' table (0x6a18/1e/24, 0x6a00/08/10).
- The decision is a strobe into 0x434: 1 the ACK's seq matches the frame waiting for one, 2 accepted, **4 send the
  ACK (PBM 7)** (0x3420, gated by 0x6a35 and the AR bit), 8 hand it up, 0x10 drop.
- PBM 7 is the ACK: data at 0x6900 (FCF0, FCF1, seq; FP from the data-pending tables 0x6b00/0x6c00), options at
  0x60e0 (+9 length without the FCS, +b antenna, +c channel index, +12 FEM bits).
- The 2015 PAN ID compression rules are not the filter's: it parses every header by 2006's.

## The ack wait (2026-11-28)

The chip takes an ack only if it has **ended** within the ack wait after its frame: register 0x408, in 32 us,
written 32 by gpHal_Init (1024 us).  Measured (tools/gp712/timedack.py, a XIAO nRF52840 whose Enh-Acks its shell
command ackie lengthens): Enh-Acks of 7, 13 and 17 bytes taken, sent at once and timed alike, 21 and 23 bytes never
(17 bytes end 192 + 23 x 32 = 928 us after the frame, 21 bytes 1056 us).  So nothing over ~20 bytes; a CSL child's
Enh-Acks are secured and carry its CSL IE, 23 bytes (31 to an extended address), and none was ever taken: the
Echo sent each CSL frame again in the next window, the child saw it as a replay (security errors).  The field is 6
bits: 64 takes no ack at all, 48 (1.5 ms) takes the 23-byte ones, 63 (2 ms) every CSL ack (cslsweep.py: 10 of 10 per
round, timed frames shifted anywhere within +-1.5 ms).  hassmic_gptime writes 63 with the filter patch, on the
driver's thread (a register written from elsewhere asserted the driver; from the data request path it hung the
kernel).  Found on the way: the filter program sees every one of those acks up to its match (0x3402: counters at
phase 1, phase 2 and the match, tools/gp712/mkseq_isa.py), the driver's gpHal_GetRxEnhancedAckFromTxPbm and
...FromTxAckAfterRx are stubs (0, 0xff), and buffer options byte 11 bit 2 (gpHal_PbmTxOptions_s acked_mode) is not
the wait (cleared at the scheduling, the chip still waited).

## Against a second radio (2026-11-27)

tools/gp712/twoway.py: gprcp beside a Silicon Labs EFR32MG26 on OpenThread's RCP (SL-OPENTHREAD 2.7.2.0), both
raw over Spinel, channel 26.  Timing: A + B (the Echo's transmit plus receive timestamp error) +425 us, sd 6 to 8,
whatever the frame's length.  Frames of version 2 (802.15.4-2015) are dropped by the receive filter unless the
radio is promiscuous; gpMacDispatcher_SetMacVersion(2) resets the driver.  PLAN.md "Thread 1.4" 2.1, 2b.2.

## Energy scan

Static analysis of gp712_mac.ko, then the unit (2026-11-27):
- The chip's raw ED byte (gpHal_RCIInterrupt 0x3115c, register handle*32+0x6002) is kept raw per channel by
  gpHal_Scan_EDConfirm (0x2dd3c); for an ED scan (scanType 0) gpHal_cbEDConfirm (0x5ab8-0x5acc) turns each through
  **gpHal_CalculateED** (0x2dfc8): gpHal_CalculateRSSI (dBm, the LNA's correction), then 0 below -89 dBm, 255 from
  -38, `floor((rssi + 89) * 51 / 10)` between.  That unsigned ED level is what goes into the result list (the
  server's static `UInt8[16]`, .bss+0x2070, kept by gpMacCore_ScanRequest at +0x14; the size check at 0x5aac lets
  size+1 through, harmless for one channel).
- The confirm's layout (MacDispatcher_cbScanConfirm_Input_par2api 0x175f8) is what the public client reads
  (gpMacDispatcher_marshalling_client.o 0x20e0); the public gpMacDispatcher_cbScanConfirm (radio_qorvo.o 0xdc), and
  Amazon's ot-rcp the same at 0xaa20, ignores status and size and reads the first byte with ldrsb as dBm.  Every
  value seen decodes as a level: +122 = -65 dBm, -114 (142) = -61, -88 (168) = -56, ot-rcp's +56 = -78, -124 = -63
  (all eight among the 51 values floor(k * 5.1) can take).  gprcp turns it back (`-89 + (ed * 10 + 50) / 51`,
  radio.c cbQorvoRadioEnergyScanDone): on the unit every channel -53 to -79 dBm (tools/gp712/timedtx.py scan).
- scanDuration d: 0..14 or 255 only (0xc394; else status 0xE8, no data), 15360 * (2^d + 1) us a channel (d=255: 128
  us); the library sends the smallest d covering the ms asked for (OpenThread's 100 ms: d=3, 138 ms); a measurement
  over that window, the scale unchanged.
- Not handled: a scan refused (0xFC one already running, 0xC3C0; 0xF1 gpHal_EDRequest busy) confirms with size 0,
  and the library's client then hands on its last value again (qorvoScanResult[0], static).  Seeing that needs our
  own gpMacDispatcher_cbScanConfirm (static in radio_qorvo.o: objcopy, or a client of ours).  OpenThread asks one
  scan at a time, so only a scan the driver itself is busy with.

## 0. Summary

- **Module 72 in the error is hex: 0x72 = 114 = gpMacDispatcher.** The format string is
  `"Warning: non matching API for module: %x. ..."`. (Decimal 72 = 0x48 would be gpZgpStub.)
- The public client announces **gpMacDispatcher 2.8.0.0**; Amazon's driver serves **2.7.0.0 only**, matched on all four
  bytes. Amazon's ot-rcp announces 2.7.0.0. Every other module the public client registers matches.
- Command ids are the same in 2.7 and 2.8; 2.8 only **adds** commands (133, 157, 167). Every fixed-length command that the
  public lib has and the kernel serves takes exactly the same request length, and the messages an OpenThread radio needs
  are byte for byte the same on both sides (checked field by field, §3.3).
- Verdict: **(a)**. Patch the one version byte (2.8 to 2.7) and keep the glue away from commands 157, 167 and 133. The same
  facts are enough to write our own client in about 600 to 800 lines of C if we want to drop Qorvo's library (§4).
- The kernel has **no CSL** at all. Enhanced-ACK probing is a stub that returns 0xE8. Timed TX exists, but it is timed on
  the chip's clock, which no RPC exposes (§5).

## 1. Version negotiation (gpVersion, module 0x81)

### 1.1 Messages (payload of a gpCom packet to module 0x81)

| Cmd | Request | Ack (from server) |
|---|---|---|
| 2 ExchangeGlobalVersion | `02 <ReleaseInfo:4>` (5 bytes, server checks `len==5`) | `FE <st> 02 <result u8> <server ReleaseInfo:4>`: result 0 equal, 1 differs |
| 3 NegotiateModuleVersion | `03 <moduleId> <nrOfVersions> <isNull=0> <n x ReleaseInfo:4>` (server checks `len>3`) | `FE <st> 03 <result u8> <selected ReleaseInfo:4>`: result 0 ok, 2 no match (selected then FF FF FF FF) |

- `ReleaseInfo` = `{major, minor, revision, patch}` (4 x u8).
- `st` = 0 ok, 1 unknown command, 3 wrong length (gpVersion_HandleCommandServer, `.ko` 0x24e6c).
- Global version: the client sends `gpVersionSw` (all zeros in the public lib). The server compares it with its own
  (`Version_ExchangeGlobalVersion_Server`, 0x2513c) and returns 1 on a mismatch. The client turns 1 into "ok"
  (`gpVersion_ExchangeCompatibility`: `cmp r0,#1; moveq r6,#0`), so this exchange is informational.
- Module negotiation on the server (`Version_NegotiateModuleVersion_Server`, 0x251a4): for the module id it walks the
  client's offered versions against its own registered list. It accepts only an **exact match on all four bytes**. It
  stores (commId, moduleId, version) in `Version_ClientVersionMapping` (24 entries x 12 bytes: u32 commId, u8 module,
  4 bytes version). Otherwise it logs and answers 2 with FF FF FF FF.
- Client side (`gpVersion_RegisterModule`, public `gpVersion_clientAdministration.o` 0x1a4): it stores the entry and, once
  the exchange has been done, calls `Version_ExchangeModuleCompatibility`, which sends cmd 3. A non-zero result logs the
  warning and then hits `gpAssert` line **210**, which is exactly what the probe printed.

### 1.2 `Version_ModuleMapping` entry (36 bytes; client: 8 entries / 0x120 bytes; server: 24 entries)

| Off | Field |
|---|---|
| +0 | u32 (set to -1 by `Version_UpdateModuleVersion`; handle/comm id) |
| +4 | moduleId (0xFF = free) |
| +5 | nrOfVersions (<= 6, asserted) |
| +6 | selected index (0xFF = none) |
| +7..+30 | versions[6], 4 bytes each |
| +0x20 | `setDefaultApiVersion` callback (called when a version is selected) |

### 1.3 Who registers what

| Module | Id | Public lib (`*_Init` -> gpVersion_RegisterModule) | Amazon ot-rcp | Kernel server (`*_InitServer`) |
|---|---|---|---|---|
| gpVersion | 0x81 (129) | 2.0.0.0 | 2.0.0.0 (0xfb38) | 2.0.0.0 |
| gpMacDispatcher | 0x72 (114) | **2.8.0.0** (`.rodata+0x20` of wrapper_client.o) | **2.7.0.0** (`movw r0,#0x702`, 0xbf0e) | **2.7.0.0** only (`.rodata+0x200`: `02 07 00 00`) |
| gpRandom | 0x6C (108) | 2.0.0.0 | 2.0.0.0 (0xefb0) | 2.0.0.0 |
| gpRxArbiter | 0x02 | 2.0.0.0 | 2.0.0.0 (0xf098) | 2.0.0.0 |
| gpAssert | 0x1D | - | - | 2.0.0.0 |
| gpEncryption | 0x7C | - | - | 2.1.0.0 |
| gpStat | 0x16 | - | - | 1.1.0.0 |
| gpTestIf | 0x74 | - | - | 2.0.0.0 |
| gpZgpStub | 0x48 (72) | - | - | 1.1.1.0, 1.1.0.0 |
| gpCoex | 0x0F | - | - | registered with gpCom only, no version |
| gpTest | 0x65 | - | - | registered with gpCom only, no version |

**Patch:** in `gpMacDispatcher_wrapper_client.o` from the archive, the bytes `02 08 00 00` sit at file offset **0x8318**
(`.rodata` at 0x82f8, plus 0x20; unique in the object). Change offset **0x8319** from 0x08 to **0x07** and re-archive, or
link the patched object ahead of the library. In a linked binary the same 4 bytes occur 8 times, so patch the object.

Note: the server also serves a client that negotiated nothing. `gpMacDispatcher_HandleCommandServer` then sets
`gpMacDispatcher_SetDummyDataInsertion(1)`: a NULL-pointer field gets zero padding of its size (11 for AddressInfo or
Security, 8 for an ext address, 27 for an ED list) instead of being left out. A client of our own should still negotiate
2.7.0.0.

## 2. Transport: `/dev/gp` (gpCom over ComIoctl)

### 2.1 Open and signals (public `gpCom_IoctlUser.o`, gpComIoctl_Init 0x204; Amazon's is the same file)
- `open("/dev/gp", 0x80002)` = O_RDWR|O_CLOEXEC.
- `sigaction(SIGIO=29, ComIoctl_SignalIOHandler)`, `fcntl(fd, F_SETOWN, getpid())`, `fcntl(F_SETFL, flags|O_ASYNC(0x2000))`.
  **SIGIO means "driver reset"**: the handler logs "Driver reset detected" and schedules ConnectionClose, which closes,
  reopens and reinitialises. A MacDispatcher `cbDriverResetIndication` (id 98) also arrives.
- Normal RX: the fd goes readable (`select()` in hal_Sched; the osal module implements poll).
  `ComIoctl_PollFunction` is then called until the queue is empty.
- The kernel takes up to 4 concurrent opens ("ioctl channels") with comm ids 0x80000040, 0x80000080, 0x80000010 and
  0x80000020 (`ComIoctl_CommIdToIdx`). Stock ot-rcp and zigbee each hold one.

### 2.2 ioctls (`ComIoctl_Ioctl`, `.ko` 0x29a0). The argument is a user pointer to a byte buffer.

| ioctl | Dir | Buffer |
|---|---|---|
| `0x40044703` (_IOW('G',3,4)) **write** | in | `[moduleId u8][commId u32 LE][len u16 LE][isNull u8 = 0][payload len]`. Header 8 bytes, payload <= **220** (else error). The kernel clears bits 0x80000000/0x40 of commId and ORs in this open's channel id, so send 0x80000040. |
| `0x80044704` (_IOR('G',4,4)) **read** | in/out | in: `[maxLen u16]` (lib passes 500, kernel clamps to 200). out: `[0][moduleId u8][0][commId u32][0][len u16][0][payload]`, where each 0 is an "isNull=0" flag byte (`readMsg_Output_buf2par`). |
| `0x40044701` / `0x80044702` | | older struct-based variants (12-byte header with a data pointer); not used by either client |

User-visible errno handling in the lib: ENODATA 61 = queue empty (stop polling), ENODEV 19 = driver gone, ENOMEM 12 =
"userspace allocation too small", ENOTTY 25 = invalid ioctl.

### 2.3 RPC framing inside the payload
- **Request** (client to server, to a module id): `[cmdId u8][arguments...]`.
  `gpCom_DataRequestAndWaitForAck(module, len, data, commId=0x80000040, ackBuf, timeout=10 000 000 us, ackId=0xFE)`
  takes a global mutex, sends, then polls reads until the module's indication handler has filled the ack buffer.
- **Ack** (server to client, same module id): `[0xFE][status u8][cmdId u8][outputs...]`. Status 0 ok, 1 unknown command,
  3 wrong length, 4 bad PD. The client asserts that `status==0` and the echoed cmd matches.
- **Indication** (server to client): `[indicationId u8][arguments...]`, dispatched by module id (`gpCom_ModuleIDCallbackTable`)
  and then by first byte. A first byte of 0xFE is an ack.
- Endianness: little-endian; u16/u32 are copied raw. Pointer arguments are `[isNull u8][struct]`, with the struct present
  only when isNull = 0.

## 3. gpMacDispatcher (module 0x72)

### 3.1 Server command table (`gpMacDispatcher_HandleCommandServer`, `.ko` 0x12070)
A jump table over cmd 1..166 (0x12104). Each case first checks the request length (cmd byte included); a wrong length is
status 3. `=N` means exactly N bytes, `>N` means more than N. **Command ids are identical in the public 2.8 client and the
2.7 server** for everything both have. 2.8 adds 133 SetAddressModeOverrideForBeacons, **157 GetCurrentTimeUs** and
**167 GetRadioSensitivity**; the kernel answers all three with status 1. The kernel has commands that the public lib does
not export: 60/61 Set/GetTxAntenna, **142 ScheduleTimedTx**, 143/144 Set/GetRegionalDomainSettings, 145/146
Set/GetAutoTxAntennaToggling.

| Cmd | Function | Len | Used by public radio_qorvo | Used by Amazon ot-rcp |
|---|---|---|---|---|
| 0x02 | RegisterNetworkStack | =7 | y | y |
| 0x03 | UnRegisterNetworkStack | =2 | | |
| 0x09 | Reset | =3 | y | y |
| 0x0C | DataRequest | >22 | y | y |
| 0x11 | ScanRequest | >8 (9) | y (ED) | y |
| 0x2E | SetPanId | =4 | y | y |
| 0x30 | SetShortAddress | =4 | y | y |
| 0x32 | SetExtendedAddress | =11 | y | y |
| 0x33 | GetExtendedAddress | =2 | y | y |
| 0x36 | SetCurrentChannel | =3 | y | y |
| 0x37 | GetCurrentChannel | =2 | y | y |
| 0x3E | SetRxOnWhenIdle | =3 | y | y |
| 0x41 | SetTransactionPersistenceTime | =4 | y | y |
| 0x48 | SetPromiscuousMode | =3 | y | y |
| 0x49 | GetPromiscuousMode | =2 | y | y |
| 0x4F | SetNumberOfRetries | =3 | y | y |
| 0x51 | SetMaxCsmaBackoffs | =3 | y | y |
| 0x53 | SetDefaultTransmitPowers | >17 | | y (region) |
| 0x54 | SetTransmitPower | =3 | y | y |
| 0x55 | GetTransmitPower | =2 | y | y |
| 0x5A | SetMinBE | =3 | | y |
| 0x60 | SetMacVersion | =3 | y | y |
| 0x66 | DataPending_QueueAdd | >13 (14) | y | y |
| 0x67 | DataPending_QueueRemove | >13 (14) | y | y |
| 0x69 | SetMaxBE | =3 | | y |
| 0x81 / 0x82 | Add/RemoveNeighbour | >13 | | y |
| 0x83 | SetDataPendingMode | =3 | y | y |
| 0x8B / 0x8C | Set/GetCsmaMode | =3 / =2 | | |
| 0x8D | DataPending_QueueClear | =2 | y | y |
| 0x8E | **ScheduleTimedTx** | >6 (7) | (not in public lib) | no |
| 0x8F | SetRegionalDomainSettings | >22 | | |
| 0x94 | SetStackInRawMode | =3 | y | y |
| 0x95 | GetStackInRawMode | =2 | | |
| 0x98 | **EnableEnhancedFramePending** | =3 | y | no |
| 0x9D | GetCurrentTimeUs | **not in kernel** | y (`qorvoRadioGetNow` <- `otPlatTimeGet`) | y (wrapper 0xe378; from 0xa7cc) |
| 0x9E | **ConfigureEnhAckProbing** | =14 | no (glue stub) | no |
| 0x9F | SetRetransmitOnCcaFail | =3 | y | y |
| 0xA1 | SetRetransmitRandomBackoff | =3 | y | y |
| 0xA3 / 0xA5 | Set Min/Max BeRetransmit | =3 | | |
| 0xA7 | GetRadioSensitivity | **not in kernel** | y (`qorvoRadioGetReceiveSensitivity`; otq/radio.c does not call it) | no |

The full lists, the driver's 113 server cases and the public client's 109, are the appendix below. Amazon's ot-rcp issues exactly 0x02 0x09 0x0C 0x11 0x2E 0x30 0x32 0x33 0x36 0x37 0x3E 0x41 0x48
0x49 0x4F 0x51 0x53 0x54 0x55 0x5A 0x60 0x66 0x67 0x69 0x81 0x82 0x83 0x8D 0x94 0x9D 0x9F 0xA1 to 0x72, and 2/3 to 0x81
(all calls to its gpCom_DataRequestAndWaitForAck at 0xb4b4).

### 3.2 Indication ids (server `MacDispatcher_cb*` = public `gpMacDispatcher_HandleIndicationClient` switch; identical)
13 DataConfirm, 15 DataIndication, 18 ScanConfirm, 21 AssociateIndication, 24 AssociateConfirm, 26 AssociateCommStatus,
29 PollConfirm, 31 PollIndication, 34 PurgeConfirm, 37 BeaconNotify, 39 OrphanIndication, 41 OrphanCommStatus, 44
SecurityFailureCommStatus, 98 DriverResetIndication, 127 PollNotify, 137 SecurityFrameCounterIndication.
An indication goes to the comm id of the client that registered that stack id (`gpMacDispatcher_StackMapping`, 3 stacks).

### 3.3 Encodings (identical on both sides; kernel `*_buf2api`/`*_par2api` vs public `*_par2buf`/`*_buf2api`)

Types (DWARF): `gpMacCore_Address_t` = union { u16 Short; MACAddress_t Extended[8] } (8 bytes);
`gpMacCore_AddressInfo_t` = { address @0, u16 panId @8, u8 addressMode @10 } (size 12), **wire = 8-byte address (always
8 bytes) + panId u16 + addressMode u8 = 11 bytes**; `gpMacCore_Security_t` = { u8 securityLevel, u8 keyIdMode,
u8 keySource[8], u8 keyIndex } = **11 bytes**; `gpMacCore_MultiChannelOptions_t` = u8 channel[3];
`gpMacCore_TxTimingOptions_t` = { u32 txTimestamp }; `gpPd_Loh_t` = { u16 length, u16 offset, u8 handle }, **wire =
`[len u16][len bytes of frame][clientPdHandle u8]`** (offset not sent; the kernel allocates its own PD and stores
client handle to server PD, `gpPd_StorePdHandle`); `StringIdentifier_t` = char[5].

| Message | Wire (after the cmd/ind byte) |
|---|---|
| RegisterNetworkStack(StringIdentifier*) | `[isNull][str 5]` ("OTHR" in both OT clients) -> ack `[stackId]` |
| Reset(Bool setDefaultPib, stackId) | `[setDefaultPib][stackId]` -> ack `[result]` |
| SetStackInRawMode(Bool, stackId) | `[1][stackId]` (both call it with 1 after Reset) |
| SetCurrentChannel(ch, stackId) / SetRxOnWhenIdle(b, stackId) / SetPromiscuousMode(m, stackId) / SetTransmitPower(int8, stackId) / SetNumberOfRetries / SetMaxCsmaBackoffs / SetMinBE / SetMaxBE / SetDataPendingMode(mode, stackId) / SetRetransmitOnCcaFail / SetRetransmitRandomBackoff / EnableEnhancedFramePending(Bool, stackId) / SetMacVersion(v, stackId) | `[u8 value][stackId]` |
| SetPanId(u16, stackId), SetShortAddress(u16, stackId) | `[u16 LE][stackId]` |
| SetExtendedAddress(MACAddress_t*, stackId) | `[isNull][8 bytes][stackId]`; GetExtendedAddress `[isNull][stackId]` -> ack `[8 bytes]` |
| SetTransactionPersistenceTime(u16, stackId) | `[u16][stackId]` |
| **DataRequest**(srcAddrMode, AddressInfo* dst, u8 txOptions, Security* sec, MultiChannelOptions mc, Loh pd, stackId) | `[srcAddrMode][isNull][AddressInfo 11][txOptions][isNull][Security 11][mc 3][len u16][frame len][pdHandle][stackId]`; ack only (result comes in DataConfirm) |
| **DataConfirm** (ind 13)(status, pdHandle, stackId) | `[status][pdHandle][txTimestamp u32][txRetryCntr u8][framePendingAfterTx u8][rxEnhancedAckFromTxPbm u8][stackId]` |
| **DataIndication** (ind 15)(src*, dst*, dsn, sec*, Loh, stackId) | `[isNull][AddressInfo 11][isNull][AddressInfo 11][dsn][isNull][Security 11][len u16][frame len][pdHandle][lqi u8][rssi i8][rxTimestamp u32][rxChannel u8][ackFrameControl u16][ackFrameCounter u32][ackKeyId u8][stackId]` (the last three: the ack *we* sent after this RX, `gpPd_*FromTxAckAfterRx`) |
| **ScanRequest**(scanType, u32 channels, duration, resultListSize, list*, stackId) | `[scanType][channels u32][scanDuration][resultListSize][stackId]` (9 incl. cmd; the list pointer is not sent). The public ED scan uses scanType 0, mask 1<<ch, size 1. |
| **ScanConfirm** (ind 18) | `[status][scanType][unscanned u32][resultListSize][isNull][resultListSize bytes][stackId]`; ED values are int8 dBm (public reads `ldrsb`) |
| DataPending_QueueAdd/Remove(AddressInfo*, stackId) | `[isNull][AddressInfo 11][stackId]` (14 incl. cmd) -> ack `[result]`; QueueClear `[stackId]` -> `[result]` |
| ScheduleTimedTx(pdHandle, TxTimingOptions, stackId) | `[pdHandle][txTimestamp u32][stackId]` -> ack `[result]` |
| ConfigureEnhAckProbing(linkMetrics, MACAddress_t*, u16 short, stackId) | `[linkMetrics][isNull][8 bytes][short u16][stackId]` -> ack `[result]` |
| DriverResetIndication (ind 98)(status, stackId) | `[status][stackId]` |

Frame conventions in raw mode (public glue = Amazon glue, field for field): TX `Loh.length = mLength - 2` (the chip adds
the FCS). Both send txOptions = `0x20 | ((txflags & 1) << 3)`, where txflags is the byte at otRadioFrame+0x1c. Security
level 5 is sent only when the frame's FCF security bit is set and `mIsSecurityProcessed` (bit 4) is clear. Retries and
backoffs are set per frame through SetNumberOfRetries/SetMaxCsmaBackoffs first. RX: `mLength = Loh.length + 2` (FCS bytes
zeroed).

kernel txOptions bits seen in `gpMacCore_DataRequest`: 0x04 (indirect), 0x10 (timed: stage for ScheduleTimedTx), 0x20
(set by both OT clients), 0x40, 0x08; the names are not confirmed.

**Timestamps:** the kernel converts chip time to host time before it sends: `ts = rawmono_us() - (chip_now - chip_ts)`
(`gpPd_DataIndication` 0x1a470 and its TX counterpart 0x1a15c). `gpOsal_TimeGetCurrent` = `getrawmonotonic64()` in
microseconds, truncated to u32. So **rxTimestamp and txTimestamp are CLOCK_MONOTONIC_RAW microseconds (mod 2^32)** and
user space can match them with `clock_gettime(CLOCK_MONOTONIC_RAW)`, without command 157.

## 4. Feasibility

**(a) Patch only the announced version: yes, with two small glue changes.**
1. 2.8.0.0 -> 2.7.0.0 for module 0x72 (one byte, §1.3). The server accepts only an exact match, and 2.7 is its only
   version.
2. Do not issue commands the 2.7 server lacks. `qorvoRadioGetNow` (reached from `otPlatTimeGet`, which the RCP uses)
   sends **157 GetCurrentTimeUs**; the kernel answers "unknown command" and the client asserts. Replace it with
   `CLOCK_MONOTONIC_RAW` microseconds, which match the kernel's RX/TX timestamps. Never call `qorvoRadioGetReceiveSensitivity`
   (167); otq/radio.c already returns a constant. 133 is not used by the radio glue.
   Amazon's own ot-rcp also contains a 157 call (0xe378), which suggests stock never relies on it at runtime either.
3. Everything else the public radio_qorvo sends is served under the same id and the same length. Messages of variable
   length that it uses were checked field by field: DataRequest with its AddressInfo/Security/MultiChannel/Loh parts,
   DataConfirm, DataIndication, ScanRequest/ScanConfirm, DataPending_QueueAdd/Remove, RegisterNetworkStack. The indication
   ids are identical. The transport, both ioctls and their framing are identical, and the probe already got as far as
   version negotiation.
   Residual risks: (i) a replaced 157/167 path missed somewhere; (ii) Qorvo's gpAssert handler (probably non-fatal, so it
   may keep running after an assert); (iii) no other open of `/dev/gp` running beside it (stock otservice / zigbeeservice
   stopped, as radio_watch does).

**(b) Our own client** (only if we want rid of Qorvo's library and its glibc/hard-float build): everything needed is
above.
- Transport: open, SIGIO, select, the two ioctls, a request/ack wait with timeout. About 150 lines.
- gpVersion: global exchange plus negotiation of 0x72 2.7.0.0. Optionally 0x02/0x6C 2.0.0.0, which OT does not need.
  About 60 lines.
- MacDispatcher calls for an OpenThread radio, about 20: RegisterNetworkStack, Reset, SetStackInRawMode, SetMacVersion,
  SetDataPendingMode, SetRetransmitOnCcaFail, SetRetransmitRandomBackoff, EnableEnhancedFramePending,
  SetTransactionPersistenceTime, SetPanId, SetShortAddress, Set/GetExtendedAddress, SetCurrentChannel, SetRxOnWhenIdle,
  SetPromiscuousMode, Set/GetTransmitPower, SetNumberOfRetries, SetMaxCsmaBackoffs, DataRequest, ScanRequest,
  DataPending_QueueAdd/Remove/Clear. Optionally SetDefaultTransmitPowers/SetRegionalDomainSettings for regional limits.
- Three indications (DataIndication, DataConfirm, ScanConfirm) plus DriverReset.
- In all, about 600 to 800 lines of C, with no PD pool needed (the client just numbers its handles).

## 5. CSL, enhanced ACK, timed TX in the kernel

- **No CSL.** There are no Csl/CSL symbols. The only trace is the parameter `pCslIeOffset` of
  `MacCore_GetRawModeEncryptionOffsets` (0xc64c), which scans header IEs for id 0x1A (the CSL IE). Nothing in the module
  calls it. There is no receive-at or sample-window scheduling and no IE generation in ACKs. Qorvo's public glue stubs
  `qorvoRadioEnableCsl`, `UpdateCslSampleTime`, `ReceiveAt` and `ConfigureEnhAckProbing` without any RPC.
- **ConfigureEnhAckProbing (0x9E)** is marshalled, but `gpMacCore_ConfigureEnhAckProbing_STACKID` (0xc8bc) is
  `mov r0,#0xE8; bx lr`, so it always fails. There is no link-metrics probing.
- **No enhanced ACK at all** (corrected 2026-11-27, after the second radio's test): EnableEnhancedFramePending (0x98)
  is marshalled, but `gpHal_MacEnableEnhancedFramePending` (0x2d174) is `bx lr`; the fields DataIndication and
  DataConfirm carry for one are stubs: `gpHal_GetRxEnhancedAckFromTxPbm` (0x2a958) returns 0,
  `gpHal_GetFrameControlFromTxAckAfterRx` (0x2a960) 0xFF, `..FrameCounter..` (0x2a968) 0xFFFFFFFF, `..KeyId..`
  (0x2a970) 0xFF.  Frames of version 2 (2015) never get past the chip's address filter (MACFILT, 0x6a00-0x6a37; names
  from Qorvo's QMatter k8e sources, whose block lines up): `gpHal_Init` sets 0x6a30 bit 0 (2003, +0x160) and bit 1
  (2006, +0x170), never bit 2 (2015); promiscuous (`gpHal_SetPromiscuousMode` 0x2cbd8) sets 0x6a2d bit 1, which
  skips the filter, and turns auto-ACK off (0x40e bit 0).  The driver's software takes version 2 frames
  (`MacCore_AnalyseMacHeader` 0x55c8).  `gpMacCore_SetMacVersion_STACKID` (0x7b70) asserts on anything but 0 or 1
  (line 738) and the scheduler restarts the driver's thread (`Sched_HandleAssert` 0x1d760): "Driver reset detected";
  the value is only read for headers the MAC builds itself.  Setting 0x6a30 bit 2 (one instruction in `gpHal_Init`,
  .text+0x31aac `mov r2,#2` -> `#6`) would let 2015 frames in, but the chip would answer them with an Imm-Ack: k8e
  keeps its 2015 and CSL-IE switches in a 16-bit register (0xc26c4 bits 11, 10) where this chip's 0x6a2d is one byte.
  So no Enh-Ack, no CSL, no link metrics on the GP712 with this driver (not tried: the patch buys nothing a Thread
  1.2 device needs).
- **ScheduleTimedTx (0x8E)**: a DataRequest with txOptions bit 0x10 stages the frame (`gpMacCore_GlobalVars+0x90 = 1`,
  pd at +0x8f) instead of sending it. ScheduleTimedTx(pd, {txTimestamp}) then arms `gpHal_ScheduleAbsoluteEvent` at
  txTimestamp, **in chip time, unconverted** (chip clock = register 0x586 x 2 us). No 2.7 RPC returns chip time (157 is
  missing), so the host cannot place an absolute TX time reliably. With it missing, transmit timing (and so a CSL
  transmitter) is not practical, which fits stock ot-rcp not claiming the TX_TIMING capability.
- TX security in raw mode: the kernel has `MacCore_PrepareOutgoingSecuredPacket` and MAC PIB key tables (SetKeyDescriptor
  etc.). Hardware security of raw frames might work with keys loaded into those tables. This is untested, and stock does
  not claim TX_SEC.

Confidence: high (about 85%) that the version byte plus avoiding 157/167 gives a working radio with the public lib. The
command ids, lengths and encodings are firm, read straight from symbol-named code on both sides. The timestamp base
(CLOCK_MONOTONIC_RAW microseconds) is firm. The chip-time reading of ScheduleTimedTx and the names of the txOptions bits
are medium confidence.

## Appendix: the command tables

### The driver's gpMacDispatcher server (gp712_mac.ko 2.10.3.2, API 2.7)

```
  1 0x01 len beq 1  gpMacDispatcher_Init
  2 0x02 len beq 7  gpMacDispatcher_RegisterNetworkStack_Input_buf2api gpMacDispatcher_RegisterNetworkStack gpMacDispatcher_RegisterCallbacks
  3 0x03 len beq 2  gpMacDispatcher_UnRegisterNetworkStack_Input_buf2api gpMacDispatcher_UnRegisterNetworkStack gpMacDispatcher_UnRegisterNetworkStack_Output_api2buf
  5 0x05 len beq 2  gpMacDispatcher_LockClaim_Input_buf2api gpMacDispatcher_LockClaim gpMacDispatcher_LockClaim_Output_api2buf
  6 0x06 len beq 2  gpMacDispatcher_LockRelease_Input_buf2api gpMacDispatcher_LockRelease
  7 0x07 len beq 2  gpMacDispatcher_LockedByThisStack_Input_buf2api gpMacDispatcher_LockedByThisStack gpMacDispatcher_LockedByThisStack_Output_api2buf
  8 0x08 len beq 1  gpMacDispatcher_Locked gpMacDispatcher_Locked_Output_api2buf
  9 0x09 len beq 3  gpMacDispatcher_Reset_Input_buf2api gpMacDispatcher_Reset gpMacDispatcher_Reset_Output_api2buf
 12 0x0c len bhi 22  gpMacDispatcher_DataRequest_Input_buf2api gpMacDispatcher_DataRequest
 17 0x11 len bhi 8  gpMacDispatcher_ScanRequest_Input_buf2api gpMacDispatcher_ScanRequest
 20 0x14 len bhi 4  gpMacDispatcher_AssociateRequest_Input_buf2api gpMacDispatcher_AssociateRequest
 23 0x17 len beq 14  gpMacDispatcher_AssociateResponse_Input_buf2api gpMacDispatcher_AssociateResponse
 28 0x1c len bhi 25  gpMacDispatcher_PollRequest_Input_buf2api gpMacDispatcher_PollRequest
 33 0x21 len bhi 2  gpMacDispatcher_PurgeRequest_Input_buf2api gpMacDispatcher_PurgeRequest
 36 0x24 len beq 6  gpMacDispatcher_Start_Input_buf2api gpMacDispatcher_Start gpMacDispatcher_Start_Output_api2buf
 43 0x2b len beq 14  gpMacDispatcher_OrphanResponse_Input_buf2api gpMacDispatcher_OrphanResponse
 46 0x2e len beq 4  gpMacDispatcher_SetPanId_Input_buf2api gpMacDispatcher_SetPanId
 47 0x2f len beq 2  gpMacDispatcher_GetPanId_Input_buf2api gpMacDispatcher_GetPanId gpMacDispatcher_GetPanId_Output_api2buf
 48 0x30 len beq 4  gpMacDispatcher_SetShortAddress_Input_buf2api gpMacDispatcher_SetShortAddress
 49 0x31 len beq 2  gpMacDispatcher_GetShortAddress_Input_buf2api gpMacDispatcher_GetShortAddress gpMacDispatcher_GetShortAddress_Output_api2buf
 50 0x32 len beq 11  gpMacDispatcher_SetExtendedAddress_Input_buf2api gpMacDispatcher_SetExtendedAddress
 51 0x33 len beq 2  gpMacDispatcher_GetExtendedAddress_Input_buf2api gpMacDispatcher_GetExtendedAddress gpMacDispatcher_GetExtendedAddress_Output_api2buf
 52 0x34 len beq 3  gpMacDispatcher_SetDsn_Input_buf2api gpMacDispatcher_SetDsn
 53 0x35 len beq 2  gpMacDispatcher_GetDsn_Input_buf2api gpMacDispatcher_GetDsn gpMacDispatcher_GetDsn_Output_api2buf
 54 0x36 len beq 3  gpMacDispatcher_SetCurrentChannel_Input_buf2api gpMacDispatcher_SetCurrentChannel
 55 0x37 len beq 2  gpMacDispatcher_GetCurrentChannel_Input_buf2api gpMacDispatcher_GetCurrentChannel gpMacDispatcher_GetCurrentChannel_Output_api2buf
 56 0x38 len bhi 2  gpMacDispatcher_SetBeaconPayload_Input_buf2api gpMacDispatcher_SetBeaconPayload
 57 0x39 len >= 1  gpMacDispatcher_GetBeaconPayload_Input_buf2api gpMacDispatcher_GetBeaconPayload gpMacDispatcher_GetBeaconPayload_Output_api2buf
 58 0x3a len beq 3  gpMacDispatcher_SetBeaconPayloadLength_Input_buf2api gpMacDispatcher_SetBeaconPayloadLength
 59 0x3b len beq 2  gpMacDispatcher_GetBeaconPayloadLength_Input_buf2api gpMacDispatcher_GetBeaconPayloadLength gpMacDispatcher_GetBeaconPayloadLength_Output_api2buf
 60 0x3c len beq 3  gpMacDispatcher_SetTxAntenna_Input_buf2api gpMacDispatcher_SetTxAntenna
 61 0x3d len beq 2  gpMacDispatcher_GetTxAntenna_Input_buf2api gpMacDispatcher_GetTxAntenna gpMacDispatcher_GetTxAntenna_Output_api2buf
 62 0x3e len beq 3  gpMacDispatcher_SetRxOnWhenIdle_Input_buf2api gpMacDispatcher_SetRxOnWhenIdle
 63 0x3f len beq 2  gpMacDispatcher_GetRxOnWhenIdle_Input_buf2api gpMacDispatcher_GetRxOnWhenIdle gpMacDispatcher_GetRxOnWhenIdle_Output_api2buf
 64 0x40 len beq 2  gpMacDispatcher_GetTransactionPersistenceTime_Input_buf2api gpMacDispatcher_GetTransactionPersistenceTime gpMacDispatcher_GetTransactionPersistenceTime_Output_api2buf
 65 0x41 len beq 4  gpMacDispatcher_SetTransactionPersistenceTime_Input_buf2api gpMacDispatcher_SetTransactionPersistenceTime
 66 0x42 len beq 3  gpMacDispatcher_SetBeaconStarted_Input_buf2api gpMacDispatcher_SetBeaconStarted
 67 0x43 len beq 2  gpMacDispatcher_GetBeaconStarted_Input_buf2api gpMacDispatcher_GetBeaconStarted gpMacDispatcher_GetBeaconStarted_Output_api2buf
 68 0x44 len beq 4  gpMacDispatcher_SetCoordShortAddress_Input_buf2api gpMacDispatcher_SetCoordShortAddress
 69 0x45 len beq 2  gpMacDispatcher_GetCoordShortAddress_Input_buf2api gpMacDispatcher_GetCoordShortAddress gpMacDispatcher_GetCoordShortAddress_Output_api2buf
 70 0x46 len beq 11  gpMacDispatcher_SetCoordExtendedAddress_Input_buf2api gpMacDispatcher_SetCoordExtendedAddress
 71 0x47 len beq 2  gpMacDispatcher_GetCoordExtendedAddress_Input_buf2api gpMacDispatcher_GetCoordExtendedAddress gpMacDispatcher_GetCoordExtendedAddress_Output_api2buf
 72 0x48 len beq 3  gpMacDispatcher_SetPromiscuousMode_Input_buf2api gpMacDispatcher_SetPromiscuousMode
 73 0x49 len beq 2  gpMacDispatcher_GetPromiscuousMode_Input_buf2api gpMacDispatcher_GetPromiscuousMode gpMacDispatcher_GetPromiscuousMode_Output_api2buf
 74 0x4a len beq 3  gpMacDispatcher_SetAssociationPermit_Input_buf2api gpMacDispatcher_SetAssociationPermit
 75 0x4b len beq 2  gpMacDispatcher_GetAssociationPermit_Input_buf2api gpMacDispatcher_GetAssociationPermit gpMacDispatcher_GetAssociationPermit_Output_api2buf
 76 0x4c len beq 3  gpMacDispatcher_SetSecurityEnabled_Input_buf2api gpMacDispatcher_SetSecurityEnabled
 77 0x4d len beq 2  gpMacDispatcher_GetSecurityEnabled_Input_buf2api gpMacDispatcher_GetSecurityEnabled gpMacDispatcher_GetSecurityEnabled_Output_api2buf
 79 0x4f len beq 3  gpMacDispatcher_SetNumberOfRetries_Input_buf2api gpMacDispatcher_SetNumberOfRetries
 80 0x50 len beq 2  gpMacDispatcher_GetNumberOfRetries_Input_buf2api gpMacDispatcher_GetNumberOfRetries gpMacDispatcher_GetNumberOfRetries_Output_api2buf
 81 0x51 len beq 3  gpMacDispatcher_SetMaxCsmaBackoffs_Input_buf2api gpMacDispatcher_SetMaxCsmaBackoffs
 82 0x52 len beq 2  gpMacDispatcher_GetMaxCsmaBackoffs_Input_buf2api gpMacDispatcher_GetMaxCsmaBackoffs gpMacDispatcher_GetMaxCsmaBackoffs_Output_api2buf
 83 0x53 len bhi 17  gpMacDispatcher_SetDefaultTransmitPowers_Input_buf2api gpMacDispatcher_SetDefaultTransmitPowers
 84 0x54 len beq 3  gpMacDispatcher_SetTransmitPower_Input_buf2api gpMacDispatcher_SetTransmitPower
 85 0x55 len beq 2  gpMacDispatcher_GetTransmitPower_Input_buf2api gpMacDispatcher_GetTransmitPower gpMacDispatcher_GetTransmitPower_Output_api2buf
 86 0x56 len beq 3  gpMacDispatcher_SetCCAMode_Input_buf2api gpMacDispatcher_SetCCAMode
 87 0x57 len beq 2  gpMacDispatcher_GetCCAMode_Input_buf2api gpMacDispatcher_GetCCAMode gpMacDispatcher_GetCCAMode_Output_api2buf
 88 0x58 len beq 3  gpMacDispatcher_SetPanCoordinator_Input_buf2api gpMacDispatcher_SetPanCoordinator
 89 0x59 len beq 2  gpMacDispatcher_GetPanCoordinator_Input_buf2api gpMacDispatcher_GetPanCoordinator gpMacDispatcher_GetPanCoordinator_Output_api2buf
 90 0x5a len beq 3  gpMacDispatcher_SetMinBE_Input_buf2api gpMacDispatcher_SetMinBE
 91 0x5b len beq 2  gpMacDispatcher_GetMinBE_Input_buf2api gpMacDispatcher_GetMinBE gpMacDispatcher_GetMinBE_Output_api2buf
 92 0x5c len beq 3  gpMacDispatcher_SetIndicateBeaconNotifications_Input_buf2api gpMacDispatcher_SetIndicateBeaconNotifications
 93 0x5d len beq 2  gpMacDispatcher_GetIndicateBeaconNotifications_Input_buf2api gpMacDispatcher_GetIndicateBeaconNotifications gpMacDispatcher_GetIndicateBeaconNotifications_Output_api2buf
 96 0x60 len beq 3  gpMacDispatcher_SetMacVersion_Input_buf2api gpMacDispatcher_SetMacVersion
 97 0x61 len beq 2  gpMacDispatcher_GetMacVersion_Input_buf2api gpMacDispatcher_GetMacVersion gpMacDispatcher_GetMacVersion_Output_api2buf
100 0x64 len beq 3  gpMacDispatcher_SetForwardPollIndications_Input_buf2api gpMacDispatcher_SetForwardPollIndications
101 0x65 len beq 2  gpMacDispatcher_GetForwardPollIndications_Input_buf2api gpMacDispatcher_GetForwardPollIndications gpMacDispatcher_GetForwardPollIndications_Output_api2buf
102 0x66 len bhi 13  gpMacDispatcher_DataPending_QueueAdd_Input_buf2api gpMacDispatcher_DataPending_QueueAdd gpMacDispatcher_DataPending_QueueAdd_Output_api2buf
103 0x67 len bhi 13  gpMacDispatcher_DataPending_QueueRemove_Input_buf2api gpMacDispatcher_DataPending_QueueRemove gpMacDispatcher_DataPending_QueueRemove_Output_api2buf
104 0x68 len beq 3  gpMacDispatcher_SetMinInterferenceLevels_Input_buf2api gpMacDispatcher_SetMinInterferenceLevels
105 0x69 len beq 3  gpMacDispatcher_SetMaxBE_Input_buf2api gpMacDispatcher_SetMaxBE
106 0x6a len beq 2  gpMacDispatcher_GetMaxBE_Input_buf2api gpMacDispatcher_GetMaxBE gpMacDispatcher_GetMaxBE_Output_api2buf
110 0x6e len beq 21  gpMacDispatcher_SetDeviceDescriptor_Input_buf2api gpMacDispatcher_SetDeviceDescriptor gpMacDispatcher_SetDeviceDescriptor_Output_api2buf
111 0x6f len beq 3  gpMacDispatcher_GetDeviceDescriptor_Input_buf2api gpMacDispatcher_GetDeviceDescriptor gpMacDispatcher_GetDeviceDescriptor_Output_api2buf
112 0x70 len beq 3  gpMacDispatcher_SetDeviceTableEntries_Input_buf2api gpMacDispatcher_SetDeviceTableEntries
113 0x71 len beq 2  gpMacDispatcher_GetDeviceTableEntries_Input_buf2api gpMacDispatcher_GetDeviceTableEntries gpMacDispatcher_GetDeviceTableEntries_Output_api2buf
114 0x72 len bhi 3  gpMacDispatcher_SetKeyDescriptor_Input_buf2api gpMacDispatcher_SetKeyDescriptor gpMacDispatcher_SetKeyDescriptor_Output_api2buf
115 0x73 len beq 3  gpMacDispatcher_GetKeyDescriptor_Input_buf2api gpMacDispatcher_GetKeyDescriptor gpMacDispatcher_GetKeyDescriptor_Output_api2buf
116 0x74 len beq 3  gpMacDispatcher_SetKeyTableEntries_Input_buf2api gpMacDispatcher_SetKeyTableEntries
117 0x75 len beq 8  gpMacDispatcher_SetSecurityLevelDescriptor_Input_buf2api gpMacDispatcher_SetSecurityLevelDescriptor gpMacDispatcher_SetSecurityLevelDescriptor_Output_api2buf
118 0x76 len beq 3  gpMacDispatcher_GetSecurityLevelDescriptor_Input_buf2api gpMacDispatcher_GetSecurityLevelDescriptor gpMacDispatcher_GetSecurityLevelDescriptor_Output_api2buf
119 0x77 len beq 3  gpMacDispatcher_SetSecurityLevelTableEntries_Input_buf2api gpMacDispatcher_SetSecurityLevelTableEntries
120 0x78 len beq 2  gpMacDispatcher_GetSecurityLevelTableEntries_Input_buf2api gpMacDispatcher_GetSecurityLevelTableEntries gpMacDispatcher_GetSecurityLevelTableEntries_Output_api2buf
121 0x79 len bhi 10  gpMacDispatcher_SetDefaultKeySource_Input_buf2api gpMacDispatcher_SetDefaultKeySource
122 0x7a len beq 6  gpMacDispatcher_SetFrameCounter_Input_buf2api gpMacDispatcher_SetFrameCounter
123 0x7b len beq 2  gpMacDispatcher_GetFrameCounter_Input_buf2api gpMacDispatcher_GetFrameCounter gpMacDispatcher_GetFrameCounter_Output_api2buf
124 0x7c len beq 4  gpMacDispatcher_SetPanCoordShortAddress_Input_buf2api gpMacDispatcher_SetPanCoordShortAddress
125 0x7d len beq 11  gpMacDispatcher_SetPanCoordExtendedAddress_Input_buf2api gpMacDispatcher_SetPanCoordExtendedAddress
126 0x7e len beq 2  gpMacDispatcher_GetPanCoordExtendedAddress_Input_buf2api gpMacDispatcher_GetPanCoordExtendedAddress gpMacDispatcher_GetPanCoordExtendedAddress_Output_api2buf
129 0x81 len bhi 13  gpMacDispatcher_AddNeighbour_Input_buf2api gpMacDispatcher_AddNeighbour gpMacDispatcher_AddNeighbour_Output_api2buf
130 0x82 len bhi 13  gpMacDispatcher_RemoveNeighbour_Input_buf2api gpMacDispatcher_RemoveNeighbour gpMacDispatcher_RemoveNeighbour_Output_api2buf
131 0x83 len beq 3  gpMacDispatcher_SetDataPendingMode_Input_buf2api gpMacDispatcher_SetDataPendingMode gpMacDispatcher_SetDataPendingMode_Output_api2buf
134 0x86 len beq 2  gpMacDispatcher_ClearNeighbours_Input_buf2api gpMacDispatcher_ClearNeighbours
139 0x8b len beq 3  gpMacDispatcher_SetCsmaMode_Input_buf2api gpMacDispatcher_SetCsmaMode
140 0x8c len beq 2  gpMacDispatcher_GetCsmaMode_Input_buf2api gpMacDispatcher_GetCsmaMode gpMacDispatcher_GetCsmaMode_Output_api2buf
141 0x8d len beq 2  gpMacDispatcher_DataPending_QueueClear_Input_buf2api gpMacDispatcher_DataPending_QueueClear gpMacDispatcher_DataPending_QueueClear_Output_api2buf
142 0x8e len bhi 6  gpMacDispatcher_ScheduleTimedTx_Input_buf2api gpMacDispatcher_ScheduleTimedTx gpMacDispatcher_ScheduleTimedTx_Output_api2buf
143 0x8f len bhi 22  gpMacDispatcher_SetRegionalDomainSettings_Input_buf2api gpMacDispatcher_SetRegionalDomainSettings
144 0x90 len >= 1  gpMacDispatcher_GetRegionalDomainSettings_Input_buf2api gpMacDispatcher_GetRegionalDomainSettings gpMacDispatcher_GetRegionalDomainSettings_Output_api2buf
145 0x91 len beq 3  gpMacDispatcher_SetAutoTxAntennaToggling_Input_buf2api gpMacDispatcher_SetAutoTxAntennaToggling
146 0x92 len beq 2  gpMacDispatcher_GetAutoTxAntennaToggling_Input_buf2api gpMacDispatcher_GetAutoTxAntennaToggling gpMacDispatcher_GetAutoTxAntennaToggling_Output_api2buf
148 0x94 len beq 3  gpMacDispatcher_SetStackInRawMode_Input_buf2api gpMacDispatcher_SetStackInRawMode
149 0x95 len beq 2  gpMacDispatcher_GetStackInRawMode_Input_buf2api gpMacDispatcher_GetStackInRawMode gpMacDispatcher_GetStackInRawMode_Output_api2buf
152 0x98 len beq 3  gpMacDispatcher_EnableEnhancedFramePending_Input_buf2api gpMacDispatcher_EnableEnhancedFramePending
158 0x9e len beq 14  gpMacDispatcher_ConfigureEnhAckProbing_Input_buf2api gpMacDispatcher_ConfigureEnhAckProbing gpMacDispatcher_ConfigureEnhAckProbing_Output_api2buf
159 0x9f len beq 3  gpMacDispatcher_SetRetransmitOnCcaFail_Input_buf2api gpMacDispatcher_SetRetransmitOnCcaFail
160 0xa0 len beq 2  gpMacDispatcher_GetRetransmitOnCcaFail_Input_buf2api gpMacDispatcher_GetRetransmitOnCcaFail gpMacDispatcher_GetRetransmitOnCcaFail_Output_api2buf
161 0xa1 len beq 3  gpMacDispatcher_SetRetransmitRandomBackoff_Input_buf2api gpMacDispatcher_SetRetransmitRandomBackoff
162 0xa2 len beq 2  gpMacDispatcher_GetRetransmitRandomBackoff_Input_buf2api gpMacDispatcher_GetRetransmitRandomBackoff gpMacDispatcher_GetRetransmitRandomBackoff_Output_api2buf
163 0xa3 len beq 3  gpMacDispatcher_SetMinBeRetransmit_Input_buf2api gpMacDispatcher_SetMinBeRetransmit
164 0xa4 len beq 2  gpMacDispatcher_GetMinBeRetransmit_Input_buf2api gpMacDispatcher_GetMinBeRetransmit gpMacDispatcher_GetMinBeRetransmit_Output_api2buf
165 0xa5 len beq 3  gpMacDispatcher_SetMaxBeRetransmit_Input_buf2api gpMacDispatcher_SetMaxBeRetransmit
166 0xa6 len beq 2  gpMacDispatcher_GetMaxBeRetransmit_Input_buf2api gpMacDispatcher_GetMaxBeRetransmit gpMacDispatcher_GetMaxBeRetransmit_Output_api2buf
```

### Qorvo's public client (libQorvoGP712_ftd.a, API 2.8)

```
 -1 0x-1 gpMacDispatcher_Init (strb None) gpCom_RegisterModule
  2 0x02 gpMacDispatcher_RegisterNetworkStack  Input_par2buf gpCom_DataRequestAndWaitForAck
  3 0x03 gpMacDispatcher_UnRegisterNetworkStack  Input_par2buf gpCom_DataRequestAndWaitForAck
  5 0x05 gpMacDispatcher_LockClaim  Input_par2buf gpCom_DataRequestAndWaitForAck
  6 0x06 gpMacDispatcher_LockRelease  Input_par2buf gpCom_DataRequestAndWaitForAck
  7 0x07 gpMacDispatcher_LockedByThisStack  Input_par2buf gpCom_DataRequestAndWaitForAck
  8 0x08 gpMacDispatcher_Locked  gpCom_DataRequestAndWaitForAck
  9 0x09 gpMacDispatcher_Reset  Input_par2buf gpCom_DataRequestAndWaitForAck
 12 0x0c gpMacDispatcher_DataRequest  Input_par2buf gpCom_DataRequestAndWaitForAck
 17 0x11 gpMacDispatcher_ScanRequest  Input_par2buf gpCom_DataRequestAndWaitForAck
 20 0x14 gpMacDispatcher_AssociateRequest  Input_par2buf gpCom_DataRequestAndWaitForAck
 23 0x17 gpMacDispatcher_AssociateResponse  Input_par2buf gpCom_DataRequestAndWaitForAck
 28 0x1c gpMacDispatcher_PollRequest  Input_par2buf gpCom_DataRequestAndWaitForAck
 33 0x21 gpMacDispatcher_PurgeRequest  Input_par2buf gpCom_DataRequestAndWaitForAck
 36 0x24 gpMacDispatcher_Start  Input_par2buf gpCom_DataRequestAndWaitForAck
 43 0x2b gpMacDispatcher_OrphanResponse  Input_par2buf gpCom_DataRequestAndWaitForAck
 46 0x2e gpMacDispatcher_SetPanId  Input_par2buf gpCom_DataRequestAndWaitForAck
 47 0x2f gpMacDispatcher_GetPanId  Input_par2buf gpCom_DataRequestAndWaitForAck
 48 0x30 gpMacDispatcher_SetShortAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 49 0x31 gpMacDispatcher_GetShortAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 50 0x32 gpMacDispatcher_SetExtendedAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 51 0x33 gpMacDispatcher_GetExtendedAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 52 0x34 gpMacDispatcher_SetDsn  Input_par2buf gpCom_DataRequestAndWaitForAck
 53 0x35 gpMacDispatcher_GetDsn  Input_par2buf gpCom_DataRequestAndWaitForAck
 54 0x36 gpMacDispatcher_SetCurrentChannel  Input_par2buf gpCom_DataRequestAndWaitForAck
 55 0x37 gpMacDispatcher_GetCurrentChannel  Input_par2buf gpCom_DataRequestAndWaitForAck
 56 0x38 gpMacDispatcher_SetBeaconPayload  Input_par2buf gpCom_DataRequestAndWaitForAck
 57 0x39 gpMacDispatcher_GetBeaconPayload  Input_par2buf gpCom_DataRequestAndWaitForAck
 58 0x3a gpMacDispatcher_SetBeaconPayloadLength  Input_par2buf gpCom_DataRequestAndWaitForAck
 59 0x3b gpMacDispatcher_GetBeaconPayloadLength  Input_par2buf gpCom_DataRequestAndWaitForAck
 62 0x3e gpMacDispatcher_SetRxOnWhenIdle  Input_par2buf gpCom_DataRequestAndWaitForAck
 63 0x3f gpMacDispatcher_GetRxOnWhenIdle  Input_par2buf gpCom_DataRequestAndWaitForAck
 64 0x40 gpMacDispatcher_GetTransactionPersistenceTime  Input_par2buf gpCom_DataRequestAndWaitForAck
 65 0x41 gpMacDispatcher_SetTransactionPersistenceTime  Input_par2buf gpCom_DataRequestAndWaitForAck
 66 0x42 gpMacDispatcher_SetBeaconStarted  Input_par2buf gpCom_DataRequestAndWaitForAck
 67 0x43 gpMacDispatcher_GetBeaconStarted  Input_par2buf gpCom_DataRequestAndWaitForAck
 68 0x44 gpMacDispatcher_SetCoordShortAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 69 0x45 gpMacDispatcher_GetCoordShortAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 70 0x46 gpMacDispatcher_SetCoordExtendedAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 71 0x47 gpMacDispatcher_GetCoordExtendedAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
 72 0x48 gpMacDispatcher_SetPromiscuousMode  Input_par2buf gpCom_DataRequestAndWaitForAck
 73 0x49 gpMacDispatcher_GetPromiscuousMode  Input_par2buf gpCom_DataRequestAndWaitForAck
 74 0x4a gpMacDispatcher_SetAssociationPermit  Input_par2buf gpCom_DataRequestAndWaitForAck
 75 0x4b gpMacDispatcher_GetAssociationPermit  Input_par2buf gpCom_DataRequestAndWaitForAck
 76 0x4c gpMacDispatcher_SetSecurityEnabled  Input_par2buf gpCom_DataRequestAndWaitForAck
 77 0x4d gpMacDispatcher_GetSecurityEnabled  Input_par2buf gpCom_DataRequestAndWaitForAck
 79 0x4f gpMacDispatcher_SetNumberOfRetries  Input_par2buf gpCom_DataRequestAndWaitForAck
 80 0x50 gpMacDispatcher_GetNumberOfRetries  Input_par2buf gpCom_DataRequestAndWaitForAck
 81 0x51 gpMacDispatcher_SetMaxCsmaBackoffs  Input_par2buf gpCom_DataRequestAndWaitForAck
 82 0x52 gpMacDispatcher_GetMaxCsmaBackoffs  Input_par2buf gpCom_DataRequestAndWaitForAck
 83 0x53 gpMacDispatcher_SetDefaultTransmitPowers  Input_par2buf gpCom_DataRequestAndWaitForAck
 84 0x54 gpMacDispatcher_SetTransmitPower  Input_par2buf gpCom_DataRequestAndWaitForAck
 85 0x55 gpMacDispatcher_GetTransmitPower  Input_par2buf gpCom_DataRequestAndWaitForAck
 86 0x56 gpMacDispatcher_SetCCAMode  Input_par2buf gpCom_DataRequestAndWaitForAck
 87 0x57 gpMacDispatcher_GetCCAMode  Input_par2buf gpCom_DataRequestAndWaitForAck
 88 0x58 gpMacDispatcher_SetPanCoordinator  Input_par2buf gpCom_DataRequestAndWaitForAck
 89 0x59 gpMacDispatcher_GetPanCoordinator  Input_par2buf gpCom_DataRequestAndWaitForAck
 90 0x5a gpMacDispatcher_SetMinBE  Input_par2buf gpCom_DataRequestAndWaitForAck
 91 0x5b gpMacDispatcher_GetMinBE  Input_par2buf gpCom_DataRequestAndWaitForAck
 92 0x5c gpMacDispatcher_SetIndicateBeaconNotifications  Input_par2buf gpCom_DataRequestAndWaitForAck
 93 0x5d gpMacDispatcher_GetIndicateBeaconNotifications  Input_par2buf gpCom_DataRequestAndWaitForAck
 96 0x60 gpMacDispatcher_SetMacVersion  Input_par2buf gpCom_DataRequestAndWaitForAck
 97 0x61 gpMacDispatcher_GetMacVersion  Input_par2buf gpCom_DataRequestAndWaitForAck
100 0x64 gpMacDispatcher_SetForwardPollIndications  Input_par2buf gpCom_DataRequestAndWaitForAck
101 0x65 gpMacDispatcher_GetForwardPollIndications  Input_par2buf gpCom_DataRequestAndWaitForAck
102 0x66 gpMacDispatcher_DataPending_QueueAdd  Input_par2buf gpCom_DataRequestAndWaitForAck
103 0x67 gpMacDispatcher_DataPending_QueueRemove  Input_par2buf gpCom_DataRequestAndWaitForAck
104 0x68 gpMacDispatcher_SetMinInterferenceLevels  Input_par2buf gpCom_DataRequestAndWaitForAck
105 0x69 gpMacDispatcher_SetMaxBE  Input_par2buf gpCom_DataRequestAndWaitForAck
106 0x6a gpMacDispatcher_GetMaxBE  Input_par2buf gpCom_DataRequestAndWaitForAck
110 0x6e gpMacDispatcher_SetDeviceDescriptor  Input_par2buf gpCom_DataRequestAndWaitForAck
111 0x6f gpMacDispatcher_GetDeviceDescriptor  Input_par2buf gpCom_DataRequestAndWaitForAck
112 0x70 gpMacDispatcher_SetDeviceTableEntries  Input_par2buf gpCom_DataRequestAndWaitForAck
113 0x71 gpMacDispatcher_GetDeviceTableEntries  Input_par2buf gpCom_DataRequestAndWaitForAck
114 0x72 gpMacDispatcher_SetKeyDescriptor  Input_par2buf gpCom_DataRequestAndWaitForAck
115 0x73 gpMacDispatcher_GetKeyDescriptor  Input_par2buf gpCom_DataRequestAndWaitForAck
116 0x74 gpMacDispatcher_SetKeyTableEntries  Input_par2buf gpCom_DataRequestAndWaitForAck
117 0x75 gpMacDispatcher_SetSecurityLevelDescriptor  Input_par2buf gpCom_DataRequestAndWaitForAck
118 0x76 gpMacDispatcher_GetSecurityLevelDescriptor  Input_par2buf gpCom_DataRequestAndWaitForAck
119 0x77 gpMacDispatcher_SetSecurityLevelTableEntries  Input_par2buf gpCom_DataRequestAndWaitForAck
120 0x78 gpMacDispatcher_GetSecurityLevelTableEntries  Input_par2buf gpCom_DataRequestAndWaitForAck
121 0x79 gpMacDispatcher_SetDefaultKeySource  Input_par2buf gpCom_DataRequestAndWaitForAck
122 0x7a gpMacDispatcher_SetFrameCounter  Input_par2buf gpCom_DataRequestAndWaitForAck
123 0x7b gpMacDispatcher_GetFrameCounter  Input_par2buf gpCom_DataRequestAndWaitForAck
124 0x7c gpMacDispatcher_SetPanCoordShortAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
125 0x7d gpMacDispatcher_SetPanCoordExtendedAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
126 0x7e gpMacDispatcher_GetPanCoordExtendedAddress  Input_par2buf gpCom_DataRequestAndWaitForAck
129 0x81 gpMacDispatcher_AddNeighbour  Input_par2buf gpCom_DataRequestAndWaitForAck
130 0x82 gpMacDispatcher_RemoveNeighbour  Input_par2buf gpCom_DataRequestAndWaitForAck
131 0x83 gpMacDispatcher_SetDataPendingMode  Input_par2buf gpCom_DataRequestAndWaitForAck
133 0x85 gpMacDispatcher_SetAddressModeOverrideForBeacons  Input_par2buf gpCom_DataRequestAndWaitForAck
134 0x86 gpMacDispatcher_ClearNeighbours  Input_par2buf gpCom_DataRequestAndWaitForAck
139 0x8b gpMacDispatcher_SetCsmaMode  Input_par2buf gpCom_DataRequestAndWaitForAck
140 0x8c gpMacDispatcher_GetCsmaMode  Input_par2buf gpCom_DataRequestAndWaitForAck
141 0x8d gpMacDispatcher_DataPending_QueueClear  Input_par2buf gpCom_DataRequestAndWaitForAck
148 0x94 gpMacDispatcher_SetStackInRawMode  Input_par2buf gpCom_DataRequestAndWaitForAck
149 0x95 gpMacDispatcher_GetStackInRawMode  Input_par2buf gpCom_DataRequestAndWaitForAck
152 0x98 gpMacDispatcher_EnableEnhancedFramePending  Input_par2buf gpCom_DataRequestAndWaitForAck
157 0x9d gpMacDispatcher_GetCurrentTimeUs  gpCom_DataRequestAndWaitForAck
158 0x9e gpMacDispatcher_ConfigureEnhAckProbing  Input_par2buf gpCom_DataRequestAndWaitForAck
159 0x9f gpMacDispatcher_SetRetransmitOnCcaFail  Input_par2buf gpCom_DataRequestAndWaitForAck
160 0xa0 gpMacDispatcher_GetRetransmitOnCcaFail  Input_par2buf gpCom_DataRequestAndWaitForAck
161 0xa1 gpMacDispatcher_SetRetransmitRandomBackoff  Input_par2buf gpCom_DataRequestAndWaitForAck
162 0xa2 gpMacDispatcher_GetRetransmitRandomBackoff  Input_par2buf gpCom_DataRequestAndWaitForAck
163 0xa3 gpMacDispatcher_SetMinBeRetransmit  Input_par2buf gpCom_DataRequestAndWaitForAck
164 0xa4 gpMacDispatcher_GetMinBeRetransmit  Input_par2buf gpCom_DataRequestAndWaitForAck
165 0xa5 gpMacDispatcher_SetMaxBeRetransmit  Input_par2buf gpCom_DataRequestAndWaitForAck
166 0xa6 gpMacDispatcher_GetMaxBeRetransmit  Input_par2buf gpCom_DataRequestAndWaitForAck
167 0xa7 gpMacDispatcher_GetRadioSensitivity  gpCom_DataRequestAndWaitForAck
```

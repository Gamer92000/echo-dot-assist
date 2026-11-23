# Plugins

A design, written 2026-11-18 and revised the same day after an independent review and a first run of the sandbox on
an Echo Dot 2 (section 11 says what each changed), and again after each round of the maintainer's decisions (section 12). The first plugin is microWakeWord, moved out
of hassmic (4.13). Phase 1 is built up to, and not including, the cut; phase 2 is built on hassmic's side (storage,
the log, the first sockets, API 0.2), with the template repository plugin authors start from beside it
(`hassmic-plugin-template`, 5.8); #193's forwarder is left to a community author; phase 4, music, is built on
hassmic's side too (API 0.3: music sources, the volume, the Echo's state, UDP of a plugin's own, the search and
proposals, Home Assistant's entities and actions), with a tone example in the template; phase 5, calls, is built on
hassmic's side (API 0.4: `call`, tokens, the call's audio without a codec, one call at a time beside Drop In); phase 6, a
copy of each request, is built on hassmic's side (API 0.5: `request.audio`, `request.text`, the second press, the
partner rule): section 13 says what is there, what was
tried, and where the code went another way than these pages. What was measured is marked *measured*,
what was read out of the code or a kernel configuration *checked*; everything else is a decision or an assumption,
and section 9 lists what has to be found out before any of it is relied on.

Why: features arrive that should not be in every release, not even off by default, and that the maintainer does not
want to keep working: a syslog forwarder (#193), a Lyrion player (#198, 1150 lines of `lyrion.c` and nine functions
of `main.c`), a SIP extension (#186, #199; `sip.c`, changes to `call.c`, 28 files), another assistant than Home
Assistant (#102; decided since: not a plugin, 4.9). A plugin is such a feature as a program of its own, beside hassmic, that
its author keeps working.

Decided before this document, not argued again: plugins are native code in a sandbox, not WASM or Lua. On an Echo
Dot 2 the interpreters ran audio code 9 to 117 times slower than native (FIR + G.711 on 10 s of audio: native 48 ms,
WAMR's fast interpreter 32x, Lua 5.4 49x; 16 biquads on 10 s of 48 kHz stereo: native 242 ms, WAMR 9x, Lua 39x;
*measured*, biscuit, hassmic running, load ~6). With them hassmic would have to offer codecs and filters as API,
which is the large, stable surface this design is there to avoid. Native plugins bring their own (libre, libopus,
dr_flac). All nine models are 32-bit ARM userlands (*checked*, `devices/*/device.mk`: `armv7a-linux-androideabi24`):
one binary per plugin.

## 1. Goals, non-goals, threats

Goals:

- A plugin can be a wake word engine, a music player, a phone or a log forwarder: sockets, PCM in and out, entities in
  Home Assistant, settings on the page.
- A plugin that is wrong or hostile cannot read hassmic's keys, hear the room outside a request or a call (but for the one plugin the owner made the wake word engine, 4.13), reach
  addresses its owner did not allow, keep the Echo from answering, or make an update roll back.
- The maintainer's share is the broker and its API. Plugins are built, tested, published and answered for by their
  authors; listing one costs the maintainer nothing after its first submission.
- The API stays small enough to keep for years: what a plugin can do itself is not in it.

Non-goals:

- Plugins as root, plugins that change hassmic's own behaviour (Amazon's wake word engine and its models, the front end, the
  firewall; a wake word engine of its own is a plugin, 4.13), plugins
  talking to each other, plugin code in the settings page.
- A plugin in Home Assistant's place. Home Assistant is what this project is for: requests go to it, and no plugin answers one or
  changes what it gets (4.9). A plugin may be given a copy of what Home Assistant hears (`request.audio`, 4.12).
- One toolchain or language. The contract is the binary (2.3).
- A guarantee against a determined author with a kernel exploit (below).
- Running without a sandbox on a model whose kernel has no seccomp filter: there are no plugins on it.

Threats, and what stands against each:

| Threat | Against it | What stays |
|---|---|---|
| A plugin that is wrong (crashes, loops, leaks, floods) | a process of its own, limits on memory, threads and descriptors, a CPU share, a watchdog, hassmic never waits on it (3.2), switched off after 5 crashes in 10 min | sound or a call of that plugin stops |
| A hostile community plugin | no file system, no sockets of its own making, only the capabilities its signed manifest names and the owner approved with the button; where it may connect is the owner's to write, never the plugin's (4.6); the microphone only inside an answered call, and a request's audio only as a one-way copy the owner approved in so many words (3.6); nothing of the page's origin | what it was approved for, it can abuse: a phone plugin can record the calls it carries, a plugin with "any device on your network" can scan it, a plugin with `request.audio` has every request said to that Echo, voices and the room behind them, for as long as it is installed; with a way out to the internet that is a recording of the household, which is why that pair takes a second confirmation and is not listed for community plugins (3.3, 5.3) |
| A plugin with the microphone all the time (`wake`, 4.13) | a capability of its own that no other plugin has; sealed: a `wake` plugin has no way out of the Echo, no network, no entities, no actions, no request copy, and none can be added by an update (3.3); what it reports is checked against the capture's clock and rate-limited; the stream is what the wake word engine in hassmic gets today, no more | it can make the Echo listen when nobody spoke (a false wake word, seen and heard as any); it can leak a little through when it reports; a kernel exploit from there has the room's audio as well as root |
| A plugin attacked from the network (a parser flaw in its SIP or SlimProto code) | the same sandbox: the attacker gets what the plugin had; no executable memory, no other program | as above, without the author's doing. The same for a file the owner uploads: microWakeWord's model parser reads uploads inside hassmic today and inside the sandbox then (4.13) |
| A plugin attacking hassmic through the broker | one parser for all messages (`plugmsg.h`), fixed sizes, every length checked, fuzzed under ASan/UBSan like `pb.h` and `mww_model.c`; a rate limit per plugin; handlers never block, and no socket a plugin can touch is ever used by a hassmic thread that may wait | a flaw there is a flaw in hassmic: the API key, the network key, the browsers' keys. This is the largest new risk hassmic itself takes (9) |
| A kernel flaw reached through an allowed system call (3.18.19, 4.4.22, 4.4.146, 4.9.77, 5.4.86: none gets fixes) | ~55 system calls of ~390, the ones with the worst record left out or cut down (PI futexes, `setsockopt` and `getsockopt` to short lists, `ioctl` to one, no `ptrace`, `bpf`, `keyctl`, `perf_event_open`, `splice`, `userfaultfd`, `personality`, namespaces); a user of its own; SELinux where the policy has the domain, a loud warning where it has not (2.7) | **accepted**: such an exploit is root. The sandbox limits mistakes and makes abuse expensive; it does not make an unknown author's code safe. The page says so for community plugins, as SECURITY.md does for Kiosk Satellite |
| The index is taken over (its repository, its CI, its key) | the key is held by a job that builds nothing (5.3); a package also carries its author's signature, and hassmic pins the author's key at the first install: no update of an installed plugin from a new key; a new install still takes the button and shows the capabilities; the index key is not the release key and opens nothing of hassmic's own; no revocation of an official plugin | a new plugin under a trusted-looking name, installed by an owner who presses the button; as built (2026-11-20) the index key also makes plugins official: a package it lists as official, by an author key an Echo pinned and asking for nothing new, updates without the button and syncs between Echos (docs/SECURITY.md "Plugins") |
| An author's key is taken over | the index's own signature is needed too (it lists only what the plugin repository's own CI built at the commit its entry names, attested, 5.3); revocation by key, which each Echo fetches by itself once a day and Echos pass on to each other (5.6); an update that changes anything the owner approved needs the button (5.4) | a hostile update inside the approved capabilities, until revoked and the list reached the Echo (a day, or longer where someone keeps the list from it: the page says how old its list is) |
| Someone on the LAN | plugins' ports only from private addresses (4.6); a plugin's own protocol is the plugin's to secure; installs only from an approved browser plus the button | what the plugin's protocol gives away (SIP without TLS, SlimProto without login) |
| An Echo of the arbitration network that was taken over (it has the network's key, so it writes acceptance vouchers and vouches its own browser onto the others, 5.4) | a voucher stands in for a button only for a package the index key listed as official, and each target checks the package itself: signatures, the author key pinned there, never back, the combinations, what this build runs; announced on each target in ways that member cannot take back alone (log; a note written before the install, on every member's page, dismissed only with the target's button or by another browser that button let in; Home Assistant's `synced` event, held until its link is up) | it can have official plugins installed on the other Echos of the network without their buttons, in versions the index key listed and not older than the target has installed or has seen listed (a target that never saw a list: any official version this build runs); nothing of its own making, no sideload, nothing wider than each target's own rules. Settings and files of a plugin it could write anyway, as an approved browser can |
| A hassmic that was taken over | the launcher is root's and does one thing whatever it is asked: a process with less than hassmic has (2.2) | nothing new: it could do more itself |

## 2. Process model

### 2.1 Who is who

```
init -> main.sh satellite (root, su)
          `- plugrun -L -- runas -r 3990 -Z u:r:hassmic:s0 <user> <groups> hassmic ...
               |- becomes runas, then hassmic (the broker)          fd 3: one end of a socketpair
               `- forks twice: the launcher (root, su), init's child   the other end
                     `- per plugin: fork, drop everything, exec     uid/gid 6900 + slot
```

- **hassmic** is the broker: it holds every resource (audio, sockets, state, Home Assistant's link) and hands a
  plugin only what its manifest names. All policy is here.
- **The launcher** (`src/tools/plugrun.c`, static, no library of Amazon's, in the bundle beside `runas`) is root's
  and has no policy: asked to start a program, it starts it as a plugin user, under the filter and the limits, always.
  Root is needed for the user, the SELinux domain, the CPU group and the OOM score; none of it can be done by hassmic,
  whose domain may enter no other (`deny hassmic * process { transition dyntransition setcurrent ... }`, *checked*,
  `scripts/system/sepolicy-hassmic.rules`).
- **A plugin** is one process (threads allowed) per plugin, a static program in any language (2.3), under seccomp, with
  three descriptors.

Why the launcher is not hassmic, and plugins not hassmic's children: hassmic ignores SIGCHLD, and Amazon's
attestation module waits for its `idme` tool with a plain `wait()`, which then blocks until every child has gone; a
long-lived child kept hassmic from opening its ports (Dot 2, 2026-10-06, `clock.c`). Plugins are the launcher's
children, the launcher is init's.

Why not root's request files (`state/<x>-request`, as updates, Wi-Fi and the ring's designs go): they are polled,
each is a path root opens, and issue #130 was about exactly those. Here root opens no path of hassmic's: programs and
sockets arrive as descriptors over the socketpair.

`main.sh` changes by one word: `$D/plugrun -L -- $D/runas ...` in place of `$D/runas ...`, and without `plugrun`
(or when it fails before the exec) the line as it is today. `plugrun -L` makes the socketpair, forks twice (the
middle one exits, so the launcher is init's), and execs `runas` with the pair's other end as fd 3 and
`HASSMIC_PLUGIN_FD=3`; hassmic's pid, exit code and the five-exits rule in `main.sh` stay as they are. hassmic sets
the descriptor close-on-exec first thing (`ledctrl`, `pryon_test` and the screen UI must not inherit it), and the
two children it forks without an exec close it themselves: the log reader (`clock.c` `clock_log_start`, which
outlives hassmic on purpose) and the model check's waiter (`artifacts.c`). The launcher ends when its socket closes
or the process it started as hassmic is gone (it knows the pid and looks every 2 s, so a forgotten copy of the
descriptor cannot keep plugins alive), after killing its children: every hassmic start has a fresh launcher and no
plugin outlives hassmic. A hassmic that ends in order (`SIGTERM`, which root sends before its own restarts) first
tells its plugins `STOP` and waits up to 2 s for them; init's own stop is `SIGKILL`, and then the launcher's kill is
all there is (4.4 "As built", a review of 7144548).

### 2.2 Launcher protocol

The only input root takes from hassmic here; `SOCK_SEQPACKET`, fixed structs, anything else closes the socket.

| Message | Fields | Descriptors (SCM_RIGHTS) |
|---|---|---|
| `READY` (launcher -> hassmic, once) | version, sandbox ok (the self test below), domain: plugin / hassmic / none, CPU group ok | |
| `SPAWN` | slot 0..7, memory MB (clamped 8..96), threads (clamped 1..16), CPU class (ordinary: the plugins' shared group; wake: a group of its own with a thread's full weight, 4.13) | the program (read-only), the plugin's control socket, its stderr pipe |
| `SPAWNED` / `FAILED` | slot, pid / errno and step | |
| `KILL` | slot | |
| `EXITED` | slot, wait status, user and system time, peak RSS | |
| `USAGE` (version 2) | slot | |
| `USED` (launcher -> hassmic, the answer) | slot, pid (0: nothing runs there), the process's CPU time so far in ms (all threads, `/proc/<pid>/stat`), the launcher's `CLOCK_MONOTONIC` at that reading, whether it could be read | |

`USAGE` is the CPU watchdog's eyes (2.6): hassmic cannot read another user's `/proc/<pid>/stat` on Android
(`hidepid`), root can. The launcher reports and judges nothing. It is asked rather than left to report every 10 s:
the window is hassmic's to choose, nothing is read for a plugin nobody watches (the wake engine's, on most Echos
the only one), and the answer's two numbers are read in one go, so the share holds however late hassmic reads it.
The price is one more message root takes from hassmic: a type and a slot, checked as `KILL`'s.

Versions only add messages. The two come in one bundle and `main.sh` starts both from one folder; where they differ
all the same: under an older launcher (1) this hassmic runs plugins without the CPU watchdog and never sends
`USAGE` (an unknown type ends the launcher and every plugin), saying so once in the log; a hassmic of version 1
under this launcher takes only its own version in `READY` and runs no plugin (a wake engine's plugin counts as
missing there: Amazon's wake word).

No development flag in the first version (a refused call that raises SIGSYS instead of EPERM comes with the SDK
for other authors, phase 2). *As built (2026-11-21, launcher version 3):* `SPAWN`'s `d` carries flags, and
`PLUGRUN_SPAWN_DEVELOP` has the launcher turn every refusal of the table's (`EPERM`) into `SECCOMP_RET_TRAP` for that
plugin; the `ENOSYS` above 386 stays, so musl's and Go's fallbacks are what they are. The owner's, per plugin, on its
card (`state/plugins/<id>/develop`, `POST /api/plugins/<id>/develop`); a running plugin is started again for it at
once; never copied between Echos. Under an older launcher (version 2) it has no effect: the card shows it as not
available there, and switching it restarts nothing (a review of 08399dc). It widens nothing: the call is not made either way. The C client installs a `SIGSYS` handler that
writes the call's number to the log and ends the plugin; the broker says "the sandbox stopped it" on its card.
`plugrun -T -D <program>` does the same without hassmic (the probe). A runtime that makes refused calls at its start
and takes the refusal (Go's file probes) cannot start with the flag; glibc's static start on the PC asks
`readlinkat` for its own path, which the PC's rows answer `ENOSYS` (a `pc` row, never in the ARM program). A launcher
of version 2 reads nothing of `d`: hassmic sends the flag to version 3 only and says so in the log otherwise.

The launcher never checks what the program is: a hassmic taken over that has it run something gets a process that can
do less than hassmic itself. Signatures are hassmic's business (5).

Self test at launcher start: a child installs the filter and must get EPERM from `open` and `socket`, then exits. If
it does not (no `CONFIG_SECCOMP_FILTER`, a kernel that lies), `READY` says so, hassmic starts no plugin and the page
says why. Never unsandboxed.

### 2.3 Launch sequence

In the forked child, in this order; any step failing is `FAILED` and `_exit`. Steps 4, 8 and 9 are *measured* on
biscuit (2026-11-18, a launcher as root in `u:r:su:s0`, a static musl C plugin: user and group 6900, the filter of
2.4, the exec by descriptor); the others are not.

1. Close every descriptor but the three received. `/dev/null` -> 0 and 1, the stderr pipe -> 2, control socket -> 3,
   the program -> 4 (close-on-exec). `setsid()`, `umask(077)`, `chdir("/")`.
2. `/proc/self/oom_score_adj` = 900: the kernel takes a plugin before the mixer or hassmic. `setpriority` +5.
   Its pid into `/dev/cpuctl/hassmic-plugins/tasks` (made at launcher start, `cpu.shares` 256, `cpu.rt_runtime_us`
   0): all plugins together weigh a quarter of one ordinary thread when the Echo is busy.
   `CONFIG_CGROUP_SCHED` is set in all seven full kernel configurations (*checked*: athena, biscuit, cannoli,
   checkers, cronos, crown, radar); a hard cap (`CONFIG_CFS_BANDWIDTH`) and memory groups (`CONFIG_MEMCG`) only on
   biscuit and radar, so neither is relied on.
3. Limits: `RLIMIT_AS` = the manifest's memory (default 32 MB, 96 at most: a Dot 2 has ~380 MB free with hassmic
   running; without memory groups this is the only memory limit), `RLIMIT_NPROC` = threads (per user, and each slot
   is a user), `RLIMIT_NOFILE` 64, `RLIMIT_STACK` 1 MB, `RLIMIT_CORE`, `RLIMIT_FSIZE`, `RLIMIT_MEMLOCK`,
   `RLIMIT_MSGQUEUE`, `RLIMIT_NICE`, `RLIMIT_RTPRIO` 0. No `RLIMIT_CPU`: plugins run for months; the watchdog
   (2.6) looks at CPU time instead.
4. `prctl(PR_CAPBSET_DROP)` for every capability, `setgroups(0)`, `setresgid`, `setresuid` to 6900 + slot. No
   supplementary group: on every kernel but cannoli's `CONFIG_ANDROID_PARANOID_NETWORK=y` (*checked*) then refuses
   an inet socket without group `inet`, a second wall behind the filter. 6900..6907 is unassigned in AOSP's
   `android_filesystem_config.h` (5999 < id < 9997) and needs no passwd entry (6900 *measured* on biscuit); to be
   checked against every model's image (9). A user per slot, not one for all: `kill`, `/proc` and `RLIMIT_NPROC`
   then separate plugins from each other as well.
5. `prctl(PR_SET_PDEATHSIG, SIGKILL)` (after the user change, which clears it), then `getppid()` must still be the
   launcher.
6. SELinux, by writing `/proc/self/attr/current` as `runas -Z` does (the exec's context is refused from `/data`,
   which is nosuid): `u:r:hassmic_plugin:s0` where the policy has it, else `u:r:hassmic:s0`, else it stays in the
   launcher's `su` (2.7 says what that means and what the page shows). *Measured* on biscuit with
   `u:r:hassmic:s0`, written as root before step 4: the probe's 39 checks and the C, Rust and Go plugins pass in
   that domain, no AVC denial seen. `hassmic_plugin` is in the rules since 2026-11-22 (2.7 "As built") and untried
   on an Echo. Written after step 4, as the launcher always has: the child, user 6900 and without capabilities, may
   still write its own `attr/current` (the file is 0666 and only a task's own); the check is SELinux's, `su`'s
   `setcurrent` and `dyntransition`.
7. SIGPIPE ignored (kept across exec), signal mask empty.
8. `PR_SET_NO_NEW_PRIVS`, then the seccomp filter (2.4), built for this pid.
9. `execve("/proc/self/fd/4", {"plugin", NULL}, {"HASSMIC_PLUGIN=1", NULL})`: by descriptor, since 3.18 has no
   `execveat`. The file is `state/plugins/<id>/plugin`, mode 0755; a process may always open its own
   `/proc/self/fd`, also when not dumpable.

**The contract is the binary, not the toolchain.** Native means an author is free in language: C, C++, Rust, Go,
Zig. What a plugin has to be:

- a static ELF for 32-bit ARM Linux (EABI5, ARMv7-A with VFP; the hard-float ABI where it has a C ABI at all), with
  no interpreter (`PT_INTERP` absent): the filter is in place before the exec, so a dynamic linker could open no
  library;
- a `PT_GNU_STACK` that is there and not executable, no segment both writable and executable (5.1);
- a program that lives on the system calls of 2.4 and nothing else, from its first instruction.

What passes (*measured* 2026-11-18 on biscuit, 3.18.19, and on donut, 4.4.22 arm64 with the plugin as a compat
task; user 6900, in `su`), each exit 0 on both:

| Runtime | Built with | What ran | Size |
|---|---|---|---|
| C on musl | `zig cc -target arm-linux-musleabihf -static` | clocks, `getrandom`, a 16 MB malloc, four threads, a mutex | 1.3 MB with debug info |
| C++ on musl (zig's libc++) | the same, `zig c++` | `std::thread` x4, mutex, vector, exceptions. **Not `std::random_device`**: libc++'s opens `/dev/urandom` and aborts on the refusal (SIGILL) | 6.2 MB with debug info |
| Rust | `armv7-unknown-linux-musleabihf`, `crt-static`, rust-lld | std threads, `Mutex`, `HashMap` (its random keys come from `getrandom`), `Instant`, `SystemTime`; `File::open`, `UdpSocket::bind`, `Command::spawn` come back as errors (EPERM) | 0.64 MB |
| Go 1.27.4 | `GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0` | four goroutines on locked OS threads, `sync.Mutex`, `crypto/rand`, `time`; `os.Open` and `net.Dial` come back as errors. Its runtime also probes files at start (`/sys/kernel/mm/transparent_hugepage`) and takes the refusal | 2.3 MB stripped |

All four have a `PT_GNU_STACK` that is read-write and no `PT_INTERP` as their toolchains build them. Each brought
something the filter had to learn (2.4): musl's `clone` flags and `mmap2` hints, Go's `clone` flags, `MAP_FIXED`,
plain `fcntl` and `sched_getaffinity`. Not measured: a musl cross gcc and libstdc++ (whose `random_device` may use
`getrandom`), Zig's own standard library, cannoli's 5.4 kernel, any of it in the plugin's SELinux domain.

What does not pass: a static **bionic** program (NDK r21e, `-static`, API 24; *measured*, biscuit). Before `main`
it calls `faccessat`, `openat`/`open` (the property area), `fstatat64`, `personality` and `socket` (logd), and
`sched_getscheduler` in `pthread_create`; with those refused it segfaults, and it runs only once `open` is allowed,
which is the one thing the sandbox is there to refuse. So a plugin is not built with the NDK that builds hassmic.
A loader in the launcher that maps a dynamic plugin before the filter was dropped earlier: an ELF loader as root's
code, for the sake of shared libraries the plugins do not need.

**The exec.** The filter has to let step 9 through. It allows `execve` only with the three pointer values the
launcher passes (path, argv, envp); a second `execve` from the plugin, with other pointers, is refused (*measured*).
That keeps a plugin's code from running another program by accident or by a simple bug, and is not claimed to be
more: `mmap2` has to allow `MAP_FIXED` (Go's runtime, 2.4), so a plugin can put a page of its choice at the
launcher's old addresses and repeat the call. It would gain nothing: the new program runs under the same filter,
the same user and `no_new_privs`, there is no file of the plugin's making to run, and whatever it runs can open
nothing. The earlier draft's check of the plugin's `PT_LOAD` ranges against those addresses is dropped with the
claim.

### 2.4 The filter

Architecture must be `AUDIT_ARCH_ARM` (kill otherwise). The 64-bit kernels of donut and cupcake run 32-bit programs
as compat tasks, which seccomp reports as the same architecture with the same numbers. Default: `EPERM`. Numbers
above 386 (`bpf`, the last of 3.18's table): `ENOSYS`, so every model shows a plugin the oldest kernel's calls, and
musl's time64 calls fall back as they do on an old kernel (musl falls back on `ENOSYS` only; `EPERM` would break
`clock_gettime` on cannoli's 5.4; *not measured*, biscuit's kernel says `ENOSYS` there by itself).

What was run, in three rounds. First the prototype's 25 calls with a forked bionic child on biscuit (`open`,
`socket`, `execve`, `connect` and `sendto` elsewhere, `PROT_EXEC` refused; a UDP socket handed over usable towards
its peer only). Then the first draft of this table with a static musl C plugin on biscuit, which needed musl's
`clone` flags (`0x7d0f00`: `CLONE_DETACHED` too; bionic's `0x3d0f00` was refused) and address hints on `mmap2`
(seen: `mmap2(0xb4d93000, 0x101000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`). Then C, C++, Rust and Go on
biscuit and donut, without `accept`, with `mremap`'s rule and the `ENOSYS` rule in place, which needed three more
things, all Go's:

- `clone` with `0x50f00` (`VM FS FILES SIGHAND SYSVSEM THREAD`, no TLS flags: `runtime/os_linux.go` `cloneFlags`);
- `MAP_FIXED` on `mmap2`: the runtime reserves its arena `PROT_NONE` and commits pages with `MAP_FIXED` over its
  own reservation (without: "runtime: mmap(0xec97e000, 12288) returned 0x0, 1 ... cannot map pages in arena
  address space"). `MAP_FIXED` only moves mappings inside the process's own address space; what it costs is the
  exec pin as an address control (2.3), which was not a wall before;
- plain `fcntl` (number 55, beside `fcntl64`): `runtime.checkfds` asks `F_GETFD` of descriptors 0 to 2 and throws
  "cannot open standard fds" on EPERM; and `sched_getaffinity` (its own pid, read-only: `GOMAXPROCS`).

With those: `open`, `socket`, `fork`, `execve` again, `kill(1)` refused, everything in 2.3's table working, on
both kernels. donut's arm64 kernel has the seccomp filter and applies it to a 32-bit task with `AUDIT_ARCH_ARM`
and the 32-bit numbers (*measured*).

Last, the table below as it stands, argument rules included (marked †), on both kernels: a probe (static musl,
holding an `AF_UNIX` datagram socket and a connected UDP socket from the launcher) made 39 checks, none wrong.
Passed: `fcntl` `F_GETFD`/`F_GETFL`/`F_DUPFD_CLOEXEC`, `ioctl` `FIONREAD`, `send` with `MSG_NOSIGNAL`, `sendto`
with no address and `MSG_DONTWAIT` (the peer received it), `getsockopt` `SO_ERROR`, `setsockopt`
`SO_RCVBUF`/`TCP_NODELAY`/`IP_TOS`, `mmap2` anonymous read-write and `MAP_FIXED` over its own page, `mremap`
`MAYMOVE`, `madvise` `DONTNEED`, `futex` `WAKE_PRIVATE`, `prctl` `PR_SET_NAME`, `tgkill` to itself,
`pthread_create`. Refused with EPERM: `fcntl` `F_SETFL`/`F_SETOWN`, `ioctl` `FIONBIO`/`TIOCSTI`, `send` with
`MSG_OOB`, `sendto` another address, `getsockopt` `SO_PEERCRED`, `setsockopt`
`SO_BINDTODEVICE`/`SO_LINGER`/`IP_MULTICAST_IF`, `mmap2` with `PROT_EXEC`, with `MAP_SHARED`, of a descriptor,
`mprotect` `PROT_EXEC`, `madvise` `MADV_HUGEPAGE`, `futex` `LOCK_PI`/`WAKE_OP`, `prctl`
`PR_SET_DUMPABLE`/`PR_SET_PDEATHSIG`, `tgkill` to pid 1, `personality`. The C, Rust and Go plugins of 2.3 run
under this narrowed filter on both kernels; C++ as before (all but `random_device`).

Two things that run taught:

- **`mmap2` must require `MAP_ANONYMOUS`**, as a bit that has to be set, not only keep the flags inside an allowed
  set: without it a private read-only mapping of a descriptor the plugin held (its own program) went through.
- **musl turns the kernel's EPERM into ENOMEM** for an anonymous `mmap` without `MAP_FIXED` (its "fixup incorrect
  EPERM" in `mmap.c`). A mapping the filter refused looks like "out of memory" to a musl program, and to whatever
  reports on it; the probe and the harness ask with the raw `syscall(SYS_mmap2)`.

For whoever writes `plugrun`: the filter is 217 BPF instructions, and a conditional jump reaches at most 255
ahead, so each rule returns its own allow or deny instead of jumping to shared ones at the end; it is generated
from the rule table (labels resolved by the generator), not written by hand.

| Group | Allowed | Conditions |
|---|---|---|
| I/O on what it holds | `read write readv writev close lseek _llseek fstat64 dup dup3 pipe2 eventfd2` | |
| | `fcntl64`, `fcntl` | `F_GETFD F_SETFD F_GETFL F_DUPFD_CLOEXEC` † (no `F_SETFL`: 4.6) |
| | `ioctl` | `FIONREAD` † |
| Waiting | `poll ppoll epoll_create1 epoll_ctl epoll_wait epoll_pwait nanosleep clock_nanosleep restart_syscall sched_yield` | |
| Sockets handed over | `recv recvfrom recvmsg send shutdown getsockname getpeername` | `send`: flags within `MSG_DONTWAIT\|MSG_NOSIGNAL` |
| | `sendto` | address NULL (args[4] == 0), flags as `send` |
| | `getsockopt` | `SOL_SOCKET`: `SO_ERROR SO_RCVBUF SO_SNDBUF SO_TYPE` † |
| | `setsockopt` | `SOL_SOCKET`: `SO_KEEPALIVE SO_RCVBUF SO_SNDBUF`; `IPPROTO_TCP`: `TCP_NODELAY`, `TCP_KEEPIDLE/INTVL/CNT`; `IPPROTO_IP`: `IP_TOS` † |
| Memory | `brk munmap` | |
| | `mmap2` † | no `PROT_EXEC`; `MAP_ANONYMOUS` and `MAP_PRIVATE` required, beyond them only `MAP_FIXED`, `MAP_NORESERVE`, `MAP_STACK`: never a descriptor, never shared; hints and `MAP_FIXED` allowed (musl, Go) |
| | `mremap` | flags 0 or `MREMAP_MAYMOVE` (no `MREMAP_FIXED`) †. No longer an address control, since `MAP_FIXED` is allowed; kept because nothing needs it) |
| | `mprotect` † | no `PROT_EXEC` |
| | `madvise` † | `MADV_DONTNEED`, `MADV_FREE` |
| Threads | `clone` | flags exactly `0x7d0f00` (musl: C, C++, Rust) or `0x50f00` (Go's runtime); both *measured*, biscuit and donut. Both have `CLONE_VM` and `CLONE_THREAD`: threads, never a process |
| | `sched_getaffinity` | Go's runtime at start (read-only) |
| | `futex` † | op without `PRIVATE`/`CLOCK_REALTIME` in `WAIT WAKE REQUEUE CMP_REQUEUE WAIT_BITSET WAKE_BITSET`; no PI operation (CVE-2014-3153 and what followed) |
| | `set_tid_address set_robust_list sigaltstack gettid exit exit_group __ARM_NR_set_tls` | |
| | `prctl` † | `PR_SET_NAME`, `PR_GET_NAME` |
| Signals | `rt_sigaction rt_sigprocmask rt_sigreturn sigreturn rt_sigtimedwait` | |
| | `tgkill` † | tgid == its own pid (glibc's `abort()`, `pthread_kill`) |
| | `tkill` | any thread id: musl's `raise()`, `abort()`, `pthread_kill()`, `pthread_cancel()` send with it (a review of 6631974: refused, `abort()` ended as `SIGSEGV`). No rule can hold it to the plugin's own threads (a thread id is one counter for every process); the slot's own user does: a signal to another user's process is `EPERM` from the kernel |
| Time, identity | `clock_gettime clock_getres gettimeofday getpid getuid32 geteuid32 getgid32 getegid32 ugetrlimit uname` | |
| Randomness | `getrandom` | |
| The start | `execve` | the launcher's three pointers (2.3) |

Not there, and why:

- `open`/`openat` and every call that takes a path: no file system.
- `socket`, `connect`, `bind`, `listen`, `accept`, `accept4`: sockets are the broker's to make. `accept` because a
  socket it returns is created with the caller's credentials: it would be the one socket a plugin's user owns, out
  of the broker's sight and count, and in the way of the firewall's rule for those users (4.6).
- `sendmsg`, `sendmmsg`: the destination is inside a struct the filter cannot read; also the only way to pass a
  descriptor on.
- `fcntl(F_SETFL)`, `ioctl(FIONBIO)`, `SO_LINGER`, `SO_RCVTIMEO`, `SO_SNDTIMEO`: they change the open socket
  itself, which a thread of hassmic's may share (4.6); sockets arrive non-blocking and stay so.
- `personality`: `READ_IMPLIES_EXEC` would undo the `PROT_EXEC` rule. The same flag comes from an ELF without a
  non-executable `PT_GNU_STACK`; the package check refuses those (5.1) and the probe asserts it.
- `kill`; `fork`/`vfork`; `ptrace`, `process_vm_readv`; `timerfd_*`, `inotify`, `signalfd`,
  `splice`/`tee`/`vmsplice`, `memfd_create`, `mlock`, `setpriority`, `sched_*` but `sched_yield` and `sched_getaffinity` (not needed; each
  has had kernel flaws); `__ARM_NR_cacheflush` (no generated code); `PR_SET_VMA` (bionic's, and bionic is out).

`PROT_EXEC` is refused although the plugin is native code already: it keeps someone who exploits a plugin's parser
from the network on return-oriented code only.

### 2.5 What a plugin has

- **Descriptors**: 0, 1 `/dev/null`; 2 a pipe hassmic reads into the log as `plugin <id>: ...`, 50 lines a minute,
  the rest counted (`/data` is flash); 3 the control socket. More arrive over it.
- **File system: none.** Not a directory either: `openat` on a directory descriptor cannot be confined below it
  before Landlock (5.13), and `..` or a link leads out. What a plugin keeps, the broker keeps for it (4.4).
- **Time**: `clock_gettime`, monotonic and wall (*measured*); the 32-bit ARM kernels before 4.1 have no vDSO, so it
  is a system call. The wall clock may be unset (it comes from Home Assistant, `clock.c`): `WELCOME` and a `TIME`
  event say whether it is set and the zone's rule.
- **Randomness**: `getrandom` (*measured* on biscuit's 3.18.19 and donut: it is there, and Rust's and Go's
  standard libraries use it). The kernel's 16 bytes of `AT_RANDOM` besides. Not `/dev/urandom`: C++'s
  `std::random_device` in libc++ opens it and aborts; a C++ plugin calls `getrandom()` or the client's
  `hm_random()`.
- **Signals**: its own handlers; from outside only the launcher's SIGTERM, then SIGKILL after 2 s.

### 2.6 Running, crashing, hanging

- **Start.** Two kinds:
  - *A `wake` plugin that is the chosen engine* starts first of all, in `main()`, before the engine is opened
    (`main.c:3741-3751` opens it synchronously ahead of the capture loop). `main()` may wait there, the capture
    thread does not exist yet: up to 5 s for `WAKE_READY` of the active word, then on without it (4.13: Amazon's
    wake word where the plugin is missing, no wake word where it is installed and not ready).
    It has to be so: started after the self test, the plugin could never be up when the engine opens, Amazon's
    "Alexa" would listen for the first ten seconds of every start, and the self test would pass on Amazon's engine
    whatever became of the plugin.
  - *Every other plugin* starts only once the self test passed (`ota_healthy()`), and cannot be why an update fell
    back.  The self test runs at every start where the push port is on (every Echo's, whether or not an update is
    on trial: `main.c` `selftest_thread`); held past its 30 s by the wake word plugin alone, it asks again every
    10 s for as long as hassmic runs, and the others start when it passes; with no microphone audio within the 30 s
    they do not start in that run.  Without the push port (`-o 0`, the PC) 10 s after start.
- **The self test with a plugin as the engine.** "Wake word engine loaded" (`main.c`'s self test before
  `ota_healthy()`) means the engine the owner chose. hassmic notes under which version the wake plugin last
  answered `WAKE_READY` (`state/plugins/<id>/ready`, the version's string). For an update on trial: if the plugin
  was ready under the version before, it must be ready under this one, or the self test fails and the Echo falls
  back: an update that breaks a working wake word is a bad update. If it was *not* ready before (never installed,
  broken already, the first start after 5.9's cut), the self test does not ask for it: the update is promoted
  without it, and a plugin that is missing or broken cannot hold every update back for ever. Nor where the owner
  chose that it does not run (`plugin_selftest_ok`): plugins switched off (the setting, `PLUGINS=0`), the plugin
  switched off on its card, a version the plugin index withdrew; nor one that needs a newer hassmic than this one
  (a fall back to an older build: it requires a scope version this one has not, 3.1). Anything else that keeps it
  down under the update (the launcher missing or silent, the sandbox, a manifest or acceptance the build does not
  read) is the update's failure and holds it back (5.9 "As built").
- **hassmic's own crashes.** Counted only when there is evidence of one: hassmic's fatal-signal handler writes a
  mark (`state/plugins/fall`: the slot whose message the broker thread was handling, if it was; one `write` to
  a descriptor opened at start). No mark, no count: init's stop is SIGKILL (`main.sh` `svc_stop`, `svc_restart`),
  so an engine switch, an update and a power cut all end hassmic "uncleanly" and must not look like crashes; an
  earlier revision counted them and would have switched every plugin off after three ordinary restarts in a day.
  A start that finds a mark *with a plugin in it* adds one to `state/plugins/falls` (back to 0 after 24 h up) and
  removes it; that plugin alone is held back for this start; at 3 no plugin but the wake engine's starts until
  the owner switches them on again, and the page says so. A hang is not seen by this; `main.sh` restarts a
  hassmic that exits, not one that stalls, with or without plugins.
  Changed after a review (2026-11-19), three ways; this paragraph first read "a start that finds the mark adds
  one", whoever it named:
  - *A mark that names no plugin is not counted.* It is a crash somewhere in hassmic while plugins ran (a decoder,
    one of Amazon's libraries, an abort in `ws.c`) with nothing to say a plugin had a part in it: logged, removed,
    forgotten. Counted, three of those in a day switched every plugin off for faults that were not theirs.
  - *A plugin's message ends where the core's work begins.* The mark names a plugin from its packet to the point
    where the broker hands what it says to the core (a detection to the wake path, a new list of words, the state
    asked for `WELCOME`). A fault after that is in hassmic's own code on hassmic's own state. Before, any crash in
    the wake path (ring, aligner, arbitration, the run) named the wake plugin.
  - *The wake engine's plugin, when a mark names it, is not refused for the run.* It is held back for the 5 minutes
    a crashing wake plugin waits (below) and then started; should hassmic fall over it again, the next start holds
    it back again. "For the run" was until someone found the page, on an Echo that hears no wake word meanwhile:
    the worse fault, as below. Its card and the log say why it waits.
  And a fourth, after the next review (13, the sixth read): the second of those left a loop open. A wake plugin
  whose report is in range and of the right form, and over which the core's own code falls all the same (a
  detection, a list of words), brought hassmic down with a mark that named nobody; the wake engine's plugin starts
  in `main()` at every start, reports again, and nothing ever slowed that down.
  - *A fault in what the core does with a plugin's report is a mark of its own kind* (`core <id>` beside
    `fall <id>`; written when the broker's thread is in the core on that plugin's behalf). It is still hassmic's
    fault and still never counted towards "three: all plugins off", and it never refuses the plugin. But the marks
    are kept with their time (`state/plugins/.cores`, on the monotonic clock, which hassmic's restarts share), and
    the third of one plugin's within 10 minutes holds that plugin's start back for the 5 minutes of the retry, then
    it is tried; if hassmic falls again, the next start waits again, without end. Such an Echo is down for seconds
    every five minutes instead of all the time; the log and the plugin's card say what over. The owner's own switch,
    a new version of the plugin and "Switch plugins on again" start it at once. (A fault on another thread than
    the broker's, in work a report set off, still names nobody: nothing ties it to the report.)
- **Watchdog**: a `PING` every 10 s, counted as unanswered only for time in which the broker thread itself ran (a
  broker that waited on `core_lock` must not kill the plugins it kept waiting); three unanswered, the control
  socket's queue full for 5 s, or more than a full core's worth of CPU over a minute for an ordinary plugin
  (asked of the launcher, 2.2's `USAGE`: hassmic cannot read a plugin's `/proc/<pid>/stat` itself; a manifest has
  no CPU share to set, the class in `SPAWN` is all there is): killed, counted as a crash.
- **Places**: 2 plugins run at once in phase 1 (4.1), and the wake word engine's plugin is always one of them: with
  both places taken the newest of the others is stopped for it, says so on its card, and starts when its owner
  switches it on again. (First: refused like any third plugin and not tried again, so an engine chosen after two
  plugins had started heard nothing. Not a place kept free instead: the engine can be chosen while two others run,
  so something is stopped in any case.)
  *As built (2026-11-22, `plugin.c` `RUN_MAX`, `run_mem`, `places`):* the launcher's 8 slots, and the
  plugins that run together within 128 MB of their `memory_mb` (4.1 says why that bound). One that finds no place
  waits for one: its card says "Waits for a place" and why (how many run, or how much of the 128 MB is taken and what
  it may use), the log says so once, and the broker's clockwork starts it when a place is free, the one that waited
  longest first, each as it fits (one too large for what is left lets a smaller one that waited less go first: a
  place is not kept empty). The wake word engine's plugin still always has one: the newest of the others is stopped
  for it, one at a time until it fits, and that one now waits for a place again instead of staying off until its
  owner switches it on.  A review of 7be9343: such a stop is no crash (`exited`'s count leaves out one stopped while it
  `waits`: five of them switched a healthy plugin off while the engine crashed and took its place each time, P1).
  The engine's place is its own whenever it is wanted and not off, running or not (`places`, `engine_waits`): between
  a crash and its start again, while it waits for room, while it is held back after hassmic fell over it.  Counted
  only while it had a process, its place went to whoever asked in between: a plugin that waited took it while the
  engine was down for a second and was stopped again at its start (P3), or another start took the last launcher slot
  while the engine's start had `bl` let go for its program, and the engine was never started again (P2).  And any
  wanted plugin that ends `start()` without a slot all the same is asked again 250 ms later, the engine's as one
  that waits for room; one whose start fails before the launcher has it (its program not opened: descriptors run
  out, an install renaming it in; no socket or pipe) after 1, 5, 30 s, then every 5 min, a crash's back-off never
  counted as one (`failn`; F1 of a review of 5aa32ee: such a start left it down for good, the engine's entry with
  its place and memory held from every plugin that waits).
- **Crash**: started again after 1, 5, 30 s, then every 5 min; 5 crashes within 10 min switch it off, with the exit
  status on the page. Not the wake engine: it is tried again every 5 min without end, because an Echo that stops
  listening for its wake word until someone finds the page is the worse fault. Meanwhile nothing listens (the
  action button works) and the page warns: Amazon's wake word stands in only for a plugin that is missing, never
  for one that is installed and down (4.13).
  What a crashed plugin held is released by the broker: its music stream closed, its call ended as "failed", its
  sockets closed, its entities unavailable.
- **Off for all**: the feature setting `plugins` (off by default: no page section beyond the switch) and
  `PLUGINS=0` in root's `hassmic.conf`. `PLUGINS=0` wins over everything, the migration of 5.9 included: no
  launcher, no plugin, and an Echo whose engine is a plugin listens for Amazon's wake word (4.13: the plugin counts as missing) and says why. The migration
  switches the feature `plugins` on by itself (the owner chose the engine that became one). No button gesture (the first draft reused the factory reset's 5 to 10 s
  window; cut, 11).
- **Factory reset** empties `state/`, plugins included. Nothing of a plugin is ever in the factory copy on
  `/system` or in a bundle.

### 2.7 SELinux

What it can add: a wall for the case that the filter has a hole but the kernel is intact. `hassmic_plugin` is a
type with no rule but: use the descriptors it was given, read and write the sockets and the pipe behind them, be
entered from `su`, execute its own file. Added to `scripts/system/sepolicy-hassmic.rules` with the index (phase 3: it reaches
only new system installs, never an update, so the first plugin runs in `hassmic` or `su` everywhere and does not
wait for it), so every install from then on has it. **Not tried on any device**: the run on biscuit was in `su`.

What it cannot: reach an Echo that is installed already. Updates never change the policy; it is written only by
the system install. So there are three kinds of Echo in the field, and not two:

| The policy has | Plugins run in | Measured | The page |
|---|---|---|---|
| `hassmic_plugin` (system install from 2026-11-22 on) | it | no: in the rules and checked against every model's policy (`tools/sepolicy-check.sh`), not tried on an Echo | "Sandbox: complete" |
| only `hassmic` (system install from 2026-11-12 on) | `u:r:hassmic:s0`: enforcing, but `allow hassmic * * *` less properties, domain changes, ptrace | yes, biscuit: plugins run, no denials | "Sandbox: without its SELinux wall" |
| neither (system install before 2026-11-12) | `u:r:su:s0`, the launcher's own, permissive: SELinux stops nothing | yes, biscuit and donut: every other run in 2.3 and 2.4 was in `su` | "Sandbox: without SELinux at all" |

The third kind is real: the Dot 3 these runs were made on (version 2026.11.13.012047) has no `hassmic` type
(writing the context gives EINVAL) and hassmic itself runs in `su` there (its `boot.log`: "runas: no SELinux
domain u:r:hassmic:s0 (Invalid argument): runs in the caller's"), because updates brought the code and nothing
brought the policy. Earlier revisions of this section said "never `su`"; held to, that gives such Echos no
plugins until someone connects them to a PC, against the maintainer's principle that plugins work without the new
domain, behind a warning. **Decided by the maintainer (12, 9.2): plugins run in `su` there too, under the
strongest of the warnings.** The reasoning: the walls that matter are the system call filter, the user
of its own and having no file system and no socket, and they are the same in all three rows. What `su` being
permissive adds for hassmic (issue #130: any process may set any property, start any service) is reached through
the property socket, which takes `socket`, `connect` or `open`, all refused; and a plugin is not root, whatever
its domain. What is lost is the second wall for the day the filter has a hole: in `su` there is then nothing
between that hole and what user 6900 may do by file modes alone, where in `hassmic` properties, domain changes
and ptrace would still be refused. The alternative, to refuse until the system install is run again, was the
cautious one and would have cost every such Echo its plugins; not taken.

The launcher finds out which (it tries the context in its self test's child) and says so in `READY`; hassmic
passes it to the page (`/api/plugins` `domain`), which shows the state on the Plugins page at all times.

Without `hassmic_plugin`, the page shows this in full and not behind an ⓘ, in two places: before **community** plugins
go on from their switch on the Plugins page (off by default, from an approved browser), and in the install dialog of
every plugin that is not official (the step that takes the action button).  An import, a preset, Compare & copy and
"make like this Echo" switch community plugins on without the first, never without the second (a review of 00d492a):

> **This Echo's sandbox is missing one of its walls.**
> Community plugins are programs by people this project does not know, and nobody has reviewed them.
> **Missing here:** the SELinux wall. *[only `hassmic`:]* If a plugin gets past the system call filter, SELinux
> would stop little of what it tries. *[neither:]* This Echo's system install is older than hassmic's own SELinux
> domain: if a plugin gets past the system call filter, SELinux stops nothing, and hassmic itself runs without
> that protection too.
> The three levels: complete (plugin domain) / without its SELinux wall (hassmic's domain) / without SELinux at
> all. This Echo: *<which>*.
> **Still in place:** a user of its own for each plugin, the system call filter (no files, no programs, no
> sockets of its own making), no file system, memory and thread limits, and only the network destinations you
> approve.
> **To close the gap:** connect the Echo to your PC (USB, or adb over Wi-Fi from this page) and run the system
> install again: `scripts/install-system.sh`. Nothing of your settings changes. Then this warning goes away.

The last paragraph is worded per model, from `device.conf`'s `INSTALL` (*checked*, `scripts/install-system.sh`):

| `INSTALL` | Models | What writes the policy | What the page says |
|---|---|---|---|
| `twrp-ab` | donut, biscuit, radar, cupcake | `scripts/install-system.sh` from the running OS (`/sepolicy`, the old one kept as `/sepolicy.pre-hassmic`) | "run `scripts/install-system.sh`" (or `scripts/setup.sh <model> --from install`) |
| `running` | athena | the same script; the policy is `/vendor/etc/selinux/precompiled_sepolicy` (`SEPOLICY_FILE`) | the same |
| `boot` | checkers, cronos, crown | `scripts/install-system.sh` hands over to `scripts/install-boot.sh`: the policy is inside `boot.img`, rebuilt by `scripts/mkbootroot.py` and flashed in amonet's fastboot | "run `scripts/install-system.sh`; it flashes the boot image again, which needs the Echo in fastboot as at the first install (`devices/<model>/README.md`)" |
| `cannoli-ubi` | cannoli | not this script (it refuses): the firmware ZIP's offline builder patches the policy | "build and flash the firmware ZIP again (`devices/cannoli/README.md`)" |

That `install-system.sh` run again leaves an installed Echo's settings alone is its documented use ("again if ...
the SELinux policy has to change"); that it does so with plugins installed is to be tried. Official plugins show
no warning but the same state line; on an Echo of the third kind the Plugins page says, plugins or not, that
hassmic itself would gain from the system install being run again.

*As built (2026-11-22).* **The domain** (`scripts/system/sepolicy-hassmic.rules`, after hassmic's own rules):
`hassmic_plugin` and `hassmic_plugin_file`, types of an attribute of their own (`hassmic_plugin_type`, which no rule
names) and not of `domain` or `file_type`: every rule a policy writes for those attributes (any domain reads `/proc`,
reaches the property socket, calls binder) would have been theirs. Its rules: `fd use` of hassmic's and su's
descriptors; `read write getattr getopt setopt shutdown` of hassmic's `unix_stream_socket` (every pair hassmic hands
over, `SOCK_SEQPACKET` included), `tcp_socket`, `udp_socket`; `write getattr` of hassmic's and su's `fifo_file` (the
stderr pipe, the launcher's report pipe); `read write getattr` of its own `fifo_file` (a pipe it makes itself,
`pipe2`, which the filter allows: a self-pipe to wake its own poll, Go's netpoll break; pipefs labels a pipe with its
maker's domain on donut, checkers, athena and cannoli, so without the rule it made one it could neither read nor
write: a review of 3326cd4); `read write getattr` of `null_device` (descriptors 0 and 1); `read getattr`,
no `open`, of `system_data_file` (the files the owner gave it, as descriptors); its own program, `hassmic_plugin_file`
`read open getattr map execute execute_no_trans`, and for the exec by `/proc/self/fd/4` `search` of `rootfs`, `proc`
and its own `/proc` directory and `read` of the two links; `fork` (threads), signals within the domain,
`getsched`; `sigchld` to su. The domain is one type for every plugin: as far as SELinux goes a plugin may signal any
other and search its `/proc` directory. What keeps plugins apart from each other is each slot's own user (2.3: no
signal to another user, its `/proc` files that user's) and the filter (`tgkill` only to its own pid, no `kill`; `tkill`, which musl's `raise()` needs, by the user alone); a
category per slot (MCS) would make SELinux a third wall there, and is not built (a review of 3326cd4). Entered from
su only (`dyntransition`, `setcurrent`); su (the launcher) may signal it and read its `/proc`;
hassmic may do to it what its `*` gives, less the denies (no domain change into it, no ptrace). No other domain has a
rule for it. **Its own file**: `/data` gives every file of hassmic's `system_data_file`, which the domain may not
execute; hassmic labels the program `hassmic_plugin_file` through the descriptor it hands the launcher, before every
start where the launcher found the domain (`plugin.c` `program_label`: `fsetxattr`, the policy's one relabel for
hassmic, from `system_data_file` to that type), and a program it cannot label is not started, with that said (its
exec would fail at step 9 with no word of why). The install's checks look for `type hassmic_plugin` as for
`type hassmic`. **Checked** (`tools/sepolicy-check.sh`): the rules compiled into each model's policy as the install
does it (magiskpolicy v30.7 under qemu, the model's rules, sepolicy-mirror.sh's, then these), and read back: the
domain's rules are exactly the eighteen above, nothing names its attribute, nothing enters it on exec, no domain but
su and hassmic reaches it, hassmic cannot enter or ptrace it (these two looked for under each of its names, the
attribute's too: magiskpolicy prints hassmic's `*` as rules against `hassmic_plugin_type`, and by `hassmic_plugin`
alone neither check could fail; seen failing with `dyntransition` and `ptrace` taken out of hassmic's deny and with a
rule of `kernel`'s against the attribute, a review of 3326cd4), the relabel pair is hassmic's only one, the program's
type may live on `/data`; passing on athena, biscuit, cannoli, checkers, cupcake, donut and radar (cronos and crown:
their boot images are not unpacked here). **Not tried on an Echo**: whether a plugin starts and runs in it (the
launcher's `READY` would say "complete" and the card's process would be in it) is the first thing to try on one,
with `dmesg` for denials (a rule left out shows there as `avc: denied ... permissive=0`).

**The page's state line** was built with the launcher's probe (`READY`'s domain, `/api/plugins` `sandbox`:
"complete", "hassmic", "su"), and the third kind's note about hassmic itself is on the Plugins page for every Echo
that has it. The PC has no SELinux: `HASSMIC_PLUGIN_DOMAIN=plugin|hassmic` has `build/plugrun-host` report one for
tests. **The community switch** (setting `plugins_community`, no entity): its own row under "Plugins", off by
default, on and off from an approved browser as any setting of the page (`POST /api/set`; every page request is
signed by an approved browser already), with no press of the action button: installing a plugin asks for the button
already, a community one as any other (W_PLUGIN), and that is the owner's look at each one (the maintainer's decision,
2026-11-22; it first took a wait for the button of its own, `W_COMMUNITY`, never released). So it is an ordinary
setting in every way: exported, copied by Compare & copy and "make like this Echo", taken from an import and a preset
(`scripts/setup.sh --preset`), as each Echo's other settings; on another Echo it lets nothing in by itself, each plugin
there is still installed with that Echo's button. Not a Home Assistant entity: Home Assistant does not switch what
this Echo runs. At start the store's switch is set from the value this start will have (`settings_peek`, a preset's
included, as for `plugins`) before the first scan, so nothing it keeps off starts in between: `settings_load` only
notes `state/config`'s value of both switches, and `settings_preset` tells them once at its end, so a preset that says
otherwise than `state/config` no longer turns them to the config's and back on the way (a review of 00d492a, 4; a
preset too long to apply leaves the config's, told then, once).  In one request (an import, Compare & copy, "make like
this Echo", a preset) the switch is applied before the other lines (`settings.c` `switches()`), and the wake word
engine is judged by the switch as it is then (`ps_community_keeps_off`), never by the scan's mark alone, which lags it
until the broker rescans: an export saying on with a community plugin as the engine (its lines in the export's order,
the engine first) had the engine refused (a review of 00d492a, 1).  The switch's dialog, and the install dialog of
every plugin that is not official (`web/plugins.js` `hmwall.of`, from the installing Echo's check: `check_json`
`sandbox`, `install_method`), have the warning above in full where the sandbox is "hassmic" or "su", worded by
`install_method` (the Makefile's `INSTALL_METHOD`, device.conf `INSTALL`): `twrp-ab` and `running` "run the system install again: `scripts/install-system.sh` (or `scripts/setup.sh
<model> --from install`)", `boot` the boot image flashed again from fastboot, `cannoli-ubi` the firmware ZIP built and
flashed again. Off: `ps_stage_check` refuses whatever is not official (a file, the index's community listing; a
listing under the build's least counts as a file's), at the check and again as the package is written; `ps_scan`
marks such a plugin installed `community_off` (refused with the switch's words: stopped, kept, its card says why, its
entities leave Home Assistant, the update's self test does not wait for it); a community wake word engine is down with words naming the switch and is not chosen while it is off
(`ps_engine_refused`). Browse shows community listings only with the switch on and counts the others. Nothing
else decides it: an Echo starts with it off unless its `state/config` (or a preset) says on.

## 3. The broker

`src/hassmic/plugin.c` (processes, the broker thread, capabilities), `plugmsg.h` (the wire format, header-only, the
same file in the SDK), `plugin_net.c` (sockets), `plugin_store.c` (packages, manifests, signatures, each plugin's
state). One broker thread polls every plugin's control socket; two worker threads do what can block (name lookups,
connects).

### 3.1 Framing and versions

Control socket: `SOCK_SEQPACKET`, one message per packet, 32 kB at most. Header `{ u16 type, u16 flags, u32 id }`,
little-endian, then the type's fixed fields; strings and blobs as `u16 length + bytes`, at the end. No JSON, no
protobuf: every type is a struct and a bounds check, and one fuzz target covers the file. `id` ties an answer to
its request; events have 0. Descriptors travel from the broker to the plugin only.

The first message is the plugin's `HELLO { sdk version; scopes: name, version ... }`, the answer `WELCOME { model;
node; name; clock set; zone; scopes: name, version ... }` or `REFUSED { why }`. Then `FILES` for each file list,
`CONFIG` (4.3), and the plugin's `READY`: the first `CONFIG` closes the start. What was granted is what is spoken
(core and the scope of each capability the owner accepted): `WELCOME` had a 32-bit set of capability bits beside it
until 2026-11-23, which bounded the protocol at 31 capabilities for good, since the greeting never grows (finding 5
of a review of 4e0a202); the greeting's lists of scopes hold 255 (`PM_SCOPES_MAX`), every scope there will ever be.
hassmic's own set of capabilities (`pm_caps_t`, `PM_CAPBIT()`, 64 bits) is never on the wire and grows when it must
(`tools/mkplugmsg.py` `CAPS_MAX` says when).

**Versions: scopes, and strict backwards compatibility** (decided by the maintainer 2026-11-22, built 2026-11-23;
`sdk/PROTOCOL.md` "Versions" is the rule as a plugin author reads it). The protocol is scopes, each with an integer
version from 1: `core` (the greeting, `CONFIG`, `READY`, `PING`, `STOP`, `TIME`, `OK`, `ERR`) and one per capability
of 3.3, named as it. Every message, every field, every enum value, every constant and every manifest key is part of
a scope from the version that brought it (`sdk/messages.def`: a message's scope column, `@N` after a scope, a field,
a value or a `keys` line). All of it is version 1 today. A capability's `CAP_*` number is its own scope's from its
version 1 (a capability more is a scope more; core does not change for it).

- **A scope's next version only adds**: a message, a field appended after every field of the earlier versions (in a
  list's element too), an enum value, a constant, a key of its capability's manifest entry. Nothing changes meaning,
  type, bound, number or direction, and nothing goes; what must break is a new scope, under a new name. A message of
  several scopes (`CONNECT`, `LOG`, `LOG_FOLLOW`, `NET`, `UDP_OPEN`, `UDP_OPENED`) never grows, nor the greeting
  (read before a version is known): what grows there is a new message.
- **Each version above 1 says whether it widens** what its capability lets a plugin do beyond the words of its
  install line (`version <scope> <N> widens|within "..."` in `messages.def`: required for every version above 1, no
  default, recorded in the lock with the version; finding 4 of a review of 4e0a202, where only a written rule kept a
  new version within the line). `requires` stays outside the accepted section: an update that requires a later
  version *within* asks for no button (its dialog lists it, "Speaks ha.entities 2, was 1"), one that comes to require
  a version that *widens* (any between the two) is wider (3.3) and asks; the dialog has a line for every version
  between the two, each in its own words (`plugin_store.c` `requires_wider`, `plugmsg.h` `pm_widens`,
  `pm_version_doc`), versions within one after another on one line ("Speaks state 2 to 4, was 1: ...; ...; ..."),
  after the capabilities' lines, and the last `PS_CHANGES_ASK` (8) of the dialog's 24 places only for lines that
  ask (finding 4 of a review of 4c55b54: a raise over many versions, listed first, could leave out a capability
  asked for anew; it still asked for the button, but the owner did not see why).  A first install and a capability asked for anew in an update show, beside the capability's
  line (version 1's words), the words of every widening version up to the one required (`ps_widens_json`: the
  check's and the card's `"widens"`, `web/plugins.js` `plgCapList`; `widening_lines` for an update's "New" lines;
  finding 5 of a review of 60a8589: one version's words, or none, were shown).  A version's words are 300 bytes at
  most (`PM_VERSION_DOC_MAX`), so every line is whole.
- **Held by the build**: `tools/mkplugmsg.py` refuses a field of a later version before one of an earlier one, `@N`
  past the scope's declared version, a grown multi-scope message or greeting, a version above 1 without its mark, a
  greeting's list of scopes of another bound than 255; `sdk/scopes.lock` records what every scope version had (each
  message's number, direction and layout with each field's enum, enum values, capabilities' numbers, constants,
  manifest keys, the version's mark), written the first time `messages.def` declares the version and never again,
  and `make unit-plugmsg` fails where `messages.def` makes a recorded version otherwise ("edits the history of button
  1").  **Tied to the releases** (finding 1 of a review of 4e0a202: an edit passed once a version's lines were first
  deleted from the lock; findings 1 and 2 of a review of 60a8589: only the newest tag by its time was read, though
  `main`'s betas and `release`'s releases each lack what the other has, and its lines only as a subset): every
  version that any release carried (each tag `v*` with a lock, betas of `main` and releases of `release`) is exactly
  what that release's lock had, in the lock and in `messages.def`, wherever either has it: nothing taken out, nothing
  added (a message added to `state 1` with its lock lines written by hand is refused).  Only a version no release
  carried may be deleted and recorded anew.  A version missing altogether fails where a release the tree descends
  from carried it (`git merge-base --is-ancestor`), and passes where only a release elsewhere did (a push to
  `release` without `main`'s newest beta); two releases that carried one version otherwise fail.  `make
  unit-plugmsg` asks where git and such a tag are (else it says it skipped), `tools/mkplugmsg.py --released` asks in
  CI (fetch-depth 0) and fails without tags, and a plain run never records anew a version a release carried.  A
  shallow clone holds nothing (finding 1 of a review of 4c55b54: `git merge-base --is-ancestor` answers "not an
  ancestor" for a tag past its boundary, so a version a release it descends from carried could go): `--released` fails
  there, any other run says it skipped ("a shallow clone").  Every tag's lock is read in one `git cat-file --batch`,
  each answer passed over whole whatever its type (finding 2: a tag whose lock was a directory lost every later tag's
  lock without a word); a lock that is no file fails `--released` and is named by any other run, which holds every
  other release all the same.  The release job asks once more, the tags fetched anew, right before it publishes
  (finding 6: `main` and `release` build at once, and both could add one version, each its own way; the way out of two
  such releases is in DEVELOPMENT.md "CI and releases").  The lock's head is `mkplugmsg.py`'s `LOCK_HEAD` word for
  word (finding 5: the lock still told the rule it had replaced): `--check` fails on another, a plain run writes it, a
  note of the lock's own stands below it after a blank line.  The lock was recorded anew once, 2026-11-23, before any
  release carried one.  `tests/unit/plugmsg_scopes/` proves the
  rules on a definition of its own (a scope at version 2 with appended fields, a value and its fallback, a key and
  its mark, its lock, and every way of breaking them), and `tests/unit/plugmsg_scopes_test.py` the releases in a
  repository of its own with both branches tagged in turn.
- **Values, constants and keys by version too** (finding 3 of a review of 4e0a202): a field that holds an enum's
  values says so (`enum:E` in `messages.def`); `pm_check` holds each such value to what the peer speaks (a later
  version's value, or one no version has: malformed, `ERR_BAD`), and both `psend` and the C client's writer hold
  every packet to `pm_check` before it goes, so no value a plugin does not speak reaches it and none a plugin sends
  is taken (the Rust client the same for what it reads and writes; the C client's answers to its requests too,
  finding 6 of a review of 60a8589).  A value of a version above 1 names its fallback, a value of an earlier version
  (`ELSEWHERE=7@2>STOPPED`; `mkplugmsg.py` refuses one without, recorded in the lock): `pm_<enum>_for` and
  `pm_fit`, which `psend` and the C client's `put()` run on every packet, send a peer of an earlier version that
  one, never dropping the message (finding 4 of a review of 60a8589: a version-1 plugin would have lost its
  `CALL_HANGUP` to the first new reason); the Rust client's `fit` and its tables, held to the vectors' `fit` lines;
  `scopes_demo.py`'s call 2 proves it both ways (`make unit-plugin_broker`, `unit-hmplugin`).  The fallback is the
  value an older peer should act on (the one the new value is a case of), never a failure or refusal turned into a
  success, a command into another command or an event into one that did not happen; where no earlier value is right
  the value names none, `>-` (finding 3 of a review of 4c55b54: every later value had to have a fallback, and a new
  key of the buttons would have reached a button-1 plugin as a press of the action button): `pm_<enum>_for` answers
  `PM_NOT_SENT`, `pm_fit` 1, and the packet is not sent to that peer at all, without a word (`psend` returns 0, done
  with, counts it and says it once a start, never as hassmic's error), recorded in the lock as any fallback
  (`MIC=4>-`), the vectors' `unsent` lines; `scopes_demo.py`'s button 2 (`BUTTON_KEY_MIC`, hassmic to the plugin)
  proves it.  As built (finding 1 of a review of 61e3d8e: `>-` passed on any enum, and an `ERR`, a `CALL_END`, an
  `ACTION` for its `from`, a `STATE` would have gone unsent, the clients waiting on an answer that never came,
  `action_send` and `entity_set` telling Home Assistant it went): `>-` only where the enum is what the message is
  about, an event whose whole point is the new value, on a message marked `drop` in `messages.def` (recorded in the
  lock with the message, `pm_droppable`; never the greeting, `OK` or `ERR`); `BUTTON` alone is.  Never an answer,
  an end, a state, a reason or qualifier: a new value there names the earlier one it is a case of, or it is a new
  message (`mkplugmsg.py` refuses `>-` on an enum a message not marked `drop` holds; `plugmsg_scopes_test.py` breaks
  it on `ERR`, `CALL_END`, `CALL_HANGUP`, `ACTION`'s `from`, `MUSIC_CMD`'s `why`, `STATE` and `MUSIC_STATE`).  So
  `psend`'s 0 for a packet left unsent is only ever `BUTTON`'s, whose one sender reads nothing of it; any other is
  hassmic's error (-1), and `plugin_button` asks before `bucket_take` (a key a plugin does not hear spends none of
  its five a second).  A plugin's own messages never go unsent for a value: `scopes_demo.py`'s audio.music 2 brings
  `MUSIC_BUFFERING` with its fallback `PLAYING` (a state is never left unsent: the last one told would stand), and
  `mkplugmsg.py` refuses `drop` on a plugin's own message (`>`; a review of 6631974).  The clients refuse a request
  of a message not spoken (its scope not required, or a later version of it) with a code of their own, `0xffff`
  (`HM_ERR_NOT_SPOKEN`, the Rust client's `ERR_NOT_SPOKEN`; `messages.def` keeps `ERR_*` below it); a value of none
  of its versions is `HM_ERR_BAD`, as any request that is wrong.  `pm_fit` rewrites a packet in place for its one
  peer: `psend` and `put()` take a `struct pm_w` they mark (`fitted`) and refuse it a second time (one encoded
  buffer, one peer; finding 7).  A message of several
  scopes holds core's enums and `CAP` only (finding 3 of a review of 60a8589). `CAP_*` values (`CONNECT`'s, `UDP_OPEN`'s kind)
  are any of this build's capabilities: one not granted stays `ERR_DENIED`. `PM_CONST_SPOKEN(NAME, ver)`: a
  constant of a later version applies only to a peer that speaks it. The manifest's keys are `keys` lines (where
  each stands: `manifest`, `setting`, `setting.item`, `net.lan`, `ha.entities.<kind>` ...), `plugmsg.h` `pm_key`:
  a key of a later version than the manifest requires of its scope refuses the manifest (`plugin_store.c`
  `keys_versioned`; where this build lacks that version, the manifest's `requires` says "needs a newer hassmic"
  first), and the plugin index's `manifest_check` holds the same table (`KEYS`, held to `messages.def` and to the
  Echo's code built as a later hassmic's by its `make check`, `tests/unit/scopes_demo.py`).
- **A manifest names its minimum per scope**: `"requires": {"core": 1, "wake": 1, "files": 1}`: core and the scope
  of each capability it asks for, and nothing else, whole numbers from 1 (`plugin_store.c` `requires_parse`, read
  before anything a scope's later version may bring). hassmic runs it where it has each at that version or a later
  one; else "needs a newer hassmic: `<scope> <n>`", at install and at every scan (a fall back to an older hassmic:
  the card says so, the package stays, `ps_plugin.unread` with `m.newer`, the self test does not wait for it). A
  scope name hassmic does not know is a later hassmic's, said the same way, never misread.
- **What is spoken with a plugin is its requires, exactly**: hassmic sends it only messages (and appended fields,
  enum values) of the versions it required (`plugin.c` `psend` holds every packet to `pm_check` at those versions,
  refuses anything else and says so: hassmic's own error), and takes only those: a type of a capability not granted is `ERR_DENIED` as ever, one of a granted
  capability's later version `ERR_UNKNOWN`, as a hassmic of that version answers. A packet is exactly its layout at
  that version (bytes after its last field are malformed). The client says in `HELLO` what it can speak (the C
  client: every scope of its `messages.def`), hassmic checks the manifest's requires against it (`REFUSED_SCOPES`
  where the client is older than its manifest) and answers in `WELCOME` with the versions spoken. A plugin's code
  declares nothing: one place, the manifest, names what it speaks, so code and manifest cannot disagree, and one
  build of a plugin runs under any manifest that requires what its SDK speaks.
- An unknown type is answered `ERR_UNKNOWN`, never a reason to end.
- The index lists each version with its `requires`; the Echo installs the highest whose requires it meets
  (`plugin_fetch.c` `pf_best`), and the page says what a version needs that this hassmic lacks. A required version
  is kept as written, a u16 (`plugin_index.c` `pi_version.req`; one above 255 was kept as 255 and looked met on a
  build at 255: finding 6 of a review of 4e0a202). Browse offers no "Update" for a plugin installed under a newer
  hassmic (a fall back) where the index has nothing newer for this one: "Installed, needs a newer hassmic" (finding
  2: it fetched the older version, which the Echo refuses as not newer).  Nor for one whose manifest this build
  does not read otherwise, where its listing names a version the index's is not newer than: its card says that
  version (`ps_scan` by `pkg_version`, as "never back" holds it), the row "Installed, version N: not one this
  hassmic reads" (`hmbrowse.offer` "unread"; finding 7 of a review of 60a8589).

*Before 2026-11-23* the protocol had one number, "API 0.x": a minor of 0 could break, a plugin named the exact minor
it was built for, and hassmic ran only that. Seven minors were made (0.1 to 0.7), each a capability or two more, and
the microWakeWord plugin needed a rebuild for each, also for the purely additive 0.7; an update of hassmic needed
each plugin's build for the new minor first, with packages kept side by side per minor (5.9). None of it was
released. Where this document says "API 0.N" elsewhere, it is that history: everything a minor brought is version 1
of the scopes it brought.

### 3.2 Rules of the broker

- **hassmic never waits on a plugin.** Every send to one is `MSG_DONTWAIT`. The capture thread (issue #77) only
  ever does such a send; a full queue drops the frame and counts it. *Measured* on biscuit: a 640-byte frame there
  and back over a `SOCK_SEQPACKET` pair takes 38 µs on average and 7 ms at worst on a busy Echo, so no path of
  hassmic's may contain the "and back".
- **No thread that may wait touches a socket a plugin holds**, except with `MSG_DONTWAIT` on a socket whose mode
  the plugin cannot change (2.4). A TCP socket is made, connected and then given away whole; hassmic keeps no copy
  (4.6).
- A plugin's messages are hostile input: lengths, ids, states and rates (200 messages a second, then dropped and
  counted, then the watchdog).
- A handler takes `core_lock` as any other caller, briefly, and never calls out while holding it.
- What a plugin says is shown as text and nowhere interpreted: not as HTML, not as a path, not as a format string.

### 3.3 Capabilities

**One capability, one power, one line the owner accepts.** A manifest names each capability it wants, with its
parameters; the install screen shows one line per capability, in the words below with the parameters filled in,
and the owner accepts the list as a whole with the action button (5.4). Nothing comes with something else: a
music plugin that also wants to set the volume asks for both, a plugin that keeps a cursor asks for storage. A
message of a capability that was not accepted is answered `ERR_DENIED` and counted.

Without any capability a plugin can start, read its own settings (4.3), write lines under its own name into the
log (50 a minute) and learn the Echo's model, name, API version, clock state and time zone. That is all.

| Capability (parameters) | The one thing it gives | The line at install |
|---|---|---|
| `storage` (kB) | keep data of its own on the Echo (4.4) | "Keeps up to *N* kB of its own data on this Echo" |
| `files` (lists, MB) | read files the owner uploads on its card (4.4) | "Reads the files you give it on its page (*what*, up to *N* MB)" |
| `state` | events: idle/listening/thinking/speaking, mute, volume | "Sees when this Echo listens and speaks" |
| `log.read` | hassmic's log without what was said and without names and addresses (4.5) | "Reads this Echo's technical log" |
| `log.read.full` | the log as the page shows it | "Reads this Echo's whole log, including what was said" |
| `net.server` (settings, ports) | sockets to the host the owner writes into a named setting | "Connects to the server you enter (*ports*)" |
| `net.lan` (ports) | sockets to private addresses, never at DNS's ports 53 and 853 (the router would ask the internet; a device that relays by other means still can: 4.6 "As built") | "Connects to any device on your network (*ports*)" |
| `net.internet` (ports) | sockets to public addresses (one on the network, also by a name, is `net.lan`'s; the router's own public address is a public one and answers from the LAN side: its admin page, a port forwarded into the home: 4.6 "As built") | "Connects to the internet" |
| `net.udp` (count) | UDP ports of its own, reachable from private addresses | "Can be reached from your network (*N* ports)" |
| `net.discover` (port, broadcast or group) | send a search to the network and hear the answers | "Searches your network for its server" |
| `audio.music` | play on the music stream, as a music source beside Sendspin and Bluetooth | "Plays sound on this Echo's speaker, whenever it likes" |
| `audio.volume` | set the Echo's volume | "Changes this Echo's volume" |
| `call` | a call's microphone and speaker while a call is connected | "Makes and takes calls: a call coming in is answered only at the Echo, one goes out only when Home Assistant or the settings page asks. Hears the room during a call" |
| `wake` | the microphone stream the wake word engine gets, all the time; reports wake words | "**Hears this Echo's microphone all the time**, to listen for its wake words" |
| `request.audio` | a copy of each request's audio as sent to Home Assistant | "**Hears everything said to this Echo after the wake word or the button**" |
| `request.text` | each request's transcript and reply, as text | "Reads what was said to this Echo and what it answered" |
| `ha.entities` (list) | entities in Home Assistant, named by the manifest | "Shows in Home Assistant: *the list*" |
| `ha.actions` (list) | actions Home Assistant, or a button on its card, can call, named by the manifest | "Home Assistant can tell it to: *the list*" (an action marked `calls`: "...and have it place a call") |
| `button` | the presses and holds of the Echo's buttons that Home Assistant's "Button" event entity hears, as they come | "Hears presses of this Echo's buttons, as Home Assistant's "Button" does" |

The network kinds are what earlier sections write as `net` entries (4.6): `net.server` is `"to": "setting:<name>"`,
`net.lan` is `"to": "lan"`, `net.internet` is `"to": "any"`, `net.udp` and `net.discover` are the entries of those
names. Each is accepted on its own line.

**What the owner accepts** is more than the capability lines, because a plugin's settings and names are also
what the owner trusted: the *accepted section* of a manifest is its capabilities with all parameters (entities
and actions are inside it, 4.2), its settings whole (each row's name, type, label, help, unit, default, limits and
options, a file list's files, fields and catalog), the plugin's name and author, and what it says of itself where
the owner reads it: its `description` (the text on its card above its settings) and its `source` (the address the
card's "Source and issues" link opens in the owner's browser). Its digest is what `accepted` holds (5.4).
(First: of a setting only name, type and label. A file list's label is in the `files` line the owner
accepts, its suffixes and sizes are what that line means, its catalog names where the owner's browser fetches
from, a row's help and default are what the owner reads and what the plugin gets: all of it was outside. Then,
until the sixth read (13), without description and source: an update could make the card say "Type your Home
Assistant password into Port", or send the link to another site, with nobody asked.)

Outside it, and free to change with any update: `version_name` and `license`. They are 24 and 32 bytes of text
without control characters, and the page shows each only as what it is: the first in brackets beside the version's
number ("version 7 (1.2.0)"), never in its place, the second after the word "Licence". Neither is a place for a
message to the owner.

**A capability's parameter that names a settings row names one the manifest has** (`"setting:<name>"` as a `to`
or as `ports`, and an entry's `"setting": "<name>"`, 4.6; not a file list): a manifest that names a row it lacks
is refused. That also ties the two for updates: a row a kept capability names cannot be taken out, because the
manifest that does so is none.

**What an update may be without the button** (5.4), and nothing else: the accepted section as it was, or less
of it.

- *The capabilities*: each as accepted, value for value (an object's keys in any order, a text by what it says,
  a number as it is written), or gone; a smaller `storage` or `files` number.
- *The settings*: each row as accepted, key for key, the rows in their order, or a row gone; a smaller
  `max_items` of a file list; a smaller `max_kb` of an item's file.

Everything else asks for the button and is named: a capability that was not accepted, another row (of any type:
"Your alarm system's code" is a number), a row's label, help, unit, default, limits, options, files, fields or
catalog, a capability's parameter that is not the same (larger, smaller where no rule above says smaller is less,
in another form, an entry more or less, an entry changed, the entries or the rows in another order, a key this
version does not read), a changed plugin name or author, a changed description (shown: the new text) or source
(shown: the new address). No list is "a list of grants of which one may go": until the sixth read (13) an entry
less of `net.server`, `net.lan`, `net.internet`, `ha.entities` or `ha.actions` counted as less, for every form
such a list might take, and so `[80, 443]` could become `[]` (which may come to mean "any") and an entry that
restricts could go. None of those capabilities is built, so nothing was reachable; the rule is gone before one
is. No parameter is taken for a quantity because it is a number:
`net.discover`'s is a port. The first rule was the other way round: it named what counts as wider and let the
rest by, and the rest was most of it (13, the fifth read). A capability that comes to be built brings its own
rule for what is less, if it has one.

**A later version of a capability's scope** (3.1) is outside the accepted section: `requires` says what of the
protocol a plugin speaks, not what it may do. But a version may let a plugin do more than its capability's line
says (an entry's key that names a host where only addresses were, say), so each version above 1 is marked in
`messages.def` as one that **widens** or one **within**, once and for good (the lock records it). An update that
comes to require a version that widens, or any version above a widening one, is wider than what was accepted: "More:
Speaks net.lan 2, was 1, which lets a plugin do more: ..." and the button, as for a capability more. One within is
shown ("It speaks a later version of the plugin protocol: Speaks ha.entities 2, was 1: ...") and asks for nothing.
Decided 2026-11-23 (finding 4 of a review of 4e0a202): the owner's rule that only install and wider updates ask
holds; a widening version is wider.

**Combinations.**

- **`wake` is sealed** (decided by the maintainer, 12). A plugin with `wake` may have, besides it, only `files`
  and `storage`. Not any `net.*`, not `ha.entities`, `ha.actions`, `request.*`, `log.*`, `state`, `call`,
  `audio.*`: nothing that carries data out of the plugin. The broker refuses to start a plugin whose manifest says
  otherwise, whoever signed it, and an update cannot add one. What is left is what it has to say to do its job:
  which wake word, where in the stream. A seal is worth what its leaks are, so each known one is closed or
  named:
  - *Free text.* A wake word's phrase goes to Home Assistant (the selects, the run's wake word phrase, the "Wake
    word" event). So the plugin does not send phrases at run time: an item's title is taken once, when the owner
    uploads it and sees it (`FILE_OK`, 4.3), kept by hassmic, cut to 40 characters of letters, digits and spaces,
    and `WAKE_WORDS` carries item ids only. Why a model failed is a code from a short list, not text.
  - *The log.* A wake plugin's lines go to the log for the owner, and are left out of what any plugin with
    `log.read` or `log.read.full` gets: the forwarder must not be its way out.
  - *Storage that outlives the seal.* An update that drops `wake` is not an update but a new install: files
    and storage are wiped first. A wake plugin's storage does not sync unless ticked, as any (5.4). *As built
    (2026-11-21, `plugin_store.c` `seal_upkeep`):* such an update asks for the button ("No longer a wake word
    plugin. The files you gave it and the data it kept while it heard the microphone are deleted once this
    version runs"), and once it is accepted and is the package that runs here, `files/` and `storage/` are wiped
    (a mark `sealed` says the package accepted before had `wake`); nothing before, so an owner who never accepts
    it, or a fallback, keeps all of it. Its settings stay: the owner wrote them. A wipe that leaves a file behind
    keeps the mark and is done again at the next scan (`rm_rf` says whether anything is left). microWakeWord's
    models are the plugin's own copies (5.9: the move copies them; the built-in engine's folder `state/mww` is not
    its list since 2026-11-21), so they go with the rest, and the built-in engine's stay. The wipe waits for the
    process of the version with `wake` to be gone (`ps_busy`, asked of the broker: a process of another package of
    the plugin still running), and is done before the new version starts (`plugin.c` `program_open`,
    `ps_seal_settle`); a process on its way out writes its plugin's storage until it is gone, and none once its
    plugin was removed (`plugin.c` `store_mine`: `revoked`, read under the store's lock). Before (a review of
    0dabfa6, R1): the wipe ran as the update was installed, the old process, told to stop, still wrote its storage
    until it was gone (the broker answers storage writes after `STOP`), and the mark was gone; and a removed plugin's
    process wrote into the storage of the same id installed again at once. The rule after that review held a process
    to the package it was started under (its manifest digest against the store's): every ordinary update then lost
    the cursor a forwarder writes in its stop callback, and the seal gains nothing by it, since the wipe already waits
    for the old process (a review of 7144548, 1).
  - *A partner.* False detections open the microphone, 20 runs in 10 minutes at most (4.13), to whatever hears
    requests. So no plugin with `request.audio` runs on an Echo whose wake engine is a **community** plugin, or one
    the index withdrew that the owner runs anyway; the Echo refuses the second of the two, whichever comes second.
  - *Soft mute.* Home Assistant's mute switch does not stop hassmic's feeding of its own engine
    (`main.c:3548` feeds whenever the wake word is local); only the hardware latch cuts the microphones. A
    plugin's stream stops on either: while `core_muted()` no block is sent, and the positions show the gap.
  - *Left open, accepted:* when a detection is reported (each starts a run that is seen and heard), and how
    much CPU the plugin uses, which another plugin on the same Echo could time. Both carry bits, not audio. Of
    the same kind: where a detection lies on the stream, which the core uses (the replay's start, the aligner, the
    wake word's tail: `wordtail.c`, the whisper detector) and logs unmarked as the core's own lines, so a plugin that
    places `begin` and `end` within what `heard_ok` takes carries about 12 bits a detection into the public log
    (one a second at most, twenty in ten minutes before it is taken as broken). The same order as the timing, and
    named as it is (docs/SECURITY.md); sealing the core's own lines for it would seal most of the wake path's log.
  The price of the seal: a wake word plugin cannot fetch its own models or report statistics; the owner uploads
  models (4.13).
- `request.audio` is the widest thing a plugin that is not sealed can be given. A community plugin with it is
  installed only after the strongest warning (2.7's wording about an unknown author, with "it will hear every
  request" on top), whatever the sandbox's state. Together with any `net.*` capability the Echo wants a second press of the action button, asked for on a
  screen of its own: "This plugin hears every request and can send what it hears to *the internet / your
  network / the server you enter*." The second press is the Echo's rule, not the page's: an install or a sync
  that skips the screen still waits for it. (`ha.entities` gets no second press: what goes there goes to the
  owner's Home Assistant.)
  The index does not list a **community** plugin with `request.audio` and `net.internet` together (the build job
  refuses it); with `net.lan` or `net.server` it is listed and flagged. An official plugin may have both.

*As built (2026-11-21, phase 4):* `state`, `net.udp`, `net.discover`, `audio.music`, `audio.volume`,
`ha.entities` and `ha.actions` are built (`ps_caps_built`; scopes since 2026-11-23, 3.1), each with its line in the owner's words and its
parameters filled in: "Can be reached from your network, and sends to devices on it (2 UDP ports of its own)",
"Searches your network for its server (UDP port 3483, to every device on it)" (or "to the group 239.255.255.250"),
"Shows in Home Assistant: "Position" (a number), "Shuffle" (a switch), ..." (each entity by its label and kind), "Home
Assistant can tell it to: "Search for the server" (also a button on its card), ...".  Their parameters are read as
net.server's are: a key this version does not read refuses the manifest (4.6, 4.10 "As built" have each form).
Of "less", one rule more: `net.udp`'s count made smaller needs no button (its ports are kept in order, the ones that
stay are the first ones, `ps_udp_ports`); every other change of these parameters asks, an entity or an action gone
included (no list is one of grants), `net.discover`'s port and group included (a port, not a quantity).  `wake` stays
sealed against all of them (`combination`, unchanged).

Button events and rows of a plugin's own on its card, left for a later minor when phase 4 was built, are API 0.7's
(below).

*As built (2026-11-21, phase 5):* `call` is built (`ps_caps_built` then; a scope since 2026-11-23), with its line "Makes and takes calls:
a call coming in is answered only at the Echo, one goes out only when Home Assistant or the settings page asks. Hears
the room during a call" (its first half since 2026-11-22, 3.6), and an action of `ha.actions` may be marked `"calls": true` (true or false, nothing
else, and only with `call`: a manifest that marks one without it is refused), said in that action's part of the
line: "Home Assistant can tell it to: "Call" (and have it place a call), "Answer" (also a button on its card; and
have it place a call)".  No rule of "less" for it: `call` added, an action marked or unmarked, each asks for the
button (an action is a value of `ha.actions`, compared value for value).  `wake` stays sealed against it
(`combination`: a call carries the room out).

*As built (2026-11-22, phase 6):* `request.audio` and `request.text` are built (`ps_caps_built` then; scopes since 2026-11-23), both
parameterless (`true`, nothing else: `CAPS_PLAIN`), with the lines of the table; the page shows `request.audio`'s (and
`wake`'s) in bold, in the dialog and on the card, as the table has them.  No rule of "less" for either: added, each
asks for the button (a capability not accepted); gone, it is less.
- *The second press* is the store's (`plugin_store.c` "request.audio's two rules", `ps_second`): wherever the button
  is asked for at all (a first install, an update wider than what was accepted), a package with `request.audio` and
  any `net.*` (`net.server`, `net.udp`, `net.discover`; since API 0.6 `net.lan` and `net.internet` too) is written only
  with `PS_PRESSED2` (both presses here) or `PS_VOUCHED_PRESSED` (a voucher for the first, held to the voucher's own
  rules again, and this Echo's press for the second); anything less is `PS_NEEDS_SECOND`, nothing written.  An update
  that is what was accepted or less asks for neither.  So adding a `net.*` to a `request.audio` plugin, or
  `request.audio` to one with a `net.*`, asks for both presses.  The screen of its own is the page's (`web/plugins.js`
  `plgSecond`), its words the Echo's (`ps_second`): "This plugin hears every request and can send what it hears to
  the server you enter." (or "your network", "the internet", joined); the page's dialog says beforehand that two
  presses will be asked for.  `web.c` "the install slot": after the first press (or a voucher) the slot is
  `PL_SECOND`, which arms nothing by itself; the browser asks again from the screen (`/api/plugins/install`), and only
  then is the second press armed (`plugin_ask`, `pl_press.press`).  Decided: the second wait is armed by the screen's
  ask, not by the first press: a second press of the button right behind the first ("did it take?") is then no second
  acceptance, and a page that skips the screen still has to ask, and the Echo still waits for the press.
- *The partner* (`ps_partner`, `ps_engine_refused`): "community" is read as "not official" (community or from a
  file): the index does not list it as official; and a withdrawn one counts the same, also when the owner runs it
  anyway (`plugin_store.c` `vouched`: official and not withdrawn; the index took its word back.  Until a review of
  b294fa0, C4, an official engine the index withdrew and the owner ran anyway passed as vouched for); a new
  revocation list asks the rule again (`plugin_withdrawn_changed` sets `partner_due`).  Enforced at install (a package with `request.audio` while the
  engine is such a plugin, and the engine's own plugin installed or updated to such a tier while a `request.audio`
  plugin is switched on), at the engine's choice (`main.c` `core_wake_engine`: the page's or Home Assistant's,
  refused with the reason), and at start (`plugin.c` `start`: the `request.audio` plugin is the one not started, its
  card saying why; `partner_check` when the engine changes, which stops one that runs and starts one that was held).
  The engine is the core's word (`plugin_wake_engine` -> `ps_engine_set`).
- *The strongest warning*: 2.7's community switch and its warning are built (2026-11-22, 2.7 "As built"): no plugin
  that is not official is installed or runs before the owner switched community plugins on (on the Plugins page,
  after the warning in full where the sandbox misses its SELinux wall, or with the other settings: an import, a
  preset, a copy), and each one still with the action button, in a dialog that has that warning in full too. A plugin that is not official with `request.audio` also
  gets, in its install dialog, the warning of its own on top ("It will hear every request: ..."), above the one every
  such plugin gets.

*As built (2026-11-22):* `net.lan`, `net.internet` and `log.read.full` are built, the last three of the
table (`ps_caps_built` had every capability then; since 2026-11-23 each is a scope of version 1, 3.1).  `net.lan` and `net.internet` take a list of 1 to 4 entries of
exactly `proto` (`tcp`, `udp`) and `ports` (1 to 8 ports, never 5555, or `"any"`), read as net.server's are (a key
this version does not read refuses the manifest; `plugin_store.c` `reach_parse`), and their lines name each entry:
"Connects to any device on your network (TCP, ports 80, 443; UDP, any port)", "Connects to the internet (TCP, port
443)" (the table's words with the ports filled in, as net.server's line names its servers).  No rule of "less": the
entries and their ports value for value, an entry gone or in another order asks (3.3's rule, `wider`'s default).  The
combinations: `wake` sealed against both, unchanged (`combination`: anything but files and storage); `log.read.full`
official only (`combination`, asked at install with the listing's tier and again at every `ps_scan`, so a plugin whose
listing is gone or under the build's least is not started and says why: the scan holds the combination to a listing
under `pi_least_index()` as to none, while the tier shown and held for updates stays the one the plugin came with,
5.5; until the review of 7be9343, N4, the scan read the listing's serial and dropped it); `request.audio` with either asks for the
second press (`ps_second`: "the internet", "your network"), unchanged in its code since phase 6 named both.  Decided:
a withdrawn official plugin with `log.read.full` that the owner runs anyway keeps the whole log (the press for that
list was the owner's, after the index's reason; the partner rule counts such an engine as unvouched because its runs
feed another plugin, which no press of the owner's covers).

*As built (2026-11-22):* the last two pieces the phases left, `button` and rows of a plugin's own on its
card.
- **`button`** is built (`ps_caps_built` then, a scope since 2026-11-23), parameterless (`true`: `CAPS_PLAIN`), with its line "Hears presses of this
  Echo's buttons, as Home Assistant's "Button" does".  `BUTTON { key, how }` (3.4) goes to every running plugin
  with it from the one place Home Assistant's "Button" event entity is told (`main.c` `button_event`, which
  `on_action`, `on_held`'s caller and `buttons.c` call), right after it: so every exclusion that entity has holds by
  construction, and none is a rule of the broker's that could drift from it.  Never the press that served a wait for
  the button (`web_approve`: a login, adb, a plugin's install, a withdrawn plugin run anyway;
  `improv_authorize`: a phone's setup), never the hold that served a note (`on_held`, `buttons.c` `held`), never a
  skip of a double or triple press.  The plugin hears and does nothing else: no answer, and the press does on the
  Echo what it does anyway.  Five a second per plugin, ten at once (`BUTTON_PER_SEC`, `bucket_take`; the rest
  counted in the log, a line once in `REFUSAL_MS` and the count after it, `button_at`/`button_n`: a count of their
  own, not `refusal`'s, since the pace is a person's or a bouncing key's and not the plugin's doing: not in its
  `dropped`, and never folding the plugin's own refusals into "and N more" for 10 s; a review of 541f0a8).  No rule of "less": added, it asks for the button (a capability not accepted);
  gone, it is less.  `wake` stays sealed against it (`combination`: only `files` and `storage` beside `wake`, a list
  of what is allowed, unchanged).  What a plugin learns by it is when a person is at the Echo and touches it, and
  which key: docs/SECURITY.md says so.
- **Rows of its own**: an entity of `ha.entities` may be marked `"page": true` (true or false and nothing else, as an
  action's `page` and `calls`; on any kind with a state, refused on a `button` and an `event`, which have none to
  show), and its state is a read-only row on the plugin's card, under its label, inside the plugin's frame (4.3 "As
  built", API 0.7).  Showing a row adds no power: it shows the owner what Home Assistant is told anyway, under a label
  the owner accepted with `ha.entities`.  So a change of `page` alone is not wider (`wider`'s `X_ENT`: the key left
  out of the comparison, `x_skip`), while any other change of the entity asks as before; and the install's line is
  unchanged by it.  `page` is part of `ha.entities` 1 (3.1: no minor of its own to require since the scopes; it
  was "API 0.7 and later" before).
- Decided where the brief left room: the rows read only, no control on them (a switch is set from Home Assistant, as
  before); values as `ENTITY_STATE` set them, no message of their own; the value carried in its row only, not again
  among the card's entities (16 texts of 255 bytes a plugin are much of `/api/plugins`); rows only while the plugin
  runs and Home Assistant lists that entity (`ha_entity_out`: its key kept too), so a stopped plugin's last values are
  not shown as if current, and an entity whose key could not be written down, which is not in Home Assistant, has no
  row and keeps its "Not listed in Home Assistant yet" notice among the entities; a press of a
  volume key held is one hold, as Home Assistant's.

### 3.4 Messages

`>` plugin to broker, `<` broker to plugin. Every request has an `OK`/`ERR` answer unless noted. The phase (8) a
group arrives in is in brackets.

| Area | Messages |
|---|---|
| Core [1] | `> HELLO`, `< WELCOME`/`REFUSED`, `< CONFIG { name = value ... }` at start and on every change, `> READY`, `< PING` `> PONG`, `< STOP` (2 s to end), `< TIME` |
| Files [1] | `< FILES { list, items: id, fields, file names and sizes }` at start and on every change; `> FILE_OPEN { list, item, file }` `< fd` (read-only); `< FILE_CHECK { list, item }` `> FILE_OK { title }` / `> FILE_BAD { why }` |
| Wake [1] | `> WAKE_WORDS { id, phrase ... }`; `< WAKE_OPEN { slot, word }` + `fd` (the stream, the first time) `> WAKE_READY { slot }` / `> WAKE_FAILED { slot, why }`; `< WAKE_CLOSE { slot }`; `< WAKE_RESET`; `> WAKE_HEARD { slot, begin, end, score }` |
| Storage [2] | `> STORE_GET/PUT/DEL { key }` (as built: `STORE_PUT { key, value }`, `STORE_GET` answered `< STORE_VALUE { value }`) |
| Log [2] | `> LOG_FOLLOW { from: cursor or "start" }`, `< LOG { cursor, lines }` (as built: `LOG` also has `gap`; `from` empty: from now) |
| Net [2: `CONNECT`; 4: the rest] | `> CONNECT { entry, host, port }` `< fd` (connected; as built: `{ kind, entry, host, port }`, `kind` the capability's bit, for TCP and UDP alike, 4.6); `> UDP_OPEN { entry }` `< fd, handle, port`; `> UDP_PEER { handle, address, port }`; `> SENDTO { handle, address, port, data }`; `> BROADCAST { handle, data }`; `> PROPOSE { setting, value }`; `< NET { address, mask, up }` |
| State [4] | `< STATE` |
| Music [4] | `> MUSIC_OPEN { rate, channels }` `< fd`, `> MUSIC_STATE { playing/paused/stopped, title, artist }`, `> MUSIC_CLOSE`, `< MUSIC_POS { frames played, at (monotonic µs), queued µs }` 5/s, `< MUSIC_CMD { pause, resume, stop, next, previous }`; with `audio.volume`: `> VOLUME_SET`, `< VOLUME` |
| Entities, actions [4] | `> ENTITY_STATE { name, value }`, `> EVENT { name, type }`, `< ENTITY_SET { name, value }`; `< ACTION { name, args, token }` |
| Call [5] | `> CALL_INCOMING { who }`, `< CALL_ANSWER` / `< CALL_HANGUP { why }`, `> CALL_OUT { token, who }`, `> CALL_RINGING_THERE`, `> CALL_LIVE` `< fd`, `> CALL_END { result }` |
| Request [6] | `< REQ_START { run, source: wake word / button / follow-up / Home Assistant's wake word, slot, phrase }` + `fd` (the first time), `< REQ_END { run, why: Home Assistant heard enough / end heard here / cancelled / timeout / error / link lost; samples sent, samples dropped }`; with `request.text`: `< REQ_TEXT { run, heard / reply, text }` |

Phase 1's set is 23 types as built (Core with `TIME`, `OK` and `ERR`; Files; Wake: `sdk/messages.def`), phase 2's
seven more (API 0.2: `STORE_GET`, `STORE_VALUE`, `STORE_PUT`, `STORE_DEL`, `LOG_FOLLOW`, `LOG`, `CONNECT`; `ERR`
gains `FAILED` and `UNSET`; a field type `bytes`, a string's form without the rule about NUL); all of it about 50. Every `REQ_*` message goes from the broker to
the plugin: there is none by which a plugin starts, ends, answers or changes a request (4.9). A plugin starts a
run in one way only: a `wake` plugin's `WAKE_HEARD`, which the core treats as it treats its own engine's.

*As built (2026-11-21, phase 4), 19 types more, 49 in all:* `UDP_OPEN { kind, entry }` answered `UDP_OPENED
{ handle, port }` with the socket (`kind` the capability's bit, as `CONNECT`'s, so that one message opens net.udp's
ports and net.discover's socket), `UDP_PEER`, `SENDTO`, `BROADCAST` as in the table, addresses as a `u32`
(a.b.c.d as `a << 24 | ...`), `PROPOSE { setting, value }`, `NET { address, mask, up }`; `MUSIC_OPEN { rate,
channels }`, `MUSIC_STATE { state, title, artist }`, `MUSIC_CLOSE`, `MUSIC_POS { frames, at, queued }`, `MUSIC_CMD {
cmd, why }` (why: the button, the stop word, another source or a Drop In call, Home Assistant), `VOLUME_SET`,
`VOLUME`; `STATE { state, muted, volume }`; `ENTITY_STATE`, `EVENT`, `ENTITY_SET`, `ACTION { name, from, args }`
(`from`: Home Assistant or the card's button; the call token of 3.6 comes with `call`, phase 5, as a field appended).
What a plugin sends many of (`MUSIC_STATE`, `SENDTO`, `BROADCAST`, `ENTITY_STATE`, `EVENT`) is answered only when it
carries an id: the SDK sends them without one and hassmic says a refusal in its log (once in 10 s, the rest counted).
None of them before `READY`.

*As built (2026-11-21, phase 5), 7 types more, 56 in all:* `CALL_INCOMING { who }`, `CALL_ANSWER`, `CALL_HANGUP
{ why }` (the button, spoken, the screen, not answered, not live in time after an answer, the plugin stopped),
`CALL_OUT { token, who }`, `CALL_RINGING_THERE`, `CALL_LIVE { token }` answered with the audio socket, `CALL_END {
result }` (hung up, busy, declined, not answered, failed there); `ACTION` gains `token` (after `from`, before
`args`: the wire changed, so 0.4).  `CALL_LIVE` has a token field, against the table above, which hassmic does not
look at (since 2026-11-22: a token is for `CALL_OUT` only, and a call coming in goes live only after a person
answered it at the Echo, 3.6; the field stays on the wire, a plugin sends it empty).  `CALL_RINGING_THERE` is
answered only when it carries an id.  None of them before `READY`; five a second, a ring refused busy one a second
(3.6).

*As built (2026-11-22, phase 6), 3 types more, 59 in all:* `REQ_START { run, source, slot, phrase }`,
`REQ_END { run, why, samples, dropped }`, `REQ_TEXT { run, kind, text }` (`sdk/messages.def`: `REQSRC_*`,
`REQEND_*` with `MUTED` beside the table's, `REQTEXT_*`, `REQ_SAMPLES_MAX`).  `REQ_START` and `REQ_END` are
`request.audio`'s, `REQ_TEXT` `request.text`'s: a plugin with `request.text` alone gets no `REQ_START`, and the text
carries the number of the request begun last.  `REQ_START` comes with **a socket of its own for each request**, not
only the first time (3.5 "As built").  None is the plugin's to send: answered `ERR BAD` (they are '<').

*As built (2026-11-22), 1 types more, 60 in all:* `BUTTON { key, how }` (`button`; `BUTTON_KEY_*`: the action
button, volume up, volume down; `BUTTON_HOW_*`: pressed, held a second), an event (id 0, no answer), from the broker
only, none before `READY`, five a second (3.3 "As built", button and rows).  The rows of a plugin's own (an entity's
`"page"`) need no message: `ENTITY_STATE` is theirs too.

### 3.5 Audio

Sockets, not shared memory. A shared ring would spare two copies of 640 bytes per 20 ms and cost a mapping that both
sides index (every index of a hostile peer checked on every use) and `mmap` of a descriptor in the filter. At 50
frames a second the copies do not show; music is 192 kB/s.

- **Music** (plugin -> speaker): a `SOCK_STREAM` pair, s16le at the rate and channels of `MUSIC_OPEN` (8..48 kHz, 1
  or 2). A thread of the broker's per stream reads it (non-blocking, with `poll`) and calls `music_write`, which
  blocks at playback speed; the socket's buffer is the back-pressure, the plugin's write says `EAGAIN` and it
  polls. Before the mixer the broker applies what `sendspin.c` applies itself today: the duck (`duck_active`,
  `settings_duck_gain`), `audio_music_gain` (in the backend), `musiclight_feed`. `MUSIC_POS` is what a player needs
  to say where it is and to keep a group in step (Lyrion's `STAT`): frames that reached the speaker, when, on the
  monotonic clock, with the output's own latency (`music_queued_us`, the 70 ms after the write, a Bluetooth
  speaker's) taken out by the broker, so no plugin knows a backend.
  *As built (a review of efde36e, M1):* while what it writes is not heard (it said it paused, another source took
  over, hassmic paused it) the thread still reads it, but only as fast as the stream's own rate, and drops it: the
  back-pressure holds in every state. Read as fast as it came, a plugin that said "paused" and wrote in a loop had
  the thread take two thirds of a core and `bl` some 340 000 times a second, under its own CPU watchdog's limit.
  *(L1 of a review of 92ac2e1):* the pace is a running total of the frames dropped since it began, not a sum of each
  read's share rounded to whole milliseconds (0 for a read under one: a plugin that wrote 8 bytes at a time into its
  socket made as small as it goes was read unpaced again, 57 % of a core); and a read of less than 5 ms of audio
  has the next one wait 5 ms (`READ_MIN_MS`), heard or not (the socket holds 170 ms): 8 bytes a write while it
  played were 24 000 rounds of the loop a second, `bl` twice in each.
- **The wake stream** (`wake`): one `SOCK_SEQPACKET` pair per plugin, handed over with the first `WAKE_OPEN`.
  Each packet is one block as `wake_feed` got it (16 kHz mono s16le, a capture block; one longer than
  `WAKE_SAMPLES_MAX`, 4096 samples, cut into several, each with its own position: the protocol's bound, which a
  reader's buffer is sized by, a review of 6631974), with its position on `wake_fed()`'s clock (a `u64` sample
  count) in front. Sent with `MSG_DONTWAIT` from the capture thread; a
  full queue (the socket's send buffer, set to 64 kB: 2 s of this stream) drops the block, and the next packet's
  position shows the plugin the gap. Not sent while muted, hard or soft (3.3). 4.13.
- **A request's copy** (`request.audio`): one `SOCK_SEQPACKET` pair per plugin, handed over with the first
  `REQ_START`. Each packet is one block exactly as `proto->audio` got it (16 kHz mono s16le, up to 1024 samples;
  the lead-in's zeros included), with the run's number and the sample offset in the run in front, so a plugin
  sees a gap where a block was dropped. Sent with `MSG_DONTWAIT` right after the block went to Home Assistant; a
  full queue (a send buffer of about 6 s, below) drops the block and counts it, and `REQ_END` says how many.
- **A call's microphone and speaker**: `SOCK_SEQPACKET` pairs, one 20 ms frame of 16 kHz
  mono s16le per packet with a `u32` sequence number in front. Towards the plugin the broker sends without waiting
  and drops when full; from the plugin it reads into `call.c`'s jitter buffer.

Not offered: encoded audio in either direction, resampling, mixing, an equalizer, a decoder. hassmic decodes for
itself with dr_flac, minimp3 and the firmware's libopus; a plugin links its own.

*As built (2026-11-22, phase 6):* a request's copy comes on a `SOCK_SEQPACKET` pair **per request**, the plugin's end
handed over with that request's `REQ_START` (not "the first time"): `REQ_START` goes through the broker's queue (4.12)
and the audio does not wait for it, so with one socket for good a request's first blocks (the lead-in, sent at once)
were readable before the message that says they begin, and every reader would need a rule about which comes first;
with a socket of its own the plugin cannot read a request's blocks before its start.  hassmic's end is closed after the
request's last block (at its end, or the plugin stopped or gone): the plugin reads what is left, then its end.  Each
packet `u32` run, `u32` offset (samples since the run's first), the block (`REQ_SAMPLES_MAX` 2048 samples at most: a
larger block, which `send_out` never makes, would be split).  The send buffer: `SO_SNDBUF` 212992 (`plugin.c`
`REQ_SNDBUF`), which the kernel caps at `net.core.wmem_max` and doubles; a packet of 1024 samples costs ~4.3 kB of it,
so 98 packets, 6.3 s.  A request's start comes at once, before the plugin can read (its `REQ_START` waits in the
broker's queue): the lead-in's 0.8 s and the ring's replay, 4 s at most (the ring), so 4.8 s.  Until a review of
b294fa0 (M1) 64 kB, 31 packets, 2 s: a plugin that read at once lost 0.35 s of a lead-in and a 1.5 s replay, 0.85 s
of one of 2 s, all of it the wake word and the command's start.  Where `wmem_max` gives less than the burst, hassmic
says so once in its log.  These sockets are the core's:
made, written and closed under `bl` by the holder of `core_lock`, never polled by the broker.

*As built (2026-11-21, phase 5):* one `SOCK_SEQPACKET` pair for both ways of a call (a pair is two ways already),
handed over with `CALL_LIVE`'s `OK`, each packet 644 bytes (`u32` number, 640 bytes of PCM).  The room's frames go
from `call.c`'s sending side (`call_send`, under `call.c`'s `tx_mu`, one send that never waits: the capture thread
waits for nobody; its send buffer ~0.5 s, then dropped and counted); the far side's are read by a thread of the
broker's per call (`media_reader`: 100 packets a second at most, the rest left in the socket, so a plugin's writes
say `EAGAIN`; another size dropped) into `call_rx`.

*As built (2026-11-21, phase 4):* the music goes to a backend stream of its own (`audio.h` `plug_open`/`plug_write`,
in the three backends), not to Sendspin's: Sendspin's player thread writes the music stream without a lock and
blocks in the write at playback speed, so sharing it would have needed a hand-over inside `sendspin.c`, and a stream
of its own beside it is what the phone's Bluetooth stream already is.  "One at a time" holds by "the newest source
wins" (4.7 "As built"): the others are paused, and between plugins the backend's one plugin stream goes to the newest
that opens one (the one before is ended: its writes fail with `EPIPE`).  The rates are 8, 11.025, 16, 22.05, 24, 32,
44.1 and 48 kHz, made stereo by the broker (the duck works on stereo); the stream thread reads 40 ms at a time, so a
pause, a duck or another source's start takes effect within that and the backend's buffer.  `MUSIC_POS`'s frames are
written frames less the queue and the output's latency as Sendspin reckons it (70 ms, `HASSMIC_OUTPUT_LATENCY_MS`, a
Bluetooth speaker's on top).  What a plugin keeps writing after another source took over (or while it says paused)
is read and dropped, as a phone without remote control is made silent (`a2dp.c` yielded).

### 3.6 The microphone

The rule the broker keeps, whatever a plugin asks. A plugin gets microphone audio in three cases only, each a
capability of its own that the owner accepted in its own words: all the time, to find wake words (`wake`, sealed:
3.3); a live call it carries (`call`); a copy of a request (`request.audio`).

- **Only a plugin with `wake` has the microphone outside a run or a call.** It gets what `wake_feed` gets
  (`main.c:3548`: the capture's 16 kHz stream after Amazon's front end, the same blocks, while the wake word is
  heard on the Echo; nothing with `-w remote`) and can send nothing out but `WAKE_HEARD`. No light shows it, as
  none shows that hassmic's own engine listens: an Echo listens for its wake word whenever it is not muted. Mute
  cuts it as it cuts hassmic's engine (the hardware latch is ahead of both).

- **A request is Home Assistant's, and a plugin with `request.audio` gets a read-only copy of it.** The copy is
  the blocks hassmic hands to `proto->audio`, byte for byte and in order: the same start (the 800 ms lead-in, then
  the replay from the wake word's end, or from 100 ms ahead of the word with `wake_word_in_request` on: whatever
  Home Assistant is sent), the same processing (`earsup`, `micdenoise`, `micgain`), the same end. Nothing else:
  not the raw microphones, not the capture ring, not the wake word's audio ahead of the replay's start, not what
  the wake word engine or the aligner saw, not a call's audio (that is `call`'s), not `state/capture.raw`.
- **Only for runs Home Assistant gets, only on the Echo that streams.** No link, no run, no copy. Of several
  Echos that heard the wake word only the round's winner starts a run, and the copy is taken where the run's
  audio is sent, so the others' plugins get nothing; that needs no rule of its own.
- **`-w remote` (the wake word in Home Assistant) is the exception to "byte for byte".** There hassmic streams to
  Home Assistant before any wake word and beside each reply, so that Home Assistant can hear the word. A plugin
  does not get that: its copy starts when Home Assistant says it heard the wake word (`core_remote_wake`: the
  sound, the ring, listening mode) and ends at microphone off. Otherwise `request.audio` would be a microphone
  that is always open on those Echos, which is what the privacy rule exists to forbid.
- **One way.** The copy is made after the block is handed to `proto->audio`, by a send that never waits. A plugin
  cannot delay, change, gate, extend or end a request, start one, or put audio or text into it; a plugin that
  does not read loses blocks and Home Assistant loses nothing (issue #77: the capture thread waits for nobody).
- **Mute**: a muted Echo starts no run, also none asked for before the mute (Home Assistant's question back, an
  announcement's `start_conversation`, a wake word or the button that cut a reply), and with `-w remote` its stream
  to Home Assistant stops; the hardware latch cuts the microphones themselves. The copy is what Home Assistant got,
  so it holds no more than that.
- **What shows it.** Nothing new, and that is honest only with these words: the copy exists exactly while hassmic
  is in LISTENING, the state the listening ring (the band on a Show, the one LED on an Echo Input) stands for. But
  that light is the owner's to dim: "LED brightness" goes down to 0 and "Light band" to 0 rows (*checked*,
  `settings.c`), and then nothing shows for Home Assistant's listening either. The copy adds no moment in which
  the Echo listens; it adds a second listener to the moments it already does. What says that there is one is the
  Plugins page (a line "hears every request" on the plugin's card, and the same line on the page's top while any
  such plugin runs) and the install screens, not a light.
- **`request.text`** is its own capability: what speech-to-text understood and what the assistant answered, as
  Home Assistant reports them (`proto_esphome.c`'s STT end and TTS start, the texts behind "Last heard" and "Last
  reply"; Wyoming tells only the transcript), whether or not the `request_text` feature is on for Home Assistant.
  Separate because it is much less: no voices, no room, nothing that was not understood; a logger or a display
  needs it without the audio, and a speaker-recognition plugin needs the audio without it. "Stop" and a bare wake
  phrase that `core_transcript` swallowed are not passed on, as they are not shown on a Show's screen.
- A call's audio goes to a `call` plugin only while the call is live. An incoming call is live only after a person
  answered it **at the Echo**: the action button, the Show's screen. Nothing else answers one: not Home Assistant,
  not the settings page, no token, and there is no automatic answer for plugins (Drop In's `drop_in_answer` does not
  apply).
  *As built:* the button and the screen send `CALL_ANSWER`; the core is told the call was answered
  (`CALL_ANSWERING`: the ringtone stops at the press, the light and the screen show it connected while the far side
  comes in), and the plugin says `CALL_LIVE` once it has the far side.  `CALL_LIVE` for a call that still rings is
  `ERR DENIED`, whatever token it carries, and no audio socket exists before `CALL_LIVE`'s `OK`: the media
  (`call_begin`) begins there and ends with the call (`call_end`, the socket shut down), on every path (the plugin's
  `CALL_END`, a hang-up here, the plugin stopped, its process gone).  Until 2026-11-22 an action marked `"calls"`
  answered too (its token in `CALL_LIVE`): an automation in Home Assistant could then answer the plugin's ring by
  itself, an automatic answer by another name (C3 of a review of fc43c40, decided the safer way).
- An outgoing call needs a **token**: one use, good for 10 s, put by the broker only into an `ACTION` that the
  manifest marks `"calls": true` (shown at install: "and have it place a call") and that came from Home Assistant's
  keyed link or an approved browser. `CALL_OUT` without one is refused. An automation that fires the plugin's other
  actions does not open the microphone. Tokens are bound to that direction: `CALL_OUT` is the only message that
  takes one, and the call it starts shows on the ring and the screen the whole time (going out, then connected).
  *As built:* 32 hex digits from `/dev/urandom`, kept per plugin (four at most, the oldest goes), spent by the first
  `CALL_OUT` that names it whatever becomes of that (busy, muted), cleared when the plugin's process starts anew.
  An automation in Home Assistant that calls an action marked `"calls"` does bring a token, so the owner's Home
  Assistant can have the plugin call out (the install line says the action places calls); it cannot answer.
- **Ringing** is hassmic's: its ringtone, its LED pattern, and on a Show the plugin's name beside `who`, which is
  the plugin's text and may say anything ("Mum"). At most 3 unanswered rings in 10 minutes per plugin, then
  `CALL_INCOMING` is refused for the rest of them; do not disturb refuses all.
  *As built:* a ring that ends unanswered counts, however it ends (not answered in its 30 s, declined on the screen,
  "stop", the caller giving up: `CALL_END` while it rings); counted over the plugin's restarts; `ERR LIMIT` while
  three lie within the window.  A ring refused busy (another call has the line) costs a look of its own: one a
  second, two at once, and asked faster it is refused busy without a look at the line, so that a plugin cannot
  find the moment the line comes free by asking again and again (W1 of a review of fc43c40).  The screen shows
  "<plugin's name>: <who>", `who` strict UTF-8 with control characters made spaces, 64 bytes, the whole cut to the
  screen's 63 bytes at a character's edge.
- Mute (the hardware latch: zeros, as `call_mic` gets today) and do not disturb refuse calls as they do for Drop In.
  Neither ends one that runs: a call muted goes on with zeros for the room, as a Drop In does, and do not disturb
  switched on during a call leaves it.
- Never: the capture stream outside a run's copy or a live call, the raw microphones, the wake word's own audio,
  `state/capture.raw`, anything while no run and no call is on.

*As built (2026-11-22, phase 6):* the copy's span is the core's `req_open` (`main.c`), set where listening mode is set
for a request (`pipeline_start` for the local wake word, the button and a follow-up; `core_remote_wake` with
`-w remote`) and cleared in `mic_stopped`, where listening mode is; why it ends is the first word of whoever ends it
(`req_ending`: heard here, cancelled, the timeout, an error, the link lost, muted; none said: Home Assistant ended it).
*Mute*: a mute while a request is open, the latch or the soft one, ends the copy (`REQEND_MUTED`) whatever the run
does; the soft mute ended a listening run already (`mute_update`).  And while muted no microphone audio leaves the
Echo (a review of b294fa0, C1, C2): `mute_update`'s `mics_off` drops a run asked to follow (`barge_in`:
`continue_conversation`, an announcement's `start_conversation`, a wake word or the button that cut a reply), ends
`-w remote`'s stream for Home Assistant's wake word (its run cancelled quietly), the run beside a reply
(`wake_listen`) and `-D`'s microphone through a reply; `core_restart_after` asks for none while muted,
`core_pipeline_finish` starts none, `core_remote_wake` and `wake_listen` act on nothing, and `pipeline_start` and
`req_begin` refuse whatever a caller missed; `mics_on` streams again on unmute.  Until then a follow-up asked for
before a mute listened on the muted Echo, to Home Assistant and to every `request.audio` plugin (Home Assistant
could have that at will with `start_conversation`), and `-w remote` streamed through the soft mute, its wake word
beginning a copy.  An announcement on a muted Echo plays and is reported played; the conversation after it does not
start.  *Wyoming with `-w remote`*: its `detection` is `core_remote_wake`, as ESPHome's wake word end (the sound,
the ring, listening mode, the copy; M3: it only set the state, and no copy began).  Under `-D` a detection during
a reply leaves the reply to the client, as a wake word heard on the Echo does under `-D`, and begins the request
there (the ring, listening mode, the copy; no wake sound over the reply; the reply's audio-stop ends nothing of it);
it cut the client's reply until a review of c4ace24 (F3).  *Text*: `REQ_TEXT` goes where
the screen's words go (`screen_words`): ESPHome's STT end after `core_transcript` let it through, its TTS start;
Wyoming's transcript, which `core_transcript` is not asked of there (the satellite protocol has no cancel to stop a
ring with), so with Wyoming a bare "stop" is passed on as the screen shows it.  Only a request's text
(`plugin.c` `req_texts`: set as a request begins, cleared by `plugin_request_none` where a run starts that is none
yet, `-w remote`'s stream): a transcript of such a run went out under the last request's number before (M3).  *The light*: unchanged, as decided (9.17).  *The page*:
the Plugins page's top names every plugin that hears requests while it runs (`plugin_json` `hears`), each card has a
row "Hears every request" (its runs, what it lost, `requests`) and "Reads what was said".  *The log*: a plugin's own
lines are private while it has `request.audio`, `request.text` or `call` (`plugin.c` `log_lines`, `PRIVATE_CAPS`,
`LOG_PRIVATE_MARK`; `call` since the review of 7be9343: it hears the room while a call is live): what it
was told may be in them, and no plugin with `log.read` gets them; the broker writes no text of a request.  The
broker's own lines about such a plugin are private too (`say`, `seal_note`'s `SEAL_PRIVATE`, as a wake plugin's are
sealed): the samples it did not take, a volume it set, a file check's reason are values it chooses, and a listener
without a way out could have passed bits through them to a `log.read` plugin with one (a review of b294fa0, C3).
The mark is kept per entry of the broker's, by the package the entry's last start ran: a table by id that nothing
emptied made the seventeenth listener installed in one run public again (a review of c4ace24, F2).
Left, accepted (docs/SECURITY.md): the volume it sets, its CPU time, when it acts, its own entities.

### 3.7 Not in the API

| Not offered | Why |
|---|---|
| Codecs, DSP, resampling, HTTP, TLS, JSON | a plugin links its own; each would be a surface to keep |
| Files, directories | 2.5 |
| Name lookups as such | a lookup is a message to the outside (port 53 passes the firewall); names are resolved only inside `CONNECT`, for a host the entry allows (4.6) |
| Writing its own settings | where a plugin may connect is a setting; 4.6 |
| Answering a request, putting audio or text into one, starting or ending one | the project is Home Assistant's satellite (4.9). A plugin can listen in (`request.audio`), not take part. What it concludes goes to Home Assistant through its own entities (4.12) |
| Amazon's wake word engine, its models and thresholds, the wake word's end, the front end, arbitration | hassmic's core, and where its measurements live. A plugin can be *another* engine (`wake`), feeding the same core |
| LED patterns and frames | the ring says what the microphone does; a plugin must not be able to say otherwise. Calls and requests get hassmic's patterns |
| The Show's screen | `src/ui` draws from hassmic's state; a declarative card may follow, drawn by the UI, once a plugin needs one |
| Bluetooth (HCI, A2DP, the proxy) | one controller, shared by timing |
| Root's requests: Wi-Fi, updates, adb, restart, reset | a plugin never reaches root, not even through hassmic |
| hassmic's settings and other plugins | read-only facts come in `WELCOME`; anything else is a path around the page's approval |
| The ESPHome link itself | entities only, named by the manifest |
| JavaScript or HTML on the settings page | the page's origin holds the browser's key K; text only |
| IPv6 | hassmic speaks IPv4 only, and the firewall's rules for plugins are IPv4's; every socket is `AF_INET` |
| TCP listeners, mDNS announcements | none of the cases needs one; a listener would be the broker's to `accept` on (2.4) |

## 4. Capabilities in detail, and what changes in hassmic

### 4.1 Layout on the Echo

`state/plugins/<id>/`: `manifest.json`, `manifest.sig`, `listing.sig` (5.1), `plugin` (0755), `author.pub`
(pinned at the first install), `accepted` (what the owner accepted, 5.4), `config` (0600: settings and secrets,
each value with the type of the row it was stored for: `<row>:<type>=<value>`. A value is a row's only while the
row is of that type and takes it: rows go without the button and a name can come back as another kind of row, and
a secret must not turn up as the value of a text row of its name, shown on a page that is plain HTTP. An install
drops what no row of the new version answers to; 13, the sixth read),
`data/` (its storage, one file per key), `files/<list>/<item>/` (what the owner uploaded, with `fields`),
`enabled`, `crashes`. hassmic's own, no root; every write through `statefile.c`. 16 plugins installed, 32 MB of
programs in all, files and storage by each plugin's accepted quota, and 32 MB left free on the partition (as sound
packs). Running at once: 2 in phase 1, the launcher's 8 slots later, when memory with real plugins has been
measured.

*As built (2026-11-22):* the launcher's 8 slots, with memory still unmeasured, so with a bound that needs no
measurement: the plugins that run at once may have 128 MB of `memory_mb` between them. `memory_mb` is the address
space the launcher lets a plugin map (`RLIMIT_AS`, 8 to 96 MB, 32 when the manifest leaves it out); what a process
keeps resident is never more than what it may map, so the sum of the limits bounds what plugins can take, whatever
real plugins turn out to use. 128 MB is a quarter of the smallest Echo's memory (the Dot 2's 512 MB); an Echo with
less gets a quarter of what it has (32 MB at least), by `/proc/meminfo`'s `MemTotal` rounded up to the next 128 MB
first (`plugin.c` `run_mem_of`): `MemTotal` leaves out the kernel's image and what is reserved at boot, a Dot 2's 512
MB reads as some 470, and a quarter of that left the eighth plugin of 16 MB waiting (P5 of a review of 7be9343). It is
eight plugins of 16 MB (microWakeWord's and the tests'), four that leave `memory_mb` out, or one of 96 beside two of
16. A plugin that does not fit waits (2.6 "Places"). The rest is sized for eight: `LANES` (a music source's place in
the core's table: every plugin that runs may have one, and two more, since a lane is free again only once its
plugin's process and stream are gone, and a plugin started in the place of one on its way out found none:
`RUN_MAX + 2`, `MUSIC_SOURCES` 14 in `core.h`, both held by `_Static_assert`s), the network's queue (`plugin_net.c`
`QUEUE` 16: two under way for each of eight) and its workers (one for each of eight, each plugin's requests one at a
time: 4.6 "As built"), the request copy's messages (`RQ_MAX` 64), the firewall's eight DROP rules for users 6900 to
6907 (`lockdown.sh` `PLUGIN_SLOTS`, unchanged). When real plugins have been measured on an
Echo the 128 MB can be what they use, not what they may.

### 4.2 Manifest

```json
{ "id": "syslog", "name": "Syslog", "version": 3, "version_name": "0.3",
  "requires": { "core": 1, "log.read": 1, "storage": 1, "net.server": 1, "ha.entities": 1, "ha.actions": 1 },
  "author": "abonforti", "author_key": "<hex>", "source": "https://github.com/...", "license": "MIT",
  "models": ["*"], "memory_mb": 8, "threads": 2, "digest": "<BLAKE2b-256 of plugin, hex>",
  "capabilities": { "log.read": true, "storage": 4,
                    "net.server": [ { "setting": "server", "proto": "udp", "ports": "setting:port" } ],
                    "ha.entities": [], "ha.actions": [] },
  "settings": [ { "name": "server", "label": "Syslog server", "type": "host" },
                { "name": "port", "label": "Port", "type": "int", "min": 1, "max": 65535, "default": 514 } ] }
```

`requires` is the least version of each scope it speaks (3.1): core and the scope of each capability, whole numbers
from 1, nothing else; a scope or a version this build has not makes it one for a newer hassmic. The file is UTF-8
throughout, without the byte 0x7f; no number in it is written with more than 39 characters; a host row's default is
a host name (labels of letters, digits and hyphens, none empty, none with a hyphen at an end); `-0` is not written
where a whole number is meant (13 has the rest of how it is read).

Read with `json.c` (a token reader, fuzzed). Every string has a length limit and a character set (`id`:
`[a-z0-9-]{3,24}`); what does not fit refuses the package.

### 4.3 Settings (`settings.c`, `web.c`, `web/app.js`)

Types: `bool`, `int` (min, max, unit), `select` (options), `text`, `host` (a name or an address; what `net.server`
refers to), `secret`, and `files` (a list of uploaded items, below; it needs the `files` capability). Kept in the plugin's `config`, not in `settings.c`'s table: that table is
static, indexed by an enum, and its names are promises to Home Assistant and to exports. `settings.c` gains the feature `plugins`, and `wake_engine` stops being a fixed list of three
(`wake_engines[3]`, `settings.c:37`, an `S_CHOICE`): its choices become `amazon`, `homeassistant` and one per
installed wake plugin, read at run time (4.13). Otherwise it gains only the
feature `plugins`.

**Settings are the owner's to write, from an approved browser, and nobody else's.** A plugin reads them (`CONFIG`)
and cannot set one. The first draft let it (`CONFIG_SET`, for a server it had discovered), which let a plugin
choose its own destination: with "connects to the server you enter" it could have entered an internet host itself
and sent the log there. What a plugin finds, it proposes: `PROPOSE { setting, value }` shows on its card as
"Found 192.168.1.20. Use it?", and only the owner's click writes the setting. One proposal per setting at a time,
text, a minute apart at least, and only a private address: a public host has to be typed by the owner.

The page: a page "Plugins" in `PAGES`, one card per installed plugin, rows by `settingRow`/`infoRow` as everywhere
(one list of rows, never side by side), built from the manifest; label and help are the manifest's text, set with
`textContent`. Every plugin card is marked as not hassmic's ("Plugin by <author>", community ones "not reviewed"),
in a frame of its own: a plugin's labels are its author's words, and a row "Home Assistant password" must not look
like the page asking. A `secret` is sealed with K as the Wi-Fi password is (`crypto.js` seal, `web.c` unseal),
reaches the plugin in `CONFIG`, is never sent back to the page and never exported. Plugin settings are not in
export files; between Echos they travel by the page's sync (5.4). The card's own rows are hassmic's: running or not, crashes, what it may
do; an action marked `"page": true` gets a button there (how the phone is called from the page).

*As built (2026-11-22): rows of a plugin's own.* An entity of `ha.entities` marked `"page": true` is a row on
the card as well: its manifest label and its state as Home Assistant has it, read-only, inside the plugin's frame,
under its description, as one list of rows like every card (`web/plugins.js` `plgRows`, `infoRow`; never side by
side).  `/api/plugins` carries them per plugin as `rows` (`plugin.c` `plugin_json`: name, kind, label, unit,
decimals, and the value once the plugin said one), only while the plugin runs and Home Assistant lists that entity
(`ha_entity_out`, as `plugin_entities_each`: the plugin enabled, accepted, runnable, and the entity's key kept): a
plugin that does not run has no state to show, and an old value is not shown as current; an entity whose key could
not be written down (`.hakeys`) is not in Home Assistant, so it has no row (a row would show a state Home Assistant
does not have) and stays among the card's entities with its "Not listed in Home Assistant yet" notice
(`hmrows.listed`: such an entity is listed whatever the rows say; a review of 541f0a8).  The value is in the row and
not again among the card's entities, which keep the entities not marked.  How a value reads
is the page's (`hmrows.text`, pure, `tests/unit/web_rows_test.py`): a switch or a binary sensor "On"/"Off", a sensor
to its `decimals` with its `unit`, a number without a float's stray digits and with its `unit`, an option or a text
as it is, "Nothing said yet" before the first; label and value as text nodes (`h()`), never markup (the test checks
the text `hmrows.text` returns, markup in it unchanged; no test renders the page's DOM).  It updates with
the card's poll.  Never a setting and never a secret: only what the plugin tells Home Assistant anyway.

**The `files` type**, made for microWakeWord's model library and meant to do for the next plugin with files of
its own (sound fonts, certificates, phone books). In the manifest:

```json
{ "name": "models", "label": "Wake word models", "type": "files",
  "item": { "files": [ { "suffix": ".json", "max_kb": 16 }, { "suffix": ".tflite", "max_kb": 1024 } ],
            "by": "stem" },
  "max_items": 16,
  "fields": [ { "name": "label", "label": "Name", "type": "text" },
              { "name": "cutoff", "label": "Cutoff", "type": "number", "min": 0.5, "max": 0.99, "step": 0.01 },
              { "name": "window", "label": "Window", "type": "int", "min": 1, "max": 20 } ],
  "catalog": { "label": "ESPHome's models",
               "base": "https://raw.githubusercontent.com/esphome/micro-wake-word-models/main/models/v2/",
               "items": [ { "id": "okay_nabu", "title": "Okay Nabu" }, { "id": "hey_jarvis", "title": "Hey Jarvis" } ] } }
```

- **An item** is a set of files that belong together, here a manifest and a model with the same stem (`by:
  stem`: `okay_nabu.json` + `okay_nabu.tflite`), as the page groups dropped files today (`app.js` `mwwGroups`).
  Its id is the stem, cut down as today's (`mwwId`: `[a-z0-9_-]{1,40}`).
- **Fields** are per item, typed like settings, kept by hassmic beside the item (`fields`), edited in the item's
  row ("Tune", as now). The plugin gets them with the list; whether a field overrides a value inside a file (the
  model's own cutoff) is the plugin's logic, and the plugin says the field's start value in `FILE_OK`.
- **A catalog** is a list in the signed manifest of items the *browser* can fetch and upload for the owner: a base
  URL and, per item, an id, a title and a path under the base (`"path": "experiments/okay_computer"`: ESPHome's
  collection keeps some models in a folder of their own, `app.js:1670-1671`), fetched as `<base><path><suffix>`.
  Data, not code; the same thing the page does today with a list that is hard-coded in `app.js` (`MWW_REPO`,
  `MWW_OFFICIAL`, `app.js:1669-1671`). The Echo fetches nothing.
- **A file of an item may be optional** (`"optional": true` on the `.json`): today a model can be added without
  a manifest and gets defaults (`web.c:791-792`); the plugin's `FILE_OK` then supplies the title and the fields.
- **Editing a field** of an item tells the plugin (`FILES`), which reloads a word that is open, as changing the
  active word's cutoff reloads it today (`core_wake_models_changed`, `main.c:622-630`).
- **Checking an upload.** Today the Echo refuses a model it cannot load (`mww_add`, `mww_store.c`), with the
  reason, before it is kept. The same here: hassmic stores the upload aside, asks the plugin (`FILE_CHECK`), and
  keeps the item on `FILE_OK` (with a title for the row: the wake phrase) or drops it on `FILE_BAD` with the
  plugin's reason as text, or after 5 s without an answer. A plugin that is not running cannot check: the upload is kept as "not checked yet", shown so in its row, and
  checked when the plugin next runs; it is not offered as a wake word before.
- **Reading**: the plugin asks for a file and gets a read-only descriptor of it (`FILE_OPEN`). A regular file's
  descriptor is safe to hand over: the filter has no `open`, `mmap2` of a descriptor is refused (2.4), and a
  descriptor opened read-only cannot be made writable. No chunks, no copy.
- **The page** draws one card section per `files` setting, all rows alike: the item's title, its fields behind
  "Tune", Save file, Delete; under the list "Add files" and the catalog's entries. The pick of the *active* wake
  word is not here: it stays hassmic's row and Home Assistant's select, as for Amazon's (4.13).

What is lost against today's bespoke section, said plainly: the layout is the generic one (no card of its own
design, no inline hints tailored to wake words beyond the manifest's help texts); the dialog that asks for a first
model while switching the engine (`mwwSwitchDialog`, `app.js:1812`) becomes hassmic's generic "this engine has no
wake word yet: add one on its card"; the engine comparison (`app.js:317-351`: what microWakeWord costs against
Amazon's) stays in `app.js` as hassmic's own text about a wake word plugin in general, with the plugin's
description beside it, since a plugin cannot write hassmic's page. Everything a person does today, add, fetch
from ESPHome's collection, rename, tune, delete, save, copy to another Echo, is still there.

New in `web.c`: `/api/plugins` (GET: installed, state; signed), `/api/plugins/<id>/files/<list>[/<item>[/<file>]]`
(the uploads, their fields, a file back; signed), `/api/plugins/<id>` (POST settings, enable, remove,
accept a proposal, an action), `/api/plugins/install` (waits for the button as `POST /api/adb` does, through
`web_approve()`).

One press of the action button stands for a login, for adb and for an install (`web_approve()`, `web.c:245`),
and a Dot cannot show which. What keeps that honest: only one thing waits at a time, an install or adb is asked
by a browser that is approved already, and the page says what the press will accept. A ring pattern of its own
for "accepting a plugin", apart from the login's, is cheap and is in the design; it tells a person at the Echo
that this press is not a login.

*As built (2026-11-21, phase 4): proposals.* `PROPOSE` needs `net.server` and names a host row one of its entries
names (what the owner then confirms is "the server you enter", nothing wider); the value is an IPv4 address written
a.b.c.d, private or link-local, not the Echo's own; one per row at a time, a minute apart (`BUSY`); the same as the
row's value changes nothing.  Kept in memory beside the plugin's entry (a restart of hassmic forgets it: the plugin
proposes again), in the log as a private line, on the card as a row "Found <address>: For "<label>". Use it?" with
"Use it" and "No" (`POST /api/plugins/<id>/propose` "take|drop <row> <value>"); "Use it" writes the row as the
owner's own value and starts the plugin again, as any change of a server's row does.  An action marked `"page":
true` is a button on the card (`POST /api/plugins/<id>/action`): for an action without arguments only (decided: a
value typed on the card would be one more way into the plugin, and the manifest refuses `page` with `args`).

### 4.4 Storage and files

Two capabilities, because who writes differs.

**`storage`**: the plugin writes. `STORE_*` on `state/plugins/<id>/data/`: keys of `[a-z0-9._-]{1,48}`, each a
file written by `statefile_write`, up to the accepted quota in kB (a forwarder's cursor: 4 kB). The risks
are the plugin's doing: flash wear and data that outlives a run and travels in a sync. Writes go through a bucket
of 10 that refills by one a minute: `statefile_write` syncs the file and its folder, one a second would be 86,400
synced writes a day per plugin on the Echo's flash, and hassmic holds its own files to once a minute (`arb.c:199`
`SAVE_MS`). A write over the bucket is refused, and the SDK keeps the latest value to write when it may.

**`files`**: the owner writes, from an approved browser, and the plugin only reads (4.3's `files` type;
`state/plugins/<id>/files/`). The risks are other ones: space (the accepted quota in MB, counted by hassmic), and
content the plugin has to parse, which is the plugin's problem inside its sandbox and no longer hassmic's
(4.13). A plugin cannot change, add or delete an item.

Neither is a file system: a plugin names a key or an item, never a path.

*As built (2026-11-21):* `state/plugins/<id>/storage/<key>` (the folder 3.3's wipe already named; `data/` in 4.1
above), keys `[a-z0-9._-]{1,48}`, not beginning with a dot and not ending in `.tmp` and digits (statefile.c's
temporary names beside each file), a value up to 16 kB, the accepted kB (1 to 1024) and 128 keys at most
(`plugin_store.c` `ps_store_*`). The bucket is the broker's, per plugin entry and in memory (`plugin.c`
`store_message`): ten, one back a minute, a delete counted as a write, a write the store refused (over the kB, a key
that is none) not spent; a restart of hassmic fills it again, which a plugin cannot cause. The C client keeps the
latest value of each key it was told `BUSY` for (eight keys) and writes it when the Echo takes writes again, asked
every 10 s. Storage may be asked before `READY` (the SDK's ready callback runs before READY goes out). It is copied
between Echos only when the owner picks its row (5.4 "As built"). After `STOP` the broker still does storage writes
(until the process is gone: `STOP_MS`, then the launcher's `KILL_MS`) and answers every other request `FAILED`; the
client writes at once from then on, and what it kept for later as `hm_run` ends (a review of 0dabfa6, R2, R4). A
process stopped for an update writes its storage until it is gone (its stop callback's cursor is what the next
version starts from), a removed plugin's none (3.3 "Storage that outlives the seal", R1; a review of 7144548, 1).
`STOP` also comes as hassmic itself ends in order: on `SIGTERM`, which root's `main.sh` sends before each restart of
its own (`hm_term`: an update, a restart asked for, artifacts installed, a reboot), hassmic tells every plugin `STOP`
and waits for them, 2 s at most (`plugin.c` `plugin_shutdown`, `main.c` `on_term`). init's own stop and restart send
`SIGKILL` to the service's process group (the Echos' init is Android 7.1's or 9's, whose `Service::StopOrReset`
does so, "is being killed" in their strings; *checked* in the firmware's `/init` that `SIGTERM` is only the reboot's
there, "Sending SIGTERM to service"; ringlive.c *measured* the kill on a Dot 3), and the launcher, a session of its
own (`setsid`), outlives hassmic and kills every plugin as hassmic's socket closes: there, and at a crash or a power cut, no stop callback
runs, so a plugin saves as it goes too (sdk/PROTOCOL.md "The start"; the template's build found it).

### 4.5 Log (`web.c`, `clock.c`)

`log_redact` and the reading of `boot.log` (`HASSMIC_LOG`) move out of `web.c` into `logread.c`, used by the
page's viewer and the broker. The broker follows the file (and `.1` across a rotation) and sends lines with a
cursor (file identity + offset); a plugin that stores its cursor gets, after a restart or once the network is back,
the lines written meanwhile. Root's lines (`main.sh`'s `say`) are in the same file.

What a plugin gets is **not** what the page shows. `log_redact` was made for the owner looking at their own log
and blanks one thing, the Sendspin pairing token (*checked*, `web.c`); the log also carries what was said
(`proto_wyoming.c`: `transcript: <text>`), Wi-Fi names, paired Bluetooth addresses and root's lines. So:

- `log.read`: lines pass a second filter in `logread.c`: lines that carry speech or a reply are dropped whole;
  Wi-Fi names, MAC and Bluetooth addresses are blanked; root's lines pass only by a list of prefixes.
- `log.read.full`: the page's view (token blanked). Official plugins only: the index refuses a community plugin
  that asks for it, and the Echo refuses to install one without an official listing.
- Neither gets a sealed plugin's lines (3.3).

How the filter knows a line carries speech is open (9): a list of prefixes kept beside 700 `fprintf`s goes stale;
marking the line where it is written does not.

*As built (2026-11-21, `logread.c`; decided so, 9.8):* marked where written. `log_private()` (the mark "(private) ",
`logread.h`) writes every line that carries what was said or answered, a name a person gave, an address that
fetches something, a login code, where the home is, or Home Assistant's words: the transcript and the reply
(ESPHome, Wyoming), timers (their names, a ring's "alarm: <name> rings"), Bluetooth announcements and device names
(`a2dp.c`, `btout.c`), the Wi-Fi network a switch asks for (`wifi.c`), the Music Assistant server's name, every
media address (a reply's `tts_proxy/<token>`, a media source's `authSig`: `proto_esphome.c`, `https.c`), Home
Assistant's and a Wyoming server's error texts, the Amazon login's code and device name (`davs.c`), the country of
the home's location (`clock.c`), a Drop In target and a light ring design as named. `log_sealed()` ("(sealed) ")
writes what a wake word plugin said or did: its stderr, the broker's lines that begin with its id (its reasons,
detections, exit status and CPU time: values it chooses), its words as the core lists them (their titles are its
`FILE_OK`s), marked when written by the plugin's `wake` as it was started (`plugin.c` `seal_note`), so a plugin
removed or updated without `wake` leaves its earlier lines as sealed as they were (the seal decided at reading time
let them pass then; a review of 08399dc, S3). Both make the whole line in one buffer, turn every control character
into a space (a reply with a line break was two lines, the second unmarked: S1) and write it in one `write(2)`, which
a pipe takes whole or not at all. `clock.c`'s reader carries a line's mark onto the piece after a cut at 2048 bytes,
and a line with a mark anywhere in it goes to no plugin (one not at the start is a marked line joined to another
that lost its end in a full pipe). The page shows those lines as they are, "(private)" and all, so the owner sees
what plugins do not get; a new line that prints such a thing takes the mark (CLAUDE.md says so). The audit of every
`%s` written to the log (a review of 08399dc, S2) found the media addresses, the ring's timer name, Amazon's code and
device name, the home's country and the error texts unmarked: marked since, `tests/unit/logwriters_test.c` and
`tests/fake_plugins_net.py` (hassmic's own writers driven by a stand-in Home Assistant) hold them to it. Of root's
event lines (below) none names a network, a device or a value of a person's. What `log.read` gets of a line:
- hassmic's own (its stamp has milliseconds: `clock.c`'s): without the mark, MAC and Bluetooth addresses blanked
  (`aa:bb:..`, `aa-bb-..`, and a run of exactly twelve hex digits, as `a2dp.c` prints them), the pairing token
  blanked; IPv4 addresses kept (they are what a forwarder's reader looks for when something does not connect, and
  the server is the owner's);
- root's (`main.sh`'s stamp, without milliseconds): only those that begin with root's event words ("== ", "!! ",
  "hassmic exited", "lockdown re-applied", "egress limited", "adb over Wi-Fi", "OTA guard"); wifi.sh names networks,
  and what else a script prints is nobody's guess;
- none without a stamp, and none of a wake word plugin's ("plugin <id>: ", and the broker's "plugins: <id>: ", which
  carries its `FILE_BAD` reasons).
The cursor is "<16 hex>:<offset>": an FNV-1a of boot.log's first 64 bytes (each text begins with a time to the
millisecond) and the offset into that text; main.sh rotates by copy and truncate, so after a rotation the same name
and offset are in `boot.log.1`, and a cursor in neither file was rotated away twice: the reader starts at the oldest
line kept and says `gap`. A cursor is read only from where a line begins (the byte before it a line end): one that
points into a line is moved to the next line's start and said as `gap` (a review of 0dabfa6, Q1: a cursor is the
plugin's to keep and hand back, and one made up to point just past a sealed line's mark had the rest of that line
filtered as a line of its own; a wake plugin's own text after its mark could carry a stamp). *Decided:* no MAC over
cursors. A plugin may already ask from "start" and read every line start the log has, so a MAC adds nothing once
cursors are held to line starts; and a cursor outlives hassmic's restarts in the plugin's storage, so its key would
have to be kept too, and a lost key would lose every forwarder's place. A text shorter than 64 bytes has no name: none of its lines is read until it has one, and
"from now" in it is the end of `boot.log.1`; only in the Echo's very first log, without a `.1`, the cursor is
`{0, 0}`, read as `boot.log` while there is no `.1` and as `.1` with `gap` once there is (a review of 08399dc: such a
cursor read the next text from its offset and skipped the lines between without a word). The broker reads without `bl` and sends a `LOG` only when it fits; a send that does not
moves nothing. One `LOG` at a time: each has an id, the client answers `OK` with it once its `log` callback returned,
and the next is sent after that `OK` (`plugin.c` `log_wait`; sdk/PROTOCOL.md "The log"). The `OK` is required, and
API 0.2 keeps its number (it was never released): a client written before flow control gets one `LOG` and no more, so
a `LOG` unanswered for 10 s on the broker's clock is said once in the log and on the plugin's card ("does not answer
LOG: built on an SDK before flow control?", `log_deaf`, `/api/plugins` `log_unanswered`; a review of 7144548, 2; the time its own `CONNECT` runs is not counted, a
review of 655efd7). Chosen over the socket's
out-queue (`SIOCOUTQ`), which empties as the client reads, and the client reads everything while one of its requests
waits: a callback that made a round trip per `LOG` had them pile up in the client past its 2 MB, which ended the
plugin as a crash (a review of 0dabfa6, R9). The cut at 2048 bytes never splits a mark either: a piece that ends in
the first bytes of one hands them to the next piece (`clock.c` `mark_begun`; Q3: "(priv" in one piece, "ate) " in the
next was a mark in neither), and `log_marked` cuts a long line on a character (Q5: it looked at the formatter's NUL).
Marked since that review too (Q6): microWakeWord's model ids and phrases (the owner named them: `mww_store.c`,
`wake_mww.c`, the core's word lists while microWakeWord is the engine, the copy to its plugin), a host that does not
resolve or connect (`net.c`: a media address's host may be the household's own name), a wake word Home Assistant
picked that is none, a light ring design named by an action, the Music Assistant address the owner entered, a
Sendspin server's reason for ending a pairing, another device's Kiosk Satellite phrase; and sealed: a wake plugin's
word that does not load ("plugin:<id>/<word>"). `log.read.full` is not built (phase 2 does not name it): asked for, it is refused ("this hassmic does
not have ... yet") as before.

*As built (2026-11-22, `logread.c` `logread_full`):* `log.read.full` is the page's view: every line as the
page shows it, the private ones with their mark ("(private) transcript: ..."), root's lines whatever they say, lines
without a stamp, MAC and Bluetooth addresses as they are; the pairing token blanked, as on the page. Never a sealed
line (the mark anywhere in it, also behind a private one: a private line joined to a sealed one), and never a line
that names a wake word plugin installed now ("plugin <id>: ", "plugins: <id>: "): 3.3's seal holds whatever a plugin
may read. The same `LOG_FOLLOW` and `LOG` (`messages.def`'s capability column may name two now: `log.read|log.read.full`,
`pm_caps` a mask), the same cursor, flow control and pace; a plugin with both capabilities gets the whole log.
Official only (3.3's combinations, at install and at every start); the index refuses a community plugin with it (5.3).

### 4.6 Network (`plugin_net.c`, `net.c`, `lockdown.sh`)

A plugin makes no socket and never owns one. The broker makes each, on a worker thread, and hands it over; what
needs an address (`connect`, `bind`, `sendto`) is the broker's to do.

What the firewall already does, without a rule per plugin (*checked*, `lockdown.sh`, `net.c`):

- hassmic's "any address" is not the process's: `hassmic_out` lets through what goes to a local address, and beyond
  that only sockets whose owner group is 3990, which is the file system group at the socket's creation
  (`net_socket`: `setfsgid` around `socket()`). A socket keeps its creator's owner when it is handed on. So the
  broker makes a LAN socket with plain `socket()` and the firewall holds it to local addresses, and an internet
  socket with `net_socket()`.
- A per-plugin owner rule, as the first sketch had it, would match nothing. It would also need
  `CONFIG_NETFILTER_XT_MATCH_OWNER`, unset on every model but cannoli; the owner match there is Android's
  `xt_qtaguid` standing in, which `lockdown.sh` relies on today.
- Inbound, 16384–32767 is open to anyone already. Ports for plugins are 28940–28999, given out by the broker,
  the same ones to a plugin at every start. A plugin cannot pick: #198 and #186 both took UDP 28933.

Two static rules are new in `lockdown.sh` (in `rules` and `keep`, worded as `iptables -S` prints them): drop
everything owned by users 6900–6907 in `hassmic_out`, and drop what comes to 28940–28999 from outside the private
ranges (`KSRC`), ahead of the range's accept. The first holds because of the filter: with `socket` and `accept`
both refused there is no call by which a plugin's user comes to own a socket; if one ever does, the sandbox failed
and the rule is what is left. (One rule if qtaguid's owner match takes a range, else eight.)

The operations:

| Operation | What the broker does | What the plugin holds |
|---|---|---|
| `CONNECT` (TCP) | resolves `host` if the entry allows that name, checks the address, makes the socket non-blocking, connects with a 10 s limit on a worker, and only then sends it | the only copy. hassmic never touches it again, so nothing the plugin does to it can hold a hassmic thread, and its `close` is the plugin's |
| `UDP_OPEN` | a UDP socket bound to a port of the plugin range | a copy for `recvfrom`; the broker keeps one for the next three |
| `UDP_PEER` | `connect` on its copy to an address the entry allows: the plugin then sends with `send`, 50 packets a second of RTP never pass the broker | |
| `SENDTO` | checks the address, `sendto` with `MSG_DONTWAIT` on its copy; for the few packets before a peer is fixed (SIP's signalling), within the plugin's 200 messages a second | |
| `BROADCAST` | the same to the entry's port at 255.255.255.255 or its group, once a second at most | |

The broker's copy of a UDP socket is safe to keep because the plugin cannot change the socket: `F_SETFL`,
`FIONBIO`, the timeouts and linger are not in the filter, UDP's `connect` and a non-blocking `sendto` do not wait,
and closing a UDP socket does not linger. The first draft kept a copy of every socket and did `connect` on it;
with `F_SETFL` allowed a plugin could clear `O_NONBLOCK` and have the broker's thread wait in `connect()` to an
address that never answers, or set a long linger and have hassmic's `close()` wait after a crash.

What the broker checks, before the firewall would (each row a capability of its own, 3.3: `net.server`, `net.lan`,
`net.internet`, `net.udp`, `net.discover`):

| Manifest entry | Allowed destination |
|---|---|
| `"to": "setting:<name>"` | the host the **owner** wrote into that setting (4.3), resolved by the broker at `CONNECT`, local or not; ports as listed or from an `int` setting |
| `"to": "lan"` | unicast private and link-local addresses (`LOCAL4` without multicast and broadcast), given as addresses, never as names; ports as listed, `"any"` is flagged in the index |
| `"to": "any"` | any address or name; made with `net_socket` |
| `"udp": count` | no destination: `UDP_OPEN` |
| `"discover": { "udp": port, "to": "broadcast" \| "<group>" }` | `BROADCAST`; answers come to the bound socket (a broadcast's answer is no reply to conntrack, as `lyrion.c` found) |

Never, whatever the entry: loopback, the Echo's own addresses, 0.0.0.0, port 5555. Loopback matters most: `-o lo`
passes the firewall, and behind it are hassmic's own ports and Amazon's daemons.

There is no lookup on its own. The broker resolves a name only as part of a `CONNECT` or `UDP_PEER` whose entry
allows that very name: the setting's value for `setting:`, anything for `any` (which has the internet anyway),
nothing for `lan`. A plugin with `lan` or `setting:` cannot spell data into DNS queries.

`NET` tells the plugin the Echo's address (SIP and SlimProto put it into their messages) and when it changes
(`netaddr.c`), since `getifaddrs` needs a netlink socket.

*As built (2026-11-21, phase 2: `net.server` and `CONNECT` alone; `plugin_net.c`):*
- **The entry** is 4.2's form, `{ "setting": "<host row>", "proto": "tcp" | "udp", "ports": [ ... ] | "setting:<int
  row>" }`, one to four of them, exactly those keys (a key this version does not read refuses the manifest: what is
  signed has one reading, and a later build's word must not be shown by an older Echo as something narrower), ports
  1 to 65535 and never 5555, a port row whose limits stay within 1..65535. Not 4.6's `"to": "setting:<name>"`: one
  form, the one the cases (4.11) were written in. Its line names each server by its row, protocol and ports
  ("Connects to the server you enter in "Syslog server" (UDP, the port you enter in "Port")"). No rule for less
  (3.3): any change of it asks for the button.
- **`CONNECT { kind, entry, host, port }`** for TCP and UDP alike: UDP connected (so `send` reaches that server only;
  `sendto` with an address is refused by the filter), which is all #193's forwarder needs; `UDP_OPEN`, `SENDTO` and
  the rest stay phase 4's. `kind` is the capability's bit, so the same message serves `net.lan` and `net.internet`
  later. `host` must be empty or exactly what the owner entered: checked before any lookup, so no query spells
  anything; ".local" names by mDNS as `net.c` does for media.
- **The owner's server and nothing else** (a review of 08399dc, N1, N2): neither row an entry names may have a
  `default` (refused at install; the index refuses it too), and the broker connects to what the owner stored
  (`ps_config_owner`), never to a default. With one, a plugin reached its author's server out of the box, outside the
  network too, with nothing typed. A change of either row, or the row cleared, starts the plugin again (`web.c`,
  `plugin_restart`): a socket it holds goes with its process, and the server the owner no longer means hears nothing
  more. A `CONNECT` before `READY` is refused (`BAD`): it may take 10 s, READY is due in 5 (N4).
- **Two worker threads** (`pnet_ask`, a queue of eight), two under way and thirty in any minute per plugin (the last
  thirty asked, a window that moves). Refused whatever the entry: 0.0.0.0/8, loopback, multicast, 240/4 and
  broadcast, an address of the Echo's own or its networks' broadcast (`getifaddrs`), an address the kernel does not
  route as unicast out of an interface other than loopback (`RTM_GETROUTE`: a "local" route delivers an address no
  interface has to the Echo itself over lo; N5), port 5555; and a socket whose own address is the server's once
  connected. A private or link-local address gets a plain socket (the firewall holds it to local addresses), any other
  a `net_socket` one (group 3990): the owner entered it. One deadline for a request, 10 s for its lookup and every
  address its name has (it was 10 s an address); a request whose process ended meanwhile is dropped unworked
  (`pnet_live`). The lookup is within that deadline too: `getaddrinfo` has none of its own (20 s and more for a name
  nobody answers, holding one of the two workers), so it runs in a thread of its own that the worker waits for until
  the deadline and no longer, four at once at most (a review of 0dabfa6, R7). Handed over non-blocking, hassmic's copy
  closed at once; the log says each connection and each failure (thirty a minute at most, as the connects), and a
  refusal of what the plugin may not do once in 10 s, the others counted into the next line (`REFUSAL_MS`; R5: a line
  each was 200 a second). While a `CONNECT` waits the broker sends that plugin no `LOG`. Which process a request
  is of is a generation new at every spawn, never repeated in a run (R6: `starts` began at 1 again when an entry
  was taken anew), so a socket for a removed plugin's process never goes to the next one's.
- **The firewall** (`lockdown.sh` `rules`): eight rules, `-A hassmic_out -m owner --uid-owner 69NN -j DROP`, first in
  the chain after adbd's (before loopback is let through), in every mode, checked by `wrong()` as every rule is. A
  range in the owner match is untried on Android's `xt_qtaguid`; the single-uid form is the one this chain has loaded
  on every model since the adbd rule. The inbound rule for 28940–28999 comes with `net.udp` (phase 4), as 7 says.

*As built (2026-11-21, phase 4: `net.udp`, `net.discover`; `plugin.c`, `plugin_store.c`, `lockdown.sh`):*
- **The entries.** `"net.udp": <1..4>` (its ports); `"net.discover": { "udp": <port>, "to": "broadcast" | "<a.b.c.d of
  224.0.0.0/4>" }`, exactly those keys, the port never 5555, the group written without zeros in front.  The ports
  are 28940-28999, the lowest free ones, kept in `state/plugins/<id>/ports` (net.udp's in order, then the search's) and
  the same at every start; a plugin's folder going frees them.  `UDP_OPEN` with `kind` net.udp opens entry *n*,
  with net.discover the search's socket; a plain socket (local only by the firewall's chain), bound to the port,
  `SO_REUSEADDR`, and a copy kept by the broker for the three operations.  The search's port is none of
  hassmic's own: not 2330 (Kiosk Satellite's claims) and not 28928-28999 (hassmic's TCP and UDP, the plugins' own
  ports), refused at install (A1 of a review of efde36e; the index's `manifest_check` the same).
- **Reachable from your network only, twice.** Each socket carries a classic BPF socket filter that drops every
  datagram whose IPv4 source is not 10/8, 172.16/12, 192.168/16 or 169.254/16 (`SKF_NET_OFF`, locked with
  `SO_LOCK_FILTER`; the plugin's `setsockopt` is held to three options anyway), which is all there is on a PC; and
  `lockdown.sh` has a chain `hassmic_in`, jumped to first in INPUT for UDP 28940:28999 on the network's interface,
  that returns for those sources and drops the rest, ahead of stock's accept of 16384:32767.  Loaded with the rest
  (`load_once`, `load_each`), checked by `wrong()` (the chain as loaded, its jump first in INPUT).  One chain, not
  rules in `keep`: `keep` is stock's rules as they must be, and a rule of ours inserted into INPUT would be appended
  after stock's accept by `missing`.  IPv4 only, as every plugin socket.
- **Where it sends.** `UDP_PEER` and `SENDTO` to a private or link-local unicast address, never the Echo's own, a
  network's broadcast, loopback or port 5555 (the firewall holds a plain socket to local addresses in any case; the
  broker refuses first), and only one the kernel routes as unicast out of an interface that is not loopback
  (`pnet_address_ok`, as for `CONNECT`: a "local" route made an address no interface has the Echo's own, and a
  datagram went over lo to every local listener of that port; A3 of a review of efde36e).  `PROPOSE` and the owner's
  click on a proposal ask the same (the click asks again whether the row is still one `net.server` names).  Fifty
  `SENDTO` a second; a new peer (`UDP_PEER`) one a second, two at once: what goes to a peer once it is fixed passes
  no broker, so the peer is what is counted (A2: unbounded, `UDP_PEER` and `send()` asked some two hundred addresses
  a second whether a port was open, `ECONNREFUSED` on the connected socket).  For phase 5 (SIP's RTP to what the server named) `net.server`'s
  host will be added there, with `call`.
- **The search.** `BROADCAST` only on the search's socket, once a second, to each up network's directed broadcast
  (its address and mask: an interface that was given no broadcast address reports 0.0.0.0), private networks only,
  or to the group (`IP_MULTICAST_TTL` 1).  The answers come to the socket as anything does, through the filter; and
  so does anything else a device on the network sends to that port while the plugin runs: an open port, which its
  install's line says ("can be reached from your network on a UDP port of its own", A4).
- **Never back to the Echo itself** (A1 of a review of efde36e).  The kernel hands what the Echo sends to a group
  or a broadcast address to its own sockets of that port as well, as if it had come in on wlan0 from the Echo's own
  address: a search to 2330 forged Kiosk Satellite's claims on its own Echo (one a second kept its wake word from
  answering), one to 28930 or 28932 reached arbitration and Drop In, one to 5353 avahi.  Three walls: the search's
  socket has `IP_MULTICAST_LOOP` 0 (a multicast is not handed back); a broadcast is handed back whatever a socket
  says, so the manifest names none of hassmic's ports (above); and `lockdown.sh` drops UDP that comes in on the
  network's interface from a plugin's port (28940-28999) and an address of the Echo's own, first in INPUT, ahead of
  stock's accepts (chain `hassmic_self`, `rules_self` a rule for every address of the Echo's own: `own4`, the
  "/32 host LOCAL" entries of `/proc/net/fib_trie` but loopback's; `ifconfig` named the first address of wlan0 only,
  and a second one's broadcast came back from it (L2 of a review of 92ac2e1); `ip` is not on the Dots and
  `xt_addrtype` in none of the models' kernels.  Read again at every look of the watcher, so a new lease is covered
  within 5 s; `wrong()` checks the chain and that its jump and `hassmic_in`'s are
  the first two of INPUT).  Nothing from the network has that source.
- **`NET`** after `READY` and when the address changes (the broker looks every 5 s while a plugin with a net
  capability runs): the first interface that is up, not loopback, with IPv4.

*As built (2026-11-22, `net.lan`, `net.internet`; `plugin.c` `connect_message`, `plugin_net.c`):*
- **The entries.** `"net.lan"` and `"net.internet"`: a list of 1 to 4 `{ "proto": "tcp" | "udp", "ports": [...] |
  "any" }` (3.3 "As built"), not 4.6's `"to": "lan"` / `"to": "any"` above: one form, net.server's, the capability's
  name saying where.  `"any"` is every port but 5555; the index flags a community plugin with a `net.lan` entry of
  any port and any `net.internet` (5.3).  `net.lan` never at DNS's ports 53 and 853, TCP or UDP (`plugin_store.h`
  `PS_PORT_RELAY`): refused in the manifest, under `"any"` by the broker and by the worker; `net.udp`'s `UDP_PEER`
  and `SENDTO` (`udp_dest_ok`) and `net.discover`'s port the same.  The home's router answers there by asking the
  internet for any name it does not know, so a listener with `net.lan` spelled what it heard into names under a
  domain of its author's and had the router carry them out (N1 of a review of 7be9343; the index's `manifest_check`
  the same, `DNS_PORTS`).  Other devices that relay are not closed (a proxy, a router's other services): the second
  press's screen says so to the owner of a plugin that hears requests and reaches "your network".
- **`CONNECT`** with `kind` `CAP_NET_LAN` (6) or `CAP_NET_INTERNET` (7), the same message as net.server's (the kind
  was the capability's bit for exactly this): the capability checked by kind in the broker (`messages.def`'s column
  says `net.server|net.lan|net.internet`), the entry by its place in that capability's list, the port one the entry
  lists or any but 5555 for `"any"` (0 only for an entry of one port), the host the plugin's own: for `net.lan` an
  IPv4 address written `a.b.c.d` (as written, no zeros in front: `ip_text`) that is private or link-local
  (`pnet_local`), refused (`DENIED`) before any socket otherwise, a name included: no lookup, so nothing a plugin of
  the network chooses goes into a DNS query, as for `setting:` above; for `net.internet` a host name or an address as
  a host row takes one (`ps_host_name_ok`), resolved by the worker as the owner's server is, and never an address on
  the local network: one written so is `DENIED` by the broker, one a name resolves to by the worker (N3 of a review
  of 7be9343: it reached every device of the home, also through a name its author points there, and "Connects to the
  internet" did not say so; a plugin that wants both asks for both).  The router's own public (WAN) address is no
  address on the local network and is taken: the router answers it from the LAN side like any server on the
  internet, often with its admin page (some show it only to the LAN) and through a port it forwards into the home
  (hairpin NAT) with that device; the Echo cannot tell which public address is its router's, so this is said, not
  closed (F2 of a review of 5aa32ee: SECURITY.md and the guide had "does not reach the home network").  The worker holds every
  address to `pnet_address_ok` whatever the kind (loopback, 0/8, multicast, 240/4, broadcast, the Echo's own and its
  networks' broadcast, an address the kernel does not route out of an interface, port 5555), a `net.lan`
  request's address to `pnet_local` again and a `net.internet` one's to not being so; a socket whose own address is
  the server's is closed (the Echo itself).  `net.lan`'s socket a plain one (the firewall holds it to the local
  network), `net.internet`'s a `net_socket` one: the owner accepted "the internet".  The same bounds as
  net.server's (two under way, thirty in a minute, 10 s for all of a request, `REFUSAL_MS` for the lines of what it
  may not do); the shared queue holds 16 (two each for eight).  The workers (N2 of a review of 7be9343): one for
  each plugin that may run (`WORKERS` `PLUGRUN_SLOTS`), and a worker takes the oldest request of a plugin none of
  the others works on, so a plugin's own two connects into nothing delay its own next request and nobody else's
  (two workers in turn for all held every plugin's for 10 s).  The lookups: counted until each one's thread ends
  (a name nobody answers holds it 20 s and more), `LOOKUPS_EACH` 2 per plugin, `LOOKUPS_EACH` x 8 of the names
  plugins chose and `LOOKUPS_OWNER` 4 more that only net.server's names take (four for all before: one plugin's
  names under a server that never answers failed every other plugin's lookups, the owner's server's too, with words
  that blamed the name); one not begun is `BUSY`, and the log says which bound.
- **The log.**  The worker's lines say the kind ("a device on the network (net.lan)", "outside the local network:
  net.internet"); for a plugin that hears requests (`request.audio`, `request.text`) or calls (`call`) they are private
  (`pnet_req.mark`): where such a plugin connects is its own choice now, a value a `log.read` plugin must not get.
  The refusal lines name no host the plugin gave.
- **The firewall** is unchanged: the plugin's user owns no socket, a plain socket of hassmic's reaches local
  addresses only, a `net_socket` one goes out.

### 4.7 Music source (`main.c`, `sendspin.c`, `a2dp.c`, `duck.c`, `audio.h`)

Today "the newest source wins" is a bit set (`MUSIC_SENDSPIN`, `MUSIC_BLUETOOTH`) and hand-written calls to each
source in `core_music`, `music_stop`, `on_action`, `on_multi`, `core_dropin`, the volume fan-out in
`core_set_volume`, `core_speaker`, `volume_sync`, and the start in `main()`. #198 touches all nine for one more
source. The seam, useful without any plugin:

```c
struct music_source {                       /* core.h; main.c keeps a table, a2dp first, as the button asks today */
    const char *id;
    void (*pause)(void);                    /* another source started, or a call */
    int  (*stop)(void);                     /* "<wake word>, stop": 1 = it played and is paused */
    int  (*button)(void);                   /* action button: 1 = taken (play/pause) */
    int  (*skip)(int dir);
    void (*volume_changed)(int percent);
};
int  music_register(const struct music_source *s);      /* the source's number */
void core_music(int source, int on);                     /* as today, by that number */
```

`sendspin.c` and `a2dp.c` register theirs (a2dp's `unyield` stays its own affair inside `pause`/`core_music`);
`plugin.c` registers one per running plugin with `audio.music`, whose functions send `MUSIC_CMD` and return at once
(`stop` and `button` answer from the state the plugin last reported). The music stream itself stays one at a time
(`music_open`, `audio.h`): the broker's stream thread opens it on `MUSIC_OPEN` after `core_music(n, 1)` paused the
others. A plugin source that also has `audio.volume` may set the Echo's volume (`VOLUME_SET` -> `core_set_volume`), as
Sendspin's server does.

*As built (2026-11-21, phase 4):* first, alone, `struct music_source` (`core.h`, commit "Music sources behind one
table"): `{ id, pause, stop, button(int resume), skip, volume_changed, unyield }`, `music_register`, `core_music(n,
on)`.  `button` takes the round: the core asks every source to pause what plays first and then every source to
resume what the button or a stop paused, which is the order the hand-written calls had (a phone, Sendspin, the
phone's resume); `unyield` is a source made silent for another one heard again, called when no other source is on
(a2dp's, after Sendspin's end and a Drop In call's).  A source never leaves the table: a plugin's is one of four
lanes (`plugin.c`, `LANES`), bound to the plugin that opened a stream and free once its process and stream are gone,
whose functions answer from the state the plugin last reported (`MUSIC_STATE`).  Who is "on" in the table is the
plugin's stream open, its player playing, and no other source having taken over since (`music_tell`, told again
until it holds: a lane's pause from another thread may come between).  Home Assistant's media player is the newest
plugin source's remote: pause, play and stop reach it (`plugin_music_ha`); its state is that source's while Home
Assistant's own media does not play; pause and play are offered only while a music plugin is listed (the features
of the entity, so the list changes with it).

*What hassmic enforces (a review of efde36e, M6).* "Plays sound on this Echo's speaker, whenever it likes" means a
plugin may start whenever it likes: a stream it opens, or "playing" said after "paused" or "stopped", makes it the
newest source and pauses the others.  It does not mean it plays through a pause: whatever hassmic pauses it for
(another source started: `MY_OTHER`; the button, the stop word, Home Assistant's pause or stop: `MY_HELD`), what it
writes from then on is dropped by the stream's thread at the stream's rate, whether it obeys `MUSIC_CMD` or goes
on saying "playing".  One held by hassmic is heard again when hassmic resumes it (the button's second round, Home
Assistant's play) or when it starts anew; one that yielded to another source also when every other source is off
(`unyield`, as a phone without remote control).  Before, a plugin was only told: one that went on saying "playing"
played over every pause and took every press of the button as its own.  Every change of it reaches the core's
table and Home Assistant's media player (`music_due`, the broker's loop: the lanes' functions run inside the
core's `core_music` or under its lock, where neither may be called).  A stream's end tells the core while its
entry still holds the stream and with it the lane (M2: told after the stream was taken out of the entry, a process
that ended in between had the lane freed, the core never heard, and its bit for that source stayed on: Amazon's
engine heard music for good, sound detection dropped every window, a phone was never heard again).  And it tells
the core by the lane it opened in (`struct mstream` `src`), never by what the entry says by then, and `start()`
takes no free entry whose stream has not ended (L3 of a review of 92ac2e1: a plugin removed while its stream ended
had its entry freed at once, another plugin's start took it, and the thread found no lane there).  `VOLUME_SET`:
five a second and twenty a minute, and the Echo's volume set from the latest at most every 500 ms, the log's line
once in 10 s (M5: each set was a line, the ring's pattern and a write to the flash).  One that waits for that is
dropped when the volume is set from elsewhere meanwhile (`plugin_core_state`; the plugin's own value coming back is
not elsewhere: `vol_own`): it was set after Home Assistant's or the keys' newer one and undid it (L4).  For the
PC's tests a stand-in source in the table (`HASSMIC_FAKE_SOURCE`): a phone over Bluetooth cannot play there.

### 4.8 Calls (`call.c`, `call.h`, `dropin.c`, `main.c`)

From #186, in-tree first: `call_claim(who)` / `call_release` / `call_claimed` (one call on the Echo at a time,
claimed from calling or ringing on; Drop In refuses "busy" while a plugin has it and the other way round).

New in `call.c`: a media without a codec. `call_begin(who, CODEC_PCM, send, arg)` hands `send` the gated,
gain-corrected 20 ms frames as PCM instead of Opus, and `call_rx` takes PCM frames by sequence number into the same
jitter buffer (a missing frame: the last one faded, where Opus conceals by itself). Gate, echo suppressor, the
call microphone (`micHfp`, `callmic_thread`), listening mode and the Voip stream stay exactly `call.c`'s; G.711 and
its 2:1 filter, RTP and SIP are the plugin's. `tx_mu`'s rule holds: `send` for a plugin is one `MSG_DONTWAIT` send.

`core_dropin(st, peer, outgoing)` becomes `core_call(who, st, peer, outgoing)` with Drop In as one caller: ring,
call sounds, LED patterns, the action button answering or ending, "stop"/"hang up", music paused, `screen.c`'s call
view. For a plugin: `CALL_INCOMING` rings (within 3.6's limits), the button sends `CALL_ANSWER`, the plugin answers
on its side and says `CALL_LIVE`, the broker begins the media. `proto` gets no new members for it (#186 added
three): what Home Assistant sees of a plugin's calls are the plugin's own entities (4.10).

*As built, in-tree first (2026-11-21, its own commit, Drop In's tests unchanged):* `call.h` `call_claim(who)`,
`call_release(who)`, `call_claimed()` on one atomic (any lock or none: `dropin.c` takes it under `dl`, the broker
under its own, without an order between them), who `CALL_DROPIN` or `CALL_PLUGIN` + the broker's entry; the call's
states are Drop In's (`CALL_IDLE` .. `CALL_LIVE`, `dropin.h`'s the same numbers).  Drop In claims as it calls
(`dropin_call`, last of its checks: a refusal before it leaves nothing claimed) and as an invite is taken, and lets go
once the core has heard its "idle" (`tell_core`, and only while no call of Drop In's began meanwhile; until
2026-11-22 in `end_call`, up to 20 ms before the core heard it: a plugin's call that began in between was shown idle
by that late "idle", C1 of a review of fc43c40); Drop In's own claim with no call (that moment) is no refusal of an
invite; an invite while another has the claim is refused in Drop In's old words, "busy in another Drop In",
which every released build maps to `busy` (words of its own would be `failed` on the Echos before).  `core_dropin` is
`core_call(who, st, peer, outgoing)`; `proto->dropin_changed` is called for `CALL_DROPIN` only.  What a person does
goes to whoever has the claim: `core_call_button` (`on_action`), `core_call_answer` and `core_call_hangup` (the
screen's lines, which stay "dropin answer"/"dropin hangup" in the UI's protocol; `stop_word`; "hang up" in a
transcript), `core_call_status` for the screen's peer.  Home Assistant's "End Drop In", the page's `/api/dropin` "end"
stay Drop In's.

*As built, then (2026-11-21, API 0.4):* `call_begin(who, codec, send, arg)` refuses a `who` that has not the claim;
`CALL_CODEC_PCM` skips the encoder (gate, suppressor, gain and the call microphone unchanged) and sends the frame as
it is in memory (s16le on every Echo); `call_rx` takes only 640-byte frames for it; the playout plays them by number,
and a missing one is the last one again, its level falling by half across each frame (`PCM_FADE`), `PLC_FRAMES` at
most, then silence (no FEC: the next frame is not borrowed).  The plugin's side is `plugin.c` "calls": one call
(`pc`), its states (ringing, answered here, going out, live), a teller thread that calls `core_call` in order with
no lock of the broker's held (the button and a "stop" come from under `core_lock`: `core_transcript` holds it) and
lets the claim go once the core said idle (a call begun meanwhile would be shown idle by the end of the one before;
a plugin's new call meanwhile is answered busy).  And `core_call` shows only the call that has the claim: a state from
any other `who` is logged and left (the second half of C1's fix, should a late state ever come again).  A person's
answer here tells the core `CALL_ANSWERING` (a plugin's only; Drop In's states are Home Assistant's and stay four):
the ringtone stops at the press and the light and the screen show the call connected while its far side comes in,
up to 10 s, where it rang on before (C4 of a review of fc43c40).  The teller is started in `plugin_start` and again
whenever states wait without one (`teller_start`; under `pthread_once` a failed start was never tried again).
The far side's numbering (`call_rx`): frames more than 64 away from the newest either way, three in a row each
following the one before, start the jitter buffer anew (a 16-bit RTP number's wrap, a stream begun again); one
stray number far ahead used to become the newest and leave every later frame "far too old", silence for the rest of
the call (W3); a frame exactly 32 behind the newest is dropped, not written over the newest's slot (W4).  The new
start is for a plugin's call only (`m.who`): Drop In's numbers are hassmic's own, and a LAN host that sent captured
Drop In packets again (they pass the AEAD, the nonce is in them) had three old frames in a row start the buffer anew
from them, the far side's own frames then dropped as "far": the Echo replayed its earlier words (D1 of a review of
e8d52b9).  `dropin.c` `on_audio` takes each frame once besides (a window of 32 numbers behind the newest, RFC 4303's),
and only a frame newer than all moves the address the room goes to and keeps the call alive (`rx_at`): before, any
replay moved them, the room went to the replaying host (sealed, but not to the other Echo) and a call outlived its
other Echo.  The line that says the buffer started anew comes once, then at most once in 10 s with the count since,
and the call's count at its end (D2: a plugin switching between two numberings had ~33 lines a second).  Drop In
lets its claim go when its queue of states for the core is empty and no call of its own runs (`tell_core`), not at
each "idle" told: a Drop In placed on the claim the one before had left, before that one's "idle" was told, lost the
claim to it and was never shown (D6); and the queue, full while the core is held up, drops results and states of
calls that are over before anything else, never the current call's states nor the last "idle" (D5: the oldest went,
"idle" included, and the claim stayed Drop In's for good).  `HASSMIC_DROPIN_LATE_IDLE=<file>` (tests): while it
exists Drop In's end goes in the order before C1's fix, so that `fake_plugins_call.py` 6d has `core_call`'s guard
alone keep a plugin's ring shown.  `core_call_button`/`_answer`/`_hangup` reach `plugin_call_button`,
`plugin_call_answer`, `plugin_call_hangup` while a plugin has the claim; Drop In's `proto->dropin_changed` is never
called for it, so Home Assistant sees a plugin's calls only through the plugin's own entities.

### 4.9 The assistant: not a plugin

The first drafts had a `pipeline` capability: a plugin answering requests in Home Assistant's place, with the
request's sink split out of `struct proto`. The maintainer decided against it: Home Assistant is a hard
requirement, it is the whole idea of the project. So there is no such capability, `struct proto` stays whole, set-up
still means Home Assistant adopted the Echo, the clock still comes from its keyed link, and none of the questions
that came with a second sink for requests (who owns what plays, follow-ups a plugin chains, announcements beside a plugin's
replies) arise.

#102 (OpenClaw) is therefore out of scope for plugins. Its place is in Home Assistant, as a conversation agent of
the voice pipeline the Echo already feeds: the Echo sends the request where it always does, and which agent
answers is Home Assistant's setting. That needs nothing on the Echo.

### 4.10 Entities (`proto_esphome.c`)

Named in the manifest, so the owner sees them at install and the list never changes while a plugin runs. Kinds:
binary sensor, sensor, text sensor, switch, button, number, select, event (with its types); actions with string and
integer arguments, each with `"calls"` and `"page"` as 3.6 and 4.3 use them. 16 entities and 8 actions per plugin.
Object id `p<length of the id>_<id>_<name>`, name "Plugin <length of the id> <id>: <label>", key given once and kept
(see As built), so it stays the same across restarts.

`listed()` learns one more source: a plugin entity is listed while the plugin is enabled (running or not; not
running, its states are unavailable). Enabling, disabling, installing or removing one calls
`core_entities_changed()`, as a feature does: the links close, HA lists again and drops what is gone. States at
most 10 a second per plugin; strings cut to HA's 255 characters; over the keyed link only, as every entity.
A message that would not fit its buffer is not sent (`pb.h` `over`), as for hassmic's own.

*As built (2026-11-21, phase 4):* entities `{ name, kind, label, icon }` and by kind `unit` and `decimals`
(sensor), `unit`, `min`, `max`, `step` (number), `options` (select), `types` (event), nothing else; 16 at most, the
names `[a-z0-9_]{1,32}` and none twice, the labels none twice (H3 of a review of efde36e).  Actions `{ name, label,
args: [{ name, type: string | int }], page }`, 8 at most, 4 arguments each, the labels none twice; `calls` (true or
false) came with `call` (phase 5, API 0.4) and is refused without it.  An entity's `page` (true or false; not on a
button or an event) came with API 0.7: a row on the card (4.3 "As built", button and rows), no change for Home Assistant.

What Home Assistant keeps an entity by is its type and its **name** (aioesphomeapi `build_unique_id` version 3:
`<mac>/0/<type>/<name>`), not its key or object id; an action's service by its object id.  A review of efde36e found
all three could be another's (H2, H3):
- **The key** is given, not computed: the next of a counter that only rises, kept with every key given in
  `state/plugins/.hakeys` (`next <hex>`, then `<hex> <id> e|a <name>` a line; `plugin_store.c` `hakeys_assign`, at
  every scan), for as long as the plugin is installed and names that entity or action; never one another plugin
  had (the counter), never in hassmic's own range (0x40000000 up).  A key is used only once the file holds it: one
  that cannot be written down is none, and that entity is not listed until it can.  `0x40000000 | FNV-1a(object id)`
  in 30 bits gave two plugins one key (a pair found in three minutes), and then one's states and commands were the
  other's, and Home Assistant took the later one for the first renamed.
- **The name** is "Plugin <length of the id> <id>: <label>" (`plugin.c` `ha_name`), and it makes the **entity id**
  as the entity is first made: `<domain>.slugify("<area> <device's name> <entity's name>")` (Home Assistant 2026.10.1:
  unidecode, lower case, every run of anything but a-z0-9 one "_").  Home Assistant removes the registry entry of an
  entity that is no longer listed (`entry_data.async_remove_entities`: a feature switched off, a plugin switched off
  or removed), so the next entity whose name slugifies alike takes its entity id, and the first gets "_2" when it
  comes back: automations and history follow the other.  "<plugin's name> <label>" was "Wake sound" for a plugin
  "Wake" with a label "sound", hassmic's own switch (H3 of a review of efde36e); "<plugin's name> · <label>" was the
  same once slugified, "Last · heard" `sensor.<node>_last_heard` (K1 of a review of 92ac2e1).  Now the slug begins
  `plugin_<digits>_`, which no name of hassmic's own makes (`tests/unit/ha_names_test.py`: every string of
  `proto_esphome.c`, its run-time names (`Light ring LED %d`) and arrays of names too, slugified with Home
  Assistant's own `slugify` where `build/ha-venv` has it, else a copy); and the id's length says where the id ends,
  so two plugins never make one entity id ("weather" with "x temp" and "weather-x" with "temp":
  `plugin_7_weather_x_temp` and `plugin_9_weather_x_temp`; the id alone, or a length after it, does not do that).  The names, and so the unique
  ids, of two plugins are never alike either (the id is in them): a dedupe of names between listed plugins
  (`ha_hidden`) was the fix before and missed a plugin switched off (K2), and is gone.  Two labels of one plugin
  that slugify alike are that plugin's own, as on any device: Home Assistant gives the second "_2".
- **The object id** is `p<length of the id>_<id, - as _>_<name>`: `p_a_b_c` was the id "a-b" with the action "c" and
  the id "a" with "b_c", one service for two.  None of hassmic's own begins with "p" and a digit (the same test).

Listed while the plugin is switched on and may run (accepted, not refused, plugins on, a launcher and a sandbox
that holds: listed with none, every entity stood there unavailable for good), running or not.  Whatever changes the
list closes the links as a feature's switch does (`ha_relist`: a digest of the list, compared wherever it may have
changed and every 5 s by the broker; `core_plugin_entities_changed`): an install, an update (one that dropped
`ha.entities` was asked about by what the new version had, so the links stayed and Home Assistant kept the old
entities, frozen: H4), a removal, the switch, a revocation list, plugins switched off, the launcher gone.  While it
does not run its states are unavailable (`missing_state`; a switch has none and says nothing).  States checked by
kind before they are kept (on/off, a number, within a number's limits, one of the options, text of strict UTF-8
with its control characters made spaces), ten a second for all of a plugin's, kept by the broker and told to Home
Assistant from its loop; events by their types.  Strict UTF-8 (`ps_utf8_char`: no overlong form, no half of a
surrogate pair, nothing past U+10FFFF) for every text of a plugin's that reaches Home Assistant, the page or the
log: a text sensor's state with C0 80 passed a check by the lead byte, Home Assistant's link dropped on it, and on
every link after, since the state is sent again at each (H1); a title is cut where it stops being UTF-8, a line of
its stderr or a reason it gives is mended ("?" for each byte that is no character), a field's value is refused.
A number's limits are the float32 Home Assistant has: the slider's top for a limit float32 does not hold (0.1) lies
past the manifest's double and was refused (H5); it is taken as the limit.  Commands (switch, number within its
limits, select, button) and actions only over the keyed link, as Drop In's action (before Home Assistant set a key
any client on the network could set a plugin's switch), to a running plugin, as `ENTITY_SET` and `ACTION`; each of
an action's arguments as its manifest types it (an empty string is no field on the wire, and came as the int 0:
H6).  Listed only, decided against the brief's "only while it runs": a plugin that crashes in a loop would close
Home Assistant's links each time.

### 4.11 The cases

| | #193 syslog | #198 Lyrion | #186/#199 SIP |
|---|---|---|---|
| Capabilities | `log.read`, `storage` (the cursor), `net.server` (udp/tcp) | `audio.music`, `audio.volume`, `net.discover` (udp 3483 broadcast), `net.server` (tcp; found by discovery: proposed, the owner confirms once), `net.internet` (tcp, for streams Lyrion names: radio) | `call`, `net.udp` (2), `net.server` (SIP by `SENDTO`, RTP by `UDP_PEER`), `ha.entities`, `ha.actions` |
| Brings | RFC 5424/3164 formatting | SlimProto, dr_flac, minimp3, its volume curve, JSON-RPC | SIP (libre or the PR's `sip.c`), MD5, G.711 + FIR, Opus |
| Settings | server, port, protocol | server | server, user, password (`secret`) |
| Entities | none | none (Lyrion is its remote) | Phone, Phone line, Phone result (event); actions `call` (`calls`, `page`), `hang_up` |
| hassmic changes | `logread.c` out of `web.c`; the broker's core; page | `struct music_source` (4.7); `MUSIC_*`; discovery, proposals | `call_claim`, `CODEC_PCM`, `core_call` (4.8); tokens |
| Plugin size, guessed | 200 lines | the PR's `lyrion.c` less its output and core glue: ~900 | the PR's `sip.c` + codecs: ~1500 |
| Lost against the PR | a syslog line never carries what was said (official: `log.read.full` if the owner wants it) | "finds its server by itself" takes one click; the rest the same | the page's own phone section becomes the card's rows and a call button; the screen names the caller as text beside the plugin's name; no automatic anything; no answer from Home Assistant or the page (3.6: a call coming in is answered only at the Echo, so an `answer` action could do nothing but leave a note, and the plugin has none) |

None of the three needs `request.audio` or `request.text`; no issue or pull request asks for them yet. None of
the three is the first plugin any more: that is microWakeWord (4.13); the syslog forwarder is left to a community author (5.8).

**#102 (OpenClaw as the assistant): not a plugin, and not on the Echo.** It belongs in Home Assistant as a
conversation agent (4.9).

### 4.12 Request audio (`main.c`, `proto_esphome.c`, `proto_wyoming.c`)

Decided by the maintainer after the `pipeline` capability was struck: a plugin must be able to get the audio that
is sent to Home Assistant. Not to answer in its place (4.9 stands), but to listen in.

**Where the copy is taken** (*checked*, `main.c`). Request audio reaches the protocol in two places, both with
`core_lock` held: `lead_in()` (the 800 ms of zeros, straight to `proto->audio`) and `send_out()`, the end of the
chain `send_mic()` -> `earsup_run` -> `send_out` (`micend_run` looks at the level, then `micdenoise_run`, then
`micgain_run`, then `proto->audio(out, m * 2)`). Both become one function:

```c
static void mic_out(const void *pcm, size_t len)    /* lock held: a request's audio as Home Assistant gets it */
{
    proto->audio(pcm, len);
    plugin_request_audio(pcm, len);                  /* plugin.c: a non-blocking send per plugin with the capability */
}
```

That is the whole tap: after the protocol has the block, the same bytes, one `MSG_DONTWAIT` send per listening
plugin, from the capture thread with the lock held, which a send that cannot wait may do. With `-w remote`
`plugin_request_audio` passes blocks on only between `core_remote_wake` and microphone off (3.6); the flag is the
core's, set and cleared where listening mode is.

`REQ_START` comes from where the run starts (`trigger()` and the follow-up's start: the source, the wake word
slot and `core_wake_phrase`), `REQ_END` from where the microphone goes off or the run is given up (`core_mic_off`,
`micend`'s end, `quiet_cancel`, the pipeline timeout, an error, a lost link), `REQ_TEXT` from
`core_transcript`'s callers and the reply's text in `proto_esphome.c`, after `core_transcript` passed it (as
`screen_words` gets them). All queued for the broker thread; none sent from under the lock but the audio.

**What comes back.** A plugin's results reach Home Assistant only through its own entities (4.10): an event
entity that carries who spoke, a sensor, a text sensor. They cannot go into Home Assistant's pipeline, and not
because this design forbids it: the ESPHome voice assistant API has no message by which a satellite hands Home
Assistant text in place of audio. The satellite sends `VoiceAssistantRequest` (start, flags, the wake word's
phrase) and `VoiceAssistantAudio`; everything else of a run comes the other way as `VoiceAssistantEventResponse`
(*checked*, `proto_esphome.c`: `VA_REQUEST`, `VA_AUDIO` out, `VA_EVENT` in). What an owner can do with a plugin's
event is an automation in Home Assistant (`conversation.process` with the text it carries, a condition on who
spoke), which runs beside the pipeline's own answer, not instead of it.

**What it is for, and what it is not.** A tap serves what wants to hear the request *as well*: who is speaking,
a recording for the owner's own training data, keyword or intent tricks beside Home Assistant's, logging. It does
not serve replacing Home Assistant's speech-to-text with an engine on the Echo: Home Assistant would still run
its own on the same audio, and the plugin's transcript has no way in.

For that there is a way that needs no tap at all, **a later case of its own**: the plugin as a Wyoming STT
server on the Echo, which Home Assistant connects to as it does to any Wyoming service and sends the audio it
got from the Echo back to. The Echo's side of the request does not change. It needs things this design does not
have yet: a TCP listener (3.7: the broker would accept and hand the connection over), model files as read-only
data in the package (tens to hundreds of MB, against a 4 MB package and a 32 MB limit for all programs), a
memory limit well above 96 MB and a CPU budget that lets it run flat out for the seconds a request takes without
starving the mixer. Whether a Dot 2 (4 cores at 1.3 GHz, ~380 MB free) runs any model worth having is unmeasured.
No capability is reserved for it; it is written down so that `request.audio` is not stretched to do it.

*As built (2026-11-22, phase 6):* `mic_out` as above (`main.c`, its two callers `lead_in` and `send_out`), with
`req_open` in front of `plugin_request_audio` (the stream of `-w remote` is no request until `core_remote_wake`).
`plugin_request_start`, `_end`, `_text` take `bl` only to queue (`rq`, 32 messages, a pipe `bwake` that wakes the
broker's poll; the broker sends them, `rq_drain`, also from `timers()`): no message is sent from under `core_lock`, as
written; the audio is the one send there.  A plugin that is not running (`READY`) when a request begins gets none of
it.  The sources: `REQSRC_WAKE_WORD` (its slot, `kw_slot`), `BUTTON`, `FOLLOW_UP` (`core_restart_after`, and a
remote follow-up), `HA_WAKE`; a reply cut by the wake word or the button restarts as that (`barge_src`).

### 4.13 `wake`, and the first plugin: microWakeWord

The pilot is no longer the syslog forwarder. It is hassmic's second wake word engine, microWakeWord, taken out of
the core and made the first plugin, official and the maintainer's own. It is a harder first case and a better
one: it runs all the time, it needs the most sensitive capability, it has owner-uploaded files and a migration,
and what it removes from hassmic is a parser of uploaded files.

**What is there today** (*checked*):

| Where | What | Lines |
|---|---|---|
| `src/hassmic/wake.h:36-43` | `struct wake_engine { open, feed, reset, property, close, afe_times }`, two instances `wake_vendor`, `wake_mww` | |
| `src/hassmic/wake.c:13-17, 30` | the engine is picked per slot by the model's name: a `.json` manifest is microWakeWord's, else the vendor's | 59 |
| `src/hassmic/wake_mww.c` | the engine: features, inference, the window's mean against the cutoff, **in `wake_feed`, on the capture thread** (`:3`); its callback is called from there | 98 |
| `src/hassmic/mww_features.c`, `mww_model.c`, `mww.h` | TFLite Micro's front end; an int8 TFLite interpreter that parses the uploaded flatbuffer | 313, 667, 89 |
| `src/hassmic/mww_store.c` | `state/mww/<id>/{manifest.json,model.tflite}`: add, edit, delete, list, install a copied one | 385 |
| `src/hassmic/web.c:790-798, 1274, 420` | `/api/mww`, `/add`, `/edit`, `/delete`, `/use`, `/file` and the upload's size limit | |
| `web/app.js:1666-1946`, `:312-351` | the model library and its dialogs; the engine comparison | ~280 |
| `src/hassmic/artifacts.c:10, 92-93, 212-215, 476-478` | the artifact kind `mww:<id>`, so that the page copies models between Echos | |
| `src/hassmic/main.c:464, 477, 514-533, 596-628, 3741-3748` | `WAKE_MWW`, `state/mww_word`, `mww_words_scan`, the engine switch, the fallback to Amazon's when a model does not load | |
| `src/hassmic/settings.c:37, 119-120` | `wake_engine`: `amazon` / `microwakeword` / `homeassistant`, exportable | |
| `Makefile:233, 379-382`, `tests/run.py:54` | the build's sources; `unit-mww` (`tests/unit/mww_test.c`, `mww_ref.py`); `tests/fake_web_mww.py` | |

**The seam** is there already and needs one more instance:

```c
extern const struct wake_engine wake_vendor, wake_plugin;      /* wake.h; wake_mww goes */
```

`wake.c` picks `wake_plugin` for a model named `plugin:<plugin id>/<word id>` where it picks `wake_mww` for
`*.json` today (`wake_is_mww`, `wake.c:13`). `wake_plugin` (in `plugin.c`) forwards: `open` sends `WAKE_OPEN` and
returns at once (0 when that plugin runs and has listed the word), `feed` is one `MSG_DONTWAIT` send, `reset`
sends `WAKE_RESET`, `property` does nothing (as `wake_mww`'s, `wake_mww.c:95`), `close` sends `WAKE_CLOSE`,
`afe_times` returns 0 (as `wake_mww`'s, `:96`). The callback is the one difference in kind: today it runs inside
`wake_feed` on the capture thread; from a plugin it comes on the broker's thread, some milliseconds later, with
the detection's place in the stream. The core is built for that: Pryon's callback also comes from a thread of its
own (`wake.h:1`), with `begin` and `end` as sample counts on `wake_fed()`'s clock (`wake.h:14-16`), and `on_wake`
(`main.c:2075`) hands the detection to the capture thread.

**The seam is wider than that one struct**, and this is the inventory (*checked*):

- *Opening is synchronous today, and the fallbacks hang on its result*, at four places: `main()`
  (`main.c:3741-3748`: no model loads, Amazon's engine at once), `wake_open_retry` (`:3446`), `wake_load_second`
  (`:3457`) and `wake_load` (`:3473`, which on a failure writes `wake_engine=amazon` into the settings,
  `:3481-3483`). The last three run on the capture thread (`wake_load` is called from its loop, `:3547`) and
  cannot wait for a plugin. So a slot gets states: closed, opening, open, failed. In `main()` the open waits
  (2.6). On the capture thread `wake_plugin.open` only sends `WAKE_OPEN` and marks the slot opening; `WAKE_READY`
  or `WAKE_FAILED` (or 5 s) arrives on the broker thread, which sets the slot's state and raises `wake_switch`,
  and the capture loop's next round acts on it as it acts on a switch today. A slot that is opening is fed.
- *"Is this microWakeWord" steers more than the engine pick*: the wake word's language and its aligner model by
  name instead of by locale (`wake_is_mww` at `main.c:1780, 1805, 1811`), the vendor-only steps of opening
  (`:2058`), the retry that only microWakeWord gets (`:3449`). Each becomes "is this a plugin's word".
  `/api/mww`'s own checks (`web.c:806, 853`) go with the endpoint.
- *The engine setting* is a static choice of three (`settings.c:37`) and becomes a list read at run time (4.3).
- *What a detection's positions feed*: the replay's start and the aligner, and also the level of the keyword
  that the gain starts from and that arbitration's own score is made of (`keyword_db` from the ring between
  `begin` and `end`, `main.c:2095`). A plugin that picks its positions picks those numbers, within the limits
  below. For the gain that is what a model with a sloppy window does today; for arbitration it means a plugin
  can make its Echo win or lose rounds. Accepted for a sealed, official engine; named for any other.

**Wanted and running are two things.** Today a microWakeWord model that does not load makes hassmic *save*
Amazon's engine as the setting (`main.c:3481-3483`, `:628`): the choice is gone. With a plugin the engine can be
down for reasons that pass (not installed yet, crashed, being updated), so the setting keeps what the owner
chose and a second, unsaved state says what listens now. What listens while the chosen plugin is down is a rule
(decided by the maintainer, 12; an earlier revision had a setting `wake_fallback` for it, which is gone):

- **The plugin is missing** (no package of it installed: the cut of 5.9 before the fetch, an Echo without
  internet, a removed plugin with the engine still chosen; also `PLUGINS=0`, under which it cannot start):
  **Amazon's wake word** listens, "Alexa" or whatever Amazon's pick is. An Echo that answers to "Alexa" for a
  while is found out by speaking to it, and one that answers to nothing looks broken.
- **The plugin is installed and not listening** (crashed, its model does not load, no `WAKE_READY`, detections
  over the rate limit): **nothing** listens for a wake word; the action button works. An owner who chose
  "Okay Nabu" so that nobody says "Alexa" does not get "Alexa" back because of a crash.

Either way the settings page shows a warning, on every page and not only on the Plugins page, that says which
of the two it is, what listens now and what ends it (install from a file or copy from another Echo; the reason
code and the next retry). Home Assistant sees it too: a binary sensor "Wake word engine" of device class
problem, listed while the engine is a plugin, on while the plugin is not the one listening. The plugin is tried
again every 5 min without end (2.6), and the Echo goes to it by itself, from Amazon's word or from none.

**What stays in hassmic, what moves:**

| Stays | Moves into the plugin | Goes |
|---|---|---|
| Pryon (`wake_pryon.c`) and the vendor engine's hints | `wake_mww.c`'s logic (features, inference, window, hold) | `/api/mww/*` in `web.c` |
| the two slots, `wake_act[]`, `state/wake_word` and one pick file per engine (`main.c:477`) | `mww_features.c`, `mww_model.c`, `mww.h`'s first half | `app.js`'s model library (replaced by the generic `files` section, 4.3) |
| Home Assistant's "Wake word" selects and the page's pick: the list comes from `WAKE_WORDS` where it comes from `mww_words_scan` (`main.c:514`) | reading its manifest (name, cutoff, window: `mww_manifest_parse`) | `artifacts.c`'s `mww:<id>` (replaced by the plugin's sync, 5.4) |
| the wake word's end by `wakealign` (a model per wake word and language, found by the word's name slug), `wordtail` | checking an upload by loading it (`FILE_CHECK`) | `mww_store.c` (the store is 4.4's `files`) |
| arbitration, with hassmic's own SNR score for an engine without front end marks (`wake_afe_times` 0), as now | `tests/unit/mww_ref.py`, `mww_test.c` (bit-exactness against pymicro-features and TFLite) | `unit-mww`, `tests/fake_web_mww.py` as they are; a `wake` test with a stand-in plugin takes their place in-tree (6) |
| `wake_word_in_request` (it uses the detection's `begin`), `-w remote`, the engine setting, the fallback's rule (Amazon's word while the plugin is missing) | | |

What microWakeWord lacks today it still lacks: the "stop" keyword, Pryon's threshold hints, the front end's
scores.

**The stream and the detections.**

- `WAKE_WORDS`: the ids of the items the plugin can listen for, from its files; sent at start and whenever they
  change. The phrases are hassmic's, fixed when each item was uploaded (3.3). The core lists them for the engine
  (16 at most, `MAX_WAKE_WORDS`, `main.c:458`) and tells Home Assistant as today (`core_wake_models_changed`).
- `WAKE_OPEN { slot, word }`: for each active slot. The plugin loads the model and answers `WAKE_READY` or
  `WAKE_FAILED` with a reason code for the log and the page. A failure, or no answer in 5 s, leaves the slot
  without a wake word (above: the plugin is installed, so Amazon's does not stand in) and the page warns.
- The stream (3.5): every block `wake_feed` gets, with its position. Not gained, not denoised: microWakeWord's
  front end normalises the level itself (`wake_mww.c:8-10`).
- `WAKE_HEARD { slot, begin, end, score }`: positions on the stream's clock, the mean probability 0..255. The
  plugin applies its own hold after a detection (`HOLD_INFERENCES`, about 1 s, `wake_mww.c:19`).

**What the core takes from a plugin.** A detection is input from outside and starts a run, so it is checked:
the slot is open with that plugin; `begin < end <= wake_fed()`; `end` no older than 300 ms; `end - begin` between
0.3 and 1.5 s; at most one a second per slot and 20 in 10 minutes, after which the engine is taken as broken,
nothing listens until the next retry and the page warns. The numbers come from what the detection is used for. Arbitration
waits 150 to 1000 ms for the other Echos' claims, learnt per Echo: a detection reported a second late claims
after the others decided, and two Echos answer. The ring holds 4 s (`RING_SAMPLES`, `main.c:1434`) and has to
cover the word, the aligner's 1.5 s back from its end, the 100 ms ahead of the word with `wake_word_in_request`,
and the time the report took: 1.5 + 0.3 s leave room, the first draft's 3 s and 1 s did not. microWakeWord's own
window is 1 s (`wake_mww.c:90`: `begin = end - 1 s`). Whether 300 ms holds on a busy Dot 2 is the first thing
the CPU measurements have to say. A detection that fails is dropped and counted. Everything after that is the
core's as for any wake word: arbitration, the sound, the ring, the run.

**Never the capture thread's wait** (issue #77). `feed` is a send that cannot wait; a plugin that falls behind
loses blocks and sees the gap in the positions (it resets its front end, as `wake_reset` has it do). The capture
thread gets faster, not slower: today the inference runs on it (`wake_mww.c:3`), then it only copies a block
into a socket.

**CPU.** This plugin never rests, so it is the one whose cost has to be known. What is known: 0.05 ms of CPU per
30 ms of audio on a PC for the largest model, "not measured on an Echo yet" (`wake_mww.c:3-4`). To measure on a
Dot 2 before anything is promised, each over 10 minutes, idle and with music playing: (1) today's in-core engine:
the capture thread's CPU time with one and with two microWakeWord slots against Amazon's; (2) the same models in
the plugin: the plugin's CPU time (the launcher's `EXITED` accounting, `/proc/<pid>/stat`), the capture thread's
time for the sends, and what the front end loses to blocks dropped; (3) detection latency: `wake_fed()` when
`WAKE_HEARD` arrives minus its `end`, as a distribution, against the 1 s limit; (4) the same with the Echo busy
(a reply playing, an update unpacking). A `wake` plugin cannot live in the plugins' CPU group with a quarter
share (2.3): the launcher puts it in a group of its own with an ordinary thread's weight and no `nice`, because a
wake word engine that starves is an Echo that does not answer.

**The security gain.** `mww_model.c` reads a TFLite flatbuffer that a browser uploaded: every offset checked,
quantisation validated, accumulators bounded, fuzzed. It does that inside hassmic, on the capture thread, next to
the API key and the network key. As a plugin the same code reads the same file in a process that has no file, no
socket and no key: a model that gets past the checks breaks the wake word and nothing else. 1552 lines and six
endpoints leave hassmic.

**Its manifest**: `wake`; `files` (the model list of 4.3, 16 items, 17 MB at the outside by today's limits,
`mww.h:57-58`; 4 MB asked); nothing else. `memory_mb` 16, `threads` 2. No settings but the list. Sealed (3.3): it
cannot fetch a model, which is why the catalog is the browser's.

## 5. Packaging, index, SDK

### 5.1 Package

`<id>-<version>.hmp`: a tar (the page reads it with `web/tar.js`) of `manifest.json`, `manifest.sig`, `plugin`,
`LICENSE`, and, for a package the index lists, `listing.sig`: a signature by the index key (5.2) over "hassmic
plugin listing 2", the plugin's id, version, tier (official or community), the manifest's digest and the index's
serial at the time (as built: the signature as hex on the first line, then "hassmic plugin listing 2\nid <id>\n
version <n>\ntier <tier>\nmanifest <digest>\nserial <n>\n"; form 1 was the release key's, made in hassmic's own
releases until 2026-11-20, and no build takes it). Every signature of a plugin is over a text that starts with
its own purpose ("hassmic plugin manifest 1", "... listing 2", "... index 1", "... revoked 1"), so that none
passes for another. With it a package proves by itself where it comes from, whichever way it reaches the Echo (5.4); without
it a package is a sideload. `manifest.sig` is the author's signature over `manifest.json`'s bytes; the manifest carries the
program's BLAKE2b-256. hassmic checks, before anything is kept: sizes, the signature against `author_key` in the
manifest, that key against the pinned `author.pub` if the plugin is installed already, the digest, the manifest's
form, and the program (2.3's contract, the part a file shows): an ELF for 32-bit ARM, EABI5, `ET_EXEC` or static `ET_DYN`,
no `PT_INTERP`, a `PT_GNU_STACK` that
is there and not executable (without it the kernel sets `READ_IMPLIES_EXEC` for the process and every readable
mapping is executable, whatever the filter refused), no segment both writable and executable.

Signatures are what `otatool` makes: monocypher's `crypto_eddsa_sign`, which is EdDSA on Curve25519 **with
BLAKE2b**, not RFC 8032's Ed25519 (*checked*, `src/tools/otatool.c`). Keys and signatures are not interchangeable
with `ssh-keygen`'s or minisign's; authors sign with `scripts/otatool.py` (stdlib only), which gains `plugin-keygen`
and `plugin-sign`.

### 5.2 Keys

| Key | Where | Signs | Opens |
|---|---|---|---|
| Release key (existing: `keys/release.pub`, private in the `release` job's environment) | in every build | hassmic's bundles; nothing of plugins (as built since 2026-11-20: the listing of official plugins was its for two days, below) | nothing of plugins |
| Index key | public `keys/plugins.pub` (in every build, built in: named in `keys/plugins.conf` with the index's address and the least serials of 5.5, as `release.conf` does for updates); private only in the index repository's protected signing environment (5.3) | `index.json`, `revoked.json`, and every listed package's listing, official and community | which plugins are official (5.4: updates without the button for what asks for nothing new, sync between Echos, `log.read.full`; never a first install without the button), lists community plugins, withdraws any plugin (as built 2026-11-21, official ones too); nothing of hassmic's own |
| Author keys | public in each manifest and index entry; private with the author | their manifests | updates of their plugins |

*As designed* official plugins did not need the index (the release key's listing), so that the first plugin could
ship before the index existed. *As built (2026-11-20, 13 "The index is the one place for plugins")* the index
makes them official too: its key signs every listing, and hassmic's releases carry no plugin. The maintainer's
word: an index repository that nothing reads has no reason to be, and plugins do not belong in hassmic's
repository. As designed, the index's revocation list did not apply to a package listed as official (a list fetched
at any time must not switch off the wake word engine of every Echo at once), and each build carries, per official
plugin id, the least version it runs (compiled in, raised at a release when an official plugin had a flaw; an older
official package is neither installed nor started, from whichever path it comes). *As built 2026-11-21:* since the
index key decides what is official, the index may withdraw official plugins too, by id or author key, and an Echo
applies its list to every tier: the immunity left a leaked author key of an official plugin with no stop but a
release of hassmic (a review of that day). The least version stays, as a floor of the build's own beside the list.
A withdrawn wake word engine is down, not missing (4.13: its words stay listed, never Amazon's), and its owner can
run it anyway with the button (5.6). What holding the index key buys is in docs/SECURITY.md "Plugins".

**A new index key** (none planned).  Every build carries one key, `keys/plugins.pub`, and takes no list and no
listing by another.  A new key therefore needs a hassmic release before the index signs with it, and the change
reaches each Echo with its update: an Echo still on the old build reads only lists the old key signed, and one on
the new build only the new key's.  What is installed is not touched: its listing was checked at install, and
`ps_scan` reads the tier from the listing as installed, by the key the build carries; under a build with another
key the listing no longer holds, so an installed plugin counts as from a file (sideload) until the index's listing
of that version is taken (a look at the index does it, below) or it is updated from the index (an update from the
new key's index is a step up, no button for that).  So a rotation is done in steps:
(1) a build that takes both keys for lists and listings (`pi_key` a list of two; nothing else changes), released and
given time to reach the Echos; (2) the index signs with the new key; (3) a build with the new key alone.  Between
(1) and (3) whoever holds either key holds the index.  A key that leaked is the case where (3) cannot wait: then (1)
is skipped, and Echos on older builds keep trusting the old key until they update; the index withdraws by author
key what the leak could have listed.
*As built (2026-11-22), step (1):* `keys/plugins.conf` names an optional second public key, `PLUGINS_PUB2` (a file of
32 bytes as `PLUGINS_PUB`; empty in this tree: one key is trusted today), with `PLUGINS_PUB2_WITH`, the hex of the
first key it belongs with, which the Makefile builds in as `-DPLUGINS_PUB2` (none, or 64 hex digits; the build stops
on one that is no key, is the first key again, is of small order, or has no `PLUGINS_PUB2_WITH`).
`plugin_index.c` `pi_keys` gives the first key and, where there is one, the second, and `pi_signed` takes a signature
by either: so the index and its revocation list (`plugin_fetch.c`'s look, the daily look, the members' list through
`pi_revoked_take`, the kept list read again at a start) and every package's listing (`plugin_store.c`
`listing_check` at install and at every scan, `pkg_version`'s) hold under either key.  A build whose first key is
another than `PLUGINS_PUB2_WITH` (a fork's own in `secrets/fork.conf`, `make PLUGINS_PUB=...`) leaves the second out,
by the keys' bytes (paths were compared, and each came out wrong: a review of a30d6ed); where `PLUGINS_PUB` is
`keys/plugins.conf`'s own (a typo in `PLUGINS_PUB2_WITH`, or `keys/plugins.pub` replaced in place) the build stops
instead (a silent drop shipped a step-(1) release without the new key: a review of 3c5d7ff, R5); `scripts/plugins-least.sh` reads the site with the same keys by the same rule.  For the
tests: `HASSMIC_PLUGINS_PUB2`, a file, beside whichever first key there is; a test's own first key
(`HASSMIC_PLUGINS_PUB`) stands in for both built-in keys, never beside the build's second.

*The steps, as built (a review of a30d6ed, K1, K2, K6).*  A is the key in use, B the new one.
(1) hassmic: B's public half as `keys/plugins2.pub`, `PLUGINS_PUB2=keys/plugins2.pub` and `PLUGINS_PUB2_WITH=<A's hex>`
in `keys/plugins.conf` (64 hex digits, any case; another key than `PLUGINS_PUB`'s stops the build, a review of 3c5d7ff,
R5); a release.  The index changes nothing: it signs with A.  Then time for that release to reach
the Echos (online updates; an Echo that does not update stays on A alone, below).
(2) The index, in one commit: `keys/index.pub` becomes B, the old one stays as `keys/index-previous.pub`, and the
environment `index`'s secret `INDEX_SIGNING_KEY` becomes B's secret half.  Its tool reads the published site, which A
signed, with either key (`index.py` `index_keys`: `published()`, `--bump`'s `built_from()`, `check --previous`,
`fetch --previous`), and builds and signs everything anew with B alone: both lists, `built-from.txt`, every package's
listing; the revocation list goes out under a serial one above the published one where `revoked.json` is unchanged
(`index.py` `load()`; later builds keep the published serial for the same list), so that an Echo of both keys takes it
as newer and keeps it as B signed it (as the same list it kept A's signature, which (3) reads as no list: a review of
3c5d7ff, R1; `pi_revoked_take` now also keeps a list of the same serial by the second key, `kept_by`, and never turns
it back to the first: a member handing over A's copy did, a review of 3fcd319, S3).  Echos of (1)'s
release take the B site; after each look at the index (the Plugins page, "Look again"), and by itself wherever a
listing installed may be one the index lists anew (`plugin_store.c` `ps_relist_reason`: 1 one by the first of two
keys or under the least, or one that failed to be taken: `plugin_fetch.c` `daily_fetch` then reads the index as well,
a review of 3c5d7ff, R2; 2 one that does not hold here: `relist_tick` reads it a minute after the start, at once when
the network is back (`net_online`), and hourly after that while the reason stands, a review of 3fcd319, S2; a version
the last look found not listed, `unlisted[]`, is no reason, and a failure for a version no longer installed is
forgotten, S4), an
Echo takes the index's listing of every version installed here that the index lists by the other key
(`plugin_fetch.c` `relist_due`, `plugin_store.c` `ps_relist`: the same manifest, so the same program; no button, no
install, settings, storage, files and the running process stay; never a lower tier; also a higher tier, and a
listing under the build's least index serial, 5.5), so its plugins hold under B from then on.  "Browse plugins" says
it on the row ("Taking the index's listing of it", and where it failed why, with "Take its listing" to ask again,
`POST /api/plugins/index` "relist").  Echos on A alone refuse the B site whole (no index, no revocation list from the
site: the week's warning), and keep their A listings, which hold for them.
The index's `keys/index-previous.pub` goes once the published site is B's (the first publish of (2) done): from then
on its tool needs it for nothing, and A's secret half is retired.  Kept longer it does no harm: A is only read.
(3) hassmic: `PLUGINS_PUB` B alone (`keys/plugins.pub` the index's new `keys/index.pub`), `PLUGINS_PUB2` and
`PLUGINS_PUB2_WITH` empty; `scripts/plugins-least.sh`, a release.  An Echo that never looked at the index between (2)
and (3) still holds A's listings, which do not hold under this build: its plugins are as from a file, and with
community plugins off (the default) not started, the wake word engine's plugin neither ("from a file, not listed by
the plugin index", `ps_engine_refused`, `main.c`'s words; the card says the listing is by a key this hassmic does not
read).  The remedy is a look at the index: the B listing of the same version is taken as in (2), and they run again;
(3) itself looks for it about a minute after its start (`relist_tick`), and hourly while that fails.  An update to
(3) whose self test finds the wake word engine's plugin stopped so does not become the factory copy while it is so
(`plugin.c` `plugin_selftest_ok`: community plugins off is the owner's choice, a listing this build does not read is
not, `listing_unread`, where the plugin ran as listed before: the `ready` mark's `listed 1`, S8, which only an
official listing writes, T5 of a review of 9cd7a77; an unread listing whose own tier line says `community` never,
whatever the mark, `listing_said`, U2 of a review of 1d704e9).  Nothing goes back
for a failed self test: no `ota_healthy`, so not the factory copy; `boot.sh` steps back only after failed starts,
`main.sh` after a satellite that keeps ending, and update.c never offers the installed version again.  (3) runs on,
its own look takes the B listing and the plugin runs again (a review of 3fcd319, S1: this said the update was not
kept, and that the build before moved the listing).  The self test, held by the wake word plugin alone, asks again
every 10 s for as long as hassmic runs (`main.c` `selftest_thread`, `plugin_selftest_ok(id, late)`): once the plugin
listens it passes, (3) becomes the factory copy then (root's `ota_watch` takes `healthy` whenever it comes, if it
names the update installed: U1 of a review of 1d704e9), and the
other plugins, which wait for the self test, start; until then they are stopped too (a review of 9cd7a77, T1: the
self test ended at 30 s, and they stayed stopped for the whole run).  A mark the self test found and that is gone
since (`plugin_selftest_forget`, for the next hassmic's handoff) is no pass then.
So (3) waits until the index lists, by B, every version still installed on the Echos: the B site lists each
version that was listed when it switched, and the index keeps listing a version that Echos still run until (3) has
been out for a while.  A version it no longer lists when an Echo takes (3) is never moved: under (3) it runs as a
file's, with community plugins off (the default) not at all, the wake word engine's neither, until the owner
installs a version the index lists (an update, the button where it asks for more) or switches community plugins
on; its card says why.  `relist_tick` reads the index once for it and then leaves it (S4).
A step back to a build of A alone (an older online build, ROLLBACK's fallback, `boot.sh`'s factory copy) after (2)
does the same to plugins already moved to B, and there no look helps (that build refuses the B site): they wait for
the update back, or for community plugins switched on.  The revocation list kept there, signed by B, is no list and
no word under that build: what it withdrew runs again, and the week's warning, which comes at once (no list read is
no word), says why (`pi_revoked_unread`, `pf_revoked_stale`).
A fork with its own releases and index sets `keys/release.conf` and `keys/plugins.conf` in `secrets/fork.conf`, as
`scripts/fork-release.sh` writes it (on the PC, and in CI from the repository variables `RELEASE_PUBLIC_KEY`,
`PLUGIN_INDEX_URL`, `PLUGINS_PUBLIC_KEY`, `PLUGINS_PUBLIC_KEY2`: DEVELOPMENT.md "Releases from a fork").

**Official plugins too live in repositories of their own** (decided 2026-11-18, 12's sixth round; the first is
`hassmic-plugin-microwakeword`). Source, build, tests and the author's key are that repository's: its CI builds the
program (twice, it has to come out the same, and a third time in its release job, which refuses one that is not the
build job's), signs the manifest, attests the package (a GitHub artifact attestation, 5.3, in its `release.yml`'s
own job, which runs for a tag's push alone) and publishes `<id>-<version>.hmp` as an asset of its own release, which
by itself is a sideload. Neither the release key nor the index key goes there. What makes the package official is
made in the index (as built 2026-11-20; before, for two days, in hassmic's release with the release key,
`keys/official.conf` and `scripts/plugin-official.sh`, all gone):

1. the index's entry names what was looked at: per version the commit, the release workflow (`workflow`),
   **the digest of its manifest** and the plugin's own release asset as its package (`<source>/releases/download/<tag>/<id>-<version>.hmp` and nothing
   else, `tools/index.py` `entry_check`). The manifest names the program's digest and the author's key, so the one
   digest pins all of it, and the entry is a commit of the maintainer's in the index's history;
2. the index's build fetches that package and checks it as an Echo will and more (the author's signature, the
   program's digest, the manifest the entry names, `manifest_check`'s rules, the file's rules, no listing in it
   yet), verifies its attestation (that repository's GitHub Actions built it at that commit, in that workflow's
   own run for the push of the release's tag, 5.3; since 2026-11-23, the same path as a community plugin's), then adds the listing signed by the index key, "official"
   for the project's own. Nothing is built and nothing of the package is run;
3. the package with its listing is on the index's site, `packages/<id>-<version>.hmp`, which is where an Echo
   fetches it from (5.4) and what the index's page offers for a download.

A new version of an official plugin is therefore a release there and a line in the index. What withdraws one is
the index's revocation list (as built 2026-11-21), and the compiled least version per official plugin is a floor of
each build's own beside it.

### 5.3 Index

A repository of its own (`hassmic-plugins`; there since 2026-11-18 with the entries, the checks, the signed list
and official plugins; since 2026-11-20 it makes every listing, official ones too, and every hassmic reads it), so
that nothing in it can touch a release of hassmic. It is a list, not a builder (the owner's decision, 2026-11-22:
"community plugins build in their own CI, just make sure it was published by the CI and not user-uploaded"; for two
days before, 2026-11-22 itself, the index built community plugins from their source, below):

- `plugins/<id>.json`, by pull request from the author: name, description, source repository and commit, author
  key, tier, and per version the plugin's own release asset as its package and the release workflow that made it.
- **Built by the plugin's own CI, attested.** Every package the index lists, official and community alike, is a
  release asset of the plugin's own GitHub repository, built by that repository's GitHub Actions from the commit the
  entry names and carrying a GitHub artifact attestation of build provenance (Sigstore, made by
  `actions/attest-build-provenance` in the release workflow: the template's and microWakeWord's
  `.github/workflows/release.yml`). Before it lists a version the index verifies that attestation: its subject is
  the package's SHA-256, its source repository the entry's `source`, its source commit the entry's `commit`, the run
  on a runner GitHub hosts (a self-hosted one is its owner's machine), started by the push of the tag the release
  asset is under (trigger `push`, ref `refs/tags/<tag>`), and both the workflow that signed it and the workflow the
  run was for are exactly the entry's `workflow` at that tag and commit. A package with no such attestation
  (uploaded by hand, built anywhere else) is refused, and so is one attested by another workflow of the same
  repository (a second one that downloads a package and attests it), by the named one called from another
  (`workflow_call`), by a run started by hand (`workflow_dispatch`: any workflow at any ref), of a branch, or of
  another tag.
- **What the workflow lets run, by commit** (since 2026-11-23, a review of the index's dc75e14): GitHub runs
  `uses: <owner>/<repo>@<commit>` for a commit of any repository of that fork network, under the original's name
  (an impostor commit): a forked `actions/checkout` that swaps the source after the checkout, pinned by its commit in
  the build and the release job, builds the same bytes twice and is attested as the tag's push. So where it asks the
  attestation of GitHub's API the index also reads the entry's workflow at the entry's commit, and every local
  workflow it calls, and holds every `uses:` in them to a commit of the action's own repository (on one of its
  branches or tags), every image to its digest; anything else is refused.
- **What that proves, and what it does not.** It proves that GitHub's CI ran the workflow file the entry names, of
  that repository at that commit, for the push of that tag, on GitHub's machine, and that the run made these bytes:
  not a file someone uploaded, not a build on the author's machine, not another workflow of the repository. It does
  not prove what the workflow does: a workflow can fetch a program from anywhere and attest it. So the maintainer
  reads, at the entry's commit in the pull request, the named workflow and everything it runs: the local reusable
  workflows it calls (the template's `build.yml`), the scripts and make targets of its steps (the template's
  `tools/release.sh`, the `Makefile`), the actions by their commits; that it builds from the repository's own source
  with pinned tools and runs for a tag's push alone. Nothing of what goes into the package may come from outside that
  commit: no action (`uses:`) not pinned by a commit of the action's own repository (one on its tags or branches:
  GitHub runs a fork's commit under the original's name too, an impostor commit, which the index refuses), all the
  way down (a composite action's own steps, a called workflow's, a docker action's image and its Dockerfile's `FROM`,
  by the same rule), no artifact of another job or run (one only to compare a build of its own with), no other job's
  outputs (`needs.*.outputs`) but a SHA-256 to compare with, nothing of the event's payload (`github.event.*`; a
  tag's annotation is not in the commit) but the tag's name, no other repository checked out at a ref that moves (a
  branch or a tag; a commit's id stays), no container image (`container:`, `docker://`) not pinned by its digest, no
  cache (`actions/cache`, the caches `setup-*` actions keep, which other runs write), no `vars` and no secret as an
  input (the signing key aside, which only the release job's environment holds: no other job names an environment,
  whose secrets it would get), no release asset already there, no toolchain resolved online by a channel (rustup's
  `stable`, `beta` or `nightly`, in a `rust-toolchain.toml` or on its command line), and no download not pinned by
  its hash; and a re-run of an old run, which GitHub attests as that tag's push again, has to make the same bytes.
  For a community plugin nobody reads the code itself. And it is GitHub's word, as the release asset is GitHub's to
  serve.
- **Checked as an Echo will and more**: the package's form (5.1, packed as `plugcheck.py pack` packs it), the
  manifest the entry names by its digest, signed by the entry's author key, `manifest_check`'s rules (the Echo's
  own, held to `plugin_store.c` by the index's `make check`), the program the manifest's and keeping the rules a file
  shows, the size (4 MB), no listing in it yet; for a community plugin also the signature, the LICENSE's SHA-256 and
  the capabilities the entry names (value for value: what the maintainer reads in the pull request), so that every
  byte listed is the entry's word whatever the release asset is replaced with later; refused what 3.3's combination
  rules refuse (`wake` with any way out; a community plugin with `request.audio` and `net.internet`, the index's own
  rule 48), `log.read.full` for a community plugin; flagged the broad ones (`net.lan` with any port,
  `net.internet`, `call`, `request.audio`, `request.text`, `log.read.full`). Nothing of a package is run in the
  build or the publish.
- **The harness, a smoke test of a pull request**: each community version the published site does not have runs
  for its first seconds under `qemu-arm -strace` against a stand-in broker, its system calls judged by the filter's
  table (5.8: not the kernel's filter), in a job of its own with no secret, no token and no write permission, inside
  bubblewrap. It runs the release's program, nothing built here, and no other job waits for it.
- **Sign**, a job in a protected environment, on the main branch only, defined by the main branch's workflow and
  never by a pull request's: fetches the packages, checks them and verifies their attestations again, runs none of
  them, writes `index.json` and signs it. The key is never in a job that ran code from a pull request.
- Published as static files (GitHub Pages): `index.json` + `index.json.sig`, `revoked.json` + `revoked.json.sig`
  (5.6), `packages/<id>-<version>.hmp`, and beside each package the bundle of its attestation that held
  (`packages/<id>-<version>.hmp.sigstore.jsonl`, one, at most 4 MB), so that anyone can verify it again.
  `index.json`: a serial that only grows (the time of the commit it is built from), then per plugin id, name,
  description, author, author key, tier, source, versions (version, api, models, capabilities, manifest digest,
  the source's commit and release workflow, package with its size and SHA-256, and where it came from: the release
  asset and its SHA-256, the attestation's bundle and its, which no Echo reads). The revocation list is `revoked.json` alone (5.6),
  with a serial of its own that is raised by hand with each change: an index that changes for another reason
  must not make every Echo fetch the list again. Signatures: 64 bytes, over "hassmic plugin index 1\n" and
  "hassmic plugin revoked 1\n" + the file.

*As built (2026-11-23, the index's `tools/attest.py`, `tools/index.py`, its README "Where a package comes from"):*
`index.py fetch` takes each version's package from its release (the published site's copy, its listing taken off,
where the site lists the version with the commit, workflow and release the entry names now, with the site's bundle;
otherwise the release's, its attestation asked of GitHub's API again); `index.py build` checks each
(`package_check`), then asks `gh attestation verify <package> --repo <owner>/<repo> --source-digest <commit>
--source-ref refs/tags/<tag> --cert-identity-regex '^https://github\.com/(?i:<owner>/<repo>)/<workflow>@refs/tags/<tag>$'
--signer-digest <commit> --deny-self-hosted-runners --predicate-type https://slsa.dev/provenance/v1
--cert-oidc-issuer https://token.actions.githubusercontent.com --format json` (`attest.argv`; `<tag>` from the
entry's `package` address; `--signer-repo` and `--signer-workflow` cannot go with the identity, and the latter is a
prefix; GitHub's API, the repository that has the name now; for a version the site lists already its kept bundle,
`--bundle`, no API, Sigstore's trust root still), reads the JSON again field by field (`attest.judge`: never gh's
exit status alone; `buildTrigger`/`githubWorkflowTrigger` `push`, `sourceRepositoryRef`/`githubWorkflowRef`
`refs/tags/<tag>`, `buildSignerURI` and `buildConfigURI` both
`https://github.com/<owner>/<repo>/<workflow>@refs/tags/<tag>` at `<commit>`: what gh has no flag for, the trigger
and `buildConfigURI`, only this reading holds), keeps the bundle of the first attestation that held within 4 MB
(none fits: refused, so a site the signing's own check would refuse is never built) and lists the version. Two
kinds of no: a verdict (`attest.Refused`: the attestation does not say what it must, or gh's own no, `VERDICTS`, gh
2.105.0's words: no attestations, a field not what a flag asks, the identity, GitHub's API's 404) leaves the version
out with a warning in a publish and is an error in a pull request's check (`--strict`); no verdict
(`attest.Unavailable`: gh missing, not gh 2.105.0 by its own `gh --version` (`attest.gh_checked`, asked once a
process, wherever the index runs), too slow, the network, Sigstore's trust root, GitHub's API's other errors, an
answer gh does not print) fails the build where the published site lists the version (an outage never takes a
version off the index; a publish while Sigstore's trust root is out of reach publishes nothing, a revocation
neither, until it is back; every such version is asked and named in the one failure, as `--strict` names every
version it leaves out), and for a new version is an error in a pull request's check and leaves it out of a
publish, said as no verdict (one release's attestations out of reach hold back neither the rest nor the revocation
list; a later publish lists it). A version the site lists, verified again with the site's own bundle for the very
bytes it held for, refused now (gh's verdict or `judge`'s) is no verdict either (`index.py` `attested`,
`KeptRefused`, since 2026-11-23): what changed is gh, Sigstore's trust root or the reading, not the release, so the
build fails; a listed version leaves the index only by a person's step (the revocation list, the entry). gh is
2.105.0 from cli/cli's release by its SHA-256 (`tools/gh.sh`, `INDEX_GH` in both jobs of `index.yml` that verify,
named by nothing else there, no `continue-on-error`: the index's `tests/test_workflow.py`), never the runner image's,
whose words a later gh may change. The real answer of gh 2.105.0 the tests hold the reading to (cli/cli's own release,
`tests/gh-verify-cli.json`) is a run by hand from a branch: refused now; the same answer made a tag's push is taken.
Each version without a verdict is printed as it is found, and once one gh call of a run took its whole time (180 s
for `attestation verify`, 60 s for an API call) every later one of its kind is no verdict at once and named
(`attest._stalled`, `not_stalled`): the publish job's ten minutes never end with no version named; a package that
is not its entry's (an error of its own) names those found before it.  The kinds are two (a review of the index's
810b9e9, where one stalled API call for a new version made every later call no verdict, the listed versions'
`--bundle` verifications too, and so failed every publish, the revocation list with it): GitHub's API (`gh
attestation verify` without `--bundle`, `gh api`) and a kept bundle's (`--bundle`, which asks no API), and the build
asks the versions with the site's bundle first, then the listed ones asked of the API (the entry names another commit,
workflow or release, or the site's copy was not usable), then the new ones (`index.py build` `order`; a review of the
index's 5a3035d: a listed version asked of the API sat among the new ones in the entries' order, and a new one's stall
before it failed the build).
What the workflow lets run (`tools/workflow.py` `check`, since 2026-11-23, a review of dc75e14; all the way down
since a review of 810b9e9, which found it stopped one level deep), right after an attestation asked of GitHub's API
holds (never beside the site's kept bundle of a version it lists with the commit and workflow the entry names: the
site lists no version whose references did not hold, and a commit's files, and the commits they pin, do not change;
so GitHub's API out of reach holds back no listed version, nor the revocation list; a bundle beside any other
version, `make site PACKAGES=<dir>`'s, is asked all the same): the entry's workflow, every workflow it calls and
every action they use, each read at the commit it is taken at through GitHub's contents API (`gh api --include`: the
HTTP status decides, not gh's words), and each `uses:` in them one of: `<owner>/<repo>[/<path>]@<40 hex>` whose
commit is the action's own repository's, found as zizmor's impostor-commit audit finds it (the head of one of its
branches or tags by the repository's own listings, else in the history of one by GitHub's compare API from that head,
"behind" or "identical", the default branch's first, at most 20 compares per commit: an honest pin is a tag's commit;
never the commit's own page, which GitHub serves for a fork's commit too; where GitHub lists more than 1000 branches
or tags, a commit on none of those read is no verdict, `Owner.cut`); `docker://<image>@sha256:<64 hex>`; a local
reusable workflow (`./.github/workflows/<file>` at a job, from the caller's repository and commit); GitHub's
self-repository reference `$/<path>` (the repository of the file it is in, at the commit that file is read at: at a
step an action, at a job a workflow, `$/.github/workflows/<file>`; a `.`, `..`, `@` or an expression in it refused;
`workflow.SELF`). What an action's commit holds is read too: its metadata as GitHub's runner looks for it
(`action.yml`, `action.yaml`, `Dockerfile`, `dockerfile`, the first there); a composite action's `runs.steps` held to
the same rules and each of their actions read in turn (an action of the author's own, by its own commit, whose step
is a fork's checkout: refused); a docker action's `runs.image` by its digest or a Dockerfile in the action (no
`.`/`..`), whose `FROM` lines and every `--from=` of a `COPY` or `ADD` name an image by its digest, an earlier stage
or `scratch`, every mount's `from=` (`RUN --mount`, its value read as CSV as BuildKit reads it, keys in any case) an
earlier stage, no build argument (`$`) in them. A Dockerfile is read only where BuildKit cannot read it otherwise,
and the rest refused, never mirrored (`workflow.dockerfile`; a review of the index's 5a3035d, which found a second
`FROM` hidden from a reading of continued lines that was not BuildKit's): a line continued (one ending in a
backslash, of any form), a line that starts with `//` or `#!` (BuildKit's syntax detection), a parser directive (`#
syntax=` would build with a builder image by its tag; `# escape=`, `# check=`; any case or spacing, anywhere), a file
that reads as JSON, a heredoc, a flag of `COPY`, `ADD` or `RUN` with a quote or a backslash in it (BuildKit takes
them out), a character outside tab and printable ASCII in an instruction (Go's flag reader takes the byte 0xa0 for a
blank), a `FROM` of other words than an image and `AS <name>`; anything else (node) runs its own files at that
commit, fixed by its id. Another repository's reusable workflow (`<owner>/<repo>/.github/workflows/<file>@<40 hex>`
at a job) is read with the strict reader and held to the same, its local calls at its own repository and commit. An
action's metadata is read where it counts (`workflow.skim`, `metadata`): its top level by lines, following every
quoted text, block text and flow collection as YAML does (actions/checkout's descriptions are folded blocks,
download-artifact's a quoted text over three lines; one document marker `---` as the first line that is not a
comment, as actions/setup-python's and pypa/gh-action-pypi-publish's open), refusing whatever could make a line
belong to another key than it seems to (a second document marker, a quoted text or plain text continued on a line not
deeper than its key, a flow collection over lines or an item in one that opens with `?`, `&`, `!` or `*`, an
indentation YAML does not take, an anchor, alias, tag, quoted or complex key, a key twice at the top, a character
YAML could break a line at), and its `runs:` with the strict reader (the index's `tests/test_skim_fuzz.py` holds it
to PyYAML where that is installed, in `make check`: random files of real actions' pieces and of those that could hide
a key, where both read a file the same keys, `runs.using` and steps' `uses`). That is stricter than GitHub and
refuses some honest actions: of 43 real ones read on 2026-11-23, cachix/install-nix-action (a step `- run :`),
houseabsolute/actions-rust-cross (an `if:` over two lines) and pypa/gh-action-pypi-publish (a character outside
printable ASCII in its `runs:`); an author whose release uses one picks another action, or writes the step in the
workflow itself. Each file is read once (cycles); more than 50 files for one version (`FILES_MAX`) is no verdict, a
person reads such a release. A job's `container:` and `services:` images by their digest. Refused: a tag, a branch, a
short commit, an expression, a commit on none of the action's own refs, a local action (`uses: ./<path>` at a step,
in a workflow or a composite action: GitHub takes it from the job's workspace, not from the commit), a `.` or `..` as
an action's repository or a part of its path (GitHub's runner joins the path without normalising it), a `uses`
anywhere else, and a workflow its reader does not read as plain YAML. The reader (`workflow.parse`, also the
template's and microWakeWord's `tools/workflow_test.py`'s, and what reads `index.yml` in the index's own test) is
strict where a YAML parser could read otherwise: a character outside tab, line feed and printable ASCII (CRLF read as
LF; Python's `splitlines` broke at U+2028, U+2029, NEL, form feed where YAML and bash see one line), a tab outside a
block text, a folded block (`>`), an indentation indicator, an anchor, alias, tag, flow mapping, complex or quoted
key, a value YAML could read as more (`: `, ` #`), a block line less deep than its first: no reading, never a part
skipped; it also counts the lines that say `uses:` against the `uses` it read. A refusal is a verdict for a version
not listed yet (left out of a publish, an error in a pull request's check), and none for one the site lists
(`KeptRefused`: an action's repository deleted or made private since answers 404, and no check that answers otherwise
one day takes a listed version off: the build fails, a person decides); GitHub's API answering anything but 200 or
404, or not at all, no verdict, as gh's. Checked against GitHub itself (2026-11-23, gh 2.105.0): actions/checkout's
v7 commit is a tag's head, an older commit of its main branch is found by compare, and
`c7d749a2d57b4b375d1ebcd17cfbfb60c676f18e`, which GitHub serves as an actions/checkout commit, is on none of its
refs: refused; the template's and microWakeWord's `release.yml` and `build.yml` with every action read at its commit
(actions/checkout, download-artifact, upload-artifact, setup-uv: node; attest-build-provenance v4.2.2, a composite
action whose step is actions/attest by `508db95`, on actions/attest's own refs, node): held; slsa-github-generator's
`generator_generic_slsa3.yml` at its v2.1.0 commit, which passed one level deep: refused (not read as plain YAML;
inside it are moving tags and a local action). The template's `make index-check RELEASED=<asset>` asks the same.
Every source is `https://github.com/<owner>/<repo>`. A community entry is an official one's (`version`, `commit`,
`workflow`, `manifest`, `package`) with `signature`, `license_sha256`, `capabilities`; the template's `make
index-check RELEASED=<asset>` prints it from the release's asset (`workflow` `.github/workflows/release.yml`) and
verifies the attestation as the index does. The template's and microWakeWord's `release.yml` run for a tag's push
alone (`on: push: tags`), call CI as a local reusable workflow (`build.yml`, `workflow_call`) and attest in their
own `release` job, so both URIs name `release.yml` (the template's `make release-test` holds that; an attestation
step inside `build.yml` would sign as `build.yml`, refused). Their `tools/workflow_test.py` allows the release job
step for step as it is written (checkout without credentials, the pinned zig, the build job's artifact into
`checked/`, `tools/release.sh`, the attestation, the release of `dist/`; no `if:` on the job; the compiler zig as
`.github/zig.txt` pins it, or in the template Rust as `.github/rust.txt` pins it file by file, `tools/rustpin.sh`,
never rustup) and runs the Package step and CI's "Which hassmic" step as GitHub runs a step without `shell:`
(`bash -e`) against a SHA-256 and an `sdk/VERSION` that are none: both fail (`tools/release.sh` refuses a SHA-256
given that is not 64 hex digits, and wants one in the release job). It also holds every action to the one commit
its `ACTIONS` names (its release tag's), `build.yml` to never saying `secrets`, naming no environment, going on past
no failure (`continue-on-error`) and setting no `defaults`, and the "Which hassmic" step to its name, id and run
alone (no `if:`). Its `tools/workflow_breaks.py` (in `make release-test`, some 20 to 40 s) breaks what it reads one
change at a time, each break naming the words of the FAIL line it has to give, and then makes each `check(...)` of
the test always true in turn, where some break has to pass (a review of 810b9e9: with any FAIL taken, 19 of 30 checks
could be switched off unseen). Each rule is a check of its own, never one operand of another check's `and`, so that
switching off each check asks each rule (a review of the index's 5a3035d: with rules joined by `and`, 47 of the
template's 68 operands and 46 of microWakeWord's 64, each made always true, passed every break): 70 checks in the
template, 66 in microWakeWord, one file of 91 breaks for both (microWakeWord skips the Rust ones), each naming the
words of exactly one check. The split found a check that could never fail: the stand-in installer of the template's
Rust download test took its prefix from the wrong argument, so "installs nothing" held whatever `tools/rustpin.sh`
did. That test is in the author's own repository, so it guards an honest author; against
a dishonest one what holds is the index's own checks and the maintainer's reading. Its reader is the index's, the
same text: held so by the index's `tests/test_references.py` and, where the index's tree is beside it, by the test
itself. The reading list's eight copies are held to the same words by `tests/test_reading.py`: in the index's `make
check` where the sibling trees are beside it, and in its CI, whose `siblings` job checks out hassmic's two docs, the
template's and microWakeWord's README, workflows and `tools/workflow_test.py` (their default branches, those files
alone, no credentials) and runs it with `--require`, where a copy not there fails (before 2026-11-23 CI saw the
index's own two copies only and skipped the rest); a job no other needs, so a sibling's branch, which moves when it
likes, never holds back a publish (a review of 810b9e9: in `check`, which `publish` needs, it did).
The page (`tools/page.py`) prints per version the workflow, the tag and the whole command with `--bundle`
(`attest.command`), which anyone runs on the release asset and the bundle beside the package: `gh attestation verify <id>-<version>.hmp --repo <owner>/<repo> --source-digest
<commit> --source-ref refs/tags/<tag> --cert-identity-regex
'^https://github\.com/(?i:<owner>/<repo>)/<workflow>@refs/tags/<tag>$' --signer-digest <commit>
--deny-self-hosted-runners --predicate-type https://slsa.dev/provenance/v1 --cert-oidc-issuer
https://token.actions.githubusercontent.com --format json --bundle <id>-<version>.hmp.sigstore.jsonl` (`<workflow>`
its dots escaped, `\.github/workflows/release\.yml`). The package is canonical
(`plugcheck.tar_bytes`): the listed one without its listing is the release's bytes, which the attestation names
(`release_sha256`). `index.py verify` holds the site's bundle to `index.json` (`attestation_sha256`) and to the
release's SHA-256. The harness (`index.py smoke`, `tools/harness.py` in `tools/sandbox.py`'s bubblewrap, under
bounds) is the pull request's job `smoke`. The tests answer for gh with a stand-in (`tests/fake_gh.py`) and hold the
reading to a real answer of gh 2.105.0 (`tests/gh-verify-cli.json`); every refusal proven by breaking it. Gone with
it: the index's own build of community plugins (2026-11-22, the index's commits 1e98a25 .. 645ca61: the commit
fetched and built twice under bubblewrap with a pinned zig 0.13.0 on two runners that had to agree byte for byte,
`tools/community.py`, the entry's `build`, `index.json`'s `build` and `harness`, the jobs `community` a and b,
`community-check`, `publish-community`), never published. Not run on GitHub; nothing published.

Tiers, to keep the maintainer's share small:

- **Official**: source in the project's organisation, a repository each, reviewed by the maintainer, expected to
  work with every release. Their listing is the index key's, as every listing is (5.2, as built): the entry names
  the plugin's own release asset as the package, the index checks it and lists it as official. The index can
  withdraw one as it can any (as built 2026-11-21; before, its tool refused a revocation that named an official
  plugin's id or author key, and an Echo took none for an official listing). Few, on purpose; microWakeWord is the
  first (#193's forwarder, once planned as the second, is left to a community author since 2026-11-21: 5.8).
- **Community**: anyone's. The first submission is merged by a maintainer or a triager after a look that the entry
  is what it says and that its release workflow builds the package from the repository's own source (no review of
  the code is promised, and the page says "not reviewed"). A later update merges itself when the checks pass, the
  pull request touches only that plugin's file, comes from the entry's owner (checked by the main branch's
  workflow, with the pull request as data), and changes nothing of what an owner approved (5.4's comparison).
  Anything else needs a person. *As built:* not the merge by itself; a person merges every entry and every update.
- Issues about a plugin go to its `source`; the page's plugin card links there, and hassmic's issue template asks
  whether it happens with plugins off.

### 5.4 Installing, accepting, syncing, updating

**How a package reaches an Echo.** Four ways, one check:

| Way | Who has internet | For |
|---|---|---|
| The browser fetches from the index (or the release) and uploads | the browser | the usual install: "Browse" on the Plugins page (*as built: not this way; the Echo fetches, the fourth row*) |
| The owner picks a file in the page ("Install from file") | nobody now: the `.hmp` was downloaded on a PC earlier, from the index's site or hassmic's release page | an Echo and a browser without internet |
| Another Echo of the network hands it over through the page | nobody | sync (below) |
| hassmic fetches it itself, as `update.c` fetches releases | the Echo | *as built:* a plugin from the index's site (2026-11-20; before, an official one from hassmic's own release): when the owner asks on the page ("Browse plugins", "Install", "Update", the Wake word page's move of 5.9, with the button as for a file), and by itself only for the matching builds of installed plugins before a hassmic update and for the wake word engine's after one (below; 2026-11-21); never an install nobody asked for |

All four end in the same place, the plugin store's one staging file (`plugin_store.c`: `state/plugins/.stage`,
`ps_stage_begin`/`_chunk`/`_check`/`_install`), and in the same checks on the Echo, which trusts nothing about how
the file came. A browser brings it in pieces over signed requests (`/api/plugins/upload/*`, 200 kB a piece, which
is also how the page carries it from another Echo), hassmic's own fetch and a push hand the store the whole
package; all of them hold the one install slot meanwhile ("One install at a time" below). (As designed this was
`artifacts.c`'s staging with a kind `plugin:<id>`; it was not built so: a package is one file with its own check,
not a folder of models, and the slot's rules are the store's and the page's.)

1. the package's form and the program (5.1);
2. the author's signature over the manifest, and that key against the pinned one if the plugin is installed;
3. where it comes from: a `listing.sig` by the index key says **official** or **community, listed** (as built: the
   listing names its tier and the index's serial; a serial under the build's least index serial, 5.5, counts as no
   listing, as built 2026-11-21); none makes it a **sideload** (a warning naming the author key's fingerprint). For
   an update, a lower tier than the version installed (official, community, sideload, in that order) is wider and
   asks for the button, whichever way the package came (as built 2026-11-21: it did not, and an unlisted package by
   an official plugin's author key went in without one);
4. not on the newest revocation list the Echo has (5.6; every tier, as built 2026-11-21);
5. the combination rules of 3.3 (`wake` sealed; `log.read.full` official only; no `request.audio` plugin beside a
   community wake engine), and for an official package the build's least version (5.2).

All of it on the Echo. The page shows and asks; it decides nothing.

In phase 1 there was no index, and a browser cannot read GitHub's release assets from the page (they are served
without CORS headers, 9.16): so the first plugin came by "Install from file", by the Echo's own fetch, or from
another Echo. *As built (2026-11-20):* the Echo reads the index itself, on the page's "Browse plugins" (13), and
the browser never fetches: the Echo checks everything anyway, and one reader is the one that decides.

The browser path and the file path differ only in who clicked: the page's "Browse" shows the index's list (name,
author, tier, each capability's line, models, whether this hassmic can run it), all of it text, fetches the
`.hmp` and uploads it; "Install from file" uploads one the owner has. The first draft had the browser upload the
index and its signature along; with the listing inside the package that is not needed, and the offline path is
the same as the online one.

**Accepting.** After the checks the page shows the capability list, one line each (3.3), from the verified
manifest, and waits for the **action button** (`web_approve()`, as adb over Wi-Fi). What was accepted is written
down: `state/plugins/<id>/accepted` holds the plugin's id, the author key, the capability section of the
manifest as accepted (canonical form) and its digest, when, and how (button here / synced, voucher named `<node>` /
migration); as built, the digest alone, and one per package the Echo keeps of the plugin (below). The broker grants at run time what `accepted` says, not what the manifest says; the two are the same
at install and may differ after an update that was not yet accepted.

**Syncing to another Echo: the acceptance travels** (decided: also for the microphone capabilities, 12). A
plugin, its settings, its files and its storage can be copied to another Echo through the page, as models
(`artifacts.c`) and settings (`web/matrix.js`) are today, also to an Echo without internet. The target does not
ask for its button again when all of this holds:

- both Echos are members of **one arbitration network** (not Kiosk Satellite mode, which has no key);
- the browser is approved on **both** (its requests are signed on each, as for "Compare & copy");
- the package has a **listing** (official, or community and listed). A sideload never syncs without the
  target's button: nobody but the owner of that one Echo ever looked at it;
- the source gives an **acceptance voucher**, made as the login voucher is (`web.c:18-20, 215-219, 957-958`): the
  target hands out a one-time nonce (`/api/plugins/accept/nonce`); the source, asked with a signed request,
  answers only for a plugin that is installed and accepted *there*, and MACs with `arb_web_key` "hassmic plugin
  acceptance 1", the target's web key, the nonce, **the browser's key, the source's node**, the plugin's id, the
  manifest's digest, the author key and the digest of the accepted section (`/api/plugins/accept/issue`); the
  target checks the MAC with its own copy of the key, that the browser asking is that browser and the source a
  member it has heard, and uses the nonce up;
- the package uploaded to the target passes every check of the list above, **on the target**, and is that very
  one: the same manifest digest and author key as in the voucher;
- what the Echo itself would want a second press for (`request.audio` with a `net.*`, 3.3) it wants here too:
  such a plugin syncs, and then waits on the target for one press;
- the plugin is not revoked on the target's list. An owner's "run although revoked" on the source is that
  Echo's alone and never travels.

Then the target installs it, writes `accepted` with "synced, voucher named `<node>`", takes the settings and
files that follow, and **says so where the owner will see it without looking for it**: a log line, a banner on its
settings page until dismissed ("microWakeWord was installed on this Echo without its action button ... the voucher
named Kitchen; the browser ... brought the package"), and in Home Assistant
an event of the event entity "Plugin" (types `installed`, `updated`, `removed`, `synced`; listed while the
feature `plugins` is on), so that an automation or the logbook has it.

What makes this safe is that nothing new is accepted: the owner accepted exactly this program with exactly these
powers, with a button, on an Echo of the same household's network. What a voucher does **not** prove is that
button press, nor which Echo wrote it: the node in it is a name any member's key signs, and the target checks
only that a member of that name exists. Every member can derive `arb_web_key`, so a member that was taken over
can write a voucher for anything, in any member's name, and can vouch its own browser onto the others first
(`web.c:215`). That is why the conditions above do
not rest on the voucher alone: what such a member can spread without a button is only a package the index key
listed (as built: as official), inside the combination rules each target enforces itself, announced on each target and
in Home Assistant in ways it cannot undo by itself ("As built" below); not a program of its own making. One press in the kitchen does put a listening plugin in
the bedroom; that is the decision, and the announcement is what stands in for the press.

What is copied, as a plan of rows in the page's matrix ("Plugins", a row per plugin and Echo, any cell the
source): the package (as an artifact); the settings (`config` without secrets), the `files` lists item by item
with their fields (by digest, so an item both have is not sent again), and `storage` if the owner ticks it (off
by default: a cursor or a registration is about one Echo). Each needs the plugin installed on the target first.

**Secrets do not sync.** A `secret` setting is never sent back to the page (4.3), so the browser cannot carry it,
and it is not in exports, as the Wi-Fi password and the Sendspin token are not. Two reasons to leave it so:
most secrets are an account of *that* Echo (two Echos registering one SIP extension is a fault, not a copy), and
the first plugin has none. The target's card shows the setting as "not set here". If a plugin comes that needs a
house-wide secret, the way exists without the browser seeing it: the source seals it to the target's public key
as arbitration's key handoff does (`arb.c:699-700` `seal`), for settings the manifest marks `"sync": true`. Not
built until asked for.

*As built (13, "Sync between Echos"), where it is more precise than the above, or other:*

- **The voucher** is BLAKE2b-128 keyed with `arb_web_key` over "hassmic plugin acceptance 1", the target's web
  key, the nonce, the browser's key, the source's node and a NUL, the plugin's id and a NUL, the manifest's digest,
  the author key and the accepted section's digest in the form this build writes (`plugin_store.c` `ps_voucher`).
  The nonce is asked for with a signed request (`POST /api/plugins/accept/nonce`: an approved browser's), eight at
  a time, good for one answer within two minutes, used up by the first install that names it whatever comes of it.
- **The source** (`/api/plugins/accept/issue`, signed) answers for a plugin that is installed, may run and is
  accepted there, to the browser that asks; not in Kiosk Satellite mode.
- **The target** (`/api/plugins/install` with `synced <node> <nonce> <voucher>`; `web.c` `sync_judge`) takes every
  field of the MAC but the node from its own check of the uploaded package, so "the very one" is not compared
  afterwards but is what the MAC is over. Then: the source a member it has heard under that node, neither end in
  Kiosk Satellite mode (this Echo's own setting, the source's by its beacons), the index key's listing as official, every
  scope its `requires` names at a version this build has (3.1), "never back" known, the version not older than the newest this Echo saw listed nor than the
  highest that was ever installed here, plugins switched on here, and the note written. A community listing does
  not sync: official or the button.
- **Which Echo** is not proved. The MAC covers the node, and every member's key makes that MAC: "a member heard
  under that node" says the name exists, not that its Echo vouched. `accepted` says "synced, voucher named
  `<node>`", the log and the page word it the same way.
- **Never an older version** than the newest the target has seen listed for the plugin: `plugin_fetch.c` keeps,
  per plugin, the highest version any look at the index found (`pf_seen`, `state/plugins/.listed`, only ever
  raised, kept over restarts: an approved browser can restart an Echo; 4096 ids). The list is the index key's
  word, and still only ever makes a sync ask for the button. Where the target has seen no list (it never looked,
  or could not reach the index) there is nothing to hold against: for a plugin not installed there, any
  official version this build runs goes, down to the build's own least version (5.2). Installed plugins are held
  by the store's "never back" as before.
- **Nor older than one that was installed here before.** "Never back" is against what is installed, and Remove
  takes no button: any approved browser, a vouched one too, could remove a plugin and have an older official
  version copied in. So the store keeps, per plugin, the highest version it ever installed
  (`state/plugins/.installed`, `ps_installed_max`; only rising, any way of installing and any plugin API, gone
  only with a factory reset), written once a package is in place (a version that never arrived is not one that
  was installed), and raised from what is installed at every scan of the store and by Remove before it removes,
  so hassmic ending between the package and the file leaves no version the floor does not know; a floor that
  could not be written is owed, and no sync goes without the button until it is. `sync_judge` reads it beside
  the listed one, and `ps_stage_install` holds a vouched install to it once more under its lock. An install with
  the target's own button is not held to it: an older version on purpose is the owner's, at the Echo. 64
  plugins; with a 65th the floor is unknown for all from then on, which the file says (a line `+`: over
  restarts, until a factory reset), as it is with the file unreadable: a sync asks for the button. The ids the
  file has keep rising all the same.
- **What is looked at again as the files are written** (`ps_stage_install` with a voucher, under the store's
  lock): official, the check itself, "never back" known, the installed floor. Held as `sync_judge` read them a
  moment before: the voucher's own rules, the newest version seen listed (it only rises, by a look that ends in
  between) and "plugins on" (switched off in between, the install ends as if a moment sooner).
- **A package is in its folder whole or not at all.** The store writes the members into `pkg.new/`, each
  through statefile.c, and puts that folder in place by name (the one there aside as `pkg.old`, the new one in,
  the names synced, the old one removed); `ps_scan` settles what an install that did not end left (`pkg_settle`:
  `pkg.new` removed, `pkg.old` back where `pkg` is missing; a lone `pkg.new` is a first install that never arrived). `accepted` and `enabled` are written after it. And
  the broker starts no program whose BLAKE2b is not its manifest's `digest` (`plugin.c` `program_is`, read
  through the descriptor the launcher execs).
- **Plugins off on the target**: the button. Nothing would run it, and Home Assistant has no entity to hear of it.
- **Falling back** is not "the press is armed": the answer is `{"install":"button","why"}`, the package stays
  checked, the page shows the ordinary dialog with the reason on top, and only "Go on" there asks for the button.
  A press must answer a list the owner saw.
- **Kiosk Satellite mode**: an Echo of ours in that mode still holds the network's key (the network runs either
  way), so "which has no key" is kept as a rule by the mode: such an Echo neither vouches nor takes.
- **An update** that is what the target accepted or less needs neither button nor voucher and is installed as any
  such update (`accepted`: "update: nothing new to accept", event `updated`). One that is wider than the target
  accepted goes on the voucher (`accepted`: "synced, voucher named `<node>`", event `synced`, a note). A plugin installed
  on the target under another author key is refused by the check, before any voucher.
- **One event an install**: `synced` in place of `installed` or `updated`, not beside it. It is not best effort:
  one that reached no link of Home Assistant's (subscribed, and keyed where the Echo has a key; the entity listed)
  is held in the core, 32 at most (as many as notes are kept, standing and dismissed-but-untold together), and
  sent when such a link subscribes. Over a restart the note carries it: each note has a mark "Home Assistant was
  told", and a note without it is given to the core again at start; a note dismissed before Home Assistant came
  is kept as "gone" for that. The mark is written with the note's own write, else within a second, so a restart
  in that second says it twice. With 32 kept (16 of them dismissed and Home Assistant away through all of it) a
  further sync is answered "button": no event is dropped to make room.
- **The note** names the node the voucher named (the page shows that Echo's name where it knows the node) and
  keeps the key of the browser that brought the package. `state/plugins/.synced`:
  - written before the install, as "pending" (statefile.c) with the manifest's digest of the package it is for;
    the install makes it "done". A failed install and a start that finds "pending" settle it by what the
    plugin's folder has (`syn_there`): that package (by digest), and the note stands; a package whose manifest
    cannot be read, and it stands too; anything else, and it goes, with a log line. A pending note is on no page,
    and no dismissal covers it. A `.synced` that is there and cannot be read (no regular file) is said in the
    log, and no sync goes without the button until a start finds it readable or gone;
  - one note an install, never replaced; sixteen at most, and with sixteen standing, read or not, a further sync
    is answered "button" (nobody pushes a note out by copying more); a file that names more is not hassmic's
    writing, and what is beyond sixteen is dropped at start with a log line;
  - named by the browser's label and by the first eight bytes of its key (`key`), which the page shows on the
    note and in its list of browsers: the label is what a login chose to call itself, and a vouched login is a
    member's to forge;
  - numbered at random, a number for good: "dismiss 1" from a tab left open names no later note;
  - shown on every page of the target and, through `/api/state` `plugins_synced`, on those pages of other members
    that poll the Echos (`renderAttention`, named by Echo): a page polls the others only while it shows them, and
    reads the state only of Echos its browser is logged in to. Not "on every page" of the network;
  - dismissed (`POST /api/plugins/synced` "dismiss `<number>`" | "dismiss all") at once only by a browser other
    than the one the note names that was let in with the target's own button. That is the stored fact
    (`state/web_button`, written when the press lets a browser in): a key not listed there counts as vouched,
    whatever the reason (let in through another Echo, by a build before the list, the list lost or not written):
    the key that forges an acceptance voucher forges login vouchers for as many browser keys as it likes, so
    "another browser" alone would be its second key, and the list the other way round (`state/web_vouched`, one
    build) made every such unknown a browser of the button's. A note whose browser's key is not known (a line of
    a build before the key was kept) is nobody's to dismiss by request. A dismissal names its notes by number,
    those that stood ("done") when it was asked: never "all up to the newest", which counted a pending one.
    Anyone else's dismiss arms the page's one wait for the action button (`W_NOTE`, under the same rule as
    adb's and an install's: "busy" while anything else waits; ended by its minute, the asker's "keep" or its
    revoke) for those notes, and what serves it is the button **held for a second** (`web_hold`; on an Echo
    without an action button both volume keys held a second and let go before the pairing hold), never a press:
    a press while it is armed goes where it would have gone without it, and the hold that served a note is not
    Home Assistant's "Button" hold as well. It was a press, and the browser that armed the wait could make the
    press happen (a Drop In rung on that Echo: the owner's answering press took the note off). The same browser
    asking about other notes than its wait was armed for is told "busy", not "waiting". That wait is bounded
    per Echo and knows no notes: three armed in any hour, withdrawn ones counted, whoever asks; then "later"
    and when (`state/web_wait`; neither a restart nor a reboot shortens it). Only a hold begun after the wait
    was armed serves it (`web_hold`'s down time), and on the Shows only a pair let go before two seconds by
    the clock; while the wait is armed such a hold is not Home Assistant's `action_hold`. The bringer's browser is not
    refused the wait outright: in the ordinary copy it is the owner's only browser. What that leaves: a vouched
    browser can spend the three and keep the owner's bringer browser from the hold for that hour (the notes
    stay; a browser of the button's that did not bring the plugin dismisses at once);
  - the event outlives the note: one dismissed before Home Assistant took `synced` stays on file as "gone" (on no
    page, not counted) until the event went out, over restarts. Home Assistant's link is whatever subscribed to
    the Echo's states, with the device key where one is set: **without a key, any client on the LAN that
    subscribes counts**, takes the event, and it is not said again.
  A note's time is 0 where the Echo's clock was not set, and the page says so.
- **The package travels through the upload** (`/api/plugins/upload/*`), not as artifact kind `plugin:<id>`, and
  member by member: the store keeps a package's five files, not its tar. The source serves each in pieces
  (`GET /api/plugins/<id>/pkg/<member>/<off>/<len>`, signed), the page packs them (`web/tar.js`) and uploads the
  result. "The very package" is checked by digest where it counts: the manifest by its digest in the voucher and
  the listing, the program by the digest the manifest names. `LICENSE` is under no signature, as before.
- **Files by digest**: `GET /api/plugins/sync` gives per item a BLAKE2b-256 over its files (suffix, size, content);
  the page sends an item's files only where the target's digest differs, then the fields that differ. The store's
  lock is held for the names only, each item's files are opened under it and read without it (an item replaced
  is a folder renamed: the open files stay whole; a size that is not what was read gives no digest, and the item
  is sent again). The answer grows with what there is (8 MB at most, else an error: never a cut JSON).
- **Settings**: `/api/plugins/<id>/set` refuses a secret row. A secret comes sealed (`secret/<name>`) or not at
  all; the page never sent one in clear.
- **`storage`** (built 2026-11-21): a row "What it keeps" per plugin that has it, compared by a digest over its keys
  and values (`ps_storage_digest`, in `GET /api/plugins/sync`); copied only where the owner picks it, as every row
  of the table (nothing picks a row by itself): the source's storage as one tar (`GET /api/plugins/<id>/storage`),
  the target's replaced whole (`/storage/begin`, `/storage/chunk/<off>`, `/storage/done`; checked first, a key that
  is none or more than that Echo accepted refused before anything stops, then the plugin stopped, the folder swapped
  by name, the plugin started again). Sealed both ways (`web.c` `seal2`): the browser's pieces under its own
  request's counter; the source's answer under that counter and a nonce of the Echo's own, new with every answer and
  sent in clear in front of it ("hassmic sealed 3 <32 hex>", purpose "hassmic seal back 3"), the GET taken once over
  restarts as a write is. The counter alone was the browser's word, and a GET counter is not kept over a restart: a
  captured request replayed after one was answered under the same key stream, and two answers XORed gave what both
  held (a review of 0dabfa6, Q2). The other sealed things are the browser's to seal under counters it never repeats
  (a Wi-Fi password, a secret, the Music Assistant token: "hassmic seal 1"; the copy's pieces): its counter's last
  three digits are random, so two tabs that ask in the same millisecond do not pick one. The first line says whether
  the plugin is a wake word plugin at the source: its manifest has `wake`, or a wipe of what it was given under the
  seal is still due (Q7). A file's "from-wake" is read where it is said (`begin`'s whole body, a field's second line),
  never in a file's bytes. *Decided:* the source's word whether it is a wake plugin's is carried by the page, not
  MAC'd by the source. The browser that copies is let in on both Echos and opens the storage itself: it could post
  any data as any plugin's on the target, a MAC or not; the rule keeps the page's own copy from carrying a wake
  plugin's data into one without `wake`, which is what the owner's page is for. **The second press**
  (`request.audio` with a `net.*`, 3.3): *as built 2026-11-22* the store's (`ps_stage_install`): a voucher that
  `sync_judge` takes stands for the first press at most; the install answers `{"install":"second"}` with the screen's
  words, the slot waits (`PL_SECOND`), the page shows the screen (`plgCarry`, `plgSecond`) and asks, and the target's
  own press installs it (`PS_VOUCHED_PRESSED`, held to the voucher's rules again), written as accepted with the
  button ("button"): no note and no `synced` event, since a person pressed there.  A press before the screen asked
  for it installs nothing.
- **Not revoked on the target's list**: what syncs is official, which no revocation list withdraws (5.6); the
  build's least version (5.2) is the check's, and so the sync's.

**Updates may change capabilities, and any change is accepted then and there.** The page shows an update when it
is open (it has the index: as built, an "Update" badge and button on the plugin's card). hassmic compares the new manifest's accepted section with the one
`accepted` names:

- **The same or less** (3.3's list: a capability or a row gone; a smaller quota, `max_items` or `max_kb`), the
  same author key, a higher `version`: installed with one click and no button.
- **Anything else**: the page shows what is not as accepted, line by line ("New: Connects to the internet",
  "More: keeps up to 64 kB, was 4 kB", "Changed: Its setting "Port" is now called ..."), and waits for the action
  button. Until it is pressed the installed version keeps running with what was accepted. There is no half-way: an
  update is not installed with the old powers. A change the Echo does not understand asks too.
- A `version` not higher than the installed one is refused without the button, whoever signed it, so an author's
  versions count upward in time. Where the installed manifest is one this build does not
  read, the version is the one its listing names; with no listing nothing says it, and the owner is asked and
  told why. A change of author key is a new install. A `wake` plugin's update that asks for a way out is refused
  outright (3.3).
- `memory_mb` and `threads` are limits, not capabilities; they may change within the launcher's caps and are
  shown: a line each in the update's dialog ("Its limits change: May use up to 96 MB of memory, was 16 MB"), for
  which the button is never asked.
- A `requires` raised (3.1; 2026-11-23, finding 4 of a review of 4e0a202): into a version that widens what its
  capability lets a plugin do (`messages.def`'s mark), wider: "More: Speaks net.lan 2, was 1, which lets a plugin
  do more: <that version's words>", the button; within, a line of its own kind (`PS_SPEAKS`, the page's "It speaks a
  later version of the plugin protocol"), no button. Every scope raised is listed. Where the installed manifest is
  one this build cannot read (a fall back) and its accepted section is the same, what it required is not known: a
  widening version above 1 is held against version 1 and asks.

A plugin is not tied to a hassmic build: what it needs is its `requires` (3.1), and every later hassmic speaks it,
so a hassmic update never needs another build of the plugin (5.9).

*As built (13, "The Echo's own fetch" and "The index is the one place for plugins"), where it is more precise than
the above:*

- **Where from.** The index's site, its base address built in (`keys/plugins.conf` `PLUGIN_INDEX`): a look is
  four GETs, `index.json`, `index.json.sig`, `revoked.json`, `revoked.json.sig`; a download one,
  `<base>packages/<id>-<version>.hmp`, the address built of the base and the checked id and version, never the
  path the list names (which is not read). Before (2026-11-19), hassmic's own releases through GitHub's API; gone.
- **What is trusted of the site**: what the index key signed, and nothing before its signature held. A list with
  a serial lower than the highest this Echo took is refused (`state/plugins/.serial` for the index, the kept
  revocation list's own serial), so whoever can serve the site's files cannot hand an Echo an older list; the
  revocation list comes with every look or the look fails (one older than the Echo's own: refused, said, and the
  look goes on with the Echo's, 5.5). A package is taken at most to the size the list names,
  and only if its size and SHA-256 are the list's (every publish lists every package anew, so where they are not,
  the Echo looks once more and holds the package to what that list says of the same version: an install right
  after a publish failed for up to the hour the list is kept); then the store checks it as an upload, and the listing inside
  has to name the manifest, id, version and tier the list named.
- **Which version.** Of each plugin the highest the list has whose `requires` this build meets (every scope it
  names at a version this build has: `plugin_fetch.c` `unmet`), not withdrawn (every tier, as built 2026-11-21),
  not under the build's least version; the list says each version's `requires`, so one download (the releases had
  several tried in turn). A newer version that needs more is named on the browse page ("needs a newer hassmic").
- **Listed or nothing.** A fetched or pushed package without the index key's listing is refused before the
  dialog; so is one that is not what the list named, and one that needs a newer hassmic.
- **The owner's fetch** ends in the same check answer, the same dialog and the same `/api/plugins/install` as an
  upload: the button for what was not accepted.
- **Without the button, by hassmic itself** (the wake engine's plugin after an update; a push): only where
  `ps_stage_check` asks for no button, that is the pinned author key and an accepted section that is what was
  accepted or less. Narrower than 5.9's former migration (which installed a plugin nobody accepted on that Echo, by a
  digest compiled in): the move is the owner's button now, and an install without the press at the cut release is
  the maintainer's to decide then (5.9).
- **hassmic asks by itself for nothing** (since 2026-11-23): a hassmic update never breaks a plugin (3.1), so there
  is no build of a plugin to fetch for one, and the wake word engine's plugin is never "installed for another API".
  Until then it fetched that plugin's build at `update.c`'s looks, and each plugin's build for an update's API before
  the bundle went to root (5.9 "Removed with it").

**One install at a time, and what a press installs** (as built, 13, the seventh read; `web.c` "the install
slot"):

- There is one staging file and one press of the action button, so one install slot, whoever brings the package
  (a browser's upload, a browser's fetch, hassmic's own fetch, a push). It has an owner (one browser, or hassmic)
  and a generation that is new with every claim and every release. Every change of it is decided under the page's
  lock and names the generation and the state it expects; it happens only if the slot still is that.
- **Who takes it from whom** is one table (`pl_rules`), read in one place (`pl_claim`): free, installed and failed
  slots at once; a browser's upload after 30 s without a piece; a browser's checked package (its dialog is open)
  after two minutes, by another browser, hassmic's own fetch and a push alike, never sooner; a wait for the button,
  a check and an install never (they end by themselves); a browser's fetch only half a minute past its own time.
  hassmic's own fetch gives way to a browser or a push at once: the owner goes first, and hassmic asks again at
  the next look for updates.
- **The press is armed for one thing**: the browser, the slot's generation, and the digest of the manifest its
  dialog listed. It installs only if the slot still is exactly that when it comes, and the store is handed that
  digest and installs it or nothing. A press that finds anything else installs nothing, goes on to the assistant,
  and that browser's page says why.
- **The wait for an install is not turned to a stranger's login** (13, the eighth and ninth reads; `web.c` "what
  waits for the action button"). One thing waits for the button at a time: this install, adb, a note, or a
  browser's login. An install that waits is ended by the press, by its minute, or by its own browser's cancel,
  and by nothing a third party sends: a request to log in from a browser that is not approved is answered "busy"
  and changes nothing, during the wait and for half a minute after the press (or after the minute the page
  promised, where no press ended it). Asked while a login of such a browser waits, the install is told "busy: a
  login waits" (the page says so, sends nobody to the Echo, and asks again by itself when the Echo's button no
  longer holds a login), that login is not ended, and once it has ended no further login is taken for half a
  minute, so the install's next ask goes first. What is not covered: a press meant for a login that just ended
  serves an install an approved browser armed in that moment (docs/SECURITY.md). The ring shows the plugin's own pattern for as long as the
  press is the plugin's, and the page reads at every look what the press would serve.
- **Whether a press is needed is decided where the files are written** (`ps_stage_install`'s `press`, the eighth
  read). The check's "no button" is a reading of the store at one moment. The install checks the package again
  under the store's lock, the one a removal takes, and writes without a press only what needs none then; with a
  voucher only what a voucher may stand for. Otherwise nothing is written, the package stays checked, and the
  next install asks for the button. hassmic's own fetch and a push install nothing in that case.
- **The staging file's writes name their claim**: a piece is admitted under the page's lock and written after it,
  so the store takes the claim's generation with every write and refuses one that is not the claim's that began
  the file. `/api/plugins/install` looks at the file in a state nobody can take the slot in (CHECKING), not in the
  CHECKED it found.
- A browser cancels its own install only. One whose upload or dialog was left so long that another took the slot
  is told so when it comes back.
- **A fetch as a whole** (the look and the download) has five minutes: 4 MB over a link down to 256 kbit/s are
  2 min 8 s, a look four requests of 15 s at most.
- **How often the index is asked for the list** is the Echo's rule, not the page's: what a page asks by itself
  when it is opened (`POST /api/plugins/index` "look": "Browse plugins", or the Plugins page with plugins on)
  reaches the index at most once an hour, whether that look found the list or failed (the failure is kept with
  its reason and its age, and shown); a press of "Look again" ("again"), "Install" or "Update" at most every 30 s.
  A cancelled fetch leaves what was known as it was.

**Packages for another plugin API** (built 2026-11-19 to 2026-11-21, removed 2026-11-23 with versioned scopes:
5.9).  One package a plugin; what was accepted is that package's (`accepted`: `digest`, `how`, `when`).

**Remove**: stopped, `state/plugins/<id>/` deleted (files and storage with it, after a question), entities gone
at HA's next listing.

**Sideload** (development, or a plugin not in the index): "Install from file" with a package that has no
listing: the warning, the capability lines, the button.

### 5.5 What hassmic remembers of the index

The index's serial (`state/plugins/index-serial`: an older index is refused), the newest revocation list it has
seen, whole and with its signature (`state/plugins/revoked.json`, `.sig`: it hands them on, 5.6), and when it last
got an answer. Not the index. A factory reset empties `state/` and with it all of this and the pinned keys; there
are no plugins then either, but old files would be taken afterwards. So each build of hassmic also carries the
least index serial and the least revocation serial it accepts (`keys/plugins.conf`, raised at a release after a
revocation that mattered), which no reset lowers.

*As built (2026-11-21):* both serials are times (the index's the time of its commit, the revocation list's the
time of its last change, written by hand), each only grows. `state/plugins/.serial` ("index <floor> <serial>":
the floor line and the newest index taken) for the index; the newest revocation list is one file,
`state/plugins/.revoked` ("floor <n>", the signature's hex, the list as the site had it: through statefile.c, so a
power cut leaves the old file or the new; it was two files, and a cut between them forgot the list), read and
checked again at the first question after every start; the newest version seen of each plugin, of those whose `requires` this build
meets (3.1; until 2026-11-23: for this build's plugin API), in `.listed`; when the Echo last had word that its revocation list is the newest in `.revoked-at`
("word <wall time or 0> <age>", below). The least serials are built: `keys/plugins.conf` `PLUGINS_LEAST_INDEX`,
`PLUGINS_LEAST_REVOKED`, set by hand before a release from the published site with `scripts/plugins-least.sh`
(which checks both signatures; not in CI, since a build is reproducible from its commit), 0 while nothing is
published. A list under them is refused; a revocation list kept from before them still withdraws what it names (it
can only withdraw too much), is not current (the page and the week's warning say it is older than this hassmic
wants), and any list from the least on replaces it (it was dropped at the start: an update that raised the least
ran what it withdrew, for good where the site was out of reach; a review, 2026-11-21); and a package whose listing
names an older index serial counts as no listing (5.4 item 3: a version the index dropped does not stay
installable as official for good through a file, a copy or a push; what is installed keeps the tier it came with).

Serials from the future (a review, 2026-11-21: one signed list with a serial far ahead froze every Echo, which
refused every honest list after it): with the clock set (`clock.c`, by Home Assistant), a serial more than a day
ahead is refused; without it, the list is taken and used. Each list is kept with a floor line, `pi_floor`: its
serial, but never past the build's own time (its commit's) + a day while the clock is not set, and never below the
line kept before it (a review of 6f39dd8: a list taken without a clock lowered a line the clock had raised to a
vouched serial, and with Home Assistant's clock more than a day behind the new list, a list older than the vouched
one was taken); kept as made, never computed again with a later clock. What holds the next list (`pi_held`, the second review of that day: a list
taken while the clock was not set left the line at the build + a day, and once the clock was set an older signed
list from between the line and the kept serial was taken over it):
- the revocation list: never one older than the kept serial, clock or no clock, unless the clock is set and says
  the kept serial is more than a day in the future; then the floor line holds (the honest list of today replaces
  the far one). The line is raised to the kept serial, on the flash, the first time the clock vouches for it (at
  the first question after a start, at any take, by the daily thread), so a clock set behind later opens nothing.
  An Echo whose clock is never set keeps a far serial until it is set: only the index key can sign one and the
  index's tool refuses to; an older list over a newer one is what anyone who can serve a file could do.
- the index: the same with the clock set; without it the floor line (an index older than the newest but younger
  than the build + a day is taken there, and offers nothing the revocation list kept withdraws).
A site's revocation list older than the one kept (a member handed a newer one over first) is refused and logged,
and the look goes on with the kept one, held to the index by the pair's rule below; the page says so beside the
list (`note`). It failed the whole look.

The word (5.6's warning): the site's answer (the same list or a newer one), now, and a newer list from a member, as
of that list's serial (with the clock set the serial itself; without it at least as old as the build is younger
than the list: all a member's list says is that it was the newest when it was made; dated "now", a member holding an archive of the
index's older lists kept the warning away with one a week: the same review); a member handing back the list the
Echo has is none (it was, and any member could keep the warning away at will, with a
fetch and a write each time). `.revoked-at` keeps the word's wall time where the clock was set (worked back from the
running time once it is set) and its age, counted on the Echo's own running time (`CLOCK_BOOTTIME`), so that a
restart keeps it whether or not the clock is ever set; the age is the larger of the two counts (the running time
misses the time the Echo was off; a clock behind the word's time says nothing: it was clipped to "fresh"). No word
known at all counts as old (it counted as fresh: a restart before the clock was set, a Wyoming Echo after any
restart, silenced the warning); so does the record's older form (a wall time alone) while the clock is not set (it
was read as an age of 0, the week's count begun anew at every such start), and where its wall time is ahead of the
clock (clipped to an age of 0 until a review of a6c5a00). Written at most once an hour: a word, and while the age grows towards the week.

And an index is taken only beside a revocation list at least as new as the one it was made with (`revoked_serial` in
`index.json`, which the index's build writes; it also leaves out of `index.json` every version that list
withdraws): the site's files of two times (a new index, an old revocation list) offered a version the newer list
withdrew (the same review).

### 5.6 Revocation

`revoked.json`, a file of its own beside the index, small, signed with the index key: a serial that only grows,
and entries by plugin id (all versions, or up to one) or by author key, each with a reason. An installed plugin
that is on the newest list hassmic has is stopped, kept, and shown with the reason; the owner can remove it or,
with the button, run it regardless.

It applies to community packages only; official ones are withdrawn by the build's least version (5.2).

*As built (2026-11-21):* to every tier, a sideload included (by its id or its author's key: a plugin from a file
that the index names can be stopped too); official ones too, since the index key decides what is official (5.2).
Refused at install (`ps_stage_check`); an installed one is stopped when a list naming it is taken
(`plugin_withdrawn_changed`) and not started after (`ps_scan` gives it its `refused`), kept, its card says "the
plugin index withdrew this version: <reason>"; one stopped so is started again when a newer list no longer names
it (it stayed stopped until the next start). "Run it anyway" is built: the card's button asks the Echo to wait for
its action button (`web.c` W_ANYWAY, one wait as every other), and the press writes `state/plugins/<id>/anyway`
with the serial of the list it was pressed for; it holds while that is the list the Echo has, a newer one asks
again. A withdrawn wake word engine is down (its words listed, never Amazon's), and the words say: choose another
engine, or run it anyway. A list is read whole or not at all: more than 2048 entries or 512 kB and it is refused,
said, and the look fails (it took the first 2048: the rest ran on). The three ways below are built as follows:
hassmic fetches `revoked.json` and its signature once a day at a random time (first 5 to 65 minutes after the start,
then every 23 to 25 hours; `plugin_fetch.c` `pf_start`'s thread) while plugins are on and at least one is installed,
whatever "Online updates" says, and nothing else then (the index as well where a listing installed may be one it
lists anew, 5.2; and where one does not hold here, `relist_tick` reads the index within a minute of the start or of
the network's return, then hourly); the members tell each other their serial (arb's `T_MSG`
kind 0x43, `M_REVOKED`, with the serial, the settings port and a ticket, to all, hourly and after a newer list:
authenticated by arb, not hidden, so any host of the LAN reads that this Echo has plugins, its serial and port) and
fetch the list from the one that has a newer one (only where its word came from a private, link-local or loopback
address, `pf_message`, and over a plain socket, which the egress lock keeps on the local network, `update_fetch_lan`:
the address is the datagram's source and the port the message's, a review of 6631974; `GET /api/plugins/revoked` on its settings port: the
signature's hex line and the list, for members only: `X-HM-Ticket` one of the two tickets that Echo told last,
`X-HM-Ctr` a nonce, `X-HM-Mac` BLAKE2b-128 with a key derived from the network's over the request, the ticket and
the nonce; a ticket for two hours, a nonce refused while it is among the last 64 taken with its ticket
(`TICKET_RING`: a ring, the oldest dropped; a cap of 64 a ticket let a member that spent them refuse every honest
request, a review of a6c5a00); anyone else, and a request sent again, answered as for a path there is none of, list
or not (a replay was answered for ever: a review of 6f39dd8; it is answered again once 64 members' requests came
after it with its ticket, with the list the site hands anyone); no clock in it, the Echos' clocks need not agree; plain HTTP, no redirect followed, 5 s
at most).  A place per member waiting to be asked, 16, keyed by the id its message names, its source address and
the port it names (a message under a known id from elsewhere is a place of its own: it rewrote the place's address,
and a member sending under an honest member's id kept the honest list out); each place asked once in 10 min
whatever serial it names, the one that began to wait first asked first, a newcomer into a full table taking a place
that waits for nothing, else one asked lately, else the one that began to wait last; and the fetches from all
members four at once, then one every 20 s (`MEMBERS_BURST`, `MEMBERS_EVERY`), their lines in the log five, then
one every 10 min, the rest counted and said (`mlog`): ids are a member's own word, signed only with the shared key,
and one member naming a new one with each message had 496 fetches and log lines in 5 s.  These bounds stop a
member's mistakes and anyone without the key, not a member that has the key and means it: it can delay another
member's newer list for as long as it keeps at it (the places kept full, a place of its own put over the honest
member's as that one arrives: a review of a6c5a00 did it for good), and silence a member's messages on arb's channel
by one message under that member's id with a counter ahead of it (arb.c `fresh`, arbitration's own older limit,
until the Echo restarts).  The maintainer's decision (2026-11-21): the handover is not made to hold against such a
member, which already disturbs arbitration, Drop In and ducking; it only helps Echos without the internet.  What
holds: an Echo that reaches the site gets a newer list with the daily look, within a day, past every member; one that
does not gets it only through members, and warns after a week without word.  Each Echo checks signature and
serials itself; the page hands nothing over (a look at the index on the Echo is the page's way). Why arb's message: one
small message on a channel that already authenticates the members, heard whatever page is open; the page's member
calls run only in a browser that shows the other Echos. With plugins off none of it asks or tells. The 7-day
warning is built: plugins on, one switched on, and no word in a week (or no list at all, no word known, or a list
older than this hassmic wants; 5.5 says what word is and how its age is kept): the Plugins page and the
Overview say so (`pf_revoked_stale`); no Home Assistant entity, since none fits (the "Wake word engine" problem
sensor is about the engine, not about plugins at large).

It reaches an Echo three ways, none of which needs the settings page open:

- **hassmic fetches it**, as `update.c` fetches releases: the firmware's libcurl (dlopen), sockets from
  `net_socket` so the egress lock lets them out, the address pinned in `keys/plugins.conf`. Five minutes after
  start and then once a day at a random minute, **while any plugin installed from the index exists**, and whatever
  "Online updates" is set to: that setting is about hassmic's own releases, and an owner who installed a plugin
  from the index has asked for this. No plugin from the index (none, or sideloaded ones only): no request. The
  install screen says "The Echo will ask the plugin index once a day whether this plugin was withdrawn"; GUIDE.md
  and SECURITY.md say that this is a request to GitHub Pages that names nothing but the Echo's address.
  `update.c`'s fetch becomes a function both use (`update_fetch(url, buf)`), `plugin_store.c` its second caller.
- **Echos pass it on.** An Echo without internet (the recommended second layer) cannot fetch. The list is signed by
  the index key, so whoever hands it over need not be trusted: any member of the arbitration network is a valid
  relay, and so is any other source. Each member tells the others its revocation serial (a `T_MSG` of its own type
  with the 4-byte serial, to all, `arb_tell(NULL)`, after a fetch that brought a newer one and once an hour); a
  member that hears a higher serial than its own gets the two files from that member's settings port (`GET
  /api/plugins/revoked` and `.sig` on 28931: unsigned and open, since the content is public and proves itself;
  as built for members only, since open it told the LAN which Echos have plugins; 512 kB at most), checks signature and serial, applies it, and announces in turn. One Echo with internet covers
  the house. The network's key authenticates who announced, not the list: a wrong announcement costs one refused
  download.
- **The page**, when open: the browser has just fetched the index and the list and hands both over (5.4). An
  extra path, and the only one for an Echo that is alone and offline.

What an attacker on the network can do is keep the list away, not change it. So the Plugins page shows how old
the Echo's list is and when it last got an answer, and warns after 7 days without one while a community plugin
runs. A list that cannot be fetched changes nothing: plugins do not stop because GitHub is down.

### 5.7 Versions

3.1's rules, plus: the SDK lives in this repository under `sdk/` and is tagged with every release; a plugin's
repository copies it in (`sdk/`'s client, `plugmsg.h` and `plugcheck.py`, with `scripts/otatool.py` beside it,
where `plugcheck.py` looks first; `PROTOCOL.md` and `messages.def`, `vectors/`, `filter.def`, the sandbox's list of
system calls that `plugcheck.py elf` holds a program to, and this repository's `LICENSE`, under which the SDK is the
author's to use and which goes into a package beside the author's own, since `hmplugin.c` is compiled into the
program: the template's and microWakeWord's `tools/sync.sh` copy exactly that) and says which it took
(`sdk/VERSION`: each scope's version and the hassmic commit); the protocol's
specification is versioned with it, per scope, and is what a manifest's `requires` refers to. Nothing a version had
is ever withdrawn: what must break is a new scope (3.1). A seam inside hassmic (the music table, `call.c`) can change freely as long as the messages keep their
meaning: that is what the broker is for.

*Decided by the maintainer 2026-11-22, built 2026-11-23: versioned scopes* (3.1).  The minors before it (0.1 to 0.7,
each a capability or two more: 0.2 storage, log.read, net.server; 0.3 state, net.udp, net.discover, audio.music,
audio.volume, ha.entities, ha.actions; 0.4 call; 0.5 request.audio, request.text; 0.6 net.lan, net.internet,
log.read.full; 0.7 button and an entity's "page") were never released, and became version 1 of every scope: no
manifest, package or state of them is migrated.  What each minor had made necessary went with them: packages side
by side per minor, the fetch of each plugin's build before a hassmic update, updates held back for the owner's word,
the week's sweep of an older minor's packages (5.9).  The microWakeWord plugin's version 8 is its version 7 with
`requires` in place of `api`; the template's examples, its Rust client (HELLO and WELCOME with their scope lists,
held to `sdk/vectors/`), the index's `manifest_check` and its listing format (`requires` per version, no `needs`)
moved with it.

### 5.8 SDK

The API is the protocol, and the SDK is whatever makes it easy to speak. In `sdk/`:

- **`PROTOCOL.md`: the broker protocol as a written specification**, complete enough to implement from: framing,
  every message with its fields, sizes and byte order, the order of the start, descriptor passing, errors, limits,
  what each capability gates. `plugmsg.h` is generated from the same table as the spec's (one source,
  `sdk/messages.def`), and so are test vectors (`sdk/vectors/`: each message as bytes) that an implementation in
  Rust or Go checks itself against. Without a spec the C header would be the definition, and every other language
  a second-class binding.
- **`hassmic_plugin.h` + `libhmplugin.a`: the reference client in C**, a plain C header (no macros that carry
  logic, no inline functions, fixed-width types, `extern "C"` guards) so that C++ includes it and Rust, Zig or Go
  bind it; built for `armv7` musl hard-float. `hm_run`, callbacks per capability, `hm_log`; in phase 1 the calls for files and wake,
  `hm_store_*`, `hm_connect`, `hm_udp_*` with the second plugin. It needs only the calls of 2.4 and no libc feature beyond what musl has statically.
  *As built:* `hm_store_get/put/del`, `hm_log_follow` with a `log` callback, `hm_connect(entry, port,
  &why)`, a `tick` callback once a second; `hm_udp_*` come with phase 4. The callbacks struct grew at its end.
  *API 0.6:* `hm_connect_lan(entry, address, port, &why)` and `hm_connect_internet(entry, host, port, &why)` (one
  `CONNECT` of their kind), `HM_CAP_LOG_READ_FULL`, `HM_CAP_NET_LAN`, `HM_CAP_NET_INTERNET`; `hm_log_follow` serves
  either log capability; SDK version 8.
- **Examples**: the first real one is the microWakeWord plugin itself, in its repository (C, with the C client). With the second
  plugin: `examples/syslog-c/` (the #193 forwarder) and `examples/syslog-rs/` (the same in Rust, speaking the
  protocol natively from the spec, target `armv7-unknown-linux-musleabihf`), which is what keeps the spec honest,
  and the test vectors. Later `examples/tone/` (a music source that plays a sine: the smallest `audio.music`).
  *As decided (2026-11-21, the maintainer):* hassmic ships and maintains no log forwarder. #193's forwarder is left
  to a community author, on API 0.2 as built here (`storage`, `log.read`, `net.server`), through the index as any
  community plugin. Where plugin authors start is a template repository, `hassmic-plugin-template` (beside this one:
  `../hassmic-plugin-template`). *As built (2026-11-21):* one example plugin, deliberately useless (it counts its
  starts and greets a server the owner names), that shows each common piece once (settings of several kinds,
  storage, the tick, the log lines, the stop, a connection to the owner's server), twice: in C on `sdk/`'s client
  (`src/main.c`, the main path) and in Rust on a protocol client written from `sdk/PROTOCOL.md` alone (`rust/`,
  optional), which checks itself against `sdk/vectors/`, the refusals included (`make rust-unit`), and so keeps the
  spec honest. Authors start with "Use this template" on GitHub, `make rename` (the id, name, author and source in
  `manifest.json`, which everything else reads), `make key` (the author key; the public half committed), `make test`
  (both builds installed with the button into this tree's PC build, `build/hassmic-host` under `build/plugrun-host`,
  against a server behind a veth pair), `make arm-smoke` (the ARM programs under qemu-arm against a stand-in broker)
  and `make index-check` (the index's own checks on the package, and the entry a pull request there adds); its CI
  builds the ARM program twice and fails when the bytes differ (reproducible builds), and a tag `v<version>` releases
  (`.github/workflows/release.yml`: the package built again and held to the build job's, signed with the author's key
  from a `release` environment, attested by `actions/attest-build-provenance` in that workflow's own job, under the
  asset name the index expects: the index takes only `release.yml`'s own run for a tag's push).
  `make sync` and `make drift` take the SDK from this tree and say where it differs, as microWakeWord's do. A plugin
  made from the template is a sideload (installed from a file, with the author key's fingerprint in the dialog) until
  the index lists it: since 2026-11-23 the index lists the package the author's own release workflow built and
  attested (5.3 "As built"; `make index-check RELEASED=<the asset>` prints the entry and verifies the attestation),
  and nothing of it is published yet.  Any language whose program keeps the rules takes that path, Rust too (its
  release workflow builds it). hassmic's own stand-in forwarder,
  `tests/plugins/testnet.c` with `tests/fake_plugins_net.py`, is its test, not a plugin. What a first forwarder's
  work found at the protocol's level stays in hassmic: storage writes answered after `STOP` until the process is
  gone (F1), the old place's `LOG` around a `LOG_FOLLOW` (F2, R8), no `LOG` while a `CONNECT` waits (F3), `LOG` flow
  control (R9), the test vectors (F4). The test vectors are built: `sdk/vectors/<MESSAGE>.vec`
  (`tools/mkplugmsg.py`: a sample, the edges `empty`/`long`/`full` and packets to refuse; `make unit-plugmsg` holds
  `plugmsg.h`'s encoder and decoder to them; the format in `sdk/PROTOCOL.md`), for a client in any language to read.
- **`plugcheck`** (Python, stdlib, on the PC; the index runs the same): `plugcheck elf <file>` checks
  the binary rules statically (ARM, EABI5, ARMv7 attributes, static, no `PT_INTERP`, `PT_GNU_STACK` without
  `PF_X`, no writable and executable segment, size) and scans for `svc` instructions with a constant number that
  is not in the filter, as a hint, since a number can also be computed, and for what is known to fail: libc++'s
  `random_device` (its symbol, or the string "/dev/urandom"), a bionic start; it words a refused `mmap` as what it is (musl reports the filter's EPERM as ENOMEM, 2.4);
  `plugcheck manifest`,
  `plugcheck sign`. Sizes as measured (0.64 to 2.3 MB stripped) fit the index's 4 MB; a C++ binary with debug
  info (6.2 MB) has to be stripped.
- **A harness that runs the plugin**: `sdk/harness.py`, a stand-in broker that speaks the protocol to the plugin
  on a socketpair, plays a script of events and checks what comes back. On the PC it runs the ARM binary under
  `qemu-arm` user mode (as `tools/qrun.sh` runs device binaries); on an Echo, `scripts/plugin-run.sh` pushes
  launcher, plugin and script over adb and runs them there under the real launcher. See below for what "under the
  filter" can mean on the PC.
- `plugin.mk` and a `Cargo` template: flags for a static build with the right stack marking. MIT, so a plugin may
  have any licence its author picks; the index wants the source public.

Toolchains: none is prescribed. The examples build with `zig cc -target arm-linux-musleabihf -static` (one
download, the same on Linux, macOS and Windows) and with Rust's musl target; a musl cross gcc does as well; Go
builds with `GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0`. Supported means: its start and its threads are in the
filter (2.4) and an example of it passes the harness on a device. Not the NDK: static bionic does not start
(2.3). hassmic itself stays on the NDK; the two never link together.

**The filter on the PC.** The request is a harness that runs a plugin under the real filter on the PC through
`qemu-arm`. That cannot be the kernel's own filter (*measured*, the system's qemu-arm: a guest's
`prctl(PR_SET_SECCOMP)` gets EINVAL and `seccomp()` ENOSYS; qemu refuses on purpose, since a guest filter would
be applied to qemu's own, host-numbered system calls), and a host filter around qemu would judge x86-64 calls made
by qemu, not the plugin's ARM ones. What the harness does instead: it runs the plugin under `qemu-arm -strace` and
judges every guest system call, number and arguments, by the same rule table the launcher's BPF is generated from
(`sdk/filter.def`), stopping the plugin at the first call the filter would refuse. That finds what a runtime
calls; it does not show how the runtime takes the refusal (qemu let the call through until the harness stopped
it), and qemu's own emulation differs from a 3.18 kernel in places. So the PC run is the quick check and the run
on an Echo is the proof; the index's pull request job does the first for a community version (`index.py smoke`), a
plugin is listed as tested on a model only after the second.

Libraries that open sockets themselves (libre does) can link with `plugin.mk`'s
`-Wl,--wrap=socket,--wrap=connect,--wrap=bind,--wrap=sendto,--wrap=fcntl,--wrap=getaddrinfo,--wrap=getifaddrs`
and the C client's `__wrap_` functions: `socket()` returns a placeholder descriptor (an `eventfd`), `connect()`
asks the broker (`CONNECT`, with the name `getaddrinfo` was asked for, which the wrap remembers instead of
resolving) and puts the socket it gets over the placeholder with `dup3`, so the number the library holds becomes
the real socket; `bind()` on UDP is `UDP_OPEN`, `sendto()` with an address `SENDTO`, `fcntl(F_SETFL, O_NONBLOCK)`
says yes and does nothing (every socket is non-blocking already). `connect()` returns when the broker answered,
never `EINPROGRESS`. This is a shim over a narrower model than the calls suggest, for C only, and will not carry
every library (9). A language's own network library (Rust's `std::net`, Go's `net`) calls `socket` itself and
gets `EPERM`: such plugins take descriptors from the broker and wrap them (`from_raw_fd`, `net.FileConn` needs
`getsockopt` and `fcntl` calls that are to be measured).

On the PC without ARM: `make host` also builds `build/plugrun-host` (the launcher without the user change and with
the filter's list translated to x86-64 numbers, or off with `HASSMIC_PLUGIN_SANDBOX=0`) and `hassmic-host` runs
plugins built for the PC from `HASSMIC_STATE`'s `plugins/`: for hassmic's own tests (6), not for authors.

### 5.9 Migration: microWakeWord leaves the core

**Decided by the maintainer (2026-11-21), and it replaces the design of "one release, no bridge" that stood here
before** (kept, with why it was dropped, in 10 and 12's history): the built-in engine is **not** cut yet. It stays,
deprecated; the owner moves to the plugin when he chooses, with one button; and the cut, with the move's removal,
comes in a later, named release, *the cut release*, of which this section says what it will do and what must be
true before it. Nothing installs the plugin by itself before it: not at start, not after an update, not anywhere.

**Deprecated, and said where it is chosen** (as built 2026-11-21):

- The settings page: the wake word engine's choice reads "microWakeWord, built in" with the tag "Deprecated" and
  says that the plugin replaces it; its help, the switch's own dialog (`app.js` `mwwSwitchDialog`, `mwwDeprecated`),
  the Wake word page's notice while it listens, and the Overview's tile ("microWakeWord (built in, deprecated)") say
  the same. Exports and the setting's value are unchanged: `microwakeword` is still what is stored.
- Home Assistant: a binary sensor of device class problem, "Built-in microWakeWord is deprecated: move to its
  plugin" (`proto_esphome.c` `KEY_MWW_DEPRECATED`, object id `wake_word_engine_deprecated`), listed only while the
  built-in engine is the one chosen, and on while it is listed; it goes with the switch to any other engine (the
  list changes, Home Assistant reads it again), and comes back with a switch back.
- CHANGELOG.md says so.

**The move, the owner's own act** (as built 2026-11-21, revised the same day after a review; the Wake word page's
card "Move to the microWakeWord plugin", `app.js` `mwwMigrate`, `mwwSwitch`, `renderMove`; `web.c` `mww_migrate`,
`mww_to_plugin`, `mww_field`, `mww_words_for_plugin`; `main.c` `core_wake_switch`). The card shows where the built-in
engine has models or is the engine (not while Home Assistant listens), and its button does, each step shown on the
card as it is done:

0. *Plugins on.* Where the feature is off, the page switches it on first and says so in the dialog before the
   button, naming the other plugins switched on there, which start with it (Home Assistant reconnects once:
   `plugins` is a feature); if the move ends without the plugin installed, it switches the feature off again, so
   the Echo is as it was, those plugins included. Where the plugin got installed and a later step failed, the
   feature stays on: the plugin the owner just accepted is there to run. Chosen over asking the owner to go to the
   Plugins page: the dialog already names the step.
1. *The official plugin installed from the plugin index*, through the path every install takes
   (`/api/plugins/fetch`, `bring_thread`, the install slot, the dialog of what it may do (`plugins.js` `plgDialog`),
   `/api/plugins/install`, the action button). The page refuses a fetched package the index does not list as
   official. Nothing without the press: a cancel or a minute without one installs nothing, and the move stops there.
   Skipped where the plugin is installed and accepted; the card then names it (version, author, tier), and where it
   is not the official one (a file, or another author's under the id `microwakeword`: an id is no proof of whose it
   is) the page asks before anything is copied, and `mww_migrate` refuses without that word ("any" in the request).
2. *The models copied*: `POST /api/mww/migrate` takes each model of the built-in engine through the store's item
   path as the page's upload of an item does (`ps_item_begin`, `ps_item_chunk` for `.tflite` as `model.tflite` and
   `.json` as `manifest.json`) into the plugin's **own** list (`state/plugins/microwakeword/files/models/<id>/`:
   `list_dir`, the same for every plugin), with the threshold and window the owner set on the built-in engine
   (its `manifest.json`, which `mww_edit` writes) staged beside them as the item's `fields` (`ps_item_field_staged`):
   checked against the plugin's own fields before anything is written, and a value outside the plugin's range (the
   built-in engine reads any threshold between 0 and 1 from a manifest; its page and the plugin 0.50 to 0.99) is
   brought to its nearest end and named in the answer (`adjusted`). Then the plugin's own `FILE_CHECK`
   (`plugin_item_checked_only`): kept only where the plugin looked at the model and took it; a refusal, or no check
   at all (the plugin stopped meanwhile), keeps nothing, and the model is named with its reason (`refused`). So
   what the answer calls copied is what the plugin has, and the copies are the built-in engine's files byte for
   byte. A model the plugin has under that id already (a move made before, "Copy the models again") gets the
   built-in engine's files and keeps the threshold and window it has (the owner may have tuned them on the
   plugin's card since: two stores), named in the answer (`kept`); before a review of 61028a8 the built-in
   engine's values went over them unsaid. A threshold is compared at the two decimals it is written with: the
   built-in engine keeps it as a float, and its page's own top, 0.99, read as 0.99000001 was called outside the
   plugin's range.
3. *The engine switched, where the built-in microWakeWord was the engine*: the words in its slots
   (`state/mww_word`: the first, and Home Assistant's "Wake word 2" if one is set) that the plugin took become the
   plugin's pick (`state/wake_word-microwakeword`), once the plugin has listed them (up to 8 s), and then the setting
   `wake_engine` is `plugin:microwakeword` (`core_wake_switch`: the pick first, then `settings_set`, so the switch's
   scan reads it). Where another engine listens (Amazon's, another wake plugin, the plugin already), the move copies
   and changes nothing else: the answer says `switched` false and `offer`s the words a switch would take, and the
   card offers the switch as a step of its own, worded as what it is ("Listen with the plugin for …": the Echo
   answers to those words and no longer to the current engine's), `POST /api/mww/migrate "switch"`, which copies
   nothing, and which is refused while the plugin is the engine already (it would put the built-in engine's words
   over the plugin's own pick). Before, the move switched away from whatever listened, Amazon's engine included (a
   review, 2026-11-21). The move's dialog for a plugin that is not the official one names its author key's
   fingerprint beside the author's name (`/api/plugins` `key`): the name is the manifest's own word.
4. *Nothing of the built-in engine's is moved, changed or deleted*: `state/mww` and `state/mww_word` stay as they
   were, and nothing of the plugin's (`checked`, `title`, `fields`) is written there; switching back
   (`wake_engine` = `microwakeword`, one click on the same card: "Switch back to the built-in engine") listens for the
   same words again, tuned as before. From the move on there are two stores: what the owner deletes, uploads anew or
   tunes on the plugin's card is the plugin's copy, and what he adds, replaces or copies from another Echo on the
   built-in engine's page is the built-in engine's ("Copy the models again" on the card brings new ones over).

*Why two stores* (2026-11-21, a review): until then the plugin's list `models` **was** the built-in engine's folder
(`list_dir`'s alias, for any plugin with the id `microwakeword` and `wake`). One folder with two owners: a model
deleted on the plugin's card was gone for the built-in engine; one uploaded there again with its `.tflite` alone had
no `manifest.json`, which the built-in engine needs, and switching back was refused; a copy from another Echo or a
new version on the built-in engine's page replaced the folder and dropped the plugin's `checked`, `title` and
`fields`; both staged into `state/mww/.new-<id>`, each removing the other's upload; and a stranger's sideload with that
id was handed the folder. Separate lists end all of it; the price is the models' bytes twice (60 to 200 kB each).
An Echo whose plugin read `state/mww` under a build before keeps working: at the first start with this build
(`plugin_store.c` `ps_mww_own`, from `plugin_start` after the first scan) the models it had checked there (they
carry its `checked`, which the built-in engine never writes) are copied into its own list through the store's item
path: `model.tflite`, `manifest.json` and the owner's threshold and window (`fields`, each through the store's field
check), never `checked` or `title`, so the plugin checks and names each copy as it runs (`look_unchecked`, its
`FILE_CHECK`). Only for the official plugin with `wake` (the alias was a wake word plugin's, and an id is no proof
of whose a plugin is), never over a model it has in its own list, and once: `state/plugins/.mww-own` is written
when a pass for that plugin ends with nothing to try again (a copy that failed for want of room or a write keeps its
marks for the next start). From that pass on the marks go from `state/mww` at every start, so it is the built-in
engine's alone, and an artifact copy `mww:<id>` lists, reads and keeps the model's two files only (`artifacts.c`
`mww_only`, `mww_file`). A start without that plugin there and read (none installed, a sideload or another
author's under the id, one without `wake`, its manifest unread or needing a newer hassmic at that start)
does nothing at all: no mark, no log, the marks and the tuned `fields` left in `state/mww` for a start that has it;
until a review of 913ec8d such a start marked the pass done and stripped the marks, so a start that missed the
plugin's manifest by chance lost the fields tuned on its card for good, and an Echo without `state/plugins` logged
"cannot write .../.mww-own" at every start. The models of a sideload of the alias era are not given to it at any
start: the move with the owner's "any" copies them, with the built-in engine's threshold and window, not the ones
tuned on the sideload's card, which stay in `state/mww/<id>/fields` (and go to the official plugin if it is
installed later, at its first start). Until a review of
61028a8 the pass ran at every start and took any folder with a `checked` file, whoever wrote it: an artifact copy
from an Echo of the alias era (or any approved browser's upload) replaced the plugin's own copy with one marked
checked that it never looked at, a title nobody checked included, which Home Assistant got as a wake word's name;
and a plugin without `wake` was given what one with it had checked. A model it never checked is not given to it. A
fallback to a build with the alias finds `state/mww` as it was and has the plugin check the models again; what it
checks there then is never copied again.

The engine is the last thing that changes: the move refuses while the plugin is not installed and accepted, is not
the official one unless the owner said so, or does not run, and when the plugin refuses the word the built-in
engine listens for now; anything that fails before step 3 leaves the engine as it was. `tests/fake_mww_migrate.py`
(`mww-migrate`) runs all of it with the real plugin's PC build.

**A hassmic update and the plugins** (as built 2026-11-23, with versioned scopes, 3.1).  A hassmic update never
breaks a plugin: every scope it has, it has at the version it had or a later one, and a later one only adds.  So
nothing of the plugins' comes before an update, and none holds one back: an online update (`update.c` `install`)
and a push (`ota.c` `handle`) go to root as they come, the plugin index is asked for nothing for them, and the wake
word plugin's mark of having listened stays for the next hassmic's self test.  One package a plugin
(`state/plugins/<id>/pkg/`), as it was installed.

The other way, a fall back to an older hassmic (an update that did not stay: boot.sh, `ROLLBACK`; the factory copy)
can meet a plugin that requires a scope version the older build has not.  That build refuses it at every scan with
its reason on the card ("it needs a newer hassmic: wake 2 ..."), keeps it with its settings and files (never back
holds against its version, which is read before what it requires), and its self test does not wait for it
(`plugin_selftest_ok`: `m.newer`); a wake word plugin so is "missing" (main.c), so Amazon's wake word listens until
the Echo runs a hassmic that has what the plugin needs, and then it runs again as before.

*Removed with it* (built 2026-11-21 for API 0.x, where a minor broke; nothing of it was released):
- packages kept side by side per API minor (`pkg/<api>/`, `PS_BESIDE`, the `beside` and `api` lines of `accepted`,
  the card's packages "kept for another hassmic" and their Delete, `ps_package_drop`, `/api/plugins/<id>/drop`,
  `ps_stage_room`, `ps_other_api`);
- the fetch of each plugin's build for the hassmic an update brings (`web.c` `web_plugins_prepare`, `keep_beside`,
  `plugin_fetch.c` `pf_for_api`, the push's 30 s for it, `PREP_S`), the bundle's `plugin-api` file and
  `ota_bundle_api`, `ota_release_ok`;
- updates held back for a plugin that would stop, Home Assistant's "Not installed yet", the page's "Install it all
  the same" (`update_anyway`, `POST /api/update/anyway`, `/api/state` `update`);
- the self test's mark forgotten for a hassmic a plugin would stop with (`plugin_selftest_forget`, `_keep`,
  `ota_forget` and its bookkeeping around the handoff: `pending`, `give_back`, the handoff's `forgot`);
- the wake engine's plugin fetched by itself after an update (`web_plugin_auto`, `update_on_look`);
- packages for an older API deleted a week after a build of the new one passed its self test (`ps_api_age`,
  `ps_obsolete_sweep`, `state/plugins/.api-since`, `main.c` `obsolete_thread`, `HASSMIC_PLUGIN_OBSOLETE`).
Kept: the self test with its marks, `late`, the handoff's nonce and late answers (`ota_handed`), the push of a
plugin's package (`HMOTA-PLUG1`, `scripts/ota-push.sh --plugin`), which an Echo without internet gets its plugins'
updates by.  `tests/fake_plugins_selftest.py` (was `fake_plugins_update.py`) keeps what of its checks still tests
something: an update and a push with nothing held, the self test at restarts with the mark (plugins off, the plugin
switched off, one for a newer hassmic: not waited for; the launcher silent, the program changed: held), the
community and listing cases, the late pass, the handoff's.

*Known limit* (said, not fixed; a review of 913ec8d):
  - A fallback to a build of the alias era finds `state/mww` without the `fields` its plugin had there once the
    copy at start took them: its plugin listens with the built-in engine's threshold and window.

**The cut release** (planned, not built; the maintainer names it when it is made). What it will do:

1. *The built-in engine goes*: `wake_mww.c`, `mww_features.c`, `mww_model.c`, `mww_store.c`'s writing half, the
   `/api/mww/*` endpoints (the move's included), the page's model library and move card, `artifacts.c`'s kind
   `mww:<id>`, the deprecated sensor, `unit-mww` and `tests/fake_web_mww.py` (the plugin's own repository keeps
   `mww_ref.py`'s bit-exactness). 1552 lines and a parser of uploaded files leave hassmic.
2. *The setting stays*: a stored `microwakeword` is read as the wake plugin `microwakeword`; nothing is rewritten,
   so the build before still understands its own setting if the Echo falls back to it.
3. *The models*: an Echo that moved has the plugin's own copies; one that has not gets them as the move makes
   them, by the cut release's install of the plugin (the same copy through the item path, `state/mww` read and
   left as it is). `state/mww` itself stays where it is, for good, since no build can know that no older build will
   come back, and an older build finds its store there as it left it. (Until 2026-11-21 the plan was an alias of the
   plugin's list to `state/mww`, for good; it is gone: two owners of one folder, the move above says why.)
4. *An Echo that has not moved* has the plugin missing: **Amazon's wake word** listens (4.13's rule: an Echo that
   answers to "Alexa" for a while is found out by speaking to it, one that answers to nothing looks broken), every
   page warns, Home Assistant's problem sensor "Wake word engine" is on, the log says it at every start, and the
   page's one click installs the plugin, with the action button as any install. Whether the cut release may install
   it without the press for exactly this id, key, accepted section and least version (the design that stood here
   until 2026-11-21) is the maintainer's to decide when it is made; until then the rule is the press.
5. *The warning before it*: the first line of the release text (CI's Notes step, from a file `RELEASE_WARNING`
   while it exists, for six months after the cut release: an Echo updating later from an older build sees the notes
   of whatever release is newest then), short enough for Home Assistant's 255 characters, and CHANGELOG.md's entry
   in full with the steps for an Echo without internet.
6. *The self test* does not ask for the plugin at the cut release where it was never ready on that Echo
   (`plugin_selftest_ok` as built), so an Echo without internet still takes it and everything after.

What must be true before it: the plugin listed official in the index with a `requires` the cut release meets; the CPU and
latency measurements of 4.13 on a Dot 2 (they decide whether the cut happens at all, 8); the move released and
in use on Echos for at least three months, so that every owner had a release with the deprecation sensor and the
button; the plugin's sync between Echos (built) seen
working on an Echo; and `RELEASE_WARNING` written. Before the cut release: no automatic install of the plugin,
anywhere.

## 6. Tests

- **Parser**: `tests/unit/plugmsg_test.c` under ASan/UBSan, as `pb_test.c`: every type round trip, every length
  one short and one long, random packets; `make unit-plugmsg`. A libFuzzer target over the broker's dispatch with a
  stand-in core, run in CI for a fixed time, its corpus in git.
- **Manifest and package**: `tests/unit/plugin_store_test.c`: bad signatures, a changed program, another author key
  on an update, a version not higher, an update with one more `net` entry or a wider one, an older index serial
  and one under the build's least, over-long strings, an ELF with `PT_INTERP`, without `PT_GNU_STACK`, with an
  executable one.
- **Broker**: `tests/fake_plugins.py` in `tests/run.py` (its own network namespace as all): `hassmic-host` with
  test plugins that (a) do what they may, (b) ask for what they may not, each refused and counted, (c) stop reading:
  hassmic's requests still answer in time, (d) crash in a loop: switched off after five, (e) flood: dropped, then
  killed, (f) `CONNECT` to an address that never answers, eight at once: the broker thread's answers to another
  plugin stay under 50 ms. Address policy against stand-in servers: loopback, the Echo's own address, a non-local
  address through a `lan` entry, a name through a `lan` entry, a name other than the setting's through a
  `setting:` entry (no DNS query leaves: a stand-in resolver counts). A proposal changes nothing until the page
  accepts it. Install, button, update, revocation, sideload, removal; HA's entity list before and after
  (`esphome_common.py`). hassmic killed three times with a plugin running: none starts the fourth time.
- **`wake`, with a stand-in plugin** (`tests/fake_plugin_wake.py`, in place of `fake_web_mww.py`): a test plugin
  that "detects" by a marker in `testdata/` audio: the detection's positions reach the fake Home Assistant's run
  as the in-core engine's did (the replay starts where it started); two slots; a detection with `end` in the
  future, 2 s old, `begin` after `end`, for a closed slot, ten in a second: each dropped and counted, the
  twenty-first in ten minutes switches to the vendor engine; a plugin that stops reading the stream: the capture
  loop's worst gap unchanged, and the plugin sees the gap in the positions; `WAKE_FAILED` and no answer: the
  vendor engine, said in the log; the plugin killed mid-run: started again, the word listens again; `-w remote`:
  no stream at all. A manifest with `wake` and `net.lan`: refused at install and at start; an update adding
  `ha.entities` to a `wake` plugin: refused.
- **The real plugin** (in its own repository, with the official plugin, not in `tests/`): `mww_ref.py`'s
  bit-exactness against pymicro-features and TFLite on all of pymicro-wakeword's models, moved with the code;
  real detections on `testdata/alexa_espeak.raw` through `hassmic-host` and the launcher; the model fuzzers.
- **Files**: uploads grouped by stem, a suffix not in the list, one over its size, the seventeenth item; fields
  out of range; `FILE_BAD`'s reason shown as text; an upload while the plugin is down waits; the descriptor a
  plugin gets is read-only (a write fails); the quota; the catalog fetched by the stand-in browser, never by
  the Echo.
- **Acceptance and sync** (`tests/fake_plugin_sync.py`, three Echos, two in one network): install needs the
  button; a sync to the member needs none and writes "synced from"; the same to the Echo outside the network, in
  Kiosk Satellite mode, with a browser not approved on the source, with a used nonce, with a package one byte
  different, with a voucher for another plugin: each asks for the button or refuses; settings and files follow,
  an item both have is not sent again, a secret arrives as "not set here", storage only when ticked.
- **Updates**: the same capabilities: one click; each kind of wider (a new capability, a larger quota, another
  port, another entity, a `calls` mark): the button, and the old version keeps running until then; narrower: one
  click; a lower version; another author key.
- **Install from file**: an official package, a listed community one, one without listing (the warning), a listing
  by the wrong key, a listing with a serial under the build's least; all with the stand-in browser offline.
- **Migration** (`tests/fake_plugin_migrate.py`, beside `fake_ha_update.py`'s stand-in GitHub): R started on a
  state with `wake_engine=microwakeword`, two models and a pick: the plugin fetched and installed without a
  button, "migration" in `accepted`, the engine switched after `WAKE_READY` without a restart, `state/mww`
  byte for byte as before and the plugin's own list with copies of its models, `state/config` still saying `microwakeword`; the self test passes before
  the fetch ends; the stand-in serving a package with one more capability, another setting, another id, another
  key, a version under the least: not installed, the fallback, the notice, the problem sensor on; no internet:
  the same, then "Install from file" ends it, and so does a sync from another Echo; the plugin installed and
  its model made not to load: no wake word (a marker "Alexa" starts no run), the button works, the page's
  warning, and Amazon's word again once the plugin is removed; `PLUGINS=0`: nothing fetched; the build before R started on R's state: its models
  load and its setting reads; a later hassmic update with the plugin unchanged: it runs on, the same package; a pushed
  update with the plugin pushed after it. The release text: CI's Notes step with `RELEASE_WARNING` present puts
  it first, and the first 255 characters hold all of it (a unit test of the step's script).
- **Starts and crashes**: a wake plugin is up before the engine opens (no "Alexa" window at start: a marker
  word in the first second is heard by the plugin's word); three ordinary restarts (`svc_restart`'s SIGKILL)
  within a day leave every plugin running; a hassmic made to crash in the broker's handler holds that one
  plugin back; the wake engine crashing six times in ten minutes is still being retried, no wake word listens
  meanwhile (Amazon's does not: the plugin is installed), the page warns, the problem sensor is on.
- **Per capability**, as each lands: `tests/fake_plugin_music.py` (the tone plugin beside Sendspin: newest wins,
  stop word, button, duck, `MUSIC_POS` against the fake output's clock, `HASSMIC_FAKE_MUSIC_PPM`),
  `fake_plugin_call.py` (beside `fake_ha_dropin.py`: claim, never answered by itself, a token only from a `calls`
  action, the fourth ring refused, mute; as built `tests/fake_plugins_call.py`, with a second Echo for Drop In and
  the Show's build for the screen).
- **Requests stay Home Assistant's**: with every capability but `request.*` granted to a test plugin, a run from
  wake word to reply: the plugin's sockets carry no audio block and no text of it.
- **The copy**: `tests/fake_plugin_request.py`: a test plugin with `request.audio` writes what it gets to its
  stderr as a digest; the fake Home Assistant's received audio has the same digest, lead-in included
  (`esphome_common.LEAD_IN`), for the wake word, the button and a follow-up, with noise reduction on and off;
  nothing arrives before `REQ_START` or after `REQ_END`; a plugin that does not read: Home Assistant's audio is
  complete and on time, `REQ_END` counts the drops; `-w remote`: nothing before the fake HA's "wake word heard";
  two Echos under one fake HA (as `fake_ha_arbitration.py`): only the winner's plugin gets blocks; no link: no
  copy; `request.text` alone: texts and no audio socket, "stop" not passed on. The page: the second confirmation
  with a `net` entry to `any`, and again when a public host is written into a named setting.
  *As built (2026-11-22):* `tests/fake_plugins_request.py` (the tree's naming) with `tests/plugins/testreq.c` (a CRC-32
  of what it got, said in its log): byte for byte against the fake Home Assistant's audio, lead-in included, for the
  wake word (its slot and phrase), the button, a follow-up, noise reduction on; the ends (Home Assistant, an error,
  cancelled, muted, the link lost, the timeout); a plugin slow to read (dropped, counted, the gap, Home Assistant
  complete and on time); the texts, a "stop" the Echo acted on not passed; `request.text` alone; the plugin sending
  the request's messages; `-w remote` (nothing before Home Assistant's wake word, then a stretch of its audio);
  Wyoming (the copy, the transcript); the second press (the first installs nothing, nor a press without the screen's
  ask, then the ask and the press; an update adding `net.udp`); the partner at the engine's choice, at the listener's
  switch, at its update, after a restart.  The sync's second press in `tests/fake_plugins_sync.py`.  The second
  press's page in a real browser (puppeteer-core, Brave; not in `tests/`).  Added by a review of b294fa0: two Echos
  of one arbitration network under one fake Home Assistant, the plugin on both (only the round's winner streams, and
  only its plugin gets a copy); no link: no copy (the wake word and the button); a plugin with every other capability
  (`state`, `net.udp`, `net.discover`, `net.server`, `audio.music`, `audio.volume`, `ha.entities`, `ha.actions`,
  `call`, `storage`, `files`, `log.read`): no `REQ_*` at all; muted, no run for a follow-up asked for before, an
  announcement's `start_conversation` or a barge-in queued before, and `-w remote`'s stream stops and starts again;
  a request's start (the lead-in, a long replay: the wake word sent too, a slow aligner) whole for a plugin that
  begins reading 3 s after `REQ_START`; Wyoming with `-w
  remote` (the copy from the detection, no text for a run no detection opened); the sync's voucher going stale
  between the presses (`fake_plugins_sync.py`: both presses then Tgt's own).
- **Revocation**: `tests/fake_plugin_revoke.py`: a stand-in index (as `fake_ha_update.py`'s GitHub): fetched with
  online updates off, not fetched with only a sideloaded plugin, a bad signature, an older serial, one under the
  build's least; two Echos of one network, only one with a way out: the other has the list within the hour and
  stops the plugin; a member announcing a serial it cannot deliver.
- **The capture thread**: a plugin that never reads its microphone socket during a call; the capture
  loop's worst gap stays what it is without it.
- **The sandbox, on every model**: `src/tools/plugprobe.c` and one test plugin per supported runtime (musl C, musl
  C++, Rust, Go: start, threads, memory, the harness's script), run by
  `scripts/plugin-probe.sh` over adb, its table per model into PLAN.md. Done on biscuit by hand (2.3, 2.4); the
  probe adds what that run did not have: every † argument rule, both ways; `personality` refused; an ELF without
  `PT_GNU_STACK` gets executable memory when started outside the package check (the reason for the check); numbers
  above 386 answer `ENOSYS`; the limits; the CPU group; the `hassmic_plugin` domain, the exec in it; the firewall's
  two rules. On the PC the SDK's harness judges
  qemu's trace by the same table (5.8), and CI checks the x86-64 translation of the list; neither stands in for
  the device. The launcher's `READY` repeats the core of it at every start.
- **Launcher**: `tests/unit/plugrun_test.c`: malformed `SPAWN`s, too many, a slot twice, the socket closed mid
  message, the socket held open by another process while hassmic is gone; as root in a container in CI for the
  user change.
- **The settings page**: the manifest's strings with markup in them stay text; a plugin card cannot be mistaken
  for hassmic's; the domain's state line and the warning before community plugins go on, per `INSTALL` (a browser
  test, as the page's others).
- **The SDK**: the Rust example against the test vectors and the harness; `plugcheck elf` against a dynamic
  binary, one with an executable stack, a bionic one.

## 7. What changes where

| File | Change | Phase |
|---|---|---|
| `src/hassmic/wake.h`, `wake.c` | `wake_plugin` as an engine, picked by `plugin:<id>/<word>` | 1 |
| `src/hassmic/main.c` | the word list from `WAKE_WORDS`; slot states at the four places that open an engine (`:3741-3748, 3446, 3457, 3473`); `wake_is_mww`'s other uses (`:1780, 1805, 1811, 2058, 3449`); wanted against running, the fallback's rule (4.13); detections checked; the wake plugin started before the engine opens, the self test's rule; fd 3 | 1 |
| `src/tools/plugrun.c` (new), `Makefile` (`all`, `host`), `scripts/bundle.sh` | the launcher | 1 |
| `scripts/system/main.sh` | `plugrun -L --` in front of `runas`; `PLUGINS=0` | 1 |
| `scripts/system/sepolicy-hassmic.rules` | `hassmic_plugin` | 3 |
| `scripts/device/lockdown.sh` | the drop rule for users 6900–6907 (4.6); the port rule with phase 4 | 2, 4 |
| `src/hassmic/plugin.c`, `plugin.h`, `plugmsg.h`, `plugin_store.c` (new) | the broker: core, files, wake; packages, `accepted`, vouchers | 1 |
| `src/hassmic/clock.c`, `artifacts.c` | fd 3 closed in the two children forked without exec; kind `plugin:<id>` (package, files) | 1 |
| `src/hassmic/settings.c` | feature `plugins`; `wake_engine`'s choices at run time (`:37`), `microwakeword` kept as the stored value | 1 |
| `src/hassmic/web.c`, `web/app.js`, `web/plugins.js` (new), `web/matrix.js` | `/api/plugins*`, the Plugins page, the `files` type, install from file, the sync rows | 1 |
| `src/hassmic/update.c`, `ota.c`, `scripts/ota-push.sh` | the official plugin fetched from the release's assets (5.9); pushed after a bundle | 1 |
| `hassmic-plugin-microwakeword` (a repository of its own: `wake_mww.c`'s logic, `mww_features.c`, `mww_model.c`, its tests), `keys/official.conf`, `.github/workflows/build.yml` | the plugin, built with the SDK and signed by its author's key there; here the line that names it and the `release` job's listing, published as a release asset (5.2) | 1 |
| `sdk/` (new: spec, C client, `plugcheck`, harness; zig in CI) | | 1 |
| `src/hassmic/wake_mww.c`, `mww_features.c`, `mww_model.c`, `mww_store.c`, `mww.h`; `web.c` `/api/mww`; `app.js:1666-1946`; `artifacts.c` `mww:`; `Makefile` `unit-mww`; `tests/fake_web_mww.py`, `tests/unit/mww_*` | removed from hassmic | 1 (release R) |
| `src/hassmic/core.h`, `main.c`, `sendspin.c`, `a2dp.c`; `call.c`, `call.h`, `dropin.c` | `struct music_source`; `call_claim` | 4; 5 |
| `.github/workflows/build.yml` (Notes step), `RELEASE_WARNING`, `CHANGELOG.md` | the warning first in the release text | 1 (release R) |
| `src/hassmic/proto_esphome.c` | the problem sensor "Wake word engine"; the event entity "Plugin" | 1 |
| `src/hassmic/logread.c` (new, out of `web.c`), `plugin_net.c` (new) | log reading and its filters; sockets to `net.server` | 2 |
| `keys/plugins.pub`, `keys/plugins.conf`, `scripts/otatool.py` | index key, author signing | 3 |
| `src/hassmic/update.c`, `plugin_store.c`, `arb.c` (a `T_MSG` type), `web.c` (`/api/plugins/revoked`) | revocations fetched and passed on | 3 |
| `src/hassmic/proto_esphome.c` | plugin entities in the listing, `listed()`, states, actions | 4 |
| `src/hassmic/call.c`, `main.c`, `screen.c` | `CODEC_PCM`, `core_call` | 5 |
| `src/hassmic/main.c` (`mic_out` in `lead_in` and `send_out`, run start and end), `proto_esphome.c`, `proto_wyoming.c` (texts), `plugin.c` | `request.audio`, `request.text` | 6 |
| `tests/` (6), `docs/GUIDE.md`, `docs/SECURITY.md`, `DEVELOPMENT.md`, `CLAUDE.md` | | with each |

## 8. Phases

0. **Find out** (no user sees it): the probe on every model. Biscuit and donut are done by hand for four
   runtimes, the filter, the user and the exec. Left: cupcake, cannoli and the other five, the
   `hassmic_plugin` domain, the CPU group.
1. **microWakeWord as a plugin.** In this order, each a release or less:
   - *The seam*: `wake_plugin` in `wake.c`, the slot states, wanted against running, the fallback's rule, the
     engine choice read at run time (4.13's inventory). In hassmic, with the core engine still there, tested
     with a stand-in plugin.
   - *The minimum of the plugin system*: the launcher and the sandbox (in `hassmic`'s domain or `su`; the
     plugin domain comes with phase 3); the broker's core, `files` and `wake` (19 message types, no network
     code); packages with the release key's listing and the build's least version; `accepted`; the Plugins page
     with the `files` type, "Install from file" and the Echo's own fetch of an official plugin; sync between
     Echos with the acceptance voucher (the page copies microWakeWord's models between Echos today, so this
     cannot wait); the SDK as far as this plugin needs it (the spec, the C client's core, files and wake calls,
     `plugcheck`, the harness). API 0.1, two plugins at once, no promise.
   - *The plugin*: in its own repository, listed by hassmic's release (5.2; since 2026-11-20 by the plugin
     index), and the CPU and latency measurements of 4.13 on a Dot 2 **before** the cut: they decide whether it
     happens.
   - *The deprecation and the move* (the maintainer's decision of 2026-11-21, 5.9; built that day, on the PC): the
     built-in engine kept and marked deprecated (the page wherever it is chosen, Home Assistant's problem sensor,
     CHANGELOG.md), and the owner's own move to the plugin with one button on the Wake word page: the plugin
     installed through the index with the action button, every model copied through the store's item path with
     its threshold and window, the same wake words, the engine switched last; switching back is the setting
     alone. With it: the wipe when an update drops `wake` (3.3). (The plugins' builds fetched before a hassmic
     update that changed the plugin API, packages for an older API deleted a week after the self test: built
     2026-11-21, removed 2026-11-23 with versioned scopes, 5.9.) Nothing installs the plugin by
     itself.
   - *The cut release* (named when it is made; 5.9 says what it will do and what must be true before it): the
     built-in engine and the move removed, the setting and the models kept for good, an Echo that has not moved on
     Amazon's wake word with the warning until the plugin is installed (with the press, unless the maintainer
     decides otherwise for that release), the warning first in the release text. Nothing automatic before it.
   Not in phase 1, though earlier revisions had them here: the seams for music and calls (they come with their
   capabilities, phases 4 and 5), `storage` and `state`, a second language's example and the test vectors (with
   the second plugin), the `hassmic_plugin` domain (phase 3), more than two plugins at once. The filter's rows
   for C++, Rust and Go stay: they are measured, they cost the first plugin nothing, and that authors are free
   in language is the maintainer's word.
   It is not a small first step: a root launcher, a broker, a page section, a seam through the wake word's
   start-up, a migration, and the capability that hears the room. What it buys: the whole chain proven on a
   plugin the maintainer keeps himself, on the capability with the hardest timing, and 1552 lines with a parser
   of uploads out of hassmic.
2. **Storage, the log and the first sockets** (first planned as the second plugin, #193's syslog forwarder, official;
   decided 2026-11-21: hassmic ships no forwarder, #193 is left to a community author). `storage`, `log.read`,
   `net.server`: the first sockets (`plugin_net.c`), `logread.c`, the firewall's rule for plugin users, the SDK's
   storage and socket calls, the development flag. The template repository `hassmic-plugin-template` holds the C and
   Rust examples (5.8), so that the written protocol is tried by a second implementation before it is promised.
   **API 1.0 when a second plugin, by anyone, has shipped and been used**: from then the 12 months.
   *Built on hassmic's side 2026-11-21 (13, "Phase 2 on hassmic's side"), on the PC:* `storage`, `log.read`,
   `net.server` with `CONNECT`, `logread.c`, `plugin_net.c`, the firewall's rule for plugin users, the SDK's storage,
   log and socket calls, the development flag, storage in the copy between Echos, API 0.2 (and with it the
   microWakeWord plugin's version 2), the test vectors (`sdk/vectors/`); the template repository
   (`hassmic-plugin-template`: the C and Rust examples, 5.8), built the same day. The forwarder itself is a community
   author's (decided 2026-11-21: hassmic ships none). Left of phase 2: the runs on an Echo (13), then API 1.0 when a
   second plugin, by anyone, has shipped and been used.
3. **Index**: its repository (there, for official plugins: 5.3) and its two jobs, `keys/plugins.pub`, browse, community listings, the
   `hassmic_plugin` domain for new installs; revocations fetched
   by hassmic and passed between Echos; the community switch and its warning. Community plugins from here.
   *Done 2026-11-20 (13, "The index is the one place for plugins"):* the repository lists every plugin and makes
   every listing, `keys/plugins.pub` and `keys/plugins.conf`, the Echo's reading of the index (signatures,
   serials, the list, per plugin the version for this build) and of the revocation list (refused at install, an
   installed one stopped), browse on the settings page. Left of phase 3: the index's build job for community
   plugins (its tool refuses community entries), the `hassmic_plugin` domain, the daily fetch of the revocation
   list and its passing on between Echos (done 2026-11-21: 13, "Withdrawals reach every Echo"), the community
   switch.
   *Done 2026-11-22 (2.7 "As built", 13 "The community switch and the plugins' domain"):* the `hassmic_plugin` domain
   in the rules, checked against every model's policy and not tried on an Echo; the community switch and its warning.
   Left of phase 3: the index's build job for community plugins (the index's repository), and the domain on an Echo.
   *Done 2026-11-22 in the index (5.3 "As built", 13 "The index builds community plugins"):* the build job for
   community plugins: from the author's pinned commit, twice, in bubblewrap with a pinned zig, on two runners that
   must agree, the LICENSE pinned, the harness as a smoke test, broad capabilities flagged, publishing that never
   waits on it; the Echo's "Browse plugins" shows the flags.  Not run on GitHub and nothing published.
   *Replaced 2026-11-23 (5.3 "As built", 13 "Community plugins built in their own CI"):* the index builds nothing;
   every package, community and official, is its repository's release asset, built and attested by that
   repository's GitHub Actions, and the index verifies the attestation before it lists it; the harness a pull
   request's smoke test.  Left of phase 3: the domain on an Echo; and of the index (not hassmic's): an update that
   merges itself.
4. **Music**: `struct music_source` in-tree first (its own commit, Sendspin's and Bluetooth's tests unchanged),
   then `audio.music`, `audio.volume`, `net.udp`, `net.discover`, proposals, `state`, `ha.entities`,
   `ha.actions`. The tone example proves the path, then #198's author is offered the port.
   *Built on hassmic's side 2026-11-21 (13, "Phase 4 on hassmic's side"), on the PC:* all of it, in that order
   (`struct music_source` in a commit of its own first), as API 0.3, with the SDK's calls, the test vectors, the
   tone example in the template repository (C and Rust) and the index's rules.  Left of phase 4: the runs on an
   Echo (13 names them), then #198's author offered the port (hassmic ships no Lyrion plugin).
5. **Calls**: `call_claim` in-tree first, then `core_call`, `CODEC_PCM`, tokens, `call`. #186.
   *Built on hassmic's side 2026-11-21 (13, "Phase 5 on hassmic's side"), on the PC:* all of it, in that order (the
   claim and `core_call` in a commit of its own first, Drop In's tests unchanged), as API 0.4, with the SDK's calls,
   the test vectors, a stand-in phone in `tests/plugins/` and the index's rules.  Left of phase 5: the runs on an
   Echo (13 names them), then #186's author offered the port (hassmic ships no SIP plugin).
6. **Request audio**: `mic_out`, `REQ_*`, `request.audio` and `request.text`, the confirmations, the index's
   rule. Last because no plugin is waiting for it; `request.text` alone could come with phase 4 if one is.
   *Built on hassmic's side 2026-11-22 (13, "Phase 6 on hassmic's side"), on the PC:* all of it, as API 0.5, with the
   SDK's callbacks, the test vectors, a stand-in listener in `tests/plugins/` and the index's rule (as far as it can be
   checked while no hassmic has `net.internet`: the index refuses the listing).  Left of phase 6: the runs on an Echo
   (13 names them); a listener plugin by anyone (hassmic ships none, and the template has no example of one on
   purpose).
7. **The last capabilities and more places** (no phase of its own above; what the phases left: 3.3's table had
   `net.lan`, `net.internet` and `log.read.full` without a phase, and phase 1 left "more than two plugins at once").
   *Built on hassmic's side 2026-11-22 (13, "Places and the last capabilities"), on the PC:* the three capabilities
   as API 0.6, the SDK's two calls, eight plugins at once within 128 MB of their memory limits, a plugin that waits
   for a place; the index's rules (22f, `CAPS_BY_API` 0.6), the template's and microWakeWord's syncs.  Left: the runs
   on an Echo (with several plugins at once: what they really keep resident, which is what the 128 MB should become).

If plugins were given up after phase 1, hassmic would have one wake word engine less in its own process and a
plugin system with one user; after phase 2, with two; Lyrion and
SIP would stay pull requests or forks.

## 9. Open questions and risks, ranked

1. **Models not yet measured.** donut is answered: its arm64 4.4.22 kernel has the seccomp filter and it holds a
   32-bit task (*measured*). cupcake has the same kernel version and likely the same configuration: likely,
   unmeasured. cannoli (5.4) could not be tested: its adbd refuses the test PC's key ("failed to authenticate"; the
   flashed bundle was built with `ro.adb.secure=1` and the build host's key baked into `/adb_keys`, found 2026-11-18;
   the builder now makes adb keyless like the other models, so a reflash and a retest are due), so the `ENOSYS`-above-386 rule and musl's and Go's time64 fallbacks are unmeasured exactly
   where they matter. The configurations in the tree show the filter on athena, biscuit, cannoli, checkers,
   cronos, crown, radar; of those only biscuit has run a plugin.
2. **Plugins in `su`** (decided, 12; 2.7). Echos whose policy predates hassmic's own domain exist (the Dot 3 on
   the bench). Plugins run there in `su` behind the strongest warning, naming all three levels and the
   reinstall: filter, user, no files and no sockets are the same in every domain, and what `su` permits needs
   calls the filter refuses. What stays a risk: it is the configuration with no second wall at all, on kernels
   that get no fixes (9.3), and nothing now makes those Echos get reinstalled, which would fix hassmic's own
   exposure there as well.
3. **Kernel exploits** (1): accepted, but it decides how community plugins are worded on the page and whether the
   index should take anonymous authors at all. More so on an Echo without the plugin domain (2.7).
4. **The broker as attack surface of hassmic**: ~50 message types handled in the process that holds every key.
   Fuzzing finds parser flaws, not logic ones (a state the call code did not expect). Mitigation: the handlers
   call the same narrow functions the in-tree users call, with the same locks.
5. **What each language's runtime needs from the filter.** Answered for a first program each: C, C++, Rust and Go
   run under the full table, argument rules included, on biscuit and donut (2.3, 2.4). The price was Go's:
   `MAP_FIXED`, plain `fcntl`, `sched_getaffinity`, a second `clone` shape, for every plugin, since the kernel
   cannot tell which runtime asks. Still open: whether larger Go programs (the garbage collector under an
   `RLIMIT_AS` of 32 MB, the network poller on descriptors handed over, `net.FileConn`) need more; and that
   "supported" tracks versions this project does not control: a Go or musl release that changes its `clone` flags
   or its start breaks plugins built with it until the filter follows. If a runtime comes to need a call
   the filter does not have, **the filter is widened** (decided, 12: authors stay free in language). What that
   costs is said here once: every call added is added for every plugin, in whatever language, on kernels that
   get no fixes, so each addition comes with its argument rule where one can be written, a row in 2.4 and a
   check in the device probe.
6. **API stability is the maintenance cost.** From 1.0 every capability is a promise for 12 months and more, now
   as a written spec that other languages implement, which makes a change dearer than a header's. `call` cuts
   through code that changes (`call.c`, `main.c`). If that is too much, stop after phase 4 (music).
7. **The filter on the PC** (5.8): settled as a limit. qemu's user mode refuses a guest's seccomp (*measured*),
   so the PC harness judges a trace by the table, and an author without an Echo can make a plugin likely to pass,
   not prove it. The index may need a device of its own for that (a Dot 2 on a maintainer's desk running the
   probe for new versions), which is maintenance this design meant to avoid.
8. **What `log.read` may show** (4.5). *Decided 2026-11-21: marked where written (`LOG_PRIVATE`, 4.5 "As built");
   root's lines by a list of its event words.* The review asks for an allow-list of prefixes. I think a list beside the
   log's writers goes stale with the next `fprintf` and fails open or shut without anyone noticing; marking speech
   where it is logged (a `log_private()` at the three places that print transcripts and replies, `proto_esphome.c`
   and `proto_wyoming.c`, which the reader process tags) holds better. Either way hassmic's log gets a notion it
   does not have today, and until it does, only an official plugin should have even `log.read`.
9. **Running the system install again on an installed Echo** (2.7): the warning's remedy. Untried with plugins
   installed; on `boot` models it means fastboot again, on cannoli a firmware build. If that is too much to ask,
   the warning stays for good on those Echos, which is what the maintainer's decision allows.
10. **The other models and rules**: the uid range 6900–6907 on every image, the exec of `/proc/self/fd/N`
   under the plugin domain, the launcher's context change on Android 9 (athena), `ENOSYS` above 386 on 5.4
   (cannoli). Each small, each to be seen on a device.
11. **Revocation's relay** (5.6): the announcement rides arbitration's `T_MSG`, which older builds must ignore
    (to be checked in `arb.c`: an unknown type), and the open `GET` on the settings port is the page's first
    unsigned path that serves a file. Both small; both new surface for something that should rarely happen.
    A fetch a day per Echo with an index plugin is also this project's first traffic that an owner did not switch
    on by a setting named for it.
12. **The socket shim** (5.8): `--wrap` over "the broker connects, then hands over" serves a C library that calls
    `socket`, `connect`, `sendto` in the usual order. It does not serve one that uses `sendmsg` with an address,
    non-blocking `connect` with a state machine that insists on `EINPROGRESS`, or its own system calls. Whether
    libre is among them is not checked; #186's own `sip.c` is easier to port than libre.
13. **Load on a Dot 2** (4 cores at 1.3 GHz, load ~6 with hassmic alone): a FLAC decoder in a group with a quarter
    share may stutter exactly when the Echo is busy. The share is a guess; measure with the tone and a real decoder.
14. **Ring limits** (3.6) are picked numbers.
15. **The float ABI in the contract** (2.3): "hard-float" is an ELF flag for programs with a C ABI; Go's binary
    ran, but what its header says was not looked at. The checker asks for EABI5 and ARMv7 and enforces the
    hard-float flag only on what links C, until that is read off the measured binaries.
16. **Where GitHub serves what**: release assets are not readable from a page of another origin (no CORS headers,
    to be confirmed), which is why 5.3 says Pages. A package limit of 4 MB keeps that comfortable.
17. **`request.audio`** (3.6, 4.12), in the maintainer's hands since it is his decision:
    - *Nothing shows it* (decided, 12: nothing new, and no rule tied to the light). Only a `wake` plugin has
      the microphone all the time; every other plugin gets audio for a reason, a request or a live call, in
      moments in which the Echo listens anyway. So the lights work as they do today, a dimmed ring or a band
      of 0 rows included, and no plugin is refused for it.
    - *`-w remote`*: the copy is cut to the part after Home Assistant's wake word, so there it is not "what Home
      Assistant gets". The other reading (everything) is an open microphone and I have not offered it.
    - *The pair with the internet*: a second confirmation and no listing for community plugins. A plugin can
      still pass audio on through a `lan` or `setting:` host that forwards it; the rule stops the direct road only.
    - *Text without the feature* (decided, 12): `request.text` reaches a plugin although "Last heard"/"Last
      reply" may be off for Home Assistant. That setting is about Home Assistant's entities; the plugin's
      capability was accepted on its own.
    - *Follow-ups and `-D` (duplex over Wyoming)*, where the microphone stays open through a reply: the copy is
      what the protocol gets, so it follows, own reply's trace included. Not looked at in detail.
18. **Who answers issues.** A plugin that misbehaves is reported to hassmic first. The card's frame and link, the
    log prefix `plugin <id>:` and the issue template are all there is against it.

Added with the decisions about `wake`, acceptance and migration. 19, 20, 22 and 23 were put to the maintainer
and are decided (12); what is left of each is said here.

19. **`wake` is sealed** (decided). What the seal does not close is listed in 3.3: the timing of detections and
    of CPU use. Two things follow that someone will ask for and the seal refuses: a wake word plugin that
    downloads its own models, and one that tells Home Assistant how sure it was.
20. **Acceptance travels, also for `wake`, `call` and `request.audio`** (decided, on 5.4's conditions). What
    stays true: a voucher proves membership of the network, not a button press, so a member that was taken over
    can spread any *listed* plugin to the others without a press. The announcement on the target (the log, the
    page's banner, Home Assistant's "Plugin" event) is the control that is left, and it is one that works only
    if someone looks.
21. **The `files` type is a guess at generality** from one user (4.3). Items by stem, typed fields, a catalog,
    a check by the plugin: it fits microWakeWord because it was cut to it. The next plugin with files may want
    one file per item, a folder, or an order. It is API 0.x; expect it to change.
22. **The cut at once** (decided, 5.9). Its risk is the Echo without internet that updates anyway: it answers
    to Amazon's wake word until a person installs the plugin, and what warns that person beforehand is 255
    characters in Home Assistant's update entity. The fallback is decided since (12, 4.13): Amazon's word only
    while the plugin is missing, with a warning on the page. So an Echo in a house that chose "Okay Nabu"
    *because* nobody should say "Alexa" answers to it from the cut until the plugin is there, and never because
    an installed plugin crashed; the price is an Echo that hears no wake word while its installed plugin is
    down. The models stay in `state/mww` for any build that comes back; the plugin has its own copies (5.9).
23. **Official plugins under the release key** (decided, with 5.2's fixes). "Official" can then never be
    someone else's plugin without that key; if official plugins by other maintainers are wanted later, they
    need a key of their own.
24. **A wake word plugin's CPU and latency** (4.13) are unmeasured on any Echo, in the core or out of it. The
    limit for a detection is now 300 ms, because arbitration and the ring leave no more. If a busy Dot 2 cannot
    keep it, the first plugin fails on the model with the most users of it, and phase 1 stops before the cut.
25. **Detections as input.** A wake plugin can start runs when it likes, within the rate limit, and picks the
    positions that the gain's start and arbitration's own score are computed from (4.13). That is what a broken
    model does too. The limit (20 in 10 minutes) is a guess; a living room with a television may reach it
    honestly.
26. **One button for three things** (4.3): a press approves a login, adb or a plugin, and only a ring pattern
    and the page tell them apart. A person who presses for whatever blinks accepts whatever waits.
27. **A wake plugin can now fail an update** (2.6): that is the point of the rule, and it means a broken plugin
    build, published with a good hassmic, makes every Echo that uses it fall back. The two are built and
    released together, which is what makes that unlikely and what makes it possible.

## 10. Where this differs from the brief it was written from

- The measured prototype filter (about 25 calls) is not the filter a real plugin needs (about 55, 2.4). The
  fuller one has since run on biscuit with a static musl plugin and threads.
- **Static bionic is out**: the brief offered "NDK -static or zig/musl". Bionic's static start opens the property
  area and a socket to logd before `main` and segfaults when refused (*measured*). Any other static toolchain may
  do (musl's, Rust's, Go's), but not the one hassmic is built with.
- The filter has to let one `execve` through; the brief had the filter before the exec without saying how. It is
  pinned to the launcher's pointers, and that pin is a hurdle only (2.3).
- `getrandom` is not missing on 3.18: it is in mainline since 3.17 and biscuit's 3.18.19 has it (*measured*).
- `clone`'s flags differ by runtime (`0x7d0f00` for musl, `0x50f00` for Go, `0x3d0f00` for bionic), Go needs
  `MAP_FIXED`, and musl's malloc passes `mmap2` an
  address hint: a filter written from bionic's behaviour does not run musl.
- Per-plugin iptables rules by owner cannot do the job: a handed-over socket is owned by hassmic, who made it, and
  `xt_owner` proper is on cannoli only. The firewall needs two static rules and no per-plugin state (4.6). The
  one call that would give a plugin's user a socket of its own is `accept`, so it is not allowed.
- hassmic's "any address" is per socket (`net_socket`, group 3990), not per process, which is what makes `lan`
  and `any` cheap.
- Plugins cannot be hassmic's children (`clock.c`: the attestation module's `wait()`).
- The signatures are monocypher's EdDSA with BLAKE2b, not standard Ed25519.
- `CONFIG_SECCOMP_FILTER`: also on cronos and crown; donut has it (*measured* since); cupcake, with the same kernel
  version, is likely to and is unmeasured.
- Shared-memory rings are not needed at these rates (3.5); the 7 ms worst case is met by never waiting, not by
  avoiding sockets.
- #102 is not a plugin at all (4.9): Home Assistant is required, and another agent belongs in it.
- The Echo need not fetch the index; the browser can, and the Echo only verifies (5.4).
- Settings a plugin can write, button events, a "safe mode" hold and larger storage were in the sketch; none is
  in the first versions (11).  Button events came with API 0.7, as a capability of their own that only hears
  (`button`, 3.3); the rest is still in none. The `pipeline` capability is in none (12).
- The pilot is not the syslog forwarder but microWakeWord taken out of the core (4.13, 12), and capabilities are
  cut one power each (3.3), where the sketch grouped them.

## 11. Review and revisions

2026-11-18, after the first draft: an independent review (findings C1 to M9 and ten smaller ones) and a run on
biscuit (B1 to B4). Each review claim about the code was looked up before it was taken.

| Finding | What changed |
|---|---|
| C1 `CONFIG_SET` and `RESOLVE` undo the network policy | Taken. Settings are owner-written only; `CONFIG_SET` is gone, a found server is a proposal the owner confirms (4.3); no lookup outside `CONNECT`, and only for a name the entry allows; `lan` takes addresses only (4.6, 3.7) |
| H1 shared sockets let a plugin block the broker | Taken. TCP: connected on a worker, then handed over, no copy kept. UDP: a copy kept, safe because `F_SETFL`, `FIONBIO`, `SO_LINGER` and the timeouts left the filter (2.4, 3.2, 4.6). The SDK's `--wrap` story changed with it (5.8) |
| H2 `accept` gives the plugin's user a socket | Taken. `accept`/`accept4` out of the filter; TCP listeners out of the API until a plugin needs one, and then the broker accepts (2.4, 3.7, 4.6) |
| H3 follow-ups keep the microphone | Was taken as limits on a plugin's follow-ups; gone with the `pipeline` capability (12): no plugin gets a request |
| H4 the index's CI holds the key and builds strangers' code | Taken: a build job without secrets, a signing job in a protected environment that runs nothing it was handed, checks from the main branch's workflow (5.3) |
| H5 one-click updates too loose | Taken: compared field by field, version must rise (5.4) |
| M1 the ELF's stack flag defeats the `PROT_EXEC` rule | Taken: `PT_GNU_STACK` required and not executable (5.1); `personality` named as never allowed (2.4, B4); both in the probe |
| M2 `mremap` unconditional | Taken for `mremap` (flags 0 or `MAYMOVE`). The one-exec argument it protected is dropped instead of defended: with B2's address hints it could not be kept, and a second exec gains nothing (2.3) |
| M3 musl threads would not start | Confirmed on the device (B2): `0x7d0f00` (2.4) |
| M4 call consent can be engineered | Taken: tokens only in actions marked `calls`, three unanswered rings in ten minutes, the plugin's name beside the caller (3.6) |
| M5 storage wears the flash | Taken: a bucket of 10, one a minute (4.4). `SAVE_MS` 60 s *checked*, `arb.c` |
| M6 `log.read` reuses the owner's redaction | Taken in substance: two capabilities, a second filter (4.5). `log_redact` blanking only `SP:0…` and `transcript:` in the log *checked*. Not taken as written: a prefix allow-list; 9.8 says why and what instead |
| M7 phase 1 is not the smallest step | (As it stood then; the pilot and the phases have since changed, 12 and 8.) Mostly taken: seams first (phase 1), API 0.x without a promise until music shipped, the pilot cut to 14 message types; out of the first versions: `STATUS`, `CONFIG_SET`, buttons, larger storage, the hold gesture, owner-key sideload, eight plugins at once (3.3, 3.4, 8). **Not taken: "cut musl"**: musl is the libc measured to start in the sandbox (B3), so it is bionic that is cut. Kept: sideload from a file, which is how the pilot is installed before there is an index |
| M8 RTP rides the control socket | Taken: `UDP_PEER` fixes the peer and the plugin sends directly (4.6) |
| M9 the pipeline seam understates two sinks | Was taken as an open design; gone with the `pipeline` capability (12) |
| `getsockopt` unfiltered | Taken (2.4) |
| `STATE` to every plugin | Taken: capability `state` (3.3) |
| hassmic's crash counter clears too soon | Taken: 24 h, and the slot being handled noted at the crash (2.6) |
| Watchdog kills the innocent | Taken: counted in the broker thread's own running time (2.6) |
| fd 3 in children forked without exec | Taken, *checked* (`clock.c` `clock_log_start`, `artifacts.c`); the launcher also watches hassmic's pid, since the log reader outlives hassmic by design (2.1) |
| Factory reset forgets serial, revocations, pins | Taken in part: a least serial in each build (5.5). Pins and the list are gone with the plugins they were for |
| Community plugins only in the plugin domain | Was taken; reversed by the maintainer (12, 2.7): they run without it, behind a warning |
| Settings phishing | Taken: plugin cards framed and named (4.3) |
| 4.11 omitted calling from the page | Taken: actions marked `page` (4.3, 4.10, 4.11) |
| Firewall rules IPv4 only | Taken: `AF_INET` only (3.7) |
| B1 static musl runs under the filter on biscuit | 2.3, 2.4, 2.5 marked measured; question 1 of the first draft answered for biscuit |
| B2 `clone` `0x7d0f00`, `mmap2` hints | 2.4's table; the exec pin's claim reduced (2.3) |
| B3 static bionic unusable | bionic out; first written as "musl only, zig", since widened to any static toolchain that passes (12, 2.3, 5.8) |
| B4 `personality` | 2.4 |

What the review said to keep is kept: the launcher by descriptors, no file system, no `socket()`, never
unsandboxed, the firewall analysis, start after the self test, hassmic's own ring and sounds for calls, the
browser fetching and the Echo verifying, seams before capabilities, and the row that says a kernel exploit is root.

### Second review

2026-11-18, of the revision with `wake` and microWakeWord, read against the code. Every code reference in it was
looked up again (all hold but `web.c:582`, where I find no use of the engine; the uses at `:806` and `:853` are
there). One constraint from the maintainer stands above it: authors stay free in language.

| Finding | What changed |
|---|---|
| C1 a wake plugin is never up when the engine opens; the self test never sees it | Taken (2.6): the wake engine's plugin starts in `main()` before the engine opens; the self test asks for `WAKE_READY` when the plugin was ready under the version before, and not when it never was, so a missing plugin cannot hold updates back for ever |
| C2 `falls` counts ordinary restarts | Taken (2.6): only the fatal-signal handler's mark counts; init's SIGKILL, updates and power cuts leave none. The boot id was not needed for that |
| C3 the voucher proves membership, not a press | Taken (5.4): button-less sync only for listed packages; rules and second presses enforced on the target; the voucher binds browser key and source node; the target announces it in the log, on the page and as a Home Assistant event; the revoked-override never travels. What remains is said there and in 9.20 |
| H1 plugin state not tied to the OTA slot | Taken (5.9): packages side by side per API minor (removed 2026-11-23: with versioned scopes every later hassmic speaks what a plugin needs, 3.1); the stored setting stays `microwakeword`; the model store is never moved (stronger than "delete after promotion": an alias for good, and *checked* that the older build ignores the added file; the alias itself was dropped 2026-11-21, 5.9: two owners of one folder; the move copies, and `state/mww` stays as it was for an older build) |
| H2 the fallback is not "as today" | Taken (4.13): wanted against running, the setting `wake_fallback` (Amazon's word or none), a problem sensor, endless retry |
| H3 the seal leaks more | Taken (3.3): phrases fixed at upload, reason codes instead of text, wake plugins' lines kept from `log.*` plugins, storage wiped when `wake` is dropped, no `request.audio` plugin beside a community wake engine, the stream stopped on soft mute. CPU timing and detection timing stay, named |
| H4 the seam is understated | Taken (4.13, 7): slot states for the three opens on the capture thread, the five other uses of `wake_is_mww`, the run-time engine choice in `settings.c`, limits of 300 ms and 0.3 to 1.5 s with the reasons, `keyword_db` and the arbitration score named as plugin-influenced |
| M1 what is accepted is too narrow | Taken (3.3, 4.2, 4.3): entities and actions inside the capability section; the accepted section also holds settings' names, types and labels and the plugin's name; a second press for `request.audio` with any `net.*`; proposals only of private addresses; "Plays sound ... whenever it likes"; a ring pattern for accepting a plugin. Not taken: a second press for `request.audio` with `ha.entities`, which reach only the owner's Home Assistant. The single button is 9.26 |
| M2 official plugins and revocation | Taken (5.1, 5.2, 5.6): signatures name their purpose; index revocation does not reach official packages; a least version per official plugin compiled into each build, which also stops an old one on the push port |
| M3 migration against the off-switches; no CORS on release assets | Taken (2.6, 5.9: `PLUGINS=0` wins, the feature switch does not; 5.4: in phase 1 the page installs from a file or has the Echo fetch) |
| M4 two stores and two pages in the bridge | Gone with the bridge (12) |
| M5 the `files` type | Taken (4.3): a path per catalog item, an optional file, uploads kept as "not checked yet", a field edit reloads the open word |
| M6 phase 1 is not minimal | Taken in part (8): out of phase 1 are the music and call seams, `storage`, `state`, the Rust example and test vectors, the store and socket calls of the SDK, the plugin SELinux domain, the trap flag, eight slots. **Not taken: dropping C++, Rust and Go from the filter**, with `MAP_FIXED` and the exec pin's argument: the maintainer's constraint, and they are measured and cost the first plugin nothing |
| Stale and contradictory text | Fixed: the goals and non-goals about the wake word, 3.7's row, the counts of message types, cupcake in 10, exports and sync in 4.3, `log.read.full` in 4.5 and 5.3, "under the filter" in 5.3, the listing's serial in 5.1, the CPU class in `SPAWN` and the watchdog's share, the SDK's examples, "about 2 s" (now 64 kB), "long enough for the notice to be seen" (gone with the bridge). Section 11's first table and section 12's earlier rounds are history and say so where they no longer hold |

## 12. Maintainer's decisions

2026-11-18, after reading the revision:

| Decision | What changed |
|---|---|
| **Home Assistant is a hard requirement.** No plugin in its place | The `pipeline` capability and all that existed for it are gone: `struct pipeline`, the "Assistant" setting, set-up without Home Assistant, follow-up limits (H3), the two-sinks question (M9), the clock without HA, HA's announcements refused. Phase 6 (then: the pipeline) is gone; today's phase 6 is request audio. #102 is out of scope: a conversation agent in Home Assistant (4.9, 4.11). The microphone rule is simpler for it: a plugin answers no request and changes none (3.6); that it never gets one either was written here first and changed by the next decision (`request.audio`, below) |
| **Community plugins also run without the `hassmic_plugin` domain** | 2.7: every plugin runs in the plugin domain or in hassmic's (first written "never in `su`"; see below and 2.7); the page detects and shows which; switching community plugins on without the domain takes a warning in full (and first the button too, dropped in the eighth round below) that names what is missing, what remains, and the command that closes the gap, per `INSTALL` method (`scripts/install-system.sh`; `boot` models through `install-boot.sh`, cannoli through its firmware ZIP) |
| **Revocation without the page open** | 5.5, 5.6: `revoked.json`, signed by the index key; hassmic fetches it daily through `update.c`'s path while an index plugin is installed, whatever "Online updates" says; Echos of one arbitration network announce their serial and pass the signed list on, so one Echo with internet covers the rest; the page is a third path; the list's age is shown. Threats and tests follow |
| **Not zig-only: the contract is the binary** | 2.3: a static 32-bit ARM ELF without interpreter, with a non-executable stack, on the allowed calls; C, C++, Rust, Go, Zig. Static bionic stays out (measured). 5.8: the protocol as a written spec with test vectors, a reference C client with a plain C header, examples in C and Rust, `plugcheck` for the ELF rules, a harness on the PC (qemu-arm) and on the device. 2.4's `clone` row and a per-runtime row are since measured (below) |

A further decision, after the above: **a plugin must be able to get the audio that is sent to Home Assistant.**
It does not bring `pipeline` back: 4.9 stands, a plugin answers nothing. New: the capability `request.audio`
(3.3, 3.5, 3.6, 4.12), a one-way copy of exactly the blocks handed to `proto->audio`, taken after the hand-over
in one new function at `main.c`'s two call sites, dropped when the plugin does not read; `request.text` beside
it for the transcript and the reply; the strongest warning for community plugins, a second confirmation when the
plugin can also reach the internet, and no index listing for community plugins with both. Where the suggested
rules did not survive the code: "the ring shows it" holds only while the owner leaves the ring lit (9.17), and
with `-w remote` "what Home Assistant gets" would be a permanently open microphone, so the copy starts at Home
Assistant's wake word there. Replacing Home Assistant's speech-to-text is a different thing (a Wyoming STT server
on the Echo, 4.12) and is written down as a later case, not designed.

A third round of decisions, and what each changed:

| Decision | What changed |
|---|---|
| **Capabilities are individual; each is accepted at install** | 3.3 re-cut: 18 capabilities, one power and one install line each; `net` split into five, `storage` and `files` apart (who writes differs, 4.4), `audio.volume` out of `audio.music`, `ha.actions` out of `ha.entities`, `state` and the two log levels on their own; a plugin without any can only start and read its settings. Only `wake` has the microphone all the time (3.6) |
| **Acceptance travels in a sync; updates that widen anything are accepted then** | 5.4: `accepted` records what the owner accepted; a sync inside one arbitration network, by a browser approved on both Echos, with an acceptance voucher (made as the login voucher, `arb_web_key`) and the byte-same package, needs no button on the target; "wider" is defined per parameter (3.3) and any of it takes the button, with the old version running until then |
| **The first plugin is microWakeWord, out of the core** | 4.13: the `wake` capability, `wake_plugin` as a third `struct wake_engine`, what stays and what moves (file by file), detections checked against the capture's clock, never the capture thread's wait, the CPU measurements to make, the parser of uploads leaving hassmic. Recommended, and since decided: `wake` sealed |
| **Models are owner-uploaded files** | 4.3: the `files` setting type (items by stem, per-item fields, a catalog the browser fetches, a check by the plugin, a read-only descriptor), and what of today's section is lost: only its bespoke layout and dialog |
| **Migration: the official plugin installs itself, fetched online, not bundled** | (First designed with a bridge release, as follows; replaced by the cut at once, below.) 5.9: release A keeps the core engine, copies the models into the plugin's file area, fetches the plugin from the release's assets and installs it without the button (justified there: this id, this key, this exact capability section, nothing else ever); release B removes the engine and falls back to Amazon's until the plugin is there; the models are never lost; no internet: a notice, install from file, or a member hands it over |
| **Install from a file, without the Echo having internet** | 5.1, 5.4: a package carries its listing (signed by the index key or the release key), so the browser-fetch path and the file path are one upload with one set of checks |
| **Sync between Echos: package, settings, files, storage** | 5.4: rows in the page's matrix; files by digest; storage only when ticked; **secrets do not sync** (never sent back to the page, usually an account of one Echo; a sealed Echo-to-Echo way is sketched for later) |
| **Phases** | 8: phase 1 is the seams, the minimum of the plugin system, the microWakeWord plugin and its migration; the syslog forwarder is second and brings the first sockets; API 0.x until it has shipped. *Since 2026-11-21:* hassmic ships no forwarder (#193 is left to a community author), phase 2 brings the sockets with the template repository's examples, and API 0.x lasts until a second plugin, by anyone, has shipped and been used |

A fourth round, on the questions this document had put to the maintainer:

| Decision | What changed |
|---|---|
| **`wake` is sealed** (9.19) | 3.3: a rule, no longer a recommendation, with the second review's leaks closed or named |
| **Microphone capabilities travel in a sync without the target's button** (9.20), on the second review's conditions | 5.4: listed packages only, rules enforced on the target, the voucher bound to browser and source, the target announces it (log, page banner, Home Assistant's "Plugin" event), the revoked-override stays where it was given |
| **The cut at once, no bridge release** (9.22) | 5.9 rewritten: release R removes the engine and installs the official plugin by itself, pinned to id, release key, accepted section and a least version; the warning as the first line of the release text, which is what Home Assistant's update entity shows (`update.c:284-298`, `build.yml`'s Notes step), and in CHANGELOG.md; without internet the fallback of 4.13 with its notice and problem sensor until the plugin is installed from a file; the models stay in `state/mww` and the setting stays `microwakeword`, so a fallback to the build before finds both; the self test asks for the plugin only once it has been ready |
| **Official plugins under the release key** (9.23), with the second review's fixes | 5.1, 5.2: purpose-named signatures, no index revocation of official packages, a compiled least version |

A fifth round, on the four questions that were left:

| Decision | What changed |
|---|---|
| **Plugins run in `su`** on Echos without hassmic's own domain, behind the strongest warning (9.2) | 2.7: a rule, no longer a recommendation |
| **Amazon's wake word stands in only while the plugin is missing; a warning on the settings page** (9.22) | 4.13: the setting `wake_fallback` is gone. Missing (not installed, or `PLUGINS=0`): Amazon's word. Installed and down (crashed, model not loading, over the rate limit): no wake word, the button works, retried every 5 min. A warning on every page either way, and the problem sensor. 2.6, 5.9, 6, 7, 8 follow |
| **No rule about the lights for `request.audio`** (9.17) | Nothing: 3.6 stands. Only `wake` has the microphone all the time; the others get audio for a reason, while the Echo listens anyway, and the lights work as today |
| **A runtime that needs more of the filter gets it** (9.5) | 9.5: the filter is widened, each addition with an argument rule where possible, a row in 2.4 and a check in the probe |

Read into the second decision, and the maintainer's to correct: "missing" means no package installed (or
`PLUGINS=0`); that the setting is gone and not kept as a switch to "none"; and that the warning is on every page
of the settings page, with the problem sensor for Home Assistant kept beside it.

Device results that followed (2026-11-18, biscuit and donut; 2.3, 2.4): C, C++, Rust and Go all run under the
filter with `clone` flags `{0x7d0f00, 0x50f00}`, `MAP_FIXED`, `fcntl` and `sched_getaffinity` added; donut's
kernel has the filter; libc++'s `std::random_device` does not work; qemu's user mode cannot carry the filter, so
the device run is the proof (9.7). Then the narrowed table with its argument rules: 39 checks right on both
kernels, `mmap2` made to require `MAP_ANONYMOUS`; plugins in `u:r:hassmic:s0` work on biscuit; and donut turned
out to have no `hassmic` domain at all, which made "never `su`" a decision again (9.2: allowed, with
the strongest warning, in the fifth round above). Not measured: cannoli, cupcake, the `hassmic_plugin` domain.

A sixth round (2026-11-18): **even official plugins go into repositories of their own**, and the index gets its
repository now.

| Decision | What changed |
|---|---|
| **A repository per plugin, official ones too** | `plugins/` is gone from this tree: microWakeWord is `hassmic-plugin-microwakeword` (source, the engine's files and the SDK copied in, its test against this tree's PC build, its own author key and CI). 3.1, 5.2, 5.7, 5.8, 8 follow |
| **The index's repository now** | `hassmic-plugins`: entries, `tools/index.py` (check, fetch, build, sign, verify), its key, its workflow; official plugins only so far (5.3) |

Read into it, and the maintainer's to correct: **the release key stays in this repository alone.** A plugin's
repository signs its manifest with a key of its own; the listing that makes the package official is still the
release key's, made in hassmic's `release` job over a manifest digest that this tree names (5.2). The other
reading, the release key as a secret of every official plugin's repository too, would have let each of them
publish at its own pace, and put the key that signs hassmic's bundles into as many places as there are plugins.
Also read into it: the SDK stays here (plugins copy it), and the index lists official plugins without being able
to make or withdraw one.

A seventh round (2026-11-21), on step 2 of the plugins' road: **the built-in microWakeWord is not cut yet.**

| Decision | What changed |
|---|---|
| **Deprecated, not cut; the owner moves with a button; the cut in a later, named release** (replaces the third round's "cut at once" for now) | 5.9 rewritten: the engine kept and said to be deprecated wherever it is chosen and in Home Assistant (a problem sensor while it listens); the move on the Wake word page (the plugin installed through the index with the action button, the models copied through the store's item path with their thresholds and windows, the engine switched last with the same words, switching back one click); *the cut release* named when it is made, with what it will do and what must be true before it. No automatic install of the plugin anywhere before it, also not after an update |
| **Before a hassmic update of another plugin API, the plugins' builds first** | 5.4, 5.9 as built: the API in the bundle; each installed plugin's build fetched and kept beside without the button; the update held back where one would stop, until the owner says go on; old-API packages deleted a week after the self test; the wipe when an update drops `wake` (3.3). All but the wipe removed 2026-11-23 (versioned scopes, 3.1, 5.9) |

An eighth round (2026-11-22), on the community switch before it was released:

| Decision | What changed |
|---|---|
| **Switching community plugins on takes no button** | 2.7 "As built": installing a plugin asks for the action button already, and that is enough. `plugins_community` is an ordinary setting of the page, on and off from an approved browser; `W_COMMUNITY` and `POST /api/plugins/community` are gone, and so is `settings.c`'s refusal of "on" from anywhere else. Exported, copied by Compare & copy, taken from an import and a preset like the rest of an Echo's settings (each plugin on another Echo still takes that Echo's button); not a Home Assistant entity. Default off; while off, every refusal and the stopped-and-kept rule as before; the warning in full where the sandbox misses its SELinux wall stays in the switch's dialog, and is in each such plugin's install dialog too, which an import, a preset or a copy does not skip (a review of 00d492a) |
| **No first start's rule** | Nothing was released with plugins and nobody runs them, so there is nothing to keep running: an Echo with no line for the switch starts with it off, whatever is installed. `main.c`'s rule, `settings_peek_config` and `ps_community_installed` are gone |

## 13. What is built

2026-11-18, the same day. Phase 1 without its last step: the seam, the minimum of the plugin system and the
microWakeWord plugin are in the tree; the cut (5.9: the engine out of hassmic, the migration) is not, because 8
makes it wait for the CPU and latency measurements on a Dot 2, and those need the plugin built for ARM and an
Echo. hassmic's own microWakeWord engine is still there beside the plugin, which is what the measurements compare.
(2026-11-21: deprecated, and the owner's move to the plugin built; the cut waits for a later, named release, 5.9.)

**Where things are.**

| What | Where |
|---|---|
| The launcher (2.1 to 2.4) | `src/tools/plugrun.c`, protocol `src/include/plugrun.h`; `build/<model>/plugrun` (static, NDK), `build/plugrun-host` |
| The filter (2.4), as data | `sdk/filter.def`; `tools/mkfilter.py` makes `build/plugfilter.h` (ARM: 235 instructions; x86_64 for the PC) and has `judge()` for a harness |
| The wire format (3.1, 3.4) | `sdk/messages.def`; `tools/mkplugmsg.py` makes `sdk/plugmsg.h` and the message reference of `sdk/PROTOCOL.md` |
| The broker (3, 2.6) | `src/hassmic/plugin.c`, `plugin.h` |
| Packages, accepted, settings, files (4.1 to 4.4, 5.1, 5.4) | `src/hassmic/plugin_store.c`, `plugin_store.h` |
| The seam (4.13) | `wake.c` (`wake_plugin`, `wake_is_plugin`, `wake_is_vendor`), `main.c` (`wake_listed`, `wake_plug`, `core_wake_plugin_changed`, `core_wake_problem`, `core_wake_engines`), `settings.c` (the engine's choices at run time, the setting `plugins`) |
| The page (4.3) | `web.c` `plugins_api`, `web/plugins.js`; the warning on every page through `/api/state`'s `warnings` |
| Home Assistant | the problem sensor "Wake word engine" (`proto_esphome.c`) |
| The SDK (5.8) | `sdk/PROTOCOL.md`, `sdk/hassmic_plugin.h` + `sdk/hmplugin.c`, `sdk/plugcheck.py` (`keygen`, `elf`, `pack`, `list`, `show`) |
| The plugin (4.13) | its own repository, `hassmic-plugin-microwakeword`, beside this one (`src/main.c`, `manifest.json`, `test.py`, `Makefile`, its CI): the SDK and the engine's sources copied in (`tools/sync.sh`; `make drift` says when they differ), which stay in `src/hassmic` too until the cut |
| The index (5.3) | its own repository, `hassmic-plugins`: `plugins/<id>.json`, `revoked.json`, `tools/index.py`, `keys/index.pub`, its CI; here `keys/plugins.pub`, `keys/plugins.conf`, `plugin_index.c`, `plugin_fetch.c` |
| On the Echo | `main.sh` (`plugrun -L --` in front of `runas`, `PLUGINS=0`), `run.sh`, bundles and installs carry `plugrun` |
| The probe (6) | `src/tools/plugprobe.c` (every rule of the filter asked from inside, both ways: 70 checks on the PC), `plugrun -T <program>` (the launcher by itself), `scripts/plugin-probe.sh` (both onto the Echo on adb, the table into `device-logs/`) |

**Tried, on the PC** (x86_64, static glibc plugins under `build/plugrun-host` with the filter's x86_64 translation):
`make unit-plugrun` (the launcher's protocol, what it refuses, the probe's 70 checks against the x86_64 filter, a musl program's `raise()`, `pthread_kill()` and `abort()` (`tests/unit/plugrun_musl.c`, zig); and
both seccomp programs, ARM's too, run in an interpreter against the table on 117,240 calls),
`make unit-plugmsg` (every message cut at every length, every byte changed, random packets, under ASan and UBSan),
`make unit-plugin_store` (packages wrong in one way each, then 50,000 at random, under ASan),
`tests/fake_plugins.py` (packages refused and taken, the button, settings and a secret, files and checks, the plugin
as the wake word engine with Home Assistant, detections that cannot be, the rule for missing against down, crashes,
a start with the plugin as the engine, an update, removal; its packages made in the two steps of 5.2). In the
plugin's repository, `make test HASSMIC=<this tree>` (pymicro-wakeword's models through the page, a real "Alexa"
in `testdata/alexa_espeak.raw` heard by the plugin, a run in Home Assistant; the package as released there a
sideload, with the listing official). In the index's, `make check` (an official package through to the signed
site, and each way an entry, a package, the revocation list or the site can be wrong).

**On Echos since (2026-11-19; PLAN.md "Plugins" has the table):** the probe on a Dot 2, a Dot 3 and a Show 5, 73
checks and none wrong on each (the hassmic domain on the Dot 2 only); on the Dot 2 hassmic under the launcher from
`main.sh`, the plugin built with zig installed through the page and listening as the engine, 1.5 points of a core
over the engine inside hassmic. No detection heard yet, no latency measured. What follows was the state the day
before.

**Not tried then: anything on an Echo.** The launcher builds for ARM with the NDK and has not run there. No plugin has
been built for ARM (no musl toolchain on the machine it was written on: the plugin's `make` wants `zig` or
`PLUGIN_CC`). So 2.3's steps 2, 3, 5, 6 and 7, the CPU groups, the
user per slot and the ARM program of 235 instructions are code that compiles, not code that was seen to work; 2.3's
and 2.4's measurements were made with the prototype before it.

**Where the code went another way.**

- *The start* (3.1): `FILES` first, `CONFIG` last, and the first `CONFIG` is what the plugin answers `READY` to;
  before `READY` it may send `FILE_OPEN` and `WAKE_WORDS`. A wake plugin loads its models before it says it runs.
- *Lists on the wire*: a list is a count where the field stands, its elements after the last field of the struct
  it is in. That lets a writer put an element's fixed fields in one call and its lists after.
- *`WAKE_WORDS`* names the file list its ids are items of. *An item's file* may say `"as"`: the name it is kept
  under, which is how microWakeWord's `model.tflite` and `manifest.json` stay what hassmic's own engine reads (5.9).
- *The `files` capability's parameter* is megabytes, as `storage`'s is kilobytes; the lists are the manifest's
  `files` settings.
- *`listing.sig`* is text: the signature as hex on its first line, then exactly what it signs. Only the release
  key's is known (official); hassmic knows no index key in this version, so a package is official or a sideload.
- *The author's key of an official plugin* is its repository's own, not the release key (5.2): the listing is the
  only thing the release key signs of a plugin.
- *Updates* (5.4, 3.3; `plugin_store.c` `wider`): held against the installed manifest whose accepted section is
  the one `accepted` names (`accepted` keeps the digest, not the section's text). The rule is 3.3's as it stands
  now (the fifth read below): the same or less without the button, everything else with it, as "New" (a capability,
  a row), "More" (a larger quota, list or file, an entry more, a `requires` raised into a version that widens) or
  "Changed" (anything else that is not as accepted, the name and the author among it); "Limit" lines (`memory_mb`,
  `threads`) and "Speaks" lines (a `requires` raised within, `requires_wider`) are shown and ask for nothing. A change
  to a capability's parameter that this version does not read (a list's entry gone, ports "any" made "514") asks
  for the button though it may be narrower: the store knows none of those forms, and the capability's own code
  brings the rule for what is less (`wider` has the place marked). The digest in `accepted` is over the section as
  3.3 has it now ("hassmic plugin accepted 3": with description and source); one a build before wrote (2: without
  those two; 1: of each setting its name, type and label only) still counts as accepted, and an update of such a
  plugin is held against its installed manifest in full. A next form is one more head and one more digest in
  `ps_manifest_parse`; nothing else asks which form a digest is of (`accepted_is`).
- *An update that drops `wake`* (3.3: a new install, files and storage wiped first) is asked for with the button
  and a line that says it keeps what it has; the wipe is not built.
- *The crash marks* (2.6) are `state/plugins/.fall` and `.falls`: without the dot they would pass for a plugin's
  id, as the staging file once did. The mark names the plugin by its id, not its launcher slot (a slot is another
  plugin's at the next start). It is written only in a run that started a plugin (the descriptor is opened then):
  before that a crash is none of theirs. The handler hands the signal on to whoever had it (bionic's debuggerd),
  and an orderly `exit()` disarms it first, so that a library's abort in its destructor is no crash. At three the
  count is not forgotten after 24 h: that is the owner's to lift ("Switch plugins on again" on the Plugins page,
  `POST /api/plugins/resume`, or the feature switched off and on). The plugin a mark names is held back for one
  run; its own switch, off and on, or a new version of it starts it. The wake engine's is held back for
  `RETRY_MS` (5 min) from the start and then started by `timers()`, as one that crashed. Only marks that name a
  plugin count, and a message stops being its plugin's where the broker calls into the core (`core_begin`): 2.6.
- *The watchdog's clock* (2.6): the broker's own (`btime`), on which one round of its loop counts a second at
  most; pings and the full queue are timed on it. "The queue full for 5 s" is: a send came back with EAGAIN, and
  for 5 s since not even the smallest message (a PING of id 0, which no answer matches) fit. CPU is measured
  against the wall clock (a plugin's CPU is its own whoever waited), and both by the launcher: `cpu_watch` asks
  it once a window (`PLUGRUN_USAGE`), the answer carries the process's CPU time and the launcher's clock at that
  reading, and more than 60 s of the one in 60 s or more of the other ends the plugin. An answer that does not
  come is asked for again after 5 s; one that says "not readable", and a launcher of version 1, are said once in
  the log: no CPU watchdog then.
- *The fingerprint* (5.4): the first 16 hex digits of the key's BLAKE2b-256, in fours; in the check's answer for
  every package, shown by the page for one without a listing, printed by `plugcheck.py show` and `pack`.
- *The engine setting* has the values `amazon`, `microwakeword` (hassmic's own engine, until the cut),
  `homeassistant` and `plugin:<id>` per wake plugin installed, plus the chosen one when it is not installed.
  The pick is kept per engine: `state/wake_word-<id>` for a plugin's.
- *Plugins other than the wake engine's* start once the self test passed, late too (2.6), or 10 s after hassmic
  where no self test runs; switching the feature on before then waits for it too.
- *The filter on the PC* has rows of its own for static glibc (`arch_prctl`, `newfstatat` of a descriptor,
  `prlimit64` to read, glibc's `clone` flags, `clone3` and `rseq` as `ENOSYS`), marked `pc` in `filter.def` and
  never in the ARM program.
- *Uploads* (5.4) go through the store's own staging (`state/plugins/.stage`, pieces in order), not `artifacts.c`'s:
  a package is one file with one check, and the artifact kind `plugin:<id>` comes with the sync that needs it.
- *The button is bound to the package the page listed*: the manifest's digest is kept from the check, nothing else
  can be uploaded or checked while a press is waited for, and what is installed has to be that package.
- *A plugin's entry is not a launcher slot*: every installed plugin has one (16), a slot and its user only while
  it has a process.
- *A new list of wake words* reaches the core 2 s apart at most and only when it is another list; 30 other lists
  in ten minutes and the plugin is taken as broken, as 20 detections are. `READY` is taken once. A control socket
  the plugin closed ends it at once. Detections that cannot be are said in the log once in 5 s.
- *For the tests*: `HASSMIC_PLUGIN_HEARD` (the detections that make an engine broken, fewer than 20),
  `HASSMIC_PLUGIN_RETRY` (the 5 minutes between restarts, shorter), `HASSMIC_PLUGIN_SANDBOX` (PC launcher only: `0`
  no filter, `broken` a kernel without one), `HASSMIC_PLUGIN_CPU` (`<percent of a core>:<ms>`: the CPU watchdog's
  share and window, less than a core and a minute), `HASSMIC_PLUGIN_CORE` (`<count>:<ms>`: how many of hassmic's
  crashes over one plugin's reports, within what time, make its start wait; fewer and sooner).

**Left of phase 1**, in the order it would be done:

1. The run on an Echo: `scripts/plugin-probe.sh` (written, never run), the launcher under `main.sh`, a plugin built
   with musl; then the CPU and latency measurements of 4.13.
2. ~~Sync between Echos with the acceptance voucher (5.4), and `artifacts.c`'s kind `plugin:<id>` for the
   package~~: built 2026-11-19, below ("Sync between Echos"), on the PC; the package goes through the upload, not
   `artifacts.c`.
3. ~~The Echo's own fetch of an official plugin from the release (5.4), the push of a `.hmp` after a bundle
   (5.9)~~: built 2026-11-19, below ("The Echo's own fetch"), on the PC. ~~Left of it: what `update.c` does before
   an update that changes the API minor, and a package of another minor deleted a week after the self test~~:
   built 2026-11-21, below ("The built-in microWakeWord deprecated"); removed 2026-11-23, versioned scopes make
   it unneeded (below, "Versioned scopes"). The migration's install of a plugin never
   accepted is not built and is no longer the plan: the move is the owner's button (5.9).
   Of 5.2: ~~`keys/official.conf`, `scripts/plugin-official.sh` and the `release` job's step~~, gone
   2026-11-20: the index makes every listing and hassmic reads it (below, "The index is the one place for
   plugins"). Neither new repository is on GitHub yet, and neither workflow has run; nothing is published.
4. ~~Of 3.3: the wipe of files and storage when an update drops `wake`~~: built 2026-11-21, below.
5. The cut release (5.9: named when it is made; the deprecation and the owner's move came first, 2026-11-21).

**The index builds community plugins (2026-11-22), in the index; this tree's side on the PC only.**  8's phase 3
had left the index's build job for community plugins.  Built in `hassmic-plugins` (its commits 1e98a25, 0574f27,
01e80b8, bccb6a9, 645ca61; 5.3 "As built" says what it does): a community entry pins its source's commit, the
manifest's digest, the author's signature, the LICENSE's SHA-256, the capabilities and the build; `index.py
community` builds it twice in bubblewrap with zig 0.13.0 on two CI runners that must agree byte for byte, holds it to
the entry, runs it in the harness (a smoke test; the Echo's seccomp filter is the boundary), flags its broad
capabilities, and the publishing jobs never wait for it.  Not built there: Rust and Go toolchains, an update that
merges itself, a pinned container.  Not run on GitHub; nothing published.  *Replaced the next day, below
("Community plugins built in their own CI"): the index builds nothing; what stays of this is the entry's digests, the
broad flags and the harness, a pull request's smoke test.*

| What | Where |
|---|---|
| The broad flags read | `plugin_index.c` `version_parse` (`broad`: a list of at most `PI_BROAD_MAX` 32 names, a name this build does not know left out, anything else refuses the list); `struct pi_version` `broad`; the index's `BOUNDS["BROAD_MAX"]`, held to `plugin_index.h` by its `make check` |
| On the page | `plugin_fetch.c` `pf_json` (`here.broad`, `top.broad`); `web/plugins.js` `hmbrowse.broad` (the words, and whether the Echo asks for its button twice: `request.audio` with any `net.*`), the browse row's line; `TIER.community`'s words (since 2026-11-23: built by its author's own GitHub CI from the named commit, attested, not reviewed) |
| The template | `hassmic-plugin-template`'s `tools/indexcheck.py` (the entry as `index.py entry` makes it, with the commit and `make build/<id> build/LICENSE` as the build; `entry_check` and `package_check` run on it; a warning where zig is not 0.13.0 or the tree has changes) and its README's "Getting listed" |

Tried: `make unit-plugin_index` (a version's `broad` read, five wrong kinds and 33 names refusing the list),
`tests/unit/web_browse_test.py` (the flags of the version shown, a name the page does not know left out, the second
press), the template's `make index-check` (its entry through the index's `entry_check` and `package_check`), the
index's `make check` (the bound against this tree).  Each proven by breaking it: the bits not set (`unit-plugin_index`
fails), the list's kind not checked (the five and the 33 fail), `hmbrowse.broad` on the newest version instead of the
one shown and with unknown names kept (`web_browse_test.py` fails).  `pf_json`'s two fields end to end only empty
(`tests/fake_plugins_fetch.py`, an official version): the PC's stand-in site lists official plugins only, as the
index's tool builds it there, and a community version needs the index's build.

**Community plugins built in their own CI (2026-11-23), in the index; this tree's side on the PC only.**  The owner's
decision of 2026-11-22: "community plugins build in their own CI, just make sure it was published by the CI and not
user-uploaded".  The index is a list: every package it lists, community and official alike, is a release asset of
the plugin's own GitHub repository, built by its GitHub Actions and carrying a GitHub artifact attestation (build
provenance, Sigstore), which the index verifies before it lists a version (5.3 "As built": the subject the package's
SHA-256, the source the entry's repository and commit, a GitHub-hosted runner, the entry's `workflow` as both the
signing workflow and the run's, started by the push of the release's tag; `gh attestation verify` with every rule it
has a flag for, and its JSON read again field by field, the trigger and `buildConfigURI` too).  What it proves:
GitHub's CI built these bytes in that workflow file of that commit for that tag's push; not what the workflow does,
which the maintainer reads in the pull request with all it calls and runs.  gh's verdicts apart from its failures:
an outage never takes a listed version off the index (the build fails), nor does a no to the bundle the site
keeps of it (the same bytes it held for: gh, Sigstore or the reading changed), and a new version waits for a later
publish; gh is one release, by its SHA-256 (the index's `tools/gh.sh`).  What a release workflow must not take from
outside its commit is the maintainer's reading, written out in full in 5.3 "What that proves" (and in the same words
in the index's README, `tools/attest.py`, docs/SECURITY.md, the template's and microWakeWord's READMEs and
`release.yml`); microWakeWord's release job now builds the program itself as the template's does.  The index's own build of community plugins (bubblewrap, a pinned zig, two
runners, the entry's `build`, `index.json`'s `build` and `harness`) is gone, never published; the harness stays as a
pull request's smoke test on the release's program.

| What | Where |
|---|---|
| The index | `hassmic-plugins`' `tools/attest.py` (the verifier: `verify`, `judge`, `subjects`, `argv`/`command`/`identity`, `Refused` against `Unavailable` by `VERDICTS`; INDEX_GH a seam), `tools/index.py` (`attested`, `release_tag`, `build --strict` (a listed version without a verdict fails it), `fetch` with the site's bundle only for the commit and workflow it listed, `verify` of the bundles, `smoke`; every entry's version names its `workflow`; a community entry `version`, `commit`, `workflow`, `manifest`, `package`, `signature`, `license_sha256`, `capabilities`; every source a GitHub repository; a package packed as `plugcheck.py pack` packs it), `tools/sandbox.py` (what stays of `tools/community.py`: the harness's bubblewrap and bounds), `.github/workflows/index.yml` (`check`, `smoke` for a pull request, `siblings`, `publish`; gh 2.105.0 from `tools/gh.sh` by its SHA-256, with the run's read-only token); its tests `tests/test_attest.py` (with `tests/fake_gh.py` and a real answer of gh 2.105.0, `tests/gh-verify-cli.json`), `test_index.py`, `test_community.py`, `test_workflow.py`, `test_reading.py` (the reading list's eight copies), `test_references.py` (`tools/workflow.py`, all the way down) |
| The template | `hassmic-plugin-template`'s `.github/workflows/release.yml` (a tag's push alone: `build.yml` called, then the program built again and held to the build job's, packed and signed by `tools/release.sh`, attested by `actions/attest-build-provenance` pinned by its commit in `release.yml`'s own job, published with the bundle beside it), `tools/release_test.py` and `tools/workflow_test.py` (`make release-test`, in `build.yml`: the trigger and where the attestation is made too, and since 2026-11-23 what `release.yml` takes from outside its commit: the pinned zig, the artifact for its SHA-256 alone, no credentials left, no cache or `vars`, hassmic at `sdk/VERSION`'s commit; the release job an allow-list, step for step, and the Package and "Which hassmic" steps run against what is not a SHA-256 or a commit), `tools/indexcheck.py` (`make index-check RELEASED=<asset>`: the entry from the release's asset, the tag's commit, the workflow, the attestation verified as the index does) |
| microWakeWord | `.github/workflows/release.yml` of its own since 2026-11-23 (a tag's push alone; CI, `build.yml`, called), its release job attests the package too (official plugins take the same path); before, the release job was in `build.yml`, which a run by hand at a tag also ran.  The release job builds the program again from the tag with the zig `.github/zig.txt` pins by its hash and refuses one that is not the build job's (`tools/release.sh`; it packed the build job's artifact before, which another job of the run could have replaced), its checkout without credentials; CI's test at the hassmic commit `sdk/VERSION` names; `make release-test` (`tools/release_test.py`, `tools/workflow_test.py`, the template's, in `build.yml`) |
| This tree | `web/plugins.js` `TIER.community` and the browse row's words; `plugin_index.c`'s note on the fields it does not read (`workflow`, `release`, `release_sha256`, `attestation`, `attestation_sha256`; no `build`, no `harness`); `tests/plugsite.py` (the stand-in site through the index's tool with its stand-in gh, and its own builder of the same bytes, bundles and `workflow` included) |

Not tried without GitHub: an attestation made by `actions/attest-build-provenance` in a run (no OIDC token outside
GitHub's runners), `gh attestation verify` against GitHub's API for a package of ours (none released), and the
workflows themselves (parsed by `act -l`; the template's release logic run as `tools/release.sh` with a throwaway
key), so the certificate a tag's push of `release.yml` makes is the one Fulcio's documentation and gh's describe,
not one seen.  The reading of gh's answer is held to a real one (cli/cli's own release asset, verified with gh
2.105.0 here: a run by hand from a branch, refused now), and the flags to the real gh 2.105.0 (with that bundle: the
identity and source ref as `attest.argv` gives them pass where they hold and refuse where not; Sigstore's trust root
out of reach is no verdict).  A site of the index from before 93174b2 (no release asset or bundle named) is not read
as the published one (`index.py verify --previous`); none was published.

**Versioned scopes (2026-11-23), in all four repositories; on the PC only.**  The maintainer's decision (2026-11-22):
"versioned scopes and strict backwards compatibility, so plugins can define their min requirement for each scope they
need", in place of 0.x's exact minor (3.1, 5.7).  Built:
- `sdk/messages.def`: `scope` lines (core and the 19 capabilities, all at 1), each message's scope (and version)
  where the capability column was (`UDP_OPEN`, `UDP_OPENED`: net.udp|net.discover; `NET`: the five net scopes),
  enums and constants with their scope, `@N` for what a later version appends.  `tools/mkplugmsg.py` holds the rules
  (3.1) and `sdk/scopes.lock`; `plugmsg.h` gains `PM_SCOPE_*`, `PM_SCOPE_VERSION_*`, `pm_spoken`, `pm_scope_of`,
  `pm_scope_cap`, `pm_check(..., ver)`; a message that grows gets readers and writers that take the version (none
  does yet: `tests/unit/plugmsg_scopes/` shows one, with its vectors' `case v<N>`).  Every decoder is strict: bytes
  after the last field are malformed (each vector file has a `trailing` refusal).
- `HELLO { sdk; scopes }`, `WELCOME { ...; scopes }`, `REFUSED_SCOPES` (was `REFUSED_API`).  `plugin.c`: each plug's
  `ver[]` from its manifest's requires, `psend` holds every message to it, `message()` answers a granted
  capability's type of a later version `ERR_UNKNOWN`.  `hmplugin.c` (SDK 10): HELLO with its whole table, WELCOME's
  versions kept, `pm_check` by them, `hm_scope()`.
- `plugin_store.c`: `requires_parse` (strict, before anything a version may bring; "needs a newer hassmic: <scope>
  <n>"; `m.newer`, the card's name and version kept), `ps_scopes_env`/`ps_scope_have`/`ps_scope_set`
  (`HASSMIC_PLUGIN_SCOPES`, in place of `ps_caps_built` and `HASSMIC_PLUGIN_UNBUILT`), one package `pkg/`
  (`pkg.new`/`pkg.old`, `never_arrived` by a lone `pkg.new`), `accepted` without `api`/`beside`.  `plugin_index.c`:
  a version's `requires` (`req[]`, `req_unknown`); `plugin_fetch.c`: `pf_best` by requires met, `pf_json`'s `scopes`
  and each version's `requires`/`needs`.  Removed: 5.9's "Removed with it".
- The index (`hassmic-plugins`): `SCOPES` in place of `CAPS_BY_API`, `manifest_check` reads `requires` as the Echo
  does, its C cross-check builds this tree's store lowered by `HASSMIC_PLUGIN_SCOPES` and holds its refusals to the
  index's `unmet()`; `index.json` versions carry `requires` (no `api`, no `needs`), and its page says what each
  speaks.  The template: manifests with `requires`, its Rust client's HELLO and WELCOME with scope lists, strict
  decoding, held to the vectors.  microWakeWord: version 8, `requires`.

Tests of it: `make unit-plugmsg` (the rules each broken once, `tests/unit/plugmsg_scopes_test.py`; the grown
message's layouts, `plugmsg_scopes_test.c`), `unit-plugin_store` (requires: every refusal, a lowered build, a fall
back's card), `unit-plugin_broker` (HELLO against requires, what is sent and taken), `unit-hmplugin` (HELLO's table,
WELCOME's versions, a message not spoken dropped), `unit-plugin_index`, `tests/fake_plugins_selftest.py` (a fall back:
not waited for, refused at install), `fake_plugins_ha.py`, `fake_plugins_fetch.py`, `fake_plugins_button.py`
(lowered builds).

**A second index key (2026-11-22), 5.2's step (1), on the PC only.**  A build takes either of two index keys for every
list and listing; nothing else changes.

| What | Where |
|---|---|
| The keys | `plugin_index.c` `pi_keys` (was `pi_key`; `PI_KEYS` 2), `pi_signed` (either); `PLUGINS_PUB2` (none by default, `_Static_assert` of its form) |
| The build | `keys/plugins.conf` `PLUGINS_PUB2` (empty); Makefile `PLUGINS_PUB2_HEX`, `-DPLUGINS_PUB2`, the fork's rule (`UPSTREAM_PLUGINS_PUB`), a key that is none or the first refused |
| A fork | `scripts/fork-release.sh` `conf` keeps `PLUGIN_INDEX`, `PLUGINS_PUB`, `PLUGINS_PUB2` of `secrets/fork.conf`; `scripts/plugins-least.sh` checks against either key, by the Makefile's rule |
| The tests' keys | `HASSMIC_PLUGINS_PUB2` beside the first; `HASSMIC_PLUGINS_PUB` in place of both built-in keys |

Tried: `make unit-plugin_index` (no second key: another key's index refused; the tests' second key: an index, a
revocation list and a listing signed by it held, a third key's refused, a revocation list by it from the site and
from a member refused and by the second taken, kept over a restart and no list in a build without that key, an
unreadable second key taking nothing; and a second build of the test with a key built in: read, its signatures held,
a test's own first key in place of both), `unit-plugin_store` (a listing by the second key refused without it, taken
as official and as community with it, the first key's beside it, a third key's refused), `unit-fork_release` (none
built from the committed file, one built in when named, a fork's own first key without this project's second, the
fork's own second, its lines kept by `fork-release.sh`, one that is the first or no key refused),
`tests/fake_plugins_fetch.py` (a site signed by a new key alone, lists and listings: refused by a build of the first
key, taken by one with both, an update listed by it installed official without the button, counted as from a file
again under the first key alone).  Each proven by breaking it: `pi_signed` with the first key only
(`unit-plugin_index`, `unit-plugin_store`, `fake_plugins_fetch.py` fail), any signature taken in the second place (a
third key's: both unit tests fail), the test's first key beside the built-in second (the second build fails), the
Makefile's fork rule and `fork-release.sh`'s kept lines (`unit-fork_release` fails).

**The steps of a new key, keys of small order (a review of a30d6ed, 2026-11-22).**  Step (2) as documented stopped
the index publishing (its tool read the published site with `keys/index.pub` alone); a build of one key turned
plugins only the other key listed into files' that a default Echo stops, with no way to take the new listing of the
same version; a key of small order (all zero, the identity) took the null signature for anything; a fork's CI never
had its own index lines; the fork rule compared paths; a kept list of the other key kept its word.

| What | Where |
|---|---|
| The index reads the old key's site | `hassmic-plugins` `tools/index.py` `index_keys` (`keys/index-previous.pub`, optional), `published()`, `built_from()`, `verify()` (one site, one key: the one that signed `index.json`) |
| The listing of the version installed | `plugin_store.c` `ps_relist_wanted`, `ps_relist`, `relist_better`; `plugin_fetch.c` `relist_due`, `relist_thread` (after `pf_look_async`'s look, one package at a time, `RELIST_AGAIN_S`), `pf_json` `relist`; `web.c` `/api/plugins/index` "relist"; `web/plugins.js` the row, `plgLook(…, 'relist')`; `plugin_index.c` `pi_signer` |
| The words for a file's | `ps_engine_refused`, `main.c` (`plugin_community_off_now` 1 community, 2 a file's) |
| Keys of small order | `src/include/edkey.h` `ed_key_weak` (the null signature verifies); `pi_keys` (-1), `ps_stage_check` (the author's), `ota.c` `load_key`, `otatool.c`; `scripts/otatool.py` `weak()` (`check`, `verify`, `keygen`); the Makefile (`key_weak`: `PLUGINS_PUB`, `PLUGINS_PUB2`, `RELEASE_PUB`); the index's `index_keys`, `entry_check` |
| A fork's index in CI | `scripts/fork-release.sh ci` from `PLUGIN_INDEX_URL`, `PLUGINS_PUBLIC_KEY`, `PLUGINS_PUBLIC_KEY2` (and the setup sets them from `secrets/fork.conf`); `.github/workflows/build.yml` |
| The fork rule | `keys/plugins.conf` `PLUGINS_PUB2_WITH`; Makefile (`override`), `scripts/plugins-least.sh` |
| A kept list of the other key | `plugin_index.c` `kept_bad`, `pi_revoked_unread`; `pf_revoked_stale`'s words |

Tried: `make unit-plugin_index` (the eight points of small order and the identity's non-canonical form: each a key
under which the null signature holds, as first or second key no key at all and the null signature taken for nothing;
keygen's never; a kept list of the second key under a build of the first: no list, unread, no word, and back with
it), `unit-plugin_store` (an author key of small order refused with the null signature and with any; the review's
case: official by the first key, the index moving to the second: its listing wanted and taken, not by the same key
and tier, a lower tier, another version or manifest; the build of the second key alone: official, accepted, the
engine; the first key's listing there: a file's, refused as the engine in words of a file's, and the second key's
listing taken back; a file's taking a community listing; a key the build does not read refused), `otatool_test.sh`
and `ota_push_test.sh` (both otatools refuse a key of small order with the null signature; an update key of small
order is none on the Echo), `unit-fork_release` (`PLUGINS_PUB2_WITH`: none refused, `make PLUGINS_PUB=`, the key
replaced in place, the same key under another path; keys of small order refused for each; CI's variables, one key,
no release key, every refusal; the setup sets them; `plugins-least.sh` by the same rule), `web_browse_test.py` (the
second press not said on an update of a plugin accepted with both), `fake_plugins_fetch.py` (the site moved to the
second key: the listing of the version installed taken without a press, still running, not started again; the build
of the new key alone with community off: official, it runs; a step back: a file's, stopped, said, and the revocation
list of the new key no word), the index's `tests/test_community.py` (built over the old key's site with
`index-previous.pub`, signed by the new alone, `check`/`fetch --previous`; without it refused; the next build without
it; `index-previous.pub` the same key, of small order, short: refused) and `tests/test_index.py` (an author key of
small order; `CAP_NAME_MAX`).  Each proven by breaking it.

**A review of 3c5d7ff (2026-11-22).**  The steps of a new key once more, end to end: a kept revocation list stayed
the old key's (R1), the listings moved only when someone opened the Plugins page (R2), a fork's dropped key stayed in
CI (R3), six encodings of small order passed the PC's checks (R4), a typo in `PLUGINS_PUB2_WITH` was a silent drop
(R5), a listing that failed for the moment was never asked again (R6); words (R7, R8, R9).

| What | Where |
|---|---|
| R1: the kept list's signer | `plugin_index.c` `kept_by` (`kept_load` by `pi_signer`), `pi_revoked_take`'s `PI_SAME` writes the list anew where another of this build's keys signed it (the floor line kept; `kept_write`); the index's `tools/index.py` `load()`: the switch publish (the published site the previous key's) raises an unchanged list's serial by one, later builds keep the published serial for the same list (`published(..., by)`, `verify()` returns the key) |
| R2: the daily look relists | `plugin_store.c` `ps_relist_reason` (`struct ps_plugin` `listing_unread`, `listing_key`, `listing_serial` from `ps_scan`), `plugin_fetch.c` `daily_fetch` (the whole look, `pf_look(PF_NOW)` then `relist_due`, where a listing may be relisted or one failed: `relist_failed`); `plugin.c` `plugin_selftest_ok`: community off excuses only where the listing holds; the card's words for an unreadable listing |
| R3: a fork's variables | `scripts/fork-release.sh` (fork.conf read a line each; `gone`: `gh variable delete` of what it no longer names) |
| R4: every encoding of small order | `scripts/otatool.py` `weak()` (also what `decode()` refuses), the Makefile's `key_bad`/`key_why` (fails closed: "nopython"); synced to the index, the template and microWakeWord |
| R5: `PLUGINS_PUB2_WITH` | the Makefile: lower case, 64 hex digits or stop; another key than `keys/plugins.conf`'s own `PLUGINS_PUB` (`PLUGINS_PUB_CONF`) stops the build |
| R6: a failure of the moment | `ps_relist` -2 (memory, the flash) -> `R_FAILED`, asked again at the next look (and by the daily look), the row says why |
| R7, R8, R9 | CHANGELOG, SECURITY.md and 5.2: the warning for an unread kept list "says why" (it came at once before); `edkey.h` names the real tests; `hmbrowse.broad`'s `twice`/`may`: wherever the button may be asked |

Tried: `make unit-plugin_index` (a list by the first key, the same by the second: `PI_SAME`, the file written anew,
its floor line kept; again: left alone; the step-(3) build of the second key alone reads it and withdraws what it
names), `unit-plugin_store` (`ps_relist_reason` for a listing by the first of two keys, under the least, not holding;
none for the second key's or a file's; `listing_unread`; `ps_relist` -2 on a write the flash refuses),
`unit-fork_release` (the variables deleted, none asked where none are there, an empty `PLUGIN_INDEX` never shifts the
key into its place; the six encodings refused as a second key; no python3 refuses; `PLUGINS_PUB2_WITH` in capitals
taken, 63 digits and another key under keys/plugins.conf's own `PLUGINS_PUB` stop the build, so does keys/plugins.pub
replaced in place), `otatool_test.sh` (the six: `weak()`, and both otatools refuse them), `web_browse_test.py` (an
update of a plugin accepted with both: "may"; the version installed: nothing), `fake_plugins_fetch.py` (the build of
the new key alone, the old key's listing, community off, the page never opened: the daily look reads the index, takes
the listing, the plugin runs official; one key, its own listing: the revocation list alone, as before),
`fake_plugins_update.py` (community off: the self test does not wait for a file's plugin, and waits for one whose
listing it does not read), the index's `test_community.py` (the switch with `revoked.json` unchanged: serial one
higher, kept by the next build, a changed list under it refused) and `test_index.py` (the six as author keys; the
published list under a lower serial in the file taken as published), the template's `indexcheck_test.py` (its folder
gone after the run).  Each proven by breaking it (R8: words only).

**A review of 3fcd319 (2026-11-22).**  What the docs said of a failed self test was not what happens (S1), the only
repair of a listing that does not hold was the daily look, a day after one that failed (S2), the kept list's signer
flipped with whoever handed it over last (S3), the daily look could read the index for good (S4), a failed look
nobody asked for became the page's list (S5), a command-line `PLUGINS_PUB2_WITH` skipped the lower case (S6), the
browse row said "twice" for sure of a plugin installed for another API only (S7), the self test held every update of
a wake plugin that had run as a file's (S8); and 5.2 did not say what (3) waits for (the note).

| What | Where |
|---|---|
| S1: a failed self test | words: CHANGELOG, GUIDE, SECURITY.md, 5.2, `plugin.c` `plugin_selftest_ok`'s comment, PLAN.md, the index's README: not the factory copy at that start; nothing goes back, the build runs on and its own look restores the plugin |
| S2: a listing that does not hold | `plugin_store.c` `ps_relist_reason` 2; `plugin_fetch.c` `relist_tick` on the daily thread (`RELIST_FIRST_MS` 1 min, `RELIST_EVERY_MS` 1 h, `RELIST_CHECK_MS` 1 min, `RELIST_GAP_MS` 5 min; `net_online`, moved from improv.c to net.c; `HASSMIC_PLUGIN_RELIST`); `pf_shutdown` (main.c, SIGTERM and the end) cancels the thread's looks and downloads |
| S3: the kept list's signer | `plugin_index.c` `pi_revoked_take`: rewritten only for `by > kept_by` (the second key is the new one) |
| S4: no reason for good | `plugin_fetch.c` `unlisted[]` (from `relist_due`: none of that number, API and manifest, withdrawn, over the bound), `relist_upkeep` (both held to what is installed); `ps_relist_reason(skip, n)` |
| S5: the page's list | `PF_DAILY` (`plugin_fetch.h`): the Echo's own looks; one that fails leaves `known` as it was |
| S6: `PLUGINS_PUB2_WITH` | the Makefile's `override` |
| S7: "may" | `web/plugins.js` `hmbrowse.broad`: `have.here === false` |
| S8: the ready mark | `plugin.c` `ready_note`: `listed 0` or `listed 1` (the listing held under the build that wrote it); `plugin_selftest_ok` holds for an unread listing only where it says 1 (a mark without the line counts as 1) |
| The note | 5.2 (3) and the index's README: (3) waits until the index lists every version still installed; one it no longer lists stays a file's |

Tried: `make unit-plugin_index` (the first key's copy after the second's: the file left alone), `unit-plugin_store`
(`ps_relist_reason` 2 for a listing that does not hold, 0 for a version skipped, not for another), `unit-fork_release`
(`make PLUGINS_PUB2_WITH=<capitals>`), `web_browse_test.py` (installed for another API: "may"),
`fake_plugins_fetch.py` (no network: nothing; the network back: the index at once; the site out of reach: the page's
list as it was; read again after the hour (3 s): the listing taken, official, running, no daily look; nothing after
it; a version the index does not list: read once), `fake_plugins_update.py` (the mark says `listed 1`; one that says
`listed 0`: the self test passes with community off and a listing it does not read).  Each proven by breaking it (S1
and the note: words only).

**A review of 9cd7a77 (2026-11-22).**  A self test held by the wake word plugin alone ended at 30 s, and with it
went every other plugin for the whole run, and the build stayed off the factory copy until a restart, though its
own look brought the plugin back a minute later (T1); the network's return within the gap after a look was forgotten
(T2); a listing the index has and this Echo cannot take kept the index read hourly (T3); a version the last look
found not listed was never looked for again in the run, also when the index listed it again (T4); a community
listing's `ready` mark said `listed 1` (T5); the card's words named the daily look (T6).

| What | Where |
|---|---|
| T1: the self test asks again | `main.c` `selftest_thread` (`SELFTEST_AGAIN_MS` 10 s, until hassmic ends), `selftest_passed` (`ota_healthy`, `plugin_go`, the obsolete packages' week), `selftest_wake_ok` (the engine as it is now, under `core_lock`); `plugin.c` `plugin_selftest_ok(id, late)`: late, a mark gone since (`plugin_selftest_forget`, the next hassmic's handoff) is no pass; root's `ota_watch` takes `healthy` whenever it comes.  No audio within the 30 s: as before, said, the others not started in that run |
| T2: the network back within the gap | `plugin_fetch.c` `relist_tick`: `relist_next` = `relist_last + relist_gap` where sooner |
| T3: a listing not to be taken | `relist_upkeep`: every version whose `relists[]` entry is `R_NOTHING` for that version into SKIP |
| T4: the daily look | `daily_fetch`: reason 2 whatever SKIP says (`ps_relist_reason(NULL, 0)`): the index once a day while a listing does not hold |
| T5: the ready mark | `plugin.c` `ready_note`: `listed 1` for `PS_OFFICIAL` only |
| T6: the card | `plugin_store.c` `ps_scan`: "The Echo looks at the index by itself within a minute and takes its listing anew" (189 bytes of `refused`'s 200) |

Tried: `unit-plugin_store` (the card's text whole), `fake_plugins_fetch.py` (T2: the network back 1 s after a look
that failed, read again when the 5 s gap is over, not at the 20 s; T3: a package whose listing is the first key's
under the build of the new key alone, downloaded once, the index read once in 4 s; T4: the index lists the version
again, the daily look reads it and takes the listing), `fake_plugins_update.py` (T5: a community listing's mark says
`listed 0`; T1: a second plugin installed, the listing unread with community off, held at 30 s with the other plugin
stopped and no `healthy`, the Echo's own look at 40 s takes the listing, the self test passes late, `healthy`
written, the other plugin running).  Each proven by breaking it.

**A review of 1d704e9 (2026-11-22).**  `healthy` said "1", not which build passed, and nothing took away one left
behind: a build held by its wake word plugin, asking every 10 s, could pass late in the seconds between root's
install of the next update and its own end, and root's next look, after the install, made that untested update the
factory copy (`factory()` saw it installed and running) (U1); a community wake word plugin whose listing this build
does not read, with a `ready` mark without the `listed` line or with 9cd7a77's `listed 1`, held every update's self
test with community plugins off, and never ran to write its mark anew (U2); nothing tested that a mark gone since the
hold is no late pass (U3).

| What | Where |
|---|---|
| U1: `healthy` names the build | `ota.c` `ota_healthy` writes `VERSION_NAME` (core.h: the bundle's VERSION, what update.c reports); root's `main.sh` `healthy` (taken into root's folder, a plain file with one name, 128 bytes, never said) promotes only where it is `$cur/VERSION`, drops anything else (a "1" of a build before included); the install branch calls it before it installs (the build that runs, the update installed until then) |
| U1: no pass while root has a bundle | `ota.c` `handed` (set by `stage()` under `hand_lock` before the request, cleared where root said no or nothing was handed over, kept where root never answered), `ota_handed`; `ota_healthy` writes nothing while it is set (1); `main.c` `selftest_try` asks nothing then; held at 30 s by a handoff alone: asked every 10 s on, the plugin not held to its mark |
| U2: an unread community listing | `plugin_store.c` `listing_said` (the tier line of a listing that does not hold, signature not asked), `ps_plugin` `listing_said`; `plugin.c` `plugin_selftest_ok`: community plugins off and that line `community` is the owner's choice |
| U3 | test only |
| Tests' pace | `main.c` `HASSMIC_SELFTEST=<window ms>:<again ms>` (30 s and 10 s on an Echo): `fake_plugins_update.py` holds the self test a dozen times, 6 s and 2 s there (the test took 260 s of `run.py`'s 300, 156 s now) |

Tried: `tests/ota_push_test.sh` (`healthy` is the build's `make version`; main.sh's `healthy` cut out: its own word
promoted, a "1", another version, a hard link, a symbolic link dropped, nothing for a factory copy already; the call
in the install branch before `otatool install`), `fake_plugins_update.py` (U2: an unread community listing with a
mark without the line and with `listed 1`, community off: passes; U3: held, the mark taken away, an ask: no pass, no
`healthy`; U1: a bundle pushed and installed, the plugin listening again: no pass, no `healthy`; root refusing the
next push: passes, `healthy` the build's version).  Each proven by breaking it.

**A review of 0119caf (2026-11-22; the forgetting of a wake plugin's mark for "install it all the same" removed 2026-11-23 with versioned scopes, 5.9).**  A wake word plugin's mark forgotten for the next hassmic ("install it all the
same", a push of another plugin API) before the bundle was with root (the release key's check; a push waits up to a
minute for another's handoff) counted as never there for this hassmic: within its self test's window the absence
passed it, its wake word plugin down, `healthy` named it, and root's install, which takes `healthy` first, made it
the factory copy (V1).  A handoff root did not answer within the minute (its loop at other work: a light ring's set,
a reset's stop) stayed "with root" for the run: a "no" after it was never read, the self test never passed, the other
plugins stayed stopped and the older plugin API's week never began until a restart (V2).  The second check of the
hall's tone in `fake_ha_dropin.py` had no level guard: noise at -5 dBFS read -27.9 dB in a frame's 50 Hz bin and
passed (V3).  The install branch's `healthy` ran `factory()`, and with it root's `sysinstall.sh`, in the daemon's
folder (`cd -P $IN`).

| What | Where |
|---|---|
| V1: a forgotten mark is this hassmic's | `plugin.c` `plugin_selftest_ok`: under `rmu`, the plugin `forgot` names counts as `was` (no pass on its absence, early or late), until `plugin_selftest_keep` or the process ends |
| V2: root's late answer | `ota.c` `unanswered` (set where `answer()` gave up, `handed_late`), `ota_handed`: once `state/ota/request` is gone (root took it), `result` read: "OK" keeps `handed`, anything else clears it and gives the forgotten mark back (`plugin_selftest_keep`, as a refusal in time does; the review of 9bbfd66 below: whatever was forgotten, `give_back`, read by a thread of its own, tied to its request by a nonce).  A root that never takes the request keeps it as before |
| V3 | test only: `tdb > level(a.voip, 2) - 1` |
| The install's `healthy` from / | `main.sh` ota_watch: `(cd /; healthy)` |
| Tests' pace | `ota.c` `HASSMIC_OTA_ANSWER_S` (root's minute), `HASSMIC_OTA_STAGE_DELAY=<ms>:<file>` (the gap between a forget and the handoff) |

Tried: `fake_plugins_update.py` (V1: a push of plugin API 0.99 forgets the mark within the window, the handoff held
open: no pass, no `healthy`, held by the plugin; V2: root busy past the handoff's 2 s, the plugin listening: no pass;
its late "OK": none; the next push's late "no": passes, `healthy` this build), `fake_ha_dropin.py` (V3),
`tests/ota_push_test.sh` (the install branch's call run from hassmic's folder: `factory()` from /).  Each proven by
breaking it.

**A review of 9bbfd66 (2026-11-22; `ota_forget`, `give_back` and the forgotten mark removed 2026-11-23 with versioned scopes, 5.9).**  Root's late answer was read only by the self test (`selftest_try`, the one
caller of `ota_handed`), whose thread ends once it passed: a run that had passed, then a push of a hassmic its wake
word plugin would stop with (the mark forgotten), root busy past the minute and then refusing, kept the mark
forgotten for the run, and a later update's self test did not wait for the plugin (W1).  The mark given back was the
refused handoff's own (`hand_forgot`), which a later `stage()` overwrote: a push that forgot it, unanswered, then a
plain push in its place before root took the first, refused in time, gave back "" (W2).  Root's `result` was not tied
to its request: a request root took after `answer()` gave up was answered while the next handoff waited, which took
that answer for its own (the pusher told the other's words; a "no" for a bundle root then installed) (W3).

| What | Where |
|---|---|
| W1: the late answer read by itself | `ota.c` `handed_late` starts `late_thread` (one at a time, `late_on`): `ota_handed` every 2 s while `handed && unanswered`; ends when the answer came or another handoff began, else with the process; nothing held across its sleep |
| W2: whatever was forgotten goes back | `ota.c` `give_back` (`hand_lock` held): when nothing is with root (`!handed`) and no forget waits for its handoff (`pending`, counted by `ota_forget`, which update.c's "install it all the same" and a push call in place of `plugin_selftest_forget`, and taken down by `unforgot` once `stage()` returned, stored or not, or for a handoff that never staged), `plugin_selftest_keep()` gives back whatever `plugin.c`'s `forgot` holds (it takes no id any more).  Called by `handed_off` (a refusal in time), `ota_handed`'s late "no" and `unforgot` (a bundle not stored, `ota_handoff`'s refusals before the handoff; the review of 06683b6 below).  `hand_forgot` is gone (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| W3: the answer tied to its request | `ota.c` `stage()`: 16 bytes of `/dev/urandom` as hex in `request` ("1 <hex>"), `hand_nonce`; `main.sh` `ota_nonce` (taken into root's folder as `take()` does, a plain file with one name, 80 bytes, 32 lower-case hex digits or "-", never said) and `echo "nonce $nonce" >> $RES` under every answer; `result_read`: the first line is the answer (what the pusher hears, unchanged for both otatools), a second line that is not `nonce <ours>` makes it another handoff's (`answer()` waits on and says so once, `ota_handed` reads nothing) |
| Pairings in the field | `boot.sh` picks the copy per service, at each service's start, from `ota/tries`: the satellite falls back to the factory copy after five quick exits (`main.sh`) while the firewall service keeps the update's `main.sh`; and in a boot that started both from the factory copy (three failed starts), a later satellite start (`tries` set to 0 after a minute up; a restart the page asks for) runs the update's hassmic beside the factory copy's `main.sh`.  An older hassmic ignores the second line; an older `main.sh` writes none, and a result without it is taken as before (the race W3 describes stays for that pairing only).  A request without a nonce is answered with `nonce -`, which this hassmic never takes |

Tried: `fake_plugins_update.py` (W1: the self test passed, a push of plugin API 0.98 forgets the mark, root busy past
the 2 s and refusing: the mark back; W2: the same push unanswered, a plain push in its place refused in time: the
mark back; the stand-in root echoes the nonce), `tests/ota_push_test.sh` (W3: `ota_nonce` cut out of main.sh on a
plain file, an old request, wrong lengths, a hard link, a link, a FIFO, a folder; a push with the C otatool, the same
line; an answer with another nonce there first: "left alone", the push hears its own; an older `main.sh`'s answer
without one: taken; after the minute (`HASSMIC_OTA_ANSWER_S` 5 there), root slow to answer a push it took late and
slow between taking the next request and deleting that answer: the late answer read is the next push's own).  Each
proven by breaking it.

**A review of 06683b6 (2026-11-22).**  `stage()` took the handoff (`handed`, `unanswered` 0, a new nonce) before it
stored anything, and a store that failed cleared it (`handed_off`): a push of a hassmic the wake word plugin would
stop with, unanswered after its minute and taken by root, then a push not stored (no room while root unpacks the
first) gave back the mark the first forgot, for a hassmic root then installed whose self test waited for the plugin
it cannot run, and ended the reading of the first one's late answer (X1).  `main.sh` `ota_watch` deleted `request`
again after `ota_nonce` had taken it: a request hassmic wrote in between (the next handoff, once it stopped waiting
for that one) went with it, its bundle answered under the other's nonce, its own answer never written, its late
reader asking for good and its forgotten mark never back (X2).  `result_read` read a first line longer than its
buffer as two lines, the rest of it as the nonce's line: "another handoff's" (X3).

| What | Where |
|---|---|
| X1: the handoff taken only once it is stored | `ota.c` `stage()`: no randomness, the bundle or its signature not stored: nothing changed; then under `hand_lock` an earlier handoff's late answer read (`late_read`, `ota_handed`'s body: the old `result` goes next), the handoff as it was kept, and `handed` 1, `unanswered` 0, the new nonce before the request is written (`ota_healthy` as before); the request not stored: the handoff as it was again, and `handed_late()` where it was unanswered (its reader had ended).  The caller: `unforgot(forgot)` (`pending--`, `give_back`) whatever came of it, `handed_off`/`handed_late` only for a stored one |
| X2: the request deleted by `ota_nonce` alone | `main.sh` `ota_nonce`: deletes `request` where it is only if it could not be taken (`mv` failed); `ota_watch`: `rm -rf result`.  A request written in that gap is the next look's: answered with its own nonce (its bundle maybe taken by this look: "missing") |
| X3: a long first line | `ota.c` `result_read`: the first `fgets` without its `\n` skips to the line's end before the nonce's line is read |

Tried: `fake_plugins_update.py` (X1: a push of plugin API 0.98 unanswered, taken by the test as root, a folder where
the next push's bundle goes: "cannot store", the mark stays forgotten for 4 s, root's late "no" to the first read: the
mark back), `tests/ota_push_test.sh` (X2: `ota_watch`'s lines from the nonce to the bundle's move run with root's
`ota_nonce` and a request written right behind it: the request stays, the old answer gone; a request that cannot be
taken: "-", deleted; X3: root's answer with a first line of 307 bytes: the push hears it).  Each proven by breaking it.

**A review of 6631974 (2026-11-23), with the index at 9b02fdb and the template at 865bacf; on the PC only.**  Nine
findings, an observation and three lows, each fixed and each fix broken once on a copy to show the test that catches
it.

| What | Where |
|---|---|
| 1: musl's `raise()`, `abort()`, `pthread_kill()`, `pthread_cancel()` send with `tkill`, which the filter refused (only `tgkill`): `abort()` ended as `SIGSEGV`, `raise()` did nothing, the development flag stopped a plugin at its first `raise()` | `sdk/filter.def` `nr tkill 238 200`, `allow tkill`: no argument rule can hold a thread id to the plugin's own (one counter for every process), the slot's user does (2.3: a signal to another user's process is `EPERM`); `plugprobe.c` asks it; `make unit-plugrun` starts `tests/unit/plugrun_musl.c` (zig, x86_64 musl) with and without the development flag |
| 2: `web.c` took a write for a piece of an upload by `strstr(path, "/chunk/")` and a prefix: a plugin, list or item named "chunk" or "upload" had every write of its never kept over a restart, and replayable after one | `web.c` `piece_route`: the three piece routes matched whole, as the handler matches them; `plugin_store.c` `ps_route_words` (every word under `/api/plugins/` and the page's `browse`) refused as ids by `ps_id_ok`, and by the index (`ROUTE_WORDS`, held to the C list by its `make check`).  List and item ids are positional in every route: none refused |
| 3 (index): `workflow.images` skipped `services:` that was no mapping (an expression) and a job that was none | refused, named (`tools/workflow.py`; `tests/test_references.py`) |
| 4 (index): a package's name, author and description were not held to its entry: a community plugin could call itself "microWakeWord" by "hassmic" | `tools/index.py` `package_check`: each the entry's, exactly (a manifest without a description is never listed) |
| 5: the C client's answer buffer was 1024 bytes and FILE_OK at its bound 2412: nothing went back | `sdk/hmplugin.c` `handle`: `PM_MAX_PACKET`; a FILE_OK `put()` refuses is answered FILE_BAD with why and said in the log.  The Rust client builds no FILE_OK (its examples have no files) and has no fixed buffer |
| 6: a member's word (`M_REVOKED`) was fetched from its datagram's source address and its message's port, by `net_socket` (anywhere) | `plugin_fetch.c` `pf_message`: private, link-local or loopback sources only; `update.c` `on_socket`: a plain socket for `update_fetch_lan` (the egress lock: local network only; on the PC no wall, not testable there) |
| 7: `%ld` and `atol()` on a request's offsets and sizes | `%9ld`; `web.c` `body_size` (1 to 9 digits and nothing more) for both begins |
| 8 (index): every listed version verified again one by one under a 10-minute job | `tools/index.py` `build`: every package held to its entry first, then the attestations `ATTEST_WORKERS` (6) at a time within each group (site's bundle, listed of the API, new), the groups in turn; the publish job 30 minutes; a listed version's re-check is kept (a refused kept bundle is no verdict and fails the build, rev56/rev58) |
| 9: the template's "never learns the address" (`getpeername` is allowed); a wake stream block had no stated bound and the C client took 4096 samples; the C client dropped an unknown type with an id | `src/main.c`'s words; `messages.def` `WAKE_SAMPLES_MAX` 4096 (wake 1 recorded anew, before any release), `plugin.c` `wp_feed` cuts a longer block, the client's buffer by it; `handle` answers `ERR_UNKNOWN` |
| Observation: HELLO's answer checked the state after `bl` was let go, not the process | `plugin.c` `PM_HELLO`: the generation too (`PLUGIN_TEST_HELLO`, `unit-plugin_broker`) |
| Lows: the template's two-target rule ran hassmic's make twice under `-j`; `HM_ERR_NOT_SPOKEN` described as covering a value; `drop` taken on a plugin's own message | the template's `hassmic-pc` (one recipe, `tools/makefile_test.py` counts the runs); the words in `hassmic_plugin.h`, `PROTOCOL.md`, this file; `tools/mkplugmsg.py` refuses `drop` on `>` (the demo's PUT and PICK now `<`) |

**The buttons and rows of a plugin's own (2026-11-22): API 0.7, on the PC only.**  What 3.3 had left "for a later
minor, when a plugin needs them" (3.3's "As built (2026-11-22)" on `button` has the rules and the decisions).

| What | Where |
|---|---|
| `button` (3.3) | `plugin_store.c` `cap_names` (bit 18), `CAPS_PLAIN`, `ps_caps_built`, `ps_cap_line`; `combination` unchanged (its list of what may be beside `wake`); `plugin.c` `plugin_button`, `BUTTON_PER_SEC`, `struct plug` `r_button`, `button_at`/`button_n` (the presses past it, apart from `refusal`); `main.c` `button_event` (the one call, after Home Assistant's) |
| Rows of its own (4.3) | `plugin_store.c` `entities_parse` (`page`: true/false, not on a button or an event, a manifest of API 0.7 or later), `struct ps_entity` `page`, `wider` (`X_ENTS`, `X_ENT`, `x_skip`); `plugin.c` `plugin_json` (`rows`, the entities' `page`, a row's value not twice); `web/plugins.js` `hmrows`, `plgRows`, `plgUses` (an entity that is a row is not said again) |
| API 0.7 (5.7) | `sdk/messages.def` (`API_MINOR` 7, `CAP_BUTTON`, `BUTTON_KEY_*`, `BUTTON_HOW_*`, `BUTTON` 120: 60 types) |
| The SDK | the `button` callback, `HM_CAP_BUTTON`, `HM_BUTTON_*`; SDK version 9; `sdk/PROTOCOL.md` "The buttons", the rows under "Home Assistant" |
| Tests | `make unit-plugin_store` (`button` read, refused with a parameter, with `wake`, in a build without it; `page` on every kind with a state, refused as 1, "yes", twice, on a button and an event, in a 0.6 manifest; `button` added asks, gone is less; `page` added, taken away, moved: no button; with a label or an entity more: the button), `unit-plugin_broker` (`BUTTON` with key and how, nothing without the capability, before `READY` or for a key there is none of, ten at once then five a second, the presses past it counted apart from the plugin's refusals and not in its `dropped`; `rows` without values, with them as JSON text, changing, none while not running or not listed, none for an entity without its key, whose "nokey" stays), `unit-hmplugin` (the callback; none set: dropped), `unit-plugmsg` (60 types, `BUTTON`'s vectors), `unit-web_rows` (new: how a value reads by kind, which entities are listed beside the rows), `unit-web_browse` (now in `make unit`); `tests/fake_plugins_button.py` (new, `tests/plugins/testbutton.c`; a stand-in Home Assistant subscribed to the "Button" event entity, its events and the plugin's the same list, the presses that served a wait in neither), `tests/fake_plugins_sync.py` (a button plugin on Tgt hears the press and the hold Home Assistant hears while a note's wait is armed or withdrawn, and not the hold that dismisses the note); every plugin test moved to API 0.7; the index's `make check`, the template's `make test`/`rust-unit`/`arm-smoke`, mww's `make test` |

Each key check proven by breaking its code (the test then fails, the code put back): `button` allowed beside `wake`
in `combination`; `button` out of `CAPS_PLAIN` (`"button": 1` taken); `page`'s API gate, its true/false check and its
refusal on a button and an event; `page` compared by `wider` (`unit-plugin_store`); `plugin_button` without the
capability check, without the `P_RUN` check, without its bucket; `rows` while not running, while not listed, and a
row's value again among the entities (`unit-plugin_broker`); the client's dispatch (`unit-hmplugin`); the page's
decimals and On/Off (`unit-web_rows`); `plugin_button` called before the page's waits in `on_action`
(`fake_plugins_button.py`: the press that approved a login reached the plugin), `button` out of `ps_caps_built`
(the install refused), and the plugin told of the action button held before `on_held` asks whether it served a note
(`fake_plugins_sync.py`).

Left: all of it on an Echo; there the presses are `buttons.c`'s as for Home Assistant, unchanged.

**Places and the last capabilities (2026-11-22): API 0.6, on the PC only.**  What the phases had left of 3.3's table
(`net.lan`, `net.internet`, `log.read.full`) and of phase 1 ("more than two plugins at once"), 8's item 7.

| What | Where |
|---|---|
| Places (2.6, 4.1) | `plugin.c` `RUN_MAX` (the launcher's `PLUGRUN_SLOTS`, 8), `RUN_MEM_MB`/`run_mem` (128 MB, a quarter of `/proc/meminfo` where less), `places`, `start` (waits, the wake engine's place, `struct plug` `mem`, `waits`, `room`, `wait_since`), `timers` (the oldest that fits started); `LANES` 10 (`RUN_MAX + 2`) and `MUSIC_SOURCES` 14, `plugin_net.c` `QUEUE` 16 and `WORKERS` 8, `RQ_MAX` 64; the card's "waiting", `/api/plugins` `run_mem` |
| `net.lan`, `net.internet` (3.3, 4.6) | `plugin_store.c` `struct ps_reach`, `reach_parse`, `reach_line`, `ps_host_name_ok`; `plugin.c` `connect_message` (by kind), `NET_CAPS`; `plugin_net.c` `pnet_req.kind`, `.mark`, the worker's `pnet_local` for `net.lan` |
| `log.read.full` (4.5) | `logread.c` `logread_full`, `logread_lines(..., full, ...)`; `plugin.c` `log_pump`; `plugin_store.c` `combination` (official only: at install and every scan) |
| API 0.6 (5.7) | `sdk/messages.def` (`API_MINOR` 6; the capability column `a|b`: `LOG_FOLLOW`, `LOG`, `CONNECT`), `tools/mkplugmsg.py` (`pm_caps`, a mask), `ps_caps_built` with all 18, `HASSMIC_PLUGIN_UNBUILT` (`ps_caps_env`: a build without some, for the PC's tests and the index's cross-check) |
| The SDK | `hm_connect_lan`, `hm_connect_internet`, `HM_CAP_LOG_READ_FULL`, `HM_CAP_NET_LAN`, `HM_CAP_NET_INTERNET`; SDK version 8; `sdk/PROTOCOL.md` |
| The page | `web/plugins.js` the state "Waits for a place", the Plugins page's line on places |
| Tests | `make unit-plugin_store` (the entries read and refused in words, the lines, the second press for either, a build without them), `unit-plugin_broker` (eight at once, the ninth waits and starts, the engine's place with the one stopped waiting again, 96 MB plugins against 128; every `CONNECT` refusal of both kinds before a socket, the worker's private line, `log.read.full`'s `LOG`), `unit-logread` (`logread_full`), `unit-hmplugin` (the two calls on the wire), `unit-plugmsg`, `unit-plugfuzz`; `tests/fake_plugins_lan.py` (with `tests/plugins/testnet.c`'s mode `reach` as `testlan`): the install's lines, refusals of the whole log for a sideload and a community listing, the entries' forms, the second press's words, every `net.lan` and `net.internet` case against other hosts behind a veth pair, the whole log against sentinels, a listing gone and the start refused, five plugins at once and a 96 MB one waiting until three stop; the index's `make check` |

Decided where the design left it open, each with its reason in the section it belongs to: the places' bound by the
memory limits' sum, not a count picked again (4.1: memory with real plugins is still unmeasured, and the limits are
the launcher's to hold); a plugin that finds no place waits and starts by itself, and the one stopped for the wake
engine waits too (2.6); the two capabilities' entries in net.server's form, `{ proto, ports }`, not 4.6's `"to"`
(4.6 "As built"), with `"any"` for ports; `net.lan` given as an address only (no lookup); a listener's connect lines
private; a withdrawn official plugin run anyway keeps `log.read.full` (3.3 "As built", API 0.6); API 0.6 though the
wire did not change (5.7).

Each key check proven by breaking its code (the test then fails, the code put back): two places as in phase 1 (ten
checks of `unit-plugin_broker`), no start of the waiting ones in `timers` (five), the memory bound left out (the 96 MB
plugins), the places not asked again at the spawn (a package newer than the list, at 96 MB where the list had 16: it
started over the bound), `net.lan`'s private address check in the broker (five), the worker's private mark for a
listener, `log_pump` without the full view (`unit-plugin_broker`, and `fake_plugins_lan.py`'s whole log), the sealed
mark in `logread_full` (`unit-logread`), `log.read.full` taken from a sideload, 5555 in an entry and the second press
for `net.lan` (`unit-plugin_store`).  `net.lan`'s private check is made twice, in the broker and again by the worker:
broken in the broker alone, `fake_plugins_lan.py` still sees every outside address refused (by the worker), which is
what the second check is for.

Left: all of it on an Echo, and the 128 MB as what real plugins keep resident once that is measured there.

*A review of 7be9343 (2026-11-22), fixed the same day:* N1 `net.lan` reached the internet through the home's DNS
forwarder: ports 53 and 853 refused for `net.lan`, `net.udp` and `net.discover` (`PS_PORT_RELAY`: manifest, `"any"`,
broker, worker; the index's `DNS_PORTS`), and the docs say plainly that a device that relays can still carry data
further (the second press's screen too).  N2 one plugin's names starved every plugin's lookups and its slow connects
every plugin's requests: lookups counted per plugin with places kept for owners' servers, a worker per place, each
plugin's requests one at a time.  N3 `net.internet` reached the home network: public addresses only, after the
lookup too.  N4 `log.read.full` under the least index serial: held at every scan (4.5's combination; the tier stays).
N5 `fake_plugins_lan.py` asserts the broker refused a name for `net.lan` without a worker asked.  A plugin with `call`:
its lines and the broker's private.  P1 a stop for the engine's place counted as a crash; P2 the engine left without
a launcher slot for good; P3 its place taken while it was down between restarts (`places` counts it whenever it is
wanted, a start left without a slot asks again in 250 ms); P4 the engine's choice right after an install "not a value
it takes" (`settings_set` reads the list anew, the store first; `plugin_installed` tells the core before the start;
`HASSMIC_PLUGIN_INSTALLED_DELAY` for the test); P5 `MemTotal` rounded up to the next 128 MB; P6 the waiting plugins'
order, the engine's held place and the memory in the timers' round tested; P7 two lanes more than places.  Each
test proven by breaking its fix: `unit-plugin_broker` (P1 to P7, N1 to N3, `call`), `unit-plugin_store` (N1, N4),
`fake_plugins_lan.py` (N1, N3, N5, P4), the index's `make check` (N1).

*A review of 5aa32ee (2026-11-22), fixed the same day:* F1 `start()`'s three returns before the launcher had the
plugin (`program_open` -1: descriptors run out, the program renamed away; `socketpair`, `pipe2`) left a wanted
plugin `P_DEAD` with no `next_start`: nothing started it again, and the engine's entry, which `places` counts
whenever it is wanted, held a place and its memory from every plugin that waits, for good.  Now asked again after
1, 5, 30 s, then every `RETRY_MS` (`failn`, cleared at a spawn; a crash's back-off, never counted as one; the card
"restarting", why).  F2 `net.internet` "does not reach the home network" (SECURITY.md, the guide, the changelog):
the router's own public address answers from the LAN side (its admin page, hairpin NAT into a forwarded port);
reworded, never an address of the home network, the router's public address one on the internet like any other
(4.6 "As built" above).  And `fake_plugins_update.py`'s "self test's mark forgotten" after a push, which failed
once under `tests/run.py -j 8`: not the prep's time (the push answered in 31 s, `PREP_S` 30 s held, otatool waits
60) but the code: the stand-in engine, stopped at its twentieth word in ten minutes (`HEARD_BURST`, the test's
audio), started again 0.4 s after the push's forget and wrote `ready` back with its first `WAKE_READY` 2 s later;
a check that came after that failed.  On an Echo the same start between the forget and root's restart (a crash's
start again, the owner's switch) had the next hassmic's self test wait for a plugin it cannot run and the update
fall back.  `plugin.c` keeps the forgotten id (`forgot`, `rmu` over look and write, and over the unlink) until the
handoff failed (`plugin_selftest_keep`: ota.c and update.c when root refused it or nothing was stored, not where
root never answered, which may install it still: `ota_handoff` 1; the mark written again where the plugin listens)
or the process ends; the test switches the plugin off and on after
the push, so the start again comes every time, and pushes one root refuses.  Each proven by breaking its fix:
`unit-plugin_broker` (F1), `fake_plugins_update.py` (the mark).

**The community switch and the plugins' domain (2026-11-22): phase 3's leftovers, on the PC only.**  What 8's
phase 3 had left of hassmic's side: 2.7's `hassmic_plugin` domain and the community switch with its warning (2.7 "As
built" has both in detail).

| What | Where |
|---|---|
| The domain | `scripts/system/sepolicy-hassmic.rules` (two types of their own attribute, eighteen rules for the domain, three for its program's type); `install-system.sh` and `install-boot.sh` check the patched policy has it |
| Its program's type | `plugin.c` `program_label` (before each start where `READY` said "plugin": `fsetxattr` on the descriptor handed over; not started if it cannot be labelled) |
| What the domain may do, read out of each model's compiled policy | `tools/sepolicy-check.sh` (by hand: needs `firmware/`, magiskpolicy and qemu; `build/sepolicy-check/<model>/plugin.has`) |
| The PC's stand-in for the launcher's probe | `build/plugrun-host` with `HASSMIC_PLUGIN_DOMAIN=plugin` or `=hassmic` |
| The switch | `settings.c` `plugins_community` (an ordinary setting, exported, since the eighth round of 12), `plugin_store.c` `ps_community`, `ps_stage_check`, `ps_scan`'s `community_off`, `ps_engine_refused`; `plugin.c` `plugin_community` (the broker's thread rescans: `plugin_withdrawn_changed`), `plugin_community_off_now`; `main.c` the switch before the first scan (`settings_peek`), the wake engine's words |
| The page | `web/plugins.js` `plgCommunityRow`, `plgCommunity` (its dialog, then `POST /api/set`), `plgWallWarning`, `plgFix` (by `/api/plugins` `install_method`), the third kind's note, browse's `hmbrowse.find(..., community)` |

Decided while building it: the program's own file type, labelled by hassmic, rather than `execute` on
`system_data_file` (which the domain would then have been able to open and run anywhere on `/data`, given a hole in
the filter); the domain without `domain`'s attribute; the switch not an entity.  First built with the action button
for "on" (a wait of its own, `W_COMMUNITY`), not exported, and a first start's rule for Echos with community plugins
running; all three dropped before any release (12, eighth round): on and off from the page as any setting, exported
and copied, off unless set.

Tried: `make unit-plugin_store` (off: a file and a community listing refused, an official package taken, switched off
between the check and the press refused as the store writes, one installed refused with the switch's words and as
the engine, on again nothing refuses), `unit-plugin_broker` (the flag at once and the rest on the broker's thread, a
plugin from a file not started and its card's words, started again by the rescan, Home Assistant's list made anew
both ways), `tests/unit/web_browse_test.py` (community listings left out and counted), `tests/fake_plugins_community.py`
(the switch end to end: off by default, every refusal, on and off from an approved browser without a press and
refused from one that is not, exported and taken from an import and a preset, off stopping and keeping, a community
wake word engine, restarts, the state line for each domain), the other plugin tests with community plugins switched on in their `state/config` where they
install plugins from files, and the page in a real browser (Brave through puppeteer-core, 1280 and 390 px: the state
line per domain, the dialog's warning per domain and `INSTALL`, on, off; with the press it then took). The domain against the policies:
`tools/sepolicy-check.sh`, seven models. **Not tried: anything of the domain on an Echo.**

A review of it (2026-11-22, the same day): the domain's eighteenth rule, `read write getattr` of its own `fifo_file`
(a pipe the plugin makes itself; S1); the switch read at start from `state/config` alone, never a preset's, and the
first start's decision saved (S2; both gone with the eighth round of 12: a preset may set it now, and there is no
first start's rule); `tools/sepolicy-check.sh`'s two checks on who reaches the domain matching the attribute and the program's
type too, which they could not before (S3); "signals to itself" reworded: the domain is one type for every plugin,
and what keeps plugins apart is each slot's user and the filter (S4, 2.7 "As built", docs/SECURITY.md).

**Phase 6 on hassmic's side (2026-11-22), on the PC only.**  What 8's phase 6 asks of hassmic: a copy of each request
for a plugin that hears it (`request.audio`), its texts (`request.text`), the second press, the partner rule, the
index's rule.  hassmic ships no listener plugin, and the template gets no example of one (5.8: a plugin that hears
every request is not a placeholder to copy); `tests/plugins/testreq.c` proves the path.

| What | Where |
|---|---|
| The tap (4.12) | `main.c` `mic_out` (in `lead_in` and `send_out`), `req_open`/`req_begin`/`req_ending`/`req_end`, `barge_src`; `pipeline_start(from, source)`, `core_remote_wake`, `mic_stopped`, `mute_update`, `heard_end`, `quiet_cancel`, `core_error`, `core_link`, the timeout |
| The texts (3.6) | `proto_esphome.c` (STT end after `core_transcript`, TTS start), `proto_wyoming.c` (the transcript) -> `plugin_request_text` |
| API 0.5 (5.7) | `sdk/messages.def` (3 types, `REQSRC_*`, `REQEND_*`, `REQTEXT_*`, `REQ_SAMPLES_MAX`), `ps_caps_built` with `request.audio` and `request.text` |
| The broker (3.4 to 3.6) | `plugin.c` "a request's copy": `plugin_request_start/audio/end/text`, `rq_push`, `rq_close`, `rq_drain`, `bwake`; private lines (`log_lines`); the card's `requests`, `second`, the page's `hears` (`plugin_json`) |
| The second press (3.3, 5.4) | `plugin_store.c` `ps_second`, `ps_stage_second`, `PS_PRESSED2`, `PS_VOUCHED_PRESSED`, `PS_NEEDS_SECOND` in `ps_stage_install`; `web.c` `PL_SECOND`, `pl.first`, `pl_press.press`, `check_json` `second`, `/api/plugins/install` "second"; `web/plugins.js` `plgSecond`, `plgFinish`, `plgCarry` |
| The partner (3.3) | `plugin_store.c` `ps_engine_set`, `ps_partner` (in `ps_stage_check`), `ps_engine_refused`; `main.c` `core_wake_engine`; `plugin.c` `start`, `partner_check`, `plugin_wake_engine` |
| The page | `web/plugins.js`: the bold lines (`PLG_BOLD`), the warning for one not official, the cards' rows, the top's "Hears every request" |
| The SDK | the `request_start`, `request_audio`, `request_end`, `request_text` callbacks (a socket per request, read to its end before `request_end`); `HM_CAP_REQUEST_*`, `HM_REQ_*`, `HM_REQEND_*`, `HM_TEXT_*`; SDK version 7 |
| Tests | `tests/fake_plugins_request.py` with `tests/plugins/testreq.c`; `tests/fake_plugins_sync.py` (the voucher and the second press); `make unit-plugin_broker`, `unit-plugin_store`, `unit-plugmsg` (59 types), `unit-hmplugin`, `unit-plugfuzz` (a listener with a request around its packets); the index's `make check` (`CAPS_BY_API` 0.5, rule 48) |

Decided where the design left it open, each with its reason in the section it belongs to: a socket per request, not
one for good (3.5); `REQ_START`/`REQ_END` to `request.audio` only, `REQ_TEXT` with the last request's number (3.4);
`REQEND_MUTED`, a mute ending the copy (3.6); the second press's wait armed by the screen's ask, not by the first press,
and a voucher standing for the first press only, the install then "button" (3.3, 5.4); "community" for the partner
read as "not official", enforced at install, at the engine's choice and at start, the listener the one not started
(3.3); a non-official listener's warning of its own, 2.7's not being built (3.3); a listener's own log lines private
(3.6); Wyoming's transcript passed as the screen shows it, "stop" included (3.6); the index refuses the listing of a
community plugin with `request.audio` and `net.internet` (rule 48, stricter than any Echo, which has no
`net.internet` yet) and flags `request.audio` with any `net.*` on its page (5.3).

Found while building it, each fixed with the test that shows it: the first test's "never a block outside a run" read
"outside 0" in every end line as a failure, and the fake music source says "stopped" for a stop.

A review of b294fa0 (2026-11-22), each fixed with a test that fails without the fix: C1, a follow-up asked for before
a mute listened on the muted Echo (also for Home Assistant, before phase 6 too); C2, `-w remote` streamed through the
soft mute and its wake word began a copy; C3, the broker's lines about a listener were public; C4, a withdrawn engine
run anyway passed as vouched for; M1, the request socket held 2 s and a long replay's start was lost; M2, a voucher
that went stale between the presses was asked for again and again (`pl.first` kept); M3, Wyoming's `-w remote` began
no copy and its transcript went out under the last request's number; and `pl.second` set in the same hold of `lk` as
`PL_SECOND`.  Each in the section it belongs to (3.3, 3.5, 3.6).

A second review (of c4ace24, 2026-11-22): F1, the partner rule asked again by a new revocation list had no test
(`unit-plugin_broker` now: an official engine withdrawn and run anyway stops the listener beside it); F2, the
private mark of the broker's lines was a table by id that filled up (16 listeners installed and removed in one run,
the next one's lines public), now one place per entry; F3, under `-D` Home Assistant's wake word during a reply cut
the client's reply over Wyoming, now left to the client as the Echo's own under `-D` (3.6).

Left of phase 6, on an Echo: the copy on a Dot's mixer and a Show's AudioRecord (the same blocks as Home Assistant's,
measured only against the PC's fake), `-w remote` against a real Home Assistant, the capture loop's worst gap with a
listener that does not read (the PC's test has Home Assistant's audio complete and on time).  Not built: the two-Echo
and all-other-capabilities checks of 6 (6 "As built").

**Phase 5 on hassmic's side (2026-11-21), on the PC only.**  What 8's phase 5 asks of hassmic, built in its order:
the claim and `core_call` first, in a commit of its own (Drop In's tests unchanged), then the capability.  hassmic
ships no SIP plugin: #186's author ports his (4.11), and `tests/plugins/testcall.c` proves the path.

| What | Where |
|---|---|
| One call at a time (4.8) | `call.h` `call_claim`, `call_release`, `call_claimed`, `CALL_*` states and owners; `dropin.c` claims and lets go, refuses "busy in another Drop In" |
| The core's side (4.8) | `main.c` `core_call` (was `core_dropin`), `core_call_button`, `core_call_answer`, `core_call_hangup`, `core_call_status`, `core_call_listen`, `core_call_refusal`; `screen.c` the peer and the UI's Answer/End through them |
| The media without a codec (4.8) | `call.c` `call_begin(who, codec, ...)`, `CALL_CODEC_PCM`, `CALL_PCM_BYTES`, `PCM_FADE`; `make unit-call` (`tests/unit/call_test.c`) |
| API 0.4 (5.7) | `sdk/messages.def` (7 types, `ACTION`'s token, `HANGUP_*`, `RESULT_*`), `ps_caps_built` with `call` |
| The manifest (3.3) | `plugin_store.c` `actions_parse` (`calls`), `ps_cap_line` (the line, "and have it place a call"), `combination` (unchanged: wake sealed against it) |
| The broker (3.4 to 3.6) | `plugin.c` "calls": `pc`, `call_message`, `call_over`, `call_timers`, `token_new`, `token_take`, `rings_unanswered`, `teller`, `call_send`, `media_reader`, `plugin_call_button/answer/hangup`; `action_send`'s token; the card's `call` in `plugin_json`, `web/plugins.js` "Calls" |
| The SDK | `hm_action_token`, `hm_call_incoming`, `hm_call_out`, `hm_call_ringing_there`, `hm_call_live`, `hm_call_end`, `hm_call_audio`; the `call_answer`, `call_hangup`, `call_audio` callbacks; SDK version 6 |
| Tests | `tests/fake_plugins_call.py` with `tests/plugins/testcall.c`; `make unit-call`, `unit-plugin_broker`, `unit-plugin_store`, `unit-plugmsg`, `unit-hmplugin`, `unit-plugfuzz` (a phone seeded); the index's `make check` (`CAPS_BY_API` 0.4, `calls`) |

Decided where the design left it open, each with its reason in the section it belongs to: one socket pair for both
ways of a call (3.5); `CALL_LIVE` with a token, for the answer in Home Assistant or on the page (3.4, 3.6; taken
back 2026-11-22: only a person at the Echo answers, the field is not looked at); every
unanswered ring counts, however it ended (3.6); the token spent by its first use whatever then becomes of it, four
kept, cleared at a restart (3.6); a teller thread for the core, the claim let go after "idle" (4.8; Drop In's the
same way since 2026-11-22); the far side's
answer within 10 s of a person's, a call out taken within two minutes, the ring Drop In's 30 s (`plugin.c`, tests
`HASSMIC_PLUGIN_CALL_TIMES`); a missing PCM frame faded by half a frame (4.8); the Drop In refusal in its old words
(4.8); "stop" and "hang up" tell the plugin `HANGUP_SPOKEN`, the screen `HANGUP_SCREEN`; the screen's protocol keeps
its "dropin answer"/"dropin hangup" lines (any call now); the template gets no call example (5.8: #186's SIP plugin is
its author's to port, its flow is shown by `tests/plugins/testcall.c` on the C client, and the Rust client has no
actions to bring a token yet).

Found while building it, each fixed with the test that shows it: a wake word heard during a plugin's call while the
far side's tone played was ignored, as in a Drop In (by design: the far side talks), so the test's request during a
call keeps the far side quiet first; the screen's lines arrive after the band's binary rows on one line, and the
test reads them as text.

Left of phase 5, on an Echo: a plugin's call on a Dot's mixer (the Voip stream and micHfp as Drop In has them) and on
a Show's AudioTrack, the ring's call patterns and the screen's call view from a plugin's call, a real SIP plugin's
round trips against `LIVE_WAIT_MS`.

**Phase 4 on hassmic's side (2026-11-21), on the PC only.** What 8's phase 4 asks of hassmic, built in its order:
the seam first, in a commit of its own, then the capabilities.  hassmic ships no Lyrion plugin: the template's
"Tone" proves the path, and #198's author ports his.

| What | Where |
|---|---|
| The seam (4.7) | `core.h` `struct music_source`, `music_register`; `main.c` the table, `core_music`, `music_pause_all`, `music_unyield`, `music_button`, `music_skip`, `music_volume_changed`, `music_stop`; `sendspin.c`, `a2dp.c` (`a2dp_music_join`) register theirs; `HASSMIC_FAKE_SOURCE` |
| API 0.3 (5.7 "Decided") | `sdk/messages.def` (19 types), `ps_caps_built` with `state`, `net.udp`, `net.discover`, `audio.music`, `audio.volume`, `ha.entities`, `ha.actions` |
| Music (3.5, 4.7) | `plugin.c` the lanes (`MY_OTHER`, `MY_HELD`), `music_open_message`, `mstream_thread` (the paced drop; `HASSMIC_PLUGIN_MSTREAM_GAP_MS` for the test of its end), `music_tell`, `music_due`, `plugin_music_ha`, `vol_flush`; `audio.h` `plug_*` in `audio_mixer.c`, `audio_android.c`, `audio_file.c` (`HASSMIC_PLUG_MUSIC`) |
| The volume, the Echo's state | `plugin.c` `plugin_core_state`, `tell_echo`; `main.c` `tell_plugins` (state, mute, volume) |
| UDP, the search (4.6) | `plugin_store.c` `discover_parse` (`PS_DISCOVER_OWN_*`), `ps_udp_ports`; `plugin.c` `udp_message` (`IP_MULTICAST_LOOP`, `r_peer`), `private_only`, `udp_dest_ok` (`pnet_address_ok`); `lockdown.sh` `rules_in`, `IN_JUMP`, `rules_self`, `SELF_JUMP`, `wrong()` |
| Proposals (4.3) | `plugin.c` `PROPOSE`, `plugin_propose_take`; `web.c` `/api/plugins/<id>/propose`; `web/plugins.js` the card's row |
| Entities, actions (4.10) | `plugin_store.c` `entities_parse`, `actions_parse`, `hakeys_assign` (`state/plugins/.hakeys`), `ps_utf8_char`; `plugin.c` `ha_name`, `ha_object_id`, `plugin_entities_each`, `plugin_entity_*`, `plugin_action*`, `entity_message`, `ha_flush`, `ha_relist`; `tests/unit/ha_names_test.py`; `proto_esphome.c` `list_plugin`, `send_plugin_state`, `plugin_command`, the media player's pause and play; `web.c` `/api/plugins/<id>/action` |
| The SDK | `hm_music_open/state/close`, the `music_writable`, `music_pos`, `music_cmd` callbacks; `hm_volume_set` and `volume`; `state`; `hm_udp_open/peer/sendto`, `hm_broadcast`, `hm_propose`, `net`; `hm_entity_state`, `hm_event`, `entity_set`, `action`; `hm_watch` (descriptors the loop polls) |
| Tests | `tests/fake_plugins_music.py` with `tests/plugins/testmusic.c`; `make unit-plugin_broker`, `unit-plugin_store`, `unit-plugmsg`, `unit-hmplugin`, `unit-plugfuzz` (a music player seeded); `kiosk_test.py` (`hassmic_in`); the index's `make check` |

Decided where the design left it open, each with its reason in the section it belongs to: API 0.3 (5.7); the
music on a backend stream of its own beside Sendspin's (3.5); `button(int resume)` and `unyield` in the seam, a
source never leaving the table, four lanes (4.7); Home Assistant's media player as a music plugin's remote, pause and
play offered while one is listed (4.7); `UDP_OPEN`'s `kind` for both capabilities, a socket filter beside the
firewall, a chain of ours jumped to first in INPUT rather than rules in `keep`, each network's directed broadcast,
`net.udp`'s sends to private addresses (4.6); a proposal only for a row net.server names, in memory, the card's
"Use it"; the card's button only for an action without arguments (4.3); entities listed while switched on, not only
while running (4.10); `net.udp`'s smaller count as "less" (3.3).  The index's plain repetition of capabilities takes
fractions (a number entity's step) and one level more (an action's arguments).

Found while building it, each fixed with the test that shows it: the stream thread read a stale `errno` after a quiet
poll and ended every paused stream (`unit-plugin_broker`: "a paused player ... stays open"); the client polled a
stream opened in a tick only from the round after (`unit-hmplugin`); an interface given no broadcast address sent
the search nowhere (`fake_plugins_music.py`).

**Phase 2 on hassmic's side (2026-11-21), on the PC only.** What 8's phase 2 asks of hassmic, built; #193's
forwarder is left to a community author, and plugin authors start from the template repository (built the same day,
5.8).

| What | Where |
|---|---|
| API 0.2 (3.1: 0.x, the exact minor) | `sdk/messages.def` (7 types, `ERR_FAILED`, `ERR_UNSET`, the field type `bytes`), `ps_caps_built` with `storage`, `log.read`, `net.server` |
| Storage (4.4) | `plugin_store.c` `ps_store_*`, `ps_storage_*`; `plugin.c` `store_message` (the bucket) |
| The log (4.5) | `logread.c` (out of `web.c`: the page's tail and blanking, the plugins' filter and cursor), `log_private`/`log_sealed` at the writers; `plugin.c` `log_pump` |
| The first sockets (4.6) | `plugin_store.c` `net_parse`, `ps_cap_line`; `plugin.c` `connect_message`, `net_done`; `plugin_net.c` |
| The firewall's rule for plugin users | `scripts/device/lockdown.sh` `rules`; `tests/unit/kiosk_test.py` |
| The development flag (2.2) | `plugrun.h` version 3, `PLUGRUN_SPAWN_DEVELOP`; `plugrun.c` `filter`, `-T -D`; `sdk/hmplugin.c` `on_sigsys`; the card's switch |
| The SDK | `hm_store_get/put/del` (writes kept and written later), `hm_log_follow` and the `log` callback, `hm_connect`, a `tick` callback; PINGs answered while a request waits |
| The page | `web/plugins.js` `plgUses` (its data, the log, its server), `plgDevelop`; "What it keeps" in "Copy plugins" (5.4 "As built") |
| Tests | `tests/fake_plugins_net.py` with `tests/plugins/testnet.c`; `make unit-plugin_store`, `unit-plugin_broker`, `unit-plugmsg`, `unit-plugrun` (the flag); the index's `make check` (net.server's rules, `CAPS_BY_API`) |

Decided where the design left it open, each with its reason in the section it belongs to: the dev flag per plugin
on its card, a trap for every `EPERM` (2.2); `net.server`'s one form and `CONNECT` for UDP too (4.6); eight firewall
rules, not a range (4.6); the log's marks at the writers, root's lines by event words, IPv4 kept, the cursor's form
(4.5, 9.8); storage's keys, limits and bucket (4.4); storage and the log askable before `READY` (the SDK's
ready callback runs first; READY is still due in 5 s), sockets not (a review of 08399dc, N4: a connect may take
10 s); `log.read.full` left unbuilt (8 names `log.read` alone);
storage copied between Echos (5.4 named it; it waited for storage). The index (`hassmic-plugins`) learns what each
plugin API's hassmic has behind a capability (`CAPS_BY_API`: a version's `needs` against the builds of its own API)
and refuses `net.server` entries the Echo refuses (its cross-check against this tree's `plugin_store.c`: 4,000
manifests changed at random, none refused by the Echo only). The microWakeWord plugin's version 2 is its version 1
built for API 0.2.

**A review of 08399dc (2026-11-21)**, every finding fixed, each with the test that would have caught it:

| Finding | What now | Test |
|---|---|---|
| N1 a server row's default was "the server you entered" | no default in a row `net.server` names (install, index rule 22a); the broker connects to what the owner stored | `unit-plugin_store`, `fake_plugins_net.py` (both refusals), the index's `make check` |
| N2 the old server kept its socket after a change or a clear | a change of a server's row starts the plugin again | `fake_plugins_net.py` (the old server hears nothing after) |
| N3 the dialog's lines cut at 300 bytes | `PS_CAP_LINE` from the manifest's bounds at every caller; the check's answer sized from them | `unit-plugin_store` (net.server and 26 file lists whole, in an update's line too), `fake_plugins_net.py` (607 bytes in the dialog) |
| N4 a connect in ready stopped the plugin for READY; 10 s an address | no `CONNECT` before `READY`; one 10 s deadline a request; a dead start's requests dropped | `unit-plugin_broker`, `unit-hmplugin`, `fake_plugins_net.py` (ready, a server that never answers: 10.3 s) |
| N5 an address routed local passed | `RTM_GETROUTE`: unicast out of an interface that is not lo; the socket's own address checked; the tests' other hosts behind a veth pair (`tests/peerhost.py`) | `fake_plugins_net.py` (the Echo's own, a local route), `unit-plugin_broker` |
| N6 no EINTR retry; thirty a minute was sixty around its turn | the poll's time left on EINTR; the last thirty, a moving window | `unit-plugin_broker` |
| S1 private text after a line break passed | `log_private`/`log_sealed` (one line, one write); the reader's mark on a cut line's next piece; a mark anywhere drops the line | `unit-logread` (`logwriters_test.c`: the real writers and the reader with sentinels) |
| S2 unmarked private lines | every `%s` into the log audited; media addresses, a ring's timer name, Amazon's code and device name, the home's country, error texts marked | `fake_plugins_net.py` (hassmic driven by a stand-in Home Assistant, sentinels) |
| S3 a wake plugin's lines passed once it was gone | sealed where written | `unit-logread` |
| S4 a wake plugin's data into a copy without `wake`; storage in clear | the copy sealed both ways (`seal2`), its first line the source's `wake`; refused into a plugin without it (files' fields too, by the page's word) | `fake_plugins_net.py` |
| S5 storage's swap, young cursors, `LOG_FOLLOW`'s generation | `storage_settle`; no cursor into a text without a name; the starts checked after `bl` | `unit-plugin_store`, `unit-logread` |
| P1 the client dropped all past 32 kept messages | kept to 2 MB, then the plugin ends and says so; no `LOG` while a `CONNECT` waits | `unit-hmplugin`, `unit-plugin_broker` |
| P2 `hm_store_put` said "kept" of what it dropped | -1 `HM_ERR_BUSY`; a kept write refused later is said | `unit-hmplugin` |
| P4 a slow tick starved the control socket | no `continue`; the sockets read after every tick | `unit-hmplugin` |
| P5 the flag under an older launcher | no restart, the card says not available; 2.2 says "at once" | |
| P6 `ps_caps_built` per API | decided as built (5.9): beside packages are held to `combination` only | |
| F1 a `STORE_PUT` after `STOP` went unanswered | answered until the process is gone (`STORE_PUT`, `STORE_DEL`: `STOP_MS` to the launcher's `SIGTERM`, `KILL_MS` more to its `SIGKILL`) | `unit-plugin_broker` |
| F2 `hm_log_follow` handed on the old place's `LOG` | dropped, kept and arriving | `unit-hmplugin` |
| F3 the forwarder lost `LOG` while its `CONNECT` waited (it asked for the log again before every connect) | no `LOG` while a `CONNECT` of the plugin waits (P1's fix) | `unit-plugin_broker` |
| F4 the test vectors | `sdk/vectors/` | `unit-plugmsg` |

**A review of 0dabfa6 (2026-11-21)**, every finding fixed, each with the test that would have caught it:

| Finding | What now | Test |
|---|---|---|
| R1 the seal bypassed through a stopping process's `STORE_PUT`; a removed plugin's process writing into its reinstall | a process's storage and files only for its package as installed, none once removed (`store_mine`); the wipe waits until no process of the old package runs and is done before the new one starts (`ps_busy`, `ps_seal_settle`) | `unit-plugin_broker` (fails at 0dabfa6's rules) |
| R2 `STOP` while a request waited: never acted on | the broker answers every request of a stopping process `FAILED` (a worker's `CONNECT` too); the client fails the request and runs its stop | `unit-hmplugin`, `unit-plugin_broker` |
| R3 the client's read of 64 overtook what a request kept | what was kept first, always; nothing after a `STOP` | `unit-hmplugin` |
| R4 a write kept for later lost at a stop | written at once after `STOP`; what is kept written as `hm_run` ends | `unit-hmplugin` |
| R5 a refused `CONNECT` a line in the log, 200 a second | once in 10 s, the others counted | `unit-plugin_broker`, `fake_plugins_net.py` |
| R6 a process's generation repeated when an entry was taken anew | one generation, new at every spawn | `unit-plugin_broker` |
| R7 the lookup outside the 10 s | a thread of its own, waited for until the deadline only | `unit-plugin_broker` (`HASSMIC_PLUGIN_LOOKUP_DELAY`, `HASSMIC_PLUGIN_NET_MS`) |
| R8 `hm_log_follow` refused dropped the old place's `LOG` | put aside, handed on when refused | `unit-hmplugin` |
| R9 the client's 2 MB could fill with `LOG` | flow control: one `LOG` until its `OK` | `unit-plugin_broker`, `unit-hmplugin` |
| The vectors had no edges | `case empty`, `long`, `full`; `refuse` lines the decoder must refuse | `unit-plugmsg` |
| Q1 a made-up cursor read a sealed line from its middle | only from a line's start; decided: no MAC | `unit-logread` |
| Q2 a replayed storage GET reused a key stream | a nonce of the Echo's own in every sealed answer; the GET taken once over restarts; the browser's counter's last digits random | `fake_plugins_net.py` (a replay after a restart), `web_crypto_test.py` |
| Q3 a mark cut in two by the reader's 2048 bytes | the mark's first bytes go to the next piece | `unit-logread` (`logwriters_test.c`, fails on 0dabfa6's `clock.c`) |
| Q4 tests that did not test what they said | a real wake plugin's lines sealed (fails without `seal_note`), read again from the start after its removal; a control line after the sentinels; Wyoming's writers | `fake_plugins_net.py` |
| Q5 `log_marked` cut inside a character | the cut byte looked at | `unit-logread` |
| Q6 names still unmarked | marked (4.5 "As built"), then the class looked for again | |
| Q7 `wake=0` while a wipe was due; "from-wake" found in a file's bytes | `wake=1` while the mark stands; "from-wake" where it is said only | `fake_plugins_net.py` |

Decided in that round: no MAC over cursors (4.5); the page's word whether a copy is a wake plugin's, not MAC'd by
the source (5.4); `LOG` flow control by the client's `OK` (4.5); the covert channel of a detection's place in the
core's own lines named, not closed (3.3, SECURITY.md).

**A review of 7144548 and ca3a3b4 (2026-11-21)**, with what building the template repository found, each fixed with
the test that would have caught it:

| Finding | What now | Test |
|---|---|---|
| 1 `store_mine` refused an older package's process: a plain update lost the cursor written in the stop callback (F1's case); and a race with `plugin_installed`'s scan | only a removed plugin's process is refused (`revoked`, read under the store's lock); an older package's process writes until it is gone, and the seal's deferred wipe still covers an update without `wake` | `unit-plugin_broker` (a plain update keeps the cursor written at stop; the update without `wake` still wipes it, what the old process wrote on its way out too) |
| 2 `LOG` flow control not negotiated: a client of before it stalls after one `LOG`, silently | API 0.2 kept (never released); a `LOG` with no `OK` for 10 s said once in the log and on the card ("does not answer LOG: built on an SDK before flow control?", `log_unanswered`); PROTOCOL.md: the `OK` is required | `unit-plugin_broker` (9 s: nothing; 10 s: once, and on `/api/plugins`; answered: gone) |
| 3 Q7 half done: `/api/plugins` `wake` from the manifest alone, file copies without "from-wake" while a wipe was due | `wake` = `ps_sealed` (the manifest's `wake`, or the mark); the card's "wake word engine" row by the capability | `unit-plugin_broker` (true while the wipe is due, false after) |
| 4 two lines unmarked: a wake word ignored during a Drop In, a microWakeWord model that does not load (its path) | `wake_mark()` | |
| 5 `strstr(path, "/storage")` misrouted an id beginning with "storage": that GET not kept over restarts | the route read as its handler reads it (`sscanf`) | `fake_plugins_net.py` (`storagebox`: 404, then 401 after a restart) |
| 6 docs still had hassmic's own forwarder | 5.8, 8, 12, 13 reworded: #193 is a community author's, the template holds the examples, API 1.0 when a second plugin by anyone has shipped and been used | |
| 7 plugins killed without `STOP` when hassmic ends | on `SIGTERM` and at `main()`'s end, `STOP` to every plugin and up to 2 s for them (`plugin_shutdown`, `main.c` `on_term`); root's `main.sh` sends `SIGTERM` before its own restarts (`hm_term`), since init's stop and restart send `SIGKILL`; a crash, a power cut or `SIGKILL` still skip the stop (PROTOCOL.md, `hassmic_plugin.h`) | `fake_plugins_net.py` (`SIGTERM`: the stop callback's `STORE_PUT` lands, in 0.0 s), the template's `make test` (`SIGTERM` keeps the count, `SIGKILL` not) |
| 8 the template built | 5.7's copied list (`filter.def`, `LICENSE`), 5.8 and phase 2 say what it is; its plugins are sideloads until the community tier opens (the index lists community plugins: 2026-11-22, built by their own CI and attested since 2026-11-23, 5.3 "As built") | |

**A review of 655efd7 (2026-11-21):** the time a client waits on its own `CONNECT` (up to 10 s, as long as the
`LOG`'s) is not counted against its `OK`, or a correct forwarder whose server drops packets was called one from before
flow control (`unit-plugin_broker`); a start that was opening its program when `plugin_shutdown` came spawns nothing
(`ending` looked at again under `bl`: a plugin spawned after the shutdown's `STOP` held its wait for the whole 2 s);
three comments that still named a syslog plugin of hassmic's own.

Not seen on an Echo: the ARM filter with `SECCOMP_RET_TRAP`, the firewall's eight rules loaded (`xt_qtaguid`'s owner
match on the plugin users), a plugin's socket through the egress lock with and without group 3990, the bucket
against the real flash. To be run on an Echo (3b): `scripts/plugin-probe.sh` with `-D`, and a plugin with
`net.server` on a Dot 2 sending to a server on the LAN and to one outside it (the template's example, or hassmic's own
`testnet` stand-in).

**Built since (2026-11-19), all of it on the PC only:** of 2.6 hassmic's own crash marks, the CPU watchdog, "the
queue full for 5 s" and pings counted on the broker's own clock (`plugin.c`); the per-parameter "wider" of 3.3
and the author key's fingerprint for a sideload (`plugin_store.c`, `web.c`, `web/plugins.js`); the libFuzzer
target over the broker's dispatch (`tests/unit/plugfuzz.c`, `make unit-plugfuzz`: 60 s, 352,040 runs, nothing
found). Tests: `make unit-plugin_broker` (`plugin.c` itself with a stand-in plugin's entry and a clock the test
moves: each rule of the watchdog and of the marks both ways), `unit-plugin_store` (each kind of wider and narrower),
`tests/fake_plugins.py` (the same through the page and a real process: a plugin that stops reading, one that
spins, hassmic ended by SIGKILL and by SIGSEGV). To be seen on an Echo: that `/proc/<pid>/stat` of a plugin's
user can be read by hassmic's, and that the tombstone is still written after hassmic's handler.

**Built after that (2026-11-19), on the PC only:**

- *The manifest is read as written* (4.2; `plugin_store.c` `form`, `whole`, `tstr`, `jlist`, `setting_parse`).
  `json.c` is a lenient reader, made for Music Assistant's answers, and each of its kindnesses was a value the Echo
  took for another than the one signed and shown: `memory_mb` 4294967312 was 16 (and a `version` past 32 bits one
  number on the Echo and another on the PC), a text `"500"`, `null` or `1.5` where a whole number is meant was the
  default or cut, `"ab\bc"` was "abc", a key written `"\u006eame"` was not there, the second of two equal keys was
  not read, `"settings": {}` was no settings and an object under `models` or `options` was walked as a list, a
  default was any text cut to 64 bytes, `"optional": "no"` and `"wake": 0` were yes. Each refuses the package now,
  with its reason. The rules, in the order they are asked: the text is one object from its first byte, without a
  NUL, with nothing but white space after it (a line end at the end of the file is taken: editors leave one);
  every number in it is written as JSON writes one and fits a double; no text in it has an escape for a control
  character, for 0x7f or for half a surrogate pair; no object in it has a key twice or one written with an escape
  (all four also where the Echo does not read, the catalog: what is signed has one reading); a number field is
  digits with a sign at most, within 32 bits signed; `models` is a list of names, `settings`, `options`,
  `item.files`, `fields` are lists and their entries objects (options: texts or whole numbers); a default is of
  its row's kind (true/false; a whole number within min..max; one of the options; text or a host name of at most
  64 bytes; none on a file list); no option and no field twice; a field's min, max, step are numbers, min not over
  max; `optional` is true or false; a capability without parameters (`state`, `log.read`, `log.read.full`,
  `audio.music`, `audio.volume`, `call`, `wake`, `request.audio`, `request.text`) has the value `true`.
  `json.c` itself is unchanged: `mass.c` and the others read what a server sends, not what an author signed.
- *A capability this build has nothing behind is refused at install* (`ps_caps_built`: `files` and `wake`, the two
  `sdk/messages.def` has messages for): "this hassmic does not have "storage" yet". Before, such a manifest was
  installed, its lines accepted with the button, and the plugin given nothing. The build that brings a capability
  adds its bit. `storage` is among the refused: 3.3 lets a wake plugin have it, no code keeps anything for it yet.
- *The combination rules also hold at the start* (3.3: "the broker refuses to start a plugin whose manifest says
  otherwise"): `ps_scan` asks them, and `ps_caps_built`, of every installed manifest and keeps the reason
  (`struct ps_plugin` `refused`); `plugin.c` `start` starts none that has one, and its card says it as its detail.
  Such a plugin is still listed, so that it can be removed. This is for a package that reached `state/` past
  `ps_stage_check` (written there, or installed by a build with other rules).
- *The event entity "Plugin"* (5.4): `proto_esphome.c` `KEY_PLUGIN` (after the live ring's LED keys), listed while
  `plugins` is on, with all four types from the start. `core_plugin_event` (`main.c`; any thread, no lock held)
  hands a type to the protocol under `core_lock`, as the button's events go. `web.c` fires `installed` or
  `updated` (by whether a package this API could run was there before) once `plugin_installed` returned, and
  `removed` after `plugin_remove`; `synced` is for the sync to fire. An ESPHome event carries its type and nothing
  else: which plugin is in the Echo's log, not in the event. `plugins` became a feature in `settings.c`'s sense for
  it: its switch closes Home Assistant's links, so that the entity comes and goes.
- *The ring while an install waits* (4.3): stock's `wait` (three cyan LEDs going round, 88 ms a step, on donut's
  file), chosen because it is in every ring model's layer config (biscuit, radar, donut, cannoli, cupcake: layer 4,
  the listening ring's, cupcake 3), is no other slot's, loops lit (so it shows until unset) and moves where the
  login's `authenticated_setup_mode` breathes. Not `wakeword-change`, the other candidate: orange, as the setup
  spinner. `web.h`'s `attention` hook says what waits (1 a login or adb, 2 a plugin's install, 0 nothing);
  `main.c` `web_attention` keeps which pattern it set and unsets that one; set by `plugin_ask`, unset by the press
  (`web_approve`), the page's cancel and a revoked browser (`plugin_withdraw`) and the minute running out
  (`login_expire`, which `expiry` asks every second), and at start for what a run before left. A slot of its own,
  `plugin`, the last of `ring_slots` in `ring.c` and `ring_band.c` (ids are kept in designs: new slots go at the
  end); the band draws it (`P_PLUGIN`), the face says "Plugins: the settings page says how to use ..." (`src/ui/face.c`; the tenth read: the pattern is a note's too, which a press does not serve). Not seen on an Echo.

**The Echo's own fetch, and the push (2026-11-19), on the PC only** (from hassmic's releases; since 2026-11-20 from
the index, below: `plugin_assets.c`, `/api/plugins/official` and the GitHub stand-in are gone, the slot, the
push and hassmic's own fetch stay as written here):

| What | Where |
|---|---|
| GitHub's answer about a release, read | `src/hassmic/plugin_assets.c` (`pa_name`, `pa_tag`, `pa_parse`), on `json.c` |
| The look at the releases, the list kept an hour, the download | `src/hassmic/plugin_fetch.c` (`pf_look`, `pf_json`, `pf_candidates`, `pf_download`) |
| One GET for both `update.c` and this (5.6's `update_fetch`), its look as a clock for others | `update.c` `update_fetch`, `update_on_look` |
| The fetch as an install: one at a time with uploads, then an upload's install | `web.c` "packages the Echo brings itself": `bring_claim`, `bring_thread`, `bring_ok`, `check_json`; `POST`/`GET /api/plugins/fetch`, `/api/plugins/official` |
| The wake engine's plugin for this build's API, by itself | `web.c` `web_plugin_auto`, `plugin_store.c` `ps_other_api` (removed 2026-11-23 with versioned scopes: 3.1, 5.9); `main.c` `core_wake_plugin` (stays: which plugin the engine is) |
| A package pushed after a bundle | `ota.c` "HMOTA-PLUG1", `web.c` `web_plugin_push`, `scripts/otatool.py plugin` (and the C `otatool`), `scripts/ota-push.sh --plugin` |
| The page | `web/plugins.js` `plgOfficial`, `plgFetch`, `plgConfirm` (the dialog, shared with "Install from file") |

- *One install at a time, whoever brings the package.* There is one staging file, so the slot `web.c` kept for
  "a press is waited for" is also taken while the Echo fetches (`PL_FETCHING`): nothing is uploaded or checked
  meanwhile, and nothing is fetched while a press is waited for. A fetch is its browser's as an upload is; its
  "Cancel" tells the thread, which lets go of the download (libcurl's progress call) and of the staging file.
  hassmic's own (the wake engine's, a push) also stand back for a browser that checked a package in the last two
  minutes or is uploading one.
- *The push needs the update key and the release key's listing*, each for its own question (`ota.c` says which):
  5.9's "the port's challenge is the update key's, the package's signature the release key's". The challenge is
  over the package's digest, and the package is sent only after the Echo said `SEND`: a refusal written while
  megabytes still arrive is lost with the connection's reset.
- *A start with the chosen wake plugin not installed for this API* read the engine setting as no value (a
  `plugin:<id>` was a value only while installed), listened on Amazon's engine without saying why and lost the
  choice at the next save. Found by the test of the fetch; `main.c` reads the choice as written first
  (`settings_peek_raw`).
- *For the tests*: `HASSMIC_PLUGIN_FRESH` (`<s>[:<s when forced>]`: how long the list is kept),
  `HASSMIC_PLUGIN_LEAST` (`<id>:<version>`: one more least version than the table's).

Tests: `make unit-plugin_assets` (235,210 checks: names and tags at their limits, an answer said in ways a reader
might take, cut at every length and changed 200,000 times at random under ASan and UBSan),
`tests/fake_plugins_fetch.py` (`plugins-fetch` in `tests/run.py`, 62 checks, with a stand-in GitHub: the list and
when GitHub is asked at all, fetch, button, install; an update without the button and one with it; each refusal
with its reason; cancel and the two orders of fetch and upload; the push both ways with both otatools; the wake
engine's plugin by itself, both ways, and not while updates are off).

Where this went another way than asked: the list is asked of GitHub when the Plugins page is opened only while
plugins are switched on (with them off, "Look" asks), which is less than "opening the page counts"; the channel's
releases are asked only where this version's own release does not have the plugin, so the list of an Echo whose
release has one plugin does not show another the channel's newest has; a version made for another API shows in
the list as the highest there is, and "Update" then takes the highest this build runs.

Tests of what came before: `make unit-plugin_store` (each rule at its limit and past it, what `ps_scan` says of a manifest
put into `state/`), `tests/fake_plugins_ha.py` (a second file, `plugins-ha` in `tests/run.py`: `fake_plugins.py`
alone takes about three of the five minutes a test has).

Where this went another way than asked or than 3.3 and 5.4 say: the reader's rules are held by `plugin_store.c`
on `json.c`'s tokens, not by a stricter `json.c`; a trailing line end is taken; `hassmic-plugins`' `manifest_check`
is now laxer than the Echo in the places PLAN.md lists, and its cross-check fails until it follows.

**The self test for what the owner chose, the second slot filled, the copy left for a start that can (2026-11-21, a
review of 913ec8d), on the PC only.** The review's scripts (`r1_selftest_launcher.py`, `r3_alias_pick.py`,
`r2_mark_noplugins.sh`) reproduced three of six; each fix has a check that fails against 8240eb0's build and passes
now (the second slot's there behind the copy at start that fails before it; the review's `r3` shows it alone). 2.6
and 5.9 "As built" have the rules.

| What | Where |
|---|---|
| The self test excuses the wake word plugin only where the owner chose that it does not run (plugins off, `PLUGINS=0`, the plugin switched off, withdrawn); the launcher missing or silent, the sandbox, a manifest, capability or acceptance the build does not read, a program not the one accepted, no package for this API hold the update as before 913ec8d (R1) | `plugin.c` `plugin_selftest_ok`, `conf_off` |
| A slot left empty takes a word of the saved pick that the plugin lists later (it checks its models one at a time): both words listen without a restart, for any wake word plugin (R3) | `main.c` `core_wake_plugin_changed` |
| The copy at start does nothing (no mark, no log, the marks and fields left) unless the official wake plugin is there and read; then the mark, and the marks stripped from then on (R2) | `plugin_store.c` `ps_mww_own` |
| No `key` on a card whose manifest was not read | `plugin.c` `plugin_json` |
| A threshold brought into the plugin's range before it is rounded: 0.995 is written 0.99 and the note names 0.995 | `web.c` `mww_field` |

- *Decisions beyond the review.* No package for this API is not counted as the owner's choice by itself: the two
  ways the owner chooses it ("Install it all the same", a push) drop the mark before root installs, and any other
  way to it (a package beside that went missing) is the update's failure. `PLUGINS=0` is told apart from a launcher
  that failed by a flag of its own, not by `why_none`'s words. The second slot is filled only from the saved pick and
  only where a slot is empty; filled whole, the pick's own order. A sideload's or a plugin's without `wake` marks are
  left in `state/mww` (not stripped) so that the official plugin installed later still gets them; nothing reads
  them meanwhile. Three known limits said in 5.9 "As built" in one line each (room beside past a ROLLBACK build's
  package, an unaccepted package beside after a crash, a fallback to the alias era without its fields).
- *Tests*: `tests/fake_plugins_update.py` (the self test with the launcher silent, the plugin's program changed, no
  package for this API: held; with the mark forgotten as "install it all the same" does: passes),
  `tests/fake_mww_migrate.py` (no plugin installed: nothing written or logged, the marks kept; the sideload's and
  the plugin without `wake`'s start: the marks kept, nothing marked done; a start without the plugin's package for
  this API: nothing copied, the fields kept, no `key` on its card, then the copy at the next start; both words
  listening after the copy at start without a restart, Home Assistant's two active; 0.995 written 0.99 and named).

**The members' handover as it holds (2026-11-21, a review of a6c5a00), on the PC only.** The review's scripts showed
a member with the network's key keeping another member's newer list out: by spending each ticket's 64 nonces as
it was told (T1), by a place of its own over the honest member's as that one arrives (E1, E3: for good, not "five
minutes at the most"), and by one arb message under the honest member's id with a counter ahead of it (C1: arb
drops that member's messages until a restart). The maintainer's decision: the handover is not made to hold
against a member with the key (it already disturbs arbitration, Drop In and ducking; the handover only helps
Echos without the internet); the guarantee is the site's daily look and the week's warning.

| What | Where |
|---|---|
| A ring of the last 64 nonces a ticket, the oldest dropped, no cap and no early ticket: spending them refuses nobody; a request sent again is refused while its nonce is in the ring (against non-members, who cannot make one) | `plugin_fetch.c` `TICKET_RING`, `struct ticket`, `pf_revoked_serve` |
| E1, E3, C1 said, not closed: a member with the key delays another's list for as long as it keeps at it, and silences a member on arb's channel by its counter; the bounds (budget, places) kept, against mistakes and non-members; "five minutes at the most" and R3's closing withdrawn | `plugin_fetch.c` (`MEMBERS_*`, `SENDERS` comments), `arb.c` `fresh`; docs/SECURITY.md, 5.6, GUIDE.md, CLAUDE.md |
| `.revoked-at`'s older form dated ahead of the clock: no word (it was clipped to an age of 0, "fresh") | `plugin_index.c` `kept_load` |

- *Tests*: `tests/fake_plugins_sync.py` (70 nonces of one ticket spent, all answered, an honest request after them
  answered; a request sent again refused while its nonce is in the ring, the newest and the oldest kept, one out
  of it answered: both checks fail against a6c5a00's `plugin_fetch.c`), `unit-plugin_index` (the older form ahead
  of the clock; fails against a6c5a00's `plugin_index.c`). The review's `repro_rev23.py` T1: Tgt takes Src's list
  while the member spends every ticket (70 of 70 answered).

**The move once, room after the install, a self test that waits for what runs (2026-11-21, a review of 61028a8; what of it is about another plugin API, room beside, `.api-since`, the prep and the builds kept beside, removed 2026-11-23 with versioned scopes, 3.1, 5.9), on
the PC only.** A review of the move's copy and of updates of another plugin API found ten things (its scripts
`r1` to `r5` reproduced five on the PC build, and none reproduces now); each fix has a check that fails against
a6c5a00's build and passes now, but for two: the prep's lock against a wall clock that steps (a PC test cannot step
it) and the dialog's words. 5.9 "As built" has the rules.

| What | Where |
|---|---|
| The alias era's copy at start once (`state/plugins/.mww-own`), only to the official plugin with `wake`, never over a model it has; the files and the tuned fields (each through the field check), never `checked` or `title`: the plugin checks and names each copy (R1, R5) | `plugin_store.c` `ps_mww_own`; `plugin.c` `plugin_start` (after the first scan) |
| An artifact copy `mww:<id>` lists, reads and keeps the model's two files only | `artifacts.c` `mww_file`, `mww_only`, `one_json`, `art_read`, `art_commit` |
| Room beside made by the store's install, after the new package is in place, for hassmic's own claim only; a held, refused or failed build leaves the old package (R3) | `plugin_store.c` `ps_stage_room`, `ps_stage_check`, `ps_stage_install`; `web.c` `keep_beside` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| The self test waits for the wake word plugin only where it runs under the new build (plugins on, switched on, accepted, a package it runs, not refused) (R2) | `plugin.c` `plugin_selftest_ok` |
| The threshold compared at its two decimals: 0.99 is the plugin's own top (R4) | `web.c` `mww_field` |
| The prep's one lock asked for against the monotonic clock | `web.c` `web_plugins_prepare` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| `.api-since` with the boot's id and clock: a restart within a boot loses nothing, a reboot an hour at most | `plugin_store.c` `since_load`, `boot_id`, `ps_api_age` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| "Copy the models again": a model the plugin has keeps its threshold and window (`kept`); the dialog says what it does while the plugin listens; a plugin not the official one named with its author key's fingerprint | `web.c` `mww_to_plugin`, `mww_migrate`; `plugin.c` `plugin_json` (`key`); `app.js` `mwwMigrate`, `renderMove` |
| A failed handoff clears what was said of an install under way | `update.c` `install` |
| "switch" while the plugin is the engine already: refused, its pick untouched | `web.c` `mww_migrate` |

- *Decisions beyond the review.* The copy at start is marked done in `state/plugins/` (not in the plugin's folder:
  a Remove and a new install would have run it again) and only when nothing is left to try; the marks still go from
  `state/mww` at every start. Only the official tier: a sideload of the alias era gets its models back with the
  move's "any", at the built-in engine's threshold and window; what was tuned on its card is not carried over (since
  a review of 913ec8d it stays in `state/mww` for the official plugin; until then that start stripped it). The room is made after the new package is in place, not before it, so no failure of the
  write itself costs the old one; `accepted` is read again after the delete. The running time counts the time
  hassmic was down within a boot (the Echo ran; a fallback's lateness is the Echo's time) rather than writing at a
  clean stop: hassmic ends by init's `SIGKILL` often enough that no clean path can be relied on (root's own restarts
  send `SIGTERM` first since a review of 7144548; a crash or a power cut never do), and the boot clock makes every restart exact
  without a write. A re-copy keeps the plugin's own fields rather than taking the built-in engine's again: the
  owner tunes the plugin's copy after the move; a model whose files differ still gets the built-in engine's files.
- *Tests*: `tests/fake_mww_migrate.py` (a sideload under the id: nothing at start; the copy at start unchecked, then
  checked and named by the plugin, fields tuned, an unknown field left out; an artifact copy with the marks: two
  files kept; a fallback's marks and a new model at the next start: nothing copied again; the official plugin
  without `wake`: nothing; copy again while the plugin listens: `kept`, 0.99 not adjusted; "switch" refused),
  `tests/fake_plugins_update.py` (a wider build for 0.7: held, 0.0 kept, then a good one: installed, 0.0 deleted;
  root refusing: no note left; the self test with plugins off and with the plugin switched off),
  `make unit-plugin_store` (room never from a newer API, only for the claim, nothing deleted by the check or by a
  build that asks for the button, deleted once installed; `.api-since` over restarts within a boot, a day of
  restarts every 10 minutes, a reboot).

**The members' handover bounded in all, a request once, the floor that never falls (2026-11-21, a review of
6f39dd8), on the PC only.** The review reproduced R1 to R5 on the PC build (its `repro_members.py`); each fix has a
check that fails against 61028a8's build (or with the fix taken out) and passes now.

| What | Where |
|---|---|
| The floor line never falls: a list taken without a clock keeps the line the clock had raised (R4) | `plugin_index.c` `pi_revoked_take` |
| Fetches from members bounded in all (four, then one every 20 s) and their log lines (five, then one every 10 min, the rest counted) on top of the 10 min per place (R2: 496 fetches and lines in 5 s) | `plugin_fetch.c` `MEMBERS_BURST`, `MEMBERS_EVERY`, `budget_at`, `sender_ready`, `mlog`, `mlog_upkeep`; `HASSMIC_PLUGIN_MEMBER=<ms>[:<burst>:<every ms>]` |
| A place per id, source address and port: a message under a known id from elsewhere never moves it (R3; not closed against a member with the key, which puts a place of its own over the honest one's: the next review, above); a full table gives up the place that began to wait last, not the one heard last | `plugin_fetch.c` `pf_message` |
| A request for the list once: a ticket told in the announcement (`M_REVOKED` now 19 bytes), the asker's nonce taken once per ticket (since the next review a ring of the last 64), a ticket for two hours (R5); the announcement shows serial and port to the LAN, said (R1) | `plugin_fetch.c` `ticket_new`, `tell`, `member_mac` ("hassmic plugin revoked 2"), `pf_revoked_serve`, `member_fetch`; `web.c` `X-HM-Ticket` |
| A member's newer list is word as of its serial, not now | `plugin_index.c` `word` |
| `.revoked-at`'s older form without a clock: no word known | `plugin_index.c` `kept_load` |
| The index's exception for a previous site without `revoked_serial` only under the time the tool began naming it | `hassmic-plugins` `tools/index.py` `OLD_FORM_BEFORE`, `verify` |

- *Decisions beyond the review.* Freshness by a ticket in the announcement rather than a time in the nonce: the
  Echos' clocks need not agree or be set (a Wyoming Echo never has one); the ticket costs 8 bytes of the message
  and two small tables. The announcement stays readable (encrypting it would hide only what the settings port's
  404 already hides, and nothing else). Places keyed by address and port as well as id ("keep both", bounded by the
  16 places and the budget) rather than the first address per id: a spoofer who told first would otherwise hold the
  honest member's id for good. The format and the key's label changed outright: no build with 6f39dd8 was
  published. The index's exception is bounded by the tool's own change time (a3cbbc5's commit), not by state.
- *Tests*: `unit-plugin_index` (the line kept over a clockless take, then a list older than the vouched one refused
  with the clock behind; a member's archive of old lists, one with and one without a clock; the older form without
  a clock), `tests/fake_plugins_sync.py` (the announcement read without the key; a request again and an unknown
  ticket refused; 496 messages under new ids: 9 fetches, 5 log lines, the count said with the honest list's line;
  a member sending under an honest member's id from another port), the index's `make check` (an old-form previous
  site newer than the change refused).

**The move copies, and an update with plugins says more (2026-11-21, a review of the move; what of it is about updates of another plugin API removed 2026-11-23 with versioned scopes, 3.1, 5.9), on the PC only.** A
review of the day's move and of updates of another plugin API found eleven things; each is fixed with a check that
fails against that build (387b298, with 6f39dd8 over it) and passes now. 5.9 "As built" has the rules.

| What | Where |
|---|---|
| Two stores: the plugin's list `models` is its own folder (no alias to `state/mww`); the move COPIES each model there, byte for byte, nothing of the plugin's written into `state/mww`; a model deleted, uploaded anew or replaced on either side leaves the other's alone | `plugin_store.c` `list_dir`; `web.c` `mww_to_plugin` |
| An Echo whose plugin read `state/mww` under a build before: its own copies at the next start (the models it had checked), the marks out of `state/mww` (once; nothing for a plugin installed later) | `plugin_store.c` `ps_mww_own`; `plugin.c` `plugin_start` |
| The engine switched only from the built-in microWakeWord; beside another engine the models are copied and the switch offered as its own step ("switch", which copies nothing) | `web.c` `mww_migrate` (`offer`, `switched`), `mww_words_for_plugin`; `app.js` `mwwMigrate`, `mwwSwitch`, `renderMove` |
| Only the official plugin without a question: a microwakeword plugin of another tier named with its author and tier, the copy only with the owner's word ("any"); a fetched package not listed official refused by the page | `web.c` `mww_migrate`; `app.js` `mwwMigrate` |
| The fields checked before anything is written: a threshold or window outside the plugin's range brought to its nearest end and said (`adjusted`), staged with the files; kept only where the plugin checked it (a plugin that stopped meanwhile keeps nothing) | `web.c` `mww_field`, `mww_to_plugin`; `plugin_store.c` `ps_item_field_staged`, `ps_item_field_of`; `plugin.c` `plugin_item_checked_only` |
| A push's prep within its 30 s, all of it: the lock by a timed wait, each request to the index and each download bounded by what is left; out of time nothing fetched, said | `web.c` `web_plugins_prepare`, `keep_beside`; `plugin_fetch.c` `pf_look_until`, `get`; `ota.c` `handle` (the prep removed 2026-11-23 with versioned scopes: 3.1, 5.9; `pf_look_until` stays, `pf_look`'s own) |
| A push the wake word plugin would stop with drops its self-test mark, logged; the plugin's listening again does not write it back until the handoff failed | `ota.c` `handle` (`plugin_selftest_forget`, `plugin_selftest_keep`), `plugin.c` `ready_note` |
| A plugin switched off, and plugins not running, hold nothing back; what will not run under the update said in the log, Home Assistant's summary and the page | `web.c` `web_plugins_prepare` (`note`, removed 2026-11-23 with versioned scopes: 3.1, 5.9); `update.c` `install`, `struct update_state` `note`; `/api/state` `update.note`; `app.js` `HELP.online_updates.status` |
| The week by running time where the clock was never set, kept over restarts; the age never goes back | `plugin_store.c` `ps_api_age` (`.api-since` "<api> <epoch\|0> <age>"), `ps_obsolete_sweep(age, after)`; `main.c` `obsolete_thread` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| Room beside: at `PS_BESIDE` the package for the oldest API older than both the running one and the update's goes | `web.c` `keep_beside`; `plugin_store.c` `ps_api_older` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| The wipe when an update drops `wake`: its mark kept until nothing is left, tried again at each scan; microWakeWord's own copies go with it, `state/mww` never | `plugin_store.c` `rm_rf` (0 / -1), `seal_upkeep` |

- *Decisions beyond the review.* A threshold out of the plugin's range is brought into it and the model copied
  (not refused before anything is written): the built-in engine listened with it, and a model left behind would
  change what the Echo answers to after the move; the card says which and to what. The one-time copy at start takes
  the models that carry the plugin's `checked` mark in `state/mww`, whatever the plugin's tier: they are what it had
  and listened with, so it is given nothing new, and the marks go so that no plugin installed later is given
  anything from there. After a failed move the feature goes off again only where nothing got installed (the Echo
  as it was, the owner's other plugins included, which the dialog names before the button); with the plugin
  installed it stays on. A plugin switched off is fetched for like the others (it may be switched on again) but
  holds nothing back. The age of the week, once the clock's count is longer, is taken over into the running time
  (it never goes back), so a clock behind later does not shorten it. The Wake word card also offers "Copy the models
  again" once the plugin listens (models added to the built-in engine since). The reviewer's scripts and the move's
  screenshots were not in the scratchpad any more: the findings were reproduced again by the new checks against the
  build before (below), the screenshots made anew.
- *Tests*: `tests/fake_mww_migrate.py` (a sideload named microwakeword refused and named; the low threshold brought
  into range; the copies byte for byte and `state/mww` without the plugin's marks; delete, upload anew and a new
  version on either side; the copy beside Amazon's engine and "switch"; the restart with an alias era's state),
  `tests/fake_plugins_update.py` (a push during an online update's slow fetch answered in 31 s, the self test's mark
  forgotten; a plugin switched off and plugins off: installed, said; without the clock "0.1 0 <age>", room beside made
  by deleting 0.0's package), `make unit-plugin_store` (the week by running time over restarts, a clock behind, the
  clock set later; the wipe retried while a file will not go), `make unit-plugin_broker` (`plugin_item_checked_only`
  keeps nothing while the plugin does not run, the page's upload keeps it unchecked), the plugin's own `make test`
  (its models in its own list).

**Withdrawals that never go back, and the members' handover bounded (2026-11-21, a second review), on the PC
only.** A review of the withdrawal round reproduced each of these on the PC build; each is fixed with a check that
failed before (against that build) and passes now.

| What | Where |
|---|---|
| A revocation list never older than the one kept, clock or no clock; the floor line raised to the kept serial once the clock vouches for it (on the flash: the first question, any take, the daily thread); a kept serial the clock shows to be from the future yields to its line; the index the same with the clock set | `plugin_index.c` `pi_held`, `held`, `floor_flush`, `pi_revoked_take`, `pi_revoked_upkeep`; `plugin_fetch.c` `pf_look` (`.serial` "index <floor> <serial>") |
| A kept list under a raised least still withdraws; not current, said | `plugin_index.c` `kept_load`, `pi_revoked_current`; `plugin_fetch.c` `pf_revoked_stale`; `web.c` `/api/plugins` `revoked.least`; `web/plugins.js` `plgRevokedRow` |
| One fetch per member in 10 min whatever serial; a member's same list is no word; `.revoked-at` at most hourly | `plugin_fetch.c` `struct sender`, `pf_message`, `member_fetch`; `plugin_index.c` `PI_SITE`/`PI_MEMBER`, `word`, `at_due` |
| A place per member waiting (16), the one that told first asked first; 5 s per member fetch | `plugin_fetch.c` `sender_ready`, `MEMBER_S`, `SENDERS`; `HASSMIC_PLUGIN_MEMBER` for the tests |
| The warning's age over restarts (wall time and running time), never "fresh" from a clock behind, no word known warned of | `plugin_index.c` `age_now`, `.revoked-at` "word <wall> <age>"; `plugin_fetch.c` `pf_revoked_stale` |
| A site list older than the kept one: the look goes on, the page says so | `plugin_fetch.c` `pf_look` (`PI_OLDER`, `known.note`); `web/plugins.js` `renderBrowse` |
| The handover for members only (MAC by a key derived from the network's), anyone else answered as for no such path; no redirect for a member's list | `plugin_fetch.c` `member_mac`, `pf_revoked_serve`; `web.c` `GET /api/plugins/revoked`; `update.c` `update_fetch_lan` |
| A previous site of the tool before `revoked_serial` taken for the first publish | `hassmic-plugins` `tools/index.py` `verify(older)`, `published` |
| "Run it anyway"'s cancel ends only that plugin's wait | `web.c` `anyway_ask` |
| A package not the kept list's (a publish since): one more look, the new list's package | `plugin_fetch.c` `pf_download` (`PF_NOW`); `web.c` `keep_beside` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |

- *Decisions beyond the review.* A newer list from a member counts as word (it cannot be made at will, and an Echo
  without internet has no other); its same list does not. Members are told apart by their id in the network (the
  one their message names), not their address: the members of the PC's tests share one (since the review of
  6f39dd8: id, address and port, and fetches bounded in all, above). Without a clock the
  revocation list's kept serial is the floor (the review's rule), the index's floor line stays (its offers are held
  to the revocation list, which never goes back), and a far serial holds a clockless Echo's revocation list until
  its clock is set: only the index key signs one. Re-listing every package at each publish stays (keeping a
  package's first listing would let the build's least index serial turn every older official plugin into a file's);
  the Echo looks once more instead. The hourly announcement stays a broadcast and still shows the LAN that an Echo
  has plugins (docs/SECURITY.md says so); hiding it would mean sending it with plugins off too.
- *Tests*: `unit-plugin_index` (`pi_held`, the roll-back with the clock set later and its line raised on each path,
  a far serial held without a clock, a member's same list no word, `.revoked-at` hourly and over restarts, a clock
  behind, a kept list under the least), `tests/fake_plugins_fetch.py` (the site's older list beside a newer index, the
  least raised with the site out of reach, the age over restarts, no word known, the handover not for strangers, an
  install right after a publish, a cancel for another plugin), `tests/fake_plugins_sync.py` (test members on arb's
  channel: the MAC, a roll-back by a member with the clock set, 200 announcements one fetch, a member that holds
  every request, a redirect), the index's `make check` (an older previous site).

**The built-in microWakeWord deprecated, the owner's move to the plugin, and a hassmic update of another plugin API (that part removed 2026-11-23 with versioned scopes, 3.1, 5.9)
(2026-11-21), on the PC only.** The maintainer's decision of that day (5.9): the engine is not cut; it is
deprecated, the owner moves with one button, and the cut comes in a later, named release.

| What | Where |
|---|---|
| Deprecated where it is chosen: the engine's choice ("microWakeWord, built in", tag "Deprecated"), its help, the switch's dialog, the Wake word page's notice, the Overview tile | `web/app.js` `HELP.wake_engine`, `CHOICE_NAMES`, `mwwDeprecated`, `renderWake` |
| Home Assistant's problem sensor while the built-in engine is chosen | `proto_esphome.c` `KEY_MWW_DEPRECATED` ("Built-in microWakeWord is deprecated: move to its plugin") |
| The move: plugins on, the plugin from the index with the button, the models through the item path with their threshold and window, the engine switched last with the same words; switching back (revised the same day, "The move copies", further up: the models copied into the plugin's own list, the engine only from the built-in one, the official plugin only) | `web/app.js` `mwwMigrate`, `renderMove`; `web.c` `mww_migrate`, `mww_to_plugin` (`POST /api/mww/migrate`); `main.c` `core_wake_switch` |
| The plugin API in the bundle | `scripts/bundle.sh` (`plugin-api`), `ota.c` `ota_release_ok`, `ota_bundle_api` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| The plugins' builds for it before the bundle goes to root; held back, said, and "install it all the same" | `web.c` `web_plugins_prepare`, `keep_beside`; `plugin_fetch.c` `pf_for_api`; `update.c` `install`, `update_anyway`, `struct update_state` `held`/`stops`; `/api/update/anyway`, `/api/state` `update`; `app.js` `HELP.online_updates.status`; `plugin.c` `plugin_selftest_forget` (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| A push of another plugin API | `ota.c` `handle` (`PREP_S`, only while online updates are on, never held) |
| Packages for an older plugin API deleted a week after this API's self test | `plugin_store.c` `ps_api_passed` (since `ps_api_age`, "The move copies", further up), `ps_api_since`, `ps_obsolete_sweep` (`state/plugins/.api-since`); `main.c` `obsolete_thread` (`HASSMIC_PLUGIN_OBSOLETE=<s>:<ms>`) (removed 2026-11-23 with versioned scopes: 3.1, 5.9) |
| The wipe when an update drops `wake` | `plugin_store.c` `seal_upkeep` (`state/plugins/<id>/sealed`), `wider`'s line for it; `list_dir`'s alias only with `wake` (gone since, "The move copies", further up) |

- *Decisions beyond the brief.* Plugins off at the move: the page switches them on itself (named in the dialog
  before the button), and off again where nothing came of it, rather than sending the owner to another page.
  The move's answer is a signed 200 with "error" in it, so that the page reads what was copied also when it
  failed. A model the plugin refuses is left out and named; the move fails only when it is the word that listens
  now (the second word is then dropped). Every plugin whose build is missing holds an online update back, not
  only the wake word engine's: "say which would stop before installing" can only be said before by holding, and
  one click goes on. "Go on" also forgets that the wake plugin was ready (`ready`), else the new hassmic's self test
  would not pass for it and the update would not stay. A push is never held and fetches only while online updates
  are on. Only packages for an *older* API are deleted after the week; one for a newer API is what the next update
  (or a downgrade back) wants, and `PS_BESIDE` bounds them. The week counts from a time the clock vouches for
  (set from Home Assistant), noted at the first hourly look after the self test where it is set. An update that
  drops `wake` still asks for the button (it is a new install, 3.3), now saying that the files go; the wipe is at
  the scan that first sees it accepted and running, not at the install, so a package kept for another hassmic
  wipes nothing until that hassmic runs it.
- *Tests*: `tests/fake_mww_migrate.py` (`mww-migrate`: the real plugin's PC build from
  `../hassmic-plugin-microwakeword`, skipped where that tree is not; the deprecated sensor, the install that waits
  for the button and is cancelled, the move refused while the plugin is off, the move, the fields, the files
  unchanged, both words in their slots, a real "Alexa" heard by the plugin, the switch back), `tests/fake_plugins_update.py`
  (`plugins-update`: held back and said in Home Assistant and on the page, fetched and kept beside once the index has
  it, "install it all the same" and the self test's mark, a push, the package for 0.0 deleted after the "week"
  and the one for 0.2 kept), `make unit-plugin_store` (the week and its edges, the wipe and what it leaves).
- *Not built*: anything of the cut release; the update entity cannot say before the download which plugins would
  stop (the bundle says its API, and the release's JSON does not).

**Withdrawals reach every Echo, and what a review of the index found (2026-11-21), on the PC only.** A review of
the three commits of the day before reproduced eleven findings on the PC build; each is fixed with a check that
failed before, and 5.5 and 5.6 are built but for what is said below.

| What | Where |
|---|---|
| The index and its revocation list of one time: `revoked_serial` in `index.json`, taken only beside a list at least that new; the index leaves out what the list withdraws | `plugin_fetch.c` `pf_look` (the revocation list first, taken whatever becomes of the index); `hassmic-plugins` `tools/index.py` `build`, `withdrawn`, `verify` |
| Every bound in one place each, held to each other; a revocation list past one refused whole and said | `plugin_index.h` `PI_*`; `tools/index.py` `BOUNDS` (its `tests/test_index.py` reads the header); `pi_revoked_parse`, `revoked_check`, `build` |
| Official plugins withdrawn by the list; a lower tier than the version installed is wider | `plugin_store.c` `ps_stage_check` (`installed_tier`), `ps_scan`; `plugin_fetch.c` `withdrawn_v`; `tools/index.py` `revoked_check` |
| Serials are times, bounded by the clock or by the build's time + a day, the floor kept as made | `plugin_index.c` `pi_serial_ok`, `pi_floor`, `pi_clock` (main.c: `clock_synced`); `.serial`, `.revoked`'s "floor"; Makefile `BUILD_EPOCH` |
| A withdrawn wake word engine: down, and the words say what to do | `main.c` `core_wake_problem_held`, `plugin.c` `plugin_withdrawn_now` |
| `.listed` by this build's plugin API only | `plugin_fetch.c` `seen_raise` |
| The kept list in one file | `plugin_index.c` `kept_load`, `pi_revoked_take`, `pi_revoked_file` |
| Started again when a list names it no more | `plugin.c` `plugin_withdrawn_changed` (`struct plug` `withdrawn`) |
| Redirects to HTTPS only, for every fetch of update.c; `User-Agent: hassmic` for the index's requests | `update.c` `fetch` |
| The parsed index compact (texts in one pool, versions in one array) | `plugin_index.c` `pi_index_parse`; `unit-plugin_index` measures a look at lists at the bounds: 4.2 MB at the peak for 4096 plugins in 792 kB, 4.4 MB for 144 plugins of 16 versions with every text at its bound in 1 MB (the text, the signature's copy, the tokens and the parsed form; ~0.4 MB kept after), where the fixed arrays were 2.6 kB a plugin, 10 MB kept for 4096 |
| The least serials (5.5) | `keys/plugins.conf` `PLUGINS_LEAST_INDEX`, `PLUGINS_LEAST_REVOKED`, `scripts/plugins-least.sh`; `pi_least_index`, `pi_least_revoked`; a listing under the index's least: `ps_stage_check` |
| The daily look, the members' list (5.6) | `plugin_fetch.c` `pf_start`, `pf_message` (`M_REVOKED`), `member_fetch`; `main.c` `arb_message`; `web.c` `GET /api/plugins/revoked` |
| "Run it anyway" (5.6) | `web.c` W_ANYWAY (`anyway_ask`, `anyway_withdraw`, `web_approve`; "what waits for the action button" argues it), `plugin_store.c` `ps_anyway`, `anyway_serial`; `web/plugins.js` `plgAnyway` |
| The week's warning, the list's date on the page | `plugin_fetch.c` `pf_revoked_stale`, `plugin_index.c` `pi_revoked_age` (`.revoked-at`); `web.c` `/api/state` warnings, `/api/plugins` `revoked`; `web/plugins.js` `plgRevokedRow` |
| The index publishes when a release is gone | `tools/index.py` `fetch` (the published site's packages first, the listing taken off; a version not had is left out, warned of), `build` |
| Actions pinned by commit in the three repositories' workflows | `.github/workflows/*.yml` |

- *Decisions beyond the review.* The listing's serial: a listing under the build's least index serial counts as no
  listing at install (a sideload, so the button), and what is installed keeps its tier (`ps_scan` holds no
  serial): else raising the least at a release would turn every official plugin installed before into a sideload
  and stop one that reads the whole log. The serials' bound without a clock is the build's time + a day, as the
  tool's; the floor is kept as made, since recomputed with a later clock a far serial taken before the clock was
  set held every honest list a day ahead for good. The revocation list's serial became a time (it was a counter
  raised by one) so that the clock can bound it at all. The members' list by arb's message, not the page (5.6 As
  built says why). The least serials by hand, not in the release job (reproducible builds).
- *Index key rotation*: 5.2 says what a new key does and how it would be done; not built (one key is read).  Its
  first step built 2026-11-22 (below, "A second index key").
- *Tests*: `unit-plugin_index` (bounds, the one file, least serials, the clock, the peak), `unit-plugin_store` (a
  lower tier, a listing under the least, official withdrawn), `tests/fake_plugins_fetch.py` (the pair, a redirect,
  `User-Agent`, `.listed`, 2049 entries, started again, an official engine withdrawn and run anyway, a lower tier,
  serials from the future and under the least, the week's warning, the daily look and plugins off),
  `tests/fake_plugins_sync.py` (the list from Src to Tgt, which cannot reach the index; Out in another network
  hears nothing; Tgt with plugins off takes nothing), the index's `make check` (`revoked_serial`, withdrawn left out,
  every bound, `BOUNDS` against `plugin_index.h`, fetch from the published site and a release gone).
- *Not built*: a second index key read beside the first (5.2); community plugins in the index (5.3).  Both built
  2026-11-22 (below, "A second index key", "The index builds community plugins"); community plugins built in their
  own CI and attested since 2026-11-23 ("Community plugins built in their own CI").

**The index is the one place for plugins (2026-11-20), on the PC only.** The maintainer's word on what was built
the day before: "why would we have an index repo if it's not used? This does not belong in the eda repo". As
built until then the index published a signed list nobody read, and official plugins went through this
repository: `keys/official.conf`, `scripts/plugin-official.sh`, a listing by the release key added in hassmic's
release job, the package an asset of hassmic's release, and the Echo read GitHub's releases API
(`plugin_assets.c`): the page's list had only `<id>-<version>.hmp` names (no name, author, tier, description, API,
capabilities), a release held 32 packages and a 33rd was dropped without a word, and the installed plugins stood
under that list. Now:

| What | Where |
|---|---|
| The listing, by the index key: "hassmic plugin listing 2" with the tier and the index's serial | `hassmic-plugins` `tools/index.py build --key` (the package as the plugin's own release published it, which must carry no listing); `sdk/plugcheck.py` `listing`, `listing_read`, `list`, `show --index-pub`; `plugin_store.c` `listing_check` |
| The key and the address, built in | `keys/plugins.pub` (the index's `keys/index.pub`), `keys/plugins.conf`; Makefile `-DPLUGIN_INDEX`, `-DPLUGINS_PUB`; `plugin_index.c` `pi_key`, `pi_base` |
| The lists read | `plugin_index.c` `pi_signed`, `pi_index_parse`, `pi_revoked_parse` (`make unit-plugin_index`: every field wrong once, 4101 plugins, a list over 1 MB, 20,000 changed at random under ASan) |
| The look, the choice, the download | `plugin_fetch.c` `pf_look` (four files, signatures, serials), `pf_best`, `pf_download` (size and SHA-256), `pf_json` |
| Revocations | `plugin_index.c` `pi_revoked_take`, `pi_withdrawn` (kept in `state/plugins/.revoked.json`, `.sig`); `plugin_store.c` `ps_stage_check`, `ps_scan`; `plugin.c` `plugin_withdrawn_changed` |
| The page | `web/plugins.js` (the Plugins page: switch, cards with "Update", "Browse plugins", "Install from file…"; `buildBrowse`, `renderBrowse`, `hmbrowse`), `web/app.js` (`PAGES` `plugins/browse` with `sub`, `route()`), `/api/plugins/index` |
| Gone | `plugin_assets.c` and `unit-plugin_assets`, `keys/official.conf`, `scripts/plugin-official.sh`, `unit-plugin_official`, the release job's plugin step, `update_api_base`/`update_dl_base`/`update_version`, `plugcheck.py official`, `HASSMIC_RELEASE_PUB` for plugins |

- *The index key decides what is official.* That gives it what the release key's listing gave (5.4: an update
  that asks for nothing new without the button, sync between Echos, `log.read.full`) and no more: a first install
  waits for the button whatever the tier. docs/SECURITY.md "Plugins" says what holding it allows. The release key
  has no part in plugins; root's trust in it for bundles is as before.
- *The listing's text* names its tier and the index's serial ("serial <time of the index's commit>"), so a
  package's bytes change with every published index; the list names each package's size and SHA-256 as listed,
  and the Echo downloads by those. The serial is read and not held to anything yet (5.5).
- *The list in memory*: as many plugins as 1 MB of JSON holds (4096 kept, the highest 16 versions each), what was
  left out counted as `dropped` and said on the page, never silent. The answer to the page is as long as the list
  (`pf_json` allocates).
- *A list that does not read is refused whole*, and so is a look whose revocation list is not taken (signature,
  form, serial): offering plugins without the newest word on which were withdrawn would offer them again.
- *Pace and privacy as before*, the look now four requests to GitHub Pages that name no version and no plugin (their User-Agent did name it, `hassmic/<version>+<commit>`, until 2026-11-21: the block above).
  "Browse plugins" opened counts as the owner asking (the Plugins page itself only with plugins on).
- *Tests*: `tests/plugsite.py` builds the stand-in site with the index's own `tools/index.py` (`--arch x86_64`
  for the PC's test plugins) where `../hassmic-plugins` is there, and checks that its own builder writes the same
  bytes, which it uses where that tree is not (CI); `tests/fake_plugins_fetch.py` (79 checks),
  `fake_plugins_race.py` on it; `tests/unit/web_browse_test.py` (node) for the browse page's choices; the index
  repository's `make check` compiles `plugin_store.c` with `plugin_index.c`.
- *Where this went another way than asked*: none of the brief's points; beyond it, a community listing is taken
  by the Echo (`PS_COMMUNITY`) though the index makes none yet, revocations reach sideloads by id and author key,
  and `bring_ok` takes any listed package for hassmic's own fetch and a push (official or community; neither
  installs without the button what asks for more).
- *Not built* (then; all but the last built 2026-11-21, the block above): 5.5's least serials, 5.6's daily fetch of
  the revocation list, its passing on between Echos, "run it regardless", the 7-day warning; the browser never
  fetches from the index (5.4's first row).

**Sync's announcement (2026-11-20), on the PC only** (after a read of 0fc16f7; 5.4 "As built" has the rules):

| What | Where |
|---|---|
| The note: before the install, pending/done, never replaced, 16, the browser's key, told | `web.c` `syn_begin`, `syn_end`, `syn_load`, `syn_told`, `syn_json`; `state/plugins/.synced`; `HASSMIC_PLUGIN_NOTES` for tests |
| Who dismisses: another browser of the button's, else the action button | `web.c` `syn_ask`, `syn_dismiss`, `struct syn_ids`, `W_NOTE` (`syn_press`, `note_withdraw`, `web_approve`); `struct client` `button`, `state/web_button` (the ninth read: it was `vouched`, `state/web_vouched`) |
| The other members' notes on the pages that poll them | `web/app.js` `renderAttention` (`d.state.plugins_synced`), `synWaits` |
| Not older than seen listed; plugins off: the button | `plugin_fetch.c` `pf_seen`, `seen_raise`, `state/plugins/.listed`; `web.c` `sync_judge` 8 to 10 |
| `synced` held until Home Assistant's link | `main.c` `core_plugin_event`, `core_plugin_held_flush`, `core_plugin_told`; `proto_esphome.c` `plugin_event` (its count), on subscribe |
| No secret in clear; digests without the store's lock; the answer whole | `web.c` `/set`, `/api/plugins/sync` (`struct dyn`, `dapp`); `plugin_store.c` `ps_setting_secret`, `ps_item_digest` (by id) |
| Wording: "the voucher named" | `PS_HOW_SYNCED`, the log, `renderAttention`, `drawCopy` |

Tests: `tests/fake_plugins_sync.py`: who may dismiss and how (the bringer, a second vouched
browser, "keep", the press taking only what was there when armed, the button's browser, the bringer that is a
button's browser), the bound, "pending" lines over a restart, plugins off, `.listed`, the event held in the run and
over a restart and not said twice, a secret in clear, `at` 0. `tests/fake_plugins_fetch.py`: `.listed` written by a
look. Not tested: the page's drawing (node's syntax check only), `/api/plugins/sync` past its bound, a file
changing under a digest, a real power cut.

**Sync between Echos (2026-11-19), on the PC only:**

| What | Where |
|---|---|
| The voucher's MAC, a package's members in pieces, an item's digest | `plugin_store.c` `ps_voucher`, `ps_pkg_read`, `ps_item_digest`; `ps_changes.unsure` |
| Nonce, issue, the judgement, the install without the button, the note | `web.c` "sync": `acc_nonces`, `sync_judge`, `syn_note`, `syn_json`; `/api/plugins/accept/nonce`, `accept/issue`, `install` with `synced ...`, `sync`, `<id>/pkg/...`, `synced` |
| How a member settles rounds, for the target | `arb.c` `arb_member_mode` |
| The table, the carrying, the dialog for another Echo | `web/plugins.js` `drawCopy`, `plgCarry`, `plgCarrySettings`, `plgCarryItem`, `plgDialog`, `plgFinish`; `web/matrix.js` `hmmatrix.steps` |
| The note on every page | `web/app.js` `renderAttention` (`/api/state` `plugins_synced`) |

Where the code went another way than 5.4 or than asked (5.4 "As built" has each): the package through the
store's upload and the install slot instead of `artifacts.c` (the slot is what guards an install: one staging
file, one owner, one press; an artifact's staging knows none of that, and its install is root's); member-wise
with the page packing the tar, since the store keeps no `.hmp`; a failed voucher does not arm the press; one
event, `synced`; the note by node; Kiosk Satellite mode as a rule, not as the absence of a key; no community
listing, no revocation list, no `storage`, no second press to carry. The page copies settings row by row onto
rows of the same name and kind and leaves the others; `enabled` is not copied (a plugin installed by sync starts
as any fresh install does).

Tests: `tests/fake_plugins_sync.py` (`plugins-sync` in `tests/run.py`, three Echos, Src and Tgt in one
network whose key the test has too, Out in another): the whole way through the API as the page goes it; each
refusal ending in "button" with its reason and the button then working (a voucher twice, for another Echo,
browser, plugin, manifest; from outside the network; in the name of a member never heard and of the target
itself; a nonce run out; Kiosk Satellite mode at either end; a sideload with the source's honest voucher and with
one made with the network's key); a browser approved on the source only; updates both ways, another author key,
never back; the slot under a synced install. `make unit-plugin_store` (the voucher's MAC against a vector made in
Python, each field shown to count), `tests/unit/web_matrix_test.py` (`steps`). Looked at once in headless
Firefox against two `hassmic-host` (no Chromium on that machine): the table, a copy of package, settings and an
item, the note and its Dismiss.

**Reviewed** (2026-11-18, an independent read of the launcher, the filter's generator, the broker, the store and the
page's API against these pages; each finding was looked up before it was taken, all were):

| Finding | What changed |
|---|---|
| Deleting an item of a setting that is no file list removed `/<item>` as the daemon's user | `item_dir` fails for anything but a file list, every entry point stops there, and `rm_rf` removes nothing outside hassmic's state folder and follows no link |
| The press was not bound to the package the page listed: a second upload while it waited was installed by it | above: the digest kept, uploads and checks refused meanwhile, a wait withdrawn when its browser is revoked |
| A wake plugin's `WAKE_WORDS` and repeated `READY` drove work under `core_lock`, log lines and Home Assistant's reconnects without bound | above: other lists only, 2 s apart, 30 in ten minutes; `READY` once; refused detections logged once in 5 s |
| The broker span when a plugin closed its control socket | the end of file ends the plugin and takes the descriptor out of the poll |
| An item's title could reach Home Assistant as broken UTF-8 (a cut inside a character, or bytes that are none) | whole, well-formed characters only, 40 of them |
| `FILE_BAD`'s reason could carry a line break into the log | control characters become spaces |
| The launcher left SIGTERM ignored in a plugin, and started plugins after a failed self test when asked | both set back before the exec; no SPAWN after a failed self test |
| Every installed plugin held one of 8 slots; uploaded files counted against the 32 MB of programs | entries apart from slots; only programs counted |

A second read, of those fixes alone, found the paths, the slots, the decoder and the launcher's changes sound, and:

| Finding | What changed |
|---|---|
| A title of 64 bytes was stored whole and cut inside its last character where the core keeps 63 | it ends on a whole character within 63 bytes |
| A second approved browser could still check its own package between the first one's dialog and its "Go on", and the press installed that | the install is one browser's from its check to its end: only it may ask for the install, see the wait or cancel it |
| The page was told "not pressed in time" while the pressed install was still being written | a state of its own between the press and the end of the install |
| A wait that ran out left uploads refused until the page was looked at again | settled at every request |
| The staging file's name passed for a plugin's id | `.stage`: no id has a dot |
| The entry of a plugin removed while it ran was never free again; the count of other word lists began anew with every process; a stale reason replaced the exit's in the log | freed with its process; counted over restarts, a process's first list apart; cleared |
| `plugrun -T` never ended for a program that waits on its control socket, and every launcher's end wrote "not the protocol" | 30 s, then it is ended; the closed socket is the ordinary end |

A third read (2026-11-19), of the two new repositories and `plugcheck.py official`:

| Finding | What changed |
|---|---|
| A manifest whose `version` was a text with lines of its own ("7\\ntier official\\nmanifest <another>\\nserial 0") got a listing from `official` that an Echo took for that other manifest: the Echo compared the listing's beginning only | `plugcheck.py` signs nothing unless id and version are what the Echo takes (`named`), and the Echo takes a listing only when nothing but the serial follows its lines (`listing_check`, `unit-plugin_store`) |
| The index's "an official plugin is not withdrawn here" went by its entries alone: entry out and plugin withdrawn in one change passed | official also by what is published; what holds in the end is the Echo's rule (5.2), which comes with revocation |
| The published site was taken unsigned for the serial rules, and a failed fetch of it as "none yet" | it has to verify against the index key; only "no such branch" is none |
| Source and package addresses passed their prefix check with `..` or escapes in them | plain path parts only |
| The index repeated a manifest's `models`, `capabilities`, `version_name` unlooked at; took a field twice, direction marks, tars no Echo reads (pax, something after the end), a signature's hex with spaces | each refused |
| `official` read a packed tar whole before any size check, and listed a package without a licence | a plain tar within the package's size, member by member; refused |
| The plugin's release job: a key check that passed without `xxd`, a failed `show` lost in a pipe, the key left behind when a step failed | Python for the check, no pipe, removed by a trap |
| Both READMEs said the page names the author key's fingerprint for a sideload (5.4 wants it; the page does not yet), and the index's spoke of revocation as there | worded as it is |

Not changed: `LICENSE` is under no signature (the manifest names the licence, not the file).

A fourth read, of those fixes: the listing's fix holds on both sides (the Echo's block rebuilt under ASan: only
`<1 to 10 digits>\n` passes). Still open then and closed since: the revocation list's serial could be set to the
last there is (now at most 100 past the published one); an official plugin could be withdrawn in two pushes, entry
out first (a published official plugin keeps its entry, tier and key); a size in GNU's base-256 form, a manifest
in UTF-16, markup and numbers past any manifest's in what the index repeats; a traceback on a deeply nested
manifest; the index's key file left behind when its decoding failed. Left: the index does not run the Echo's own
manifest rules (an unknown capability, a size over its cap), so it can list a version no Echo installs.

A fifth read (2026-11-19), of the commit that brought the per-parameter updates, the crash marks and the watchdog;
its reviewer ran 31 pairs of an old and a new manifest through `ps_stage_check` (now in `unit-plugin_store`):

| Finding | What changed |
|---|---|
| An update was installed without the button with a file list renamed ("Wake word models" to "Home Assistant backups and SSH keys"), other suffixes, 256 times the size, 16 times the items, other fields, another catalog address | 3.3 turned round: only the same or less goes without the button (`wider`); the accepted section and its digest take in the settings whole |
| ... with a number row added or renamed ("Your bank card's PIN"), a secret retyped to a choice, a text row's help rewritten to ask for a password | every row, of every type, key for key |
| ... with `net.discover` 5353 made 53 (the test's pair is 353 since the review of 7be9343, which refuses 53), `net.lan` 443 made `1-65535`, 2 made `1.000...e9` (read through 40 bytes), 2 made -1 or 0, a list's entry repeated four times in two key orders | no number is a quantity unless a rule names it (`storage`, `files`, `max_kb`, `max_items`: whole 32-bit numbers from the parser); any other parameter the same, value for value; a list of grants may only lose entries, each of the old ones standing for one |
| ... with a host row under an unchanged `net.server` entry given a default, retyped to a choice of one, a port row's range opened | as above: rows are compared whole |
| `memory_mb` and `threads` changed unseen (5.4: "and are shown") | a line each in the check's answer (`limit`), no button |
| Any crash in the core's wake path named the wake plugin (the mark was set while the broker called the core), and the next start refused that plugin for the run: no wake word until someone found the page | `core_begin` before the broker calls into the core; the wake engine's plugin, named, waits `RETRY_MS` and is started |
| Any hassmic crash counted towards "three: all plugins off" | only a mark that names a plugin counts (2.6) |
| The CPU watchdog read `/proc/<pid>/stat` of another user, which Android hides | the launcher reads it when asked (`PLUGRUN_USAGE`/`USED`, version 2); hassmic decides |
| An installed manifest this build does not read had no card and could not be removed | listed as a plugin that cannot run, under its folder's name, with the reader's reason; Remove works |
| `api` took " 0.1", "+0.1", "00.1" | digits, a dot, digits |
| A text that is not UTF-8 passed | the whole file is UTF-8, or refused |
| One "nothing waits any more" from the page took a phone's setup pattern off the ring, and the other way round | `main.c` keeps what each of the two asked for and shows the highest |
| (from the index's cross-check) a number longer than 39 characters was read by its beginning; the byte 0x7f passed where its escape did not; a host default of "", "-" or "..." passed, and `-0` as an option or a default | each refused |

Where that went another way than asked: the entries of a list of grants and the settings' rows keep their order
(an update that only sorts them asks for the button: order is not known to mean nothing); removing a key *inside*
a row or an entry asks (only whole rows, entries and capabilities may go); `USAGE` is asked for, not sent every
10 s (2.2 says why); the wake engine's plugin named by a mark waits the full 5 minutes, not the ladder's 1, 5 and
30 s (it is hassmic that fell, and each try may cost a restart); a manifest the reader refuses is listed with what
little is known of it: its folder's name, no capabilities, no settings.

Tests of it: `make unit-plugin_store` (the reviewer's 31 pairs, 42 of the rule's own, and 3000
manifests with one value of their capabilities or settings changed at random, of which 1763 were still manifests
and 181 went without the button, each a removal or a smaller quantity the rule names), `unit-plugin_broker` (56:
the launcher's numbers both ways, a mark from a fault in the core's wake path, three marks that name nobody, the
wake engine's plugin held and started), `unit-plugrun` (69: `USED` for a child that waits and one that burns CPU
in a second thread), `tests/fake_plugins.py` (the same through the page and real processes).

Two more the tests found on the way: the engine setting was read at start before the plugins installed were known,
so "plugin:<id>" was no value yet and the Echo started on Amazon's engine and switched over a moment later; and
the broker's thread could list the wake words anew while `main()` was still loading them (`wake_set_up`). A model
the owner adds no longer becomes the wake word by sorting ahead of the one that listens.

A sixth read (2026-11-19), of the two commits that brought the manifest's reading and the fifth read's fixes. It
found no update that gains anything *inside* the accepted section without the button; what follows sat outside
it, or is state and robustness:

| Finding | What changed |
|---|---|
| `description` and `source` were in neither the digest nor `wider`: an update could make a card say "Type your Home Assistant password into Port", or send "Source and issues" to another site, button-free | both are in the accepted section (digest form 3) and compared: "Changed: What it says about itself: ..." with the new text, "Changed: Its "Source and issues" link opens ..." with the new address. `version_name` and `license` stay outside, free to change, and the page shows them only as a label beside the version's number and after "Licence" (3.3) |
| A package for a plugin API this build does not speak: installed as a first install with no card and no Remove; as an "update" without the button it rewrote `accepted`, and the running package then counted as not accepted and stopped (the wake engine's plugin: no wake word); "never back" was per API folder | one rule (5.4): only an addition beside an installed, accepted package for this build's API, never a first install; what was accepted is per package (`accepted`: `digest` + `api`, `beside <api> <digest>`), the running one's line untouched; "never back" over all of a plugin's packages; two beside at most; the card names each and deletes it (`ps_package_drop`, `POST /api/plugins/<id>/drop`); a plugin here for other APIs only, and an empty folder, are cards that cannot run, removable |
| A stored value outlived its row: a secret `tok`, its row dropped by one update and a text row `tok` "Nickname" added by a later one, was that row's value, shown on the page; a number or a choice inherited a text never held against its limits | values are stored with their row's type (`<row>:<type>=<value>`) and read only by a row of that type that takes them now; an install drops what no row of the new version answers to, every scan what a row would not take (4.1) |
| A crash in what the core does with a plugin's report was neither named nor counted: for the wake engine's plugin, a restart loop with nothing to slow it | a mark of its own kind (`core <id>`), kept with its time; the third within 10 minutes holds that plugin's start for the 5 minute retry, each time anew; never counted against the plugins, never a refusal (2.6) |
| `ps_scan` waited for ever on a FIFO named `manifest.json`, under the store's lock | every file the store reads from `state/` is opened without waiting and only if it is a regular file (`open_reg`, `ps_fopen`); the crash marks' files too |
| "An entry less is less" held for every form a list capability might take (`[80, 443]` to `[]`, an entry that restricts taken out, a `setting:<row>` whose row is gone); unreachable, in the path that decides | no generic rule for lists: a parameter is the same, or the capability is gone, or the owner is asked; a capability's own rule has its place marked in `wider`. A capability that names a row the manifest lacks is refused by the parser (3.3), so a row a kept capability names cannot go |
| `max_items` was left out of the comparison in every row, not only a file list's | only where it means something |
| The "Plugin" event said `updated` over a card that was only listed (an unreadable manifest), and `installed` twice for a plugin here for another API | `installed` unless a package this build runs was here and accepted |
| "Never back" was skipped when the installed manifest did not parse | the version its listing names, else the button and why |
| An id could end in `-` or have `--`: `abc--1.hmp` was version 1 of `abc-` | refused (`ps_id_ok`) |
| A reason cut an author's key at 24 bytes, inside a character, and went to the page | cut on a whole character; so is every reason and every line of the dialog that had to be cut, and a tar member's name is shown as ASCII |
| A host row took "", "-" and "..." from the owner where a default had to be a host name | one rule (`value_ok`, `host_name_ok`); nothing typed is the row unset |
| With 2 plugins running the wake engine's plugin was refused a start, with no retry | it always has a place: the newest of the others is stopped for it (2.6) |
| A branch in `form()` that could not be taken (a number over 39 characters was refused before it) | gone |

Where that went another way than asked: a package for another API *is* taken from the page's "Install from
file" (as an addition, with a dialog of its own), because an Echo without internet has no other way to hold the
next hassmic's package before the update, and what `update.c` is to do for the others (5.9) is not built; a row
that a kept capability names is not "asked for" when it goes but cannot go at all (the parser refuses that
manifest); the hold-back for marks of the second kind is for any plugin they name, not the wake engine's alone
(an ordinary one loops the same way, only slower); a stored value that its row would not take is ignored on
read and taken away at the next scan or install, not in the read itself (which runs under the page's every
look); values stored by a build before this one have no type, and are taken once as their row's type of then;
a fault on another thread than the broker's, in work a plugin's report set off, still names nobody; with no
listing and an unreadable manifest an older version can come in with the button (nothing says it is older, and
the owner is told so); `sdk/plugcheck.py`'s id rule and `hassmic-plugins`' Python rules are laxer than the Echo
now (ids ending in `-` or with `--`, a capability naming a row the manifest lacks): they follow separately.

Tests of it: `make unit-plugin_store` (195 checks: the reviewer's 31 pairs, 54 of the rule's own, 3000 manifests
with one value of their capabilities or settings changed at random, 1777 still manifests and 182 without the
button, and 400 with one of the manifest's own four texts changed, 310 still manifests and 147 without the
button, each of those a version label or a licence name; packages for other APIs through every step above; stored
values against rows that go, come back and change kind; FIFOs in place of each file), `unit-plugin_broker` (74:
marks of the second kind from a detection and from a list of words, the third held and started, the places),
`unit-plugin_assets` (235,164), `tests/fake_plugins_ha.py` (the dialog's lines, the card of a package kept and of
one that cannot run, the secret through the page), `tests/fake_plugins_fetch.py` (the state a hassmic update
leaves, made as that update makes it).

Found nothing: a way for hassmic to get more out of the launcher than a filtered process (descriptors of every
kind as the program, the order of the drops, the message parsing); the generator (155,060 calls against the table
in the reviewer's own interpreter); reads past the end in the tar, ELF, manifest and message parsing; lock order.

A seventh read (2026-11-19), of the commit that brought the Echo's own fetch (7910430), after the sixth read's
fixes. One finding was a hole, confirmed by running it (6 of 6 on the PC); the rest is pace, time and what a
neighbour on the LAN can cost:

| Finding | What changed |
|---|---|
| **The owner's press could install another browser's package, a sideload included.** `/api/plugins/install` read the slot, let go of the lock for its check of the file, and armed the press without looking again; a second approved browser's fetch in that window took the slot, its thread's end wrote "checked" over the wait, and the press, still armed for the first browser, installed whatever the slot held by then. The same root: hassmic's own fetch and a push wiped a slot that had just begun to wait; hassmic's own fetch took a dialog older than two minutes while another browser took it at once; the install without the button installed the digest it read last, not the one listed | the install slot is a state machine with one owner per transition (5.4 "One install at a time"): a generation per claim, every change under the lock and only from the claim and state it names (`pl_claim`, `pl_move`, `pl_close`); one table for who takes what from whom after how long (`pl_rules`); the press armed for browser, generation and listed digest, and installing only that (`plugin_accepted`, `ps_stage_install`'s expect); the install without the button installs the digest it listed; a browser cancels only its own; a checked package is its browser's for two minutes against everyone |
| The automatic fetch and the push could take an older official version than the one installed for the plugin API before | already closed by the sixth read's "never back" over all of a plugin's packages, which is the store's check and so theirs too; now with a test of both |
| Cancelling a fetch during its look left every browser an empty list that "failed: cancelled" | a cancelled look changes nothing that is known (`PF_CANCELLED`) |
| "At most once an hour" towards GitHub was the page's rule only: every look it asked for counted as a press (30 s), and after a failed look every page load asked again, three requests each | the Echo's rule (`plugin_fetch.c`): "look" (a page by itself) once an hour whatever came of it, a failure kept with reason and age; "again" (the button), "Install", "Update" every 30 s |
| A slow server that stayed above libcurl's 512 B/s held the slot for up to an hour (4 x 900 s); nobody could cancel hassmic's own fetch | one fetch as a whole has five minutes (`FETCH_MS`, argued in 5.4); hassmic's own gives way to a browser or a push at once; a cancel frees the slot at once while it only downloads; `stop` is heard in the data and the socket callbacks too, and by a look that waits for another look |
| The push port (older than plugins): a request line sent a byte at a time held it for an hour, 30 s a read, one connection at a time: no push, no adb over Wi-Fi, no `--plugin` meanwhile; `HMOTA-PUSH1` allocated 16 MB before any signature | times per step as a whole (the line 5 s, a challenge's answer 10 s, a body 10 s of grace and then 32 kB/s or better); a thread per connection, four at once; a plugin's package allocated after the challenge; one unsigned bundle in memory at a time (its signature is over all of it and comes before it: it cannot be checked sooner, and written to flash unchecked it would wear what the port is there to keep junk from) |
| libcurl's first use was guarded in `update.c` only; `https.c` and `davs.c` loaded it on their own | one loader with the lock in it (`fwcurl.c`), the library's own first init under it |
| `ps_scan` did not ask that the manifest in `pkg/<api>/` names that API | it does, as `pkg_manifest`: such a package is a card that cannot run |
| An update's check counted the program it replaces twice against the 32 MB | not that one (`programs_bytes`) |
| The push's answer cut a manifest's line at 100 bytes, possibly inside a character | at a character (`ps_cut`) |
| A fault of hassmic's while `main()` lists and loads the engine plugin's words named nobody (another thread than the broker's) | a thread says for which plugin it is in the core (`plugin_core_enter`/`_leave`: a place per thread, atomics only, read by the handler under its own thread id); the mark is then "core <id>" as from the broker's thread |
| "Source and issues" opens whatever https address the manifest gives | it is accepted text since the sixth read; the install's dialog now shows the address whole, as text; the card's link opens a new tab with `noopener noreferrer` (as before) and names the address in its title |
| `sdk/plugcheck.py` packed and signed ids ending in `-` or with `--`, which every Echo refuses; the Echo's reason for a bad id did not say the rule | one rule (`ID`), a test of both sides' ids (`unit-plugin_official`), the reason says it; both repositories beside this one synced |

Where that went another way than asked: nothing asserts the invariant at run time (`web.c` has no debug-only
checks to add one to; it is the comment at the head of the section and the test); the bundle of a push still is
in memory before its signature is checked, for the reason in the table; the push port now serves four
connections at once instead of one, which the finding did not ask for and its fix needs (a deadline alone still
lets a sender at 32 kB/s hold a single-file port for the nine minutes 16 MB take); a name lookup inside this
libcurl is the one thing `stop` does not interrupt, so a slot whose fetch sits there is taken from it half a
minute past the fetch's time instead; a browser that was told "overtaken" reads it for two minutes or until its
next upload, not for ever.

Tests of it: `tests/fake_plugins_race.py` (48 checks; `HASSMIC_PLUGIN_ASK_DELAY` widens the window between
`/install`'s check and its ask, `HASSMIC_PLUGIN_SLOT` shortens the table's times): the review's interleaving with
the times as they are (the second browser's fetch and upload and a push refused in the window, the press installs
the first browser's package), with the times run out (another browser's fetch, a push and hassmic's own fetch each
take the slot in the window: the first browser is told, the press installs nothing), the same browser's second
package under its first install, two uploads begun at once, hassmic's fetch giving way, the fetch's whole time, a
cancelled look, an older version for this API under a newer one for another (hassmic's fetch and a push), the
list's pace, the push port's times.  `tests/fake_plugins.py`: a second browser's upload under an open dialog is
refused now (it took the slot before).

An eighth read (2026-11-20), of the install slot's commit (225c70e), with the sync's report. One finding was a
hole older than plugins, confirmed by running it; two more were confirmed the same way:

| Finding | What changed |
|---|---|
| **The owner's press could approve a stranger's browser.** While anything waited for the action button (a plugin's install, adb, another browser's login), an unsigned `POST /api/login` with a new key ended that wait ("two at once: neither") and said so; the same key's next ask was armed as a login. The owner, sent to the Echo by his page, pressed and approved it: the log said "login approved", the page that the button was not pressed in time, the ring showed the login's pattern, not the plugin's. Any host on the LAN, in released builds (login against login and against adb) | one armed wait, ended only by the press, its time or its owner's signed word (`web.c` "what waits for the action button", with the argument): anyone else is told "busy". An approved browser's wait goes ahead of logins not yet approved, which are not armed during it nor for 30 s after it (`wait_over`, `strangers_from`); the other way round a signed ask is told "busy", leaves a mark, and goes first when that login has ended. Two unapproved browsers at once: the login is contested (state 4): not armed, the ring dark, the press nobody's, both told, armed again after 10 s without a competing ask. Unsigned asks counted per address and overall (`ask_rate`). Improv's wait: no stranger's login armed while it shows (`improv_waits`). The page: why a login was refused and when to ask again; what the press serves, for adb and for an install (`holds`, `/api/state` `button`) |
| The push port could still be locked out without the key: the body's floor was a line from the start, which a burst banked (16 MB less a byte, then 8.7 min of silence with the one bundle place held); four connections that asked for adb and never signed held all four places (0 of 39 other attempts got a challenge in 40 s), and the fifth was closed without a line | `ota.c`: 64 kB in every 10 s counted anew from each step, the whole within 10 s + size at 32 kB/s; two unsigned bundles in memory, one per address, a newcomer taking the place of the one slowest over the last second or two unless that is faster than 256 kB/s; before the key two connections an address and four in all, the oldest ended for a newcomer; a refused connection told "FAILED busy ..." and kept half open a second so the line arrives; both otatools print it (and no longer die of SIGPIPE or lose the answer when the Echo closes mid-upload) |
| **An install without the button was not re-decided when it wrote.** `ps_stage_install` bound the bytes (`expect`), not the decision: the plugin removed between a check that said "no button" and the install, the install counted as the first and a fresh acceptance was written with no press | the decision is the store's: `ps_stage_install(how, expect, press, ...)` runs the whole check again under `ps_lock`, in the critical section that writes and that `ps_remove` takes, and installs without a press only what needs none then (`PS_NEEDS_BUTTON` otherwise, nothing written; with a voucher only an official package where "never back" could be held). `/install` then leaves the package checked for the button; a push and hassmic's own fetch install nothing |
| A staging write after the lock was let go: a piece admitted for one browser, its cancel, another's `upload/begin`, then the write, at offset 0 of the other's file | every write carries its claim's generation and the store refuses one that is not the claim's that began the file (`stage_gen`; the other way, a "writing" state every claim waits for, would hold the slot's states to a flash write's time) |
| `/install` ran `ps_stage_check` in CHECKED, which a browser loses after two minutes | in CHECKING, which nobody takes; back to CHECKED where nothing is armed or installed |
| The core marks at start blamed the wake plugin for everything `main()` did meanwhile, opening Amazon's model included (the plugin missing, Amazon's words standing in) | the bracket is around the plugin's own data only: its list of words (`plugin_words_scan`) and, at start, its word opened and its answer waited for |
| Core marks never refused any plugin: an ordinary one whose report the core falls over took hassmic down every five minutes for good | for a plugin that is not the wake engine's they count as its crashes do: the third waits 30 s, the fourth five minutes, the fifth within ten minutes switches it off (`start()`); the wake engine's is retried as before |
| `fwcurl.c` kept a failed first load for the life of the process | tried again 60 s after the last try at the earliest, both `dlopen` errors in the log; a library that loads and lacks a function is not tried again |
| `/api/vouch/login` kept scanning after the nonce that matched and could overwrite its answer | stops at the match |
| 5.4's opening said all four ways end in `artifacts.c`'s staging, kind `plugin:<id>` | it says what is built |

Where that went another way than asked. *A signed wait does not end a login that waits* (the finding asked that
unsigned asks never block an install): ending it would turn the press of someone about to approve his phone into
an open adb for whoever holds an approved browser, the same redirect the other way. So the install is told
"busy" once, for that login's time at most, and then goes first; a stranger cannot renew it. *A contested login
does not swallow the press*: it is not armed, so the press is an ordinary one; swallowed, two keys that keep
asking would take the action button away. *The push port is not proof against a flood*: two addresses that each
keep sending at 256 kB/s or more hold both places, and several addresses that connect faster than a push signs
can end it; both are said in `ota.c`'s head and in docs/SECURITY.md. *`fwcurl.c` has no test*: nothing on the PC
makes a `dlopen` fail once and then work without a hook in the loader.

Tests of it: `tests/fake_web.py` `one_wait` (login against login: contested, nobody approved, the press the
assistant's, one at a time then works; a stranger's asks under an approved browser's adb wait, the press, the time
after; adb asked under a stranger's login; each with what the ring was asked to show) and `ask_rate`;
`tests/fake_plugins_race.py` (the review's sequence: an install waits, three unsigned logins, the press installs
and approves nobody; an install asked under a stranger's login; the plugin removed between check and install on
the page, under a push and under hassmic's own fetch, `HASSMIC_PLUGIN_OWN_DELAY`; the takeovers moved ahead of
`/install`, whose window is closed now); `tests/fake_ota_port.py` (new: connections before the key per address
and in all, both otatools' "busy", two burst-and-stall senders against a signed push); `tests/fake_improv.py` (a
login asked while the phone waits); `make unit-plugin_store` (the decision at install, the writes' generation),
`unit-plugin_broker` (the fifth mark).

A ninth read (2026-11-20), of the eighth's commit (c14c780) and of the sync's announcement (b11c0be). Three
findings were confirmed by running them on the PC build: the press for one browser's login still approved
another's, a bundle on its way was ended by idle connections, and a press took off the note of a plugin no page
had shown.

| Finding | What changed |
|---|---|
| **A press meant for one browser's login approved a stranger's.** The one-armed-wait rule held for signed waits only. After a login had ended (contested: armed again for whoever asked alone 10 s later; run out; approved) the next unapproved key was armed at once, with the same ring, and the page stopped asking at its first "refused" | `web.c` `wait_over` for every kind: no other unapproved key is armed until 30 s (`QUIET_MS`) after the press, or after the wait's arming + `LOGIN_MS` where no press ended it; the key whose own login ran out may ask again at once (`again`); a contested login holds until `QUIET_MS` after the later of the first asker's arming + `LOGIN_MS` and the last competing ask (`CONTEST_MS` is gone); invariants I1, I2 and each boundary argued at the section's head; the page keeps asking while "contested", "busy" or over the count (`web/app.js`) |
| An approved browser told "busy: a login" was ahead of strangers for 30 s only, and its page did not ask again | the page asks again when `/api/state` `button.holds` leaves "login" (`waitNoLogin`: `adbOpen`, `plgFinish`); after a login an approved browser was told "busy" for, not even that login's own key is armed in the quiet time |
| A login armed before a phone's Improv wait began was approved by the press | `web_approve`: an armed login meeting `improv_waits()` ends unapproved ("busy"), the press goes on to `improv_authorize` |
| `/api/adb` "off" from any approved browser withdrew another's adb wait | only the wait of the browser that says it; adb is closed either way |
| Three addresses use up the count of all unsigned asks (`ASK_ALL`) | named in docs/SECURITY.md and the guide; the browser whose login is armed is told "waiting" over the count |
| `tests/webclient.py` `login_with_button` asked and pressed again after any "refused" | only while "busy", bounded; "contested", "rate", "timeout" fail the test |
| **A bundle push was ended by idle connections** from two other addresses: the listener ended the oldest connection before the key, and a bundle is before the key for all of its body | `ota.c` listener: the oldest among those that hold no bundle's place (`pconn.body`; at most `BODIES_MAX` < `PRE_MAX` are spared), told "FAILED busy ..." before the shutdown |
| A bundle's place just taken looked slowest (`w_now` 0) and went to the next stranger | not given away in its first 2 s (`BODY_YOUNG_MS`), once per address: none for an address whose last bundle ended unsigned within 10 s (`spent[]`, `BODY_AGAIN_MS`) |
| **A press took off a pending sync's note** (A): `upto = syn_serial` counted the note between `syn_begin` and `syn_end`, which no page shows; the direct path skipped the asker's own pending note and then removed it | a dismissal names the "done" notes by number (`struct syn_ids`), at ask time; the asker's own are left out there |
| The bringer, or any vouched browser, armed `W_NOTE` every minute (B) | once for notes never asked about (`asked`, on file), then once per `NOTE_AGAIN_MS` (an hour) per Echo; "later" with `retry_s` |
| A note dismissed before Home Assistant took `synced`: the event lost over a restart (B) | kept as "gone" on file until the event went out (`SYN_GONE`, `SYN_KEPT`) |
| `vouched` failed open (C) | the positive fact: `state/web_button`, written by `web_approve`'s login; not listed = vouched; `state/web_vouched` is deleted |
| `ps_stage_install` failing after the package was written took the note away (D) | `syn_failed`: the folder read anew, the note stands when it has that manifest, and `synced` is said |
| Old-format notes dismissable by request; unread lines dropped silently; serials reused after a restart (E) | `known` 0: the button for everyone; a log line per dropped line; numbers at random (1 to 999999999), unique among those kept |
| `plugin_said` dropped its oldest silently; who counts as Home Assistant's link (F) | a log line (`SAID_LOST`); 5.4 and docs/SECURITY.md say that without a device key any subscriber counts |
| `.listed` only rises; ids past the 48th have no floor (G) | said in docs/SECURITY.md and the guide; cleared by a factory reset only. Not cleared by removing the plugin: a vouched browser could remove it and then copy an old version |
| "on every page of every Echo", "unread" (H) | `web/plugins.js`, 5.4, `sync_judge` 10: what is the case |

Where that went another way than asked. *The browser that brought a plugin is not refused the wait for the
button*: in the ordinary copy the page's own browser brings the package, so it is the owner's only browser on
that Echo, and refused outright his notes could never go until sixteen of them stopped every copy. It is bounded
as every vouched browser's wait is. *A signed wait is still armed right after a login ended*: held back like a
stranger, anyone could keep adb and installs away with one login a minute; what that leaves is said in
docs/SECURITY.md. *A withdrawn wait counts to the end of its minute* for the quiet time, not from the withdrawal:
whoever its page sent may still be on his way. *The count of all asks is not raised or shared out*: any share per
address is used up by addresses in turn; it is named instead. *The connections in their request line's or
challenge's time are not spared* (the finding left it open): sparing them is the eighth read's lockout again.

Tests of it: `tests/fake_web.py` `one_wait` (cases a, b, c with the second browser left asking, presses in
between; its own "Try again"; adb armed over a contested record; reserved; "off" from another browser),
`ask_rate` (the armed browser over the count); `tests/fake_improv.py` (a login armed before the phone's wait);
`tests/fake_ota_port.py` (2.5 MB signed at 320 kB/s under idle connections from two addresses, which fails on the
eighth read's `ota.c`; a place 0.3 s old against the next bundle; no grace twice); `tests/fake_plugins_sync.py`
(`HASSMIC_PLUGIN_SYNC_DELAY`: a wait armed, and a direct dismissal, while a note is pending; "later", over a
restart too; the event of a dismissed note after a restart; `HASSMIC_PLUGIN_FAIL_ACCEPTED`; an old-format line
and an unreadable one; `state/web_button` there, gone, and `web_vouched` ignored). Not tested: the pages' own
asking again (no browser test in the tree), the `SAID_LOST` line, a failed write of `state/web_button`.

A tenth read (2026-11-20), of the ninth's commit (2250848), by two reviewers who each ran what they found on the
PC build. The push port was locked out again by four addresses, a press let in a browser the presser never asked
for in four more ways, and the sync's note could be lost, got around, and taken off by a press its own bringer
caused.

| Finding | What changed |
|---|---|
| **The push port: four addresses that each sent a bundle's request line and nothing more kept everyone out** (owner 0 of 39). A place taken over left its connection counted as "a bundle on its way" (`pconn.body`) for the ten seconds of its signature's read, which never looked at the place | `ota.c`: a place names its connection (`ubody.who`), a takeover clears that connection's `body` under `cl`; `read_by` looks at the place's generation every 200 ms as `body_read` does. The head comment's "room is always made" and "one address costs every other address nothing" say what holds now |
| A bundle that was arriving when a newcomer took its place cost its address the next one's grace | `spend`: only an end by the floor, a close, a bad signature, or a place taken from a sender nothing had come from in the window |
| A verified push stood for a moment as "not signed, no place" and could be ended by the listener | `body_done` clears `body` and `pre` under one hold of `cl` |
| **A press that served nothing held nobody back** (a contested login, the quiet time): the stranger was armed when the time from the arming was up, and the owner's second press let it in | `web_approve`: such a press moves `strangers_from`, and a contested record's time, to its own time + `QUIET_MS`; I2 says so |
| **The armed key used the count of asks up itself**: its own "waiting" asks were counted, other keys' asks over the count were dropped before they could contest, and it re-armed through `again` | `login_ask`: the armed key's asks are not counted; an ask over the count from another key is refused "rate" and still turns an armed login contested (`contest`), so no `again` beside dropped asks |
| **A restart of hassmic forgot every wait**: a stranger's first ask after it was armed, the press on its way let it in | `state/web_wait` (`hold_save`, `hold_load`): the boot's id and the monotonic time before which no unapproved key is armed, written when a wait is armed and when a press ends one; read at start for the same boot. No pause after a start that had nothing waiting |
| A phone that stayed connected over Bluetooth after the network was back kept `improv_waits()` up for good | `improv.c`: let go after `ONLINE_SECS` (60) online with nothing running (`ble_server_drop`) |
| Comments on the login page's poll disagreed | once a second while "waiting", every two while held back: said so in `web.c` and `web/app.js` |
| **An update that failed after its program was written** left the new program under the old manifest, the note dropped, and the plugin ran after a restart (D) | the store installs a package whole or not at all (`pkg/<api>.new`, two renames, `pkg_settle`); a pending or failed sync's note is settled by the manifest digest of what the folder has (`syn_there`); `plugin.c` `program_is` starts no program that is not its manifest's |
| **The wait for a note's dismissal was neither bounded nor fair, and its press could be caused** (A, B, C): a fresh note each round got a wait at once; one browser's arm-and-keep spent the hour for all; the bringer armed it and rang a Drop In, and the owner's answering press dismissed the note | `W_NOTE` is served by the button held for a second (`web_hold`, `buttons.c` `held`), never by a press, and that hold is not the "Button" entity's; three waits armed per hour per Echo whatever the notes (`note_arms`, in `state/web_wait`), `asked` is gone; "busy" for numbers the armed hold will not take |
| **Remove, then sync, put an older official version in** (F) | `state/plugins/.installed`, `ps_installed_max`: rule 8's second half, and held again in `ps_stage_install` for a voucher |
| More than 16 "done" lines in `.synced` wrote past `struct syn_ids` (E) | 16 standing at most from the file (the rest dropped, said); `ids_add` bounds every number added |
| Up to 32 untold notes were queued into a core queue of 16 at start; `syn_begin` dropped the oldest "gone" note's event at 32 | `PLUGIN_HELD` 32 = `SYN_KEPT` (the assert says that); at 32 kept a sync is answered "button" |
| `state/web_clients`, `web_button`, `web_key` opened with `fopen` (a FIFO hung the start with the lock held); `.synced` unreadable was "no notes" | `ps_fopen` for all three and for `web_wait`; `.synced` there and unreadable: logged, and no sync without the button (`syn_blind`) |
| A browser pushed out of the list of 32 kept its line in `state/web_button` | written again when a browser of the button's leaves the list (`button_stale` for a write that failed) |
| The note's `by` was the label the login chose | the key's first eight bytes beside it, on the note and in the list of browsers |
| A vouched install rechecked only two of the rules as it wrote | the store's own four are rechecked (5, 6, 7, the installed half of 8); the comment names the ones that are not, and why |
| "116 checks" | the counts of this section's own tests are no longer given where they went stale |

Where that went another way than asked. *The armed key's asks are not capped, they are not counted*: its answer
is "waiting" and changes nothing, so there is nothing a cap would protect; what mattered is that they no longer
use up the others' count. *An ask over the count that contests is answered "rate"*, not "contested": it is
refused for the count, and its page asks again either way. *`again` has no flag of its own for dropped asks*: an
ask of another key, counted or over the count, contests the armed login, and `again` needs a login that ran out
uncontested. *The time kept over a restart is written when a press ends a wait too*, not only when one is armed:
else a browser let in a moment before a restart kept the next one out for the rest of that wait's minute. *A
failed install does not raise the installed floor* (the floor is written after the members, before the folder is
put in place). *The Show's face no longer says "A plugin wants in: press ..."* for the plugins' pattern: the same
pattern shows for a note, which a press does not serve; it points at the page. *Models without an action
button* got a hold of their own (both volume keys a second, let go before the pairing hold), which does nothing
but serve a note's wait: no new "Button" event there.

What it costs, and is said in docs/SECURITY.md: after a restart within a boot a browser that is not approved
may wait out the rest of a wait that stood (up to a minute and a half); a press in the quiet time makes the next
stranger wait half a minute from it; a vouched browser can spend an Echo's three note waits an hour.

Tests of it: `tests/fake_ota_port.py` (four addresses that sit on a request line: adb and a signed push get
through; a bundle at 80 kB/s that loses its place keeps its address's grace), `tests/fake_web.py` `one_wait`
(each press that served nothing holds strangers back anew, after a contest and in the quiet time; a restart
while a login waits, a restart with nothing waiting, another boot), `ask_rate` (forty uncounted asks of the armed
key; an ask over the count contests, logged once), `files` (FIFOs at `web_clients`, `web_button`, `web_wait`;
the button's list after a browser was pushed out), `tests/fake_improv.py` (a phone that lingers: dropped after
its time, the browser's login taken again), `tests/fake_plugins_sync.py` (a press with the note's wait armed: the
assistant's, and a Drop In answered; the hold serves it and is no "Button" event; "busy" for other numbers;
three waits an hour, a fresh note, a restart and another boot; `HASSMIC_PLUGIN_FAIL_PACKAGE`; a program that is
not its manifest's after a restart; version 1 after version 2 was removed; 24 "done" lines; 22 untold notes'
events; `.synced` as a FIFO), `make unit-plugin_store` (a package whole after a failure, `pkg_settle` both ways,
the floor), `make unit-plugin_broker` (`program_is`).  Three tests that log in a new browser after a restart met the wait
kept over it and were adapted (`fake_ha_timer_sound.py`, `fake_web_sounds.py`: a short quiet time;
`fake_plugins_race.py`: started as another boot).  The reviewers' `web_press.py` cases 1 and 2 wait until the
stranger is armed, however long, and press then: they print a violation under any quiet time that ends; what
holds is `QUIET_MS` from each press. Not tested: `body_done`'s one hold (a race of microseconds),
`buttons.c`'s hold on a real keypad and the volume pair's on a Show (the PC plays the hold with SIGURG), a power
cut between the two renames (the unit test lays out the folders such a cut leaves), a failed write of
`state/web_wait`.

An eleventh read (2026-11-20), of the tenth's commit (ce0e449), one reviewer, two findings run on the PC build
(the phone that scans, the page's writes under its lock), the rest by reading.

| Finding | What changed |
|---|---|
| **A phone that was never authorized and sent "scan" every few seconds was never let go** (`ONLINE_SECS` began anew with each scan), logins stayed "busy", and every scan was a request to root and a file on the flash | `improv.c`: on an Echo that is set up a scan needs the button (`E_NOT_AUTHORIZED`, nothing asked of root; the spec names an authorization for the switch only and none for the scan; an Echo that is not set up is authorized from the start). The linger counts from when the network came back with the phone connected and is moved on only by a switch under way or a scan asked within the button's `AUTH_SECS` |
| The installed floor past 64 ids was memory only: forgotten by a restart (the 65th read 0), and no floor was raised while it stood | `.installed` carries a line `+` (`floors_over`): closed over restarts; the ids on file keep rising |
| The floor was raised before the package was in place (a failed rename, a crash: a floor for a version never there), and a first install that died left `author.pub` and an empty `pkg/` | raised after the renames; `ps_scan` raises each plugin's floor to what its folder has, `ps_remove` to what it removes, a failed raise is owed (`floor_owe`: closed until written); `never_arrived` removes what a first install left (only that shape: `pkg/` empty, nothing but the key beside it) |
| **A bundle that had arrived whole could lose its place while its signature was checked**, was no longer "a bundle's connection" for the listener, and was staged all the same: installed, the PC told "busy" | `ota.c` `body_whole`: a place whose bundle is whole is not given away, and a place found taken at that moment ends the push unstaged. A connection the listener ended (`pconn.ended`) takes no place, and a read it made fail spends no address |
| `plugin.c` read the manifest's digest under the store's lock and opened the program after it: an update in between, "not the one its manifest names ... Remove it" for a healthy plugin | `program_open`: digest and descriptor under one hold of `ps_lock`; on a mismatch once more after a rescan (the list is read again only after an install let go of the lock), then the reason |
| **A hold begun before the note's wait was armed served it**; the Shows' pair counted as "let go before the pairing hold" by key repeats only; the guide offers `action_hold` to automations | `web_hold(down_ms)` against `login.at`; `buttons.c` passes the down time, and the pair's hold is 1 to 2 s by the clock; web.c and the guide say that a hold begun while a note's wait shows is the note's and fires no "Button" event ("nobody holds it by habit" is gone) |
| `state/web_wait` was written (two fsyncs) under the page's lock, from asks, the press, the hold and the expiry thread | `hold_save` decides under `lk`, `hold_flush` writes without it: one writer (`hold_wr_lk`) that takes the latest left, so the file never goes back; a request that arms flushes before it answers, the press and the hold wake the expiry thread. Pace as before: about one write per wait armed or contested |
| The revoke ignored a failed write of `state/web_button` | `button_stale`, as `add_client` |
| I2's "from each press of his, QUIET_MS" had an exception in `again`; "over a restart within one boot" was silent where the boot id cannot be read | a press that serves nothing clears `again`; without a boot id (said once in the log) the file names `-` and a start holds its time by the clock, no further than a wait can put it (`LOGIN_MS + 2 QUIET_MS`), the notes' waits counted as armed at every start |

Where that went another way than asked, or was decided. *The scan is refused outright* on a set-up Echo
without the button, not rate-limited: nothing is written for it at all. *A scan within the button's minute
moves the linger on*, a later one (state "provisioned" with the phone still connected) does not. *A floor that
cannot be written does not fail the install*: the package is in place by then; the sync is closed instead until
the floor is written. *More than 64 ids closes the sync for every plugin*, also those the file knows (their
floors still rise): the docs said "the floor is unknown", and one rule is easier to hold than two. *The owner
who pressed a moment after his login's minute waits the quiet time* (30 s; his page asks again by itself): the
price of "QUIET_MS from each press" having no exception. *Pairing is still told by the keys' repeats*; only the
note's hold got the clock's bound, so a pair held past 2 s without repeats does nothing.

Tests of it: `tests/fake_improv.py` (a scan without the button: not authorized, nothing asked of root; a phone
asking for scans every second is let go in its time), `make unit-plugin_store` (a first install that never
arrived; the floor after a failed write, by the scan after a restart and by Remove, `HASSMIC_PLUGIN_FAIL_FLOOR`;
64 ids and one more, over a restart; a known id rising in a closed file), `tests/fake_ota_port.py` (a whole bundle
whose check is made slow, `HASSMIC_OTA_CHECK_DELAY`, keeps its place and is installed), `make unit-plugin_broker`
(a package replaced since the list was read: read again and started), `make unit-buttons` (the down time handed
on; the pair 1.3 s: the hold, 2.3 s without repeats: not; a hold taken is no event), `tests/fake_plugins_sync.py`
(SIGURG = down for a second: a hold from before the arming takes nothing and is Home Assistant's; one from after
takes the note), `tests/fake_web.py` `one_wait` (no boot id: the file names `-`, a restart holds the wait; a late
press, then the key's own "Try again": busy). Not tested, by reading: the listener-ended connection (a race of
microseconds between its request line and the shutdown), the revoke's failed write, the order of two writers in
`hold_flush`, a flash that fails the floor's write twice.

A twelfth read (2026-11-20), of the eleventh's commit (bbfe206), one reviewer; the first finding run on the PC
build (the Echo joined and saved the phone's network, 5 times of 5), the second as a unit case, the rest by
reading. The first is not this branch's: `improv.c` on `main` has it (below).

| Finding | What changed |
|---|---|
| **A phone took a set-up Echo onto a network of its choosing with no press**: connected while the Echo advertised for its lost network, the network back, it ended its link and connected again on the advertising `ble.c` switches on as a link ends; the worker had by then left the mode (`M_OFF`), where the state stood at "authorized" and nothing ever ended the link. The other order gave each link an `ONLINE_SECS` of its own. And "provisioned" counted as authorized for as long as the phone stayed: one press, any number of switches | `improv.c`: authorization is `may()`, asked per command: not set up and advertising for that, or advertising for a lost network within `AUTH_SECS` of a press (`auth_until`, only `improv_authorize` sets it; spent by a switch that worked; gone with the mode). The state shown is derived from it (`show()`), never read. A link in `M_OFF` is dropped as it comes and by the worker; the advertising's data is cleared before a drop for `M_OFF`. `M_LOST` ends `ONLINE_SECS` after the network's return (`back_since`), connected or not, moved on only by `work_at` |
| After `program_open`'s reread the new program ran under the old manifest's capabilities and limits | `struct run`: digest, descriptor, capabilities, memory and threads from one hold of `ps_lock`; `start`'s first look only decides whether to start |
| A request could answer "waiting" while another thread's write of that wait was still in flight (`hold_flush`'s fast path) | `hold_flush(armed)`: a caller about to answer "waiting" waits for the writer's lock whether or not anything is left for it |
| `button_stale` did not survive a restart: a line of `state/web_button` for a key no longer approved stayed, and that key back on a voucher was "button" after the next start | `load_button` marks a line that names no client and writes the file again (tried again with the next browser added) |
| `floors_owed == 2` ("more than one floor owed") ended only with a restart | a raise whose write failed stays raised in memory; `floors_owed` is "memory is ahead of the file", cleared by any write that works; every scan tries. `floor_owe`, `owed_id`, `owed_version` are gone; `HASSMIC_PLUGIN_FAIL_FLOOR` fails every write of the file |
| `body_done`'s `!c->ended` could not be reached | removed, the comment says why it cannot be |

Decided. *The press is the Echo's, not one link's*: whichever phone is connected within `AUTH_SECS` may switch,
the one there at the press or one that comes after; Home Assistant's app and its integration connect anew between
steps, and a press bound to the link that was up would fail them. None that connects later, and none after the
one switch. *A press is not taken while "provisioned" shows or a switch runs.* *Floors: memory keeps what is
owed* instead of the scan retrying each folder and counting failures, as asked: a floor owed for a plugin that
was removed meanwhile has no folder for a scan to retry, and it is written with the rest. *An Echo adopted while
a phone is connected* goes by the set-up rules from then (it kept `M_OOBE`, open to that phone, for as long as it
stayed). `ble.c` is unchanged but for comments: `adv_upkeep` switching advertising on at a link's end is what
`M_OOBE` and `M_LOST` want, the order is the server's to keep (`ble.h`).

Tests of it: `tests/fake_improv.py` (a second switch and a scan after the one the press was for; a link while
nothing is advertised, dropped and nothing taken; a press with no phone there, a phone within its time and one
after; the network back and a phone that ends its link itself, or waits to be let go, and connects again on the
first advertising: no link and no advertising after the one time, nothing asked of root, a browser's login not
held up), `make unit-plugin_broker` (the reread with another capability set), `tests/fake_web.py` `files` (a
stale line of `web_button`, gone at the start, vouched a restart later), `make unit-plugin_store` (two plugins'
floors owed, a scan that cannot write, one that can; a removed plugin's floor). Each fails on bbfe206's code.
Not tested: the reconnect that beats the worker to the lock (both events before the worker runs: microseconds;
the same bound holds by `back_since`, by reading), `hold_flush`'s wait for the writer (two threads within one
write), `body_done` (code removed).

A thirteenth read (2026-11-20), of the twelfth's commit (5b05d5b), one reviewer; six sequences run on the PC build
(`tests/fake_improv.py`'s controller and phone), the rest by reading. All of it is the phone's setup and the
button it shares with the page; like the twelfth's first finding it is not this branch's alone (`main` below).
The hole "a switch without a press" stayed closed; what these are about is the press being captured.

| Finding | What changed |
|---|---|
| **Any press was the phone's setup's on an Echo ten minutes without its network**, with no phone connected and nothing on the ring: the press never reached the music, the assistant or Home Assistant's "Button", opened `AUTH_SECS` for whoever connected next, and the advertising said "authorized" to everyone in range | `improv_authorize` takes a press only while `waits`: a phone connected, served, shown "authorization required", which is while the ring shows it. The time it opens stays the Echo's (clients connect anew between steps), spent by one switch |
| **A central that was never authorized held the one link for as long as the Echo was offline** (advertising off: the owner's phone could not find the Echo), the ring showed its wait, the owner's press authorized it; identify was done for it as often as asked | in `M_LOST` a link without authorization is ended after `WAIT_SECS` 120, offline too, the advertising goes on (`ble.c` `srv_gone` pokes the loop: after a link we ended ourselves the advertising had stayed off until something else woke the controller's thread), and its address is not served for `SHUN_SECS` 60 unless the button's time runs. Identify once per 10 s, without authorization on a set-up Echo once per `IDENT_SECS` 60, for all links together |
| **A stranger's browser login was armed the moment the phone's state said "authorized"**, during the switch and under "provisioned": the same ring, and the owner's second press approved it | `improv_holds()`: no unsigned login is armed while, in `M_LOST`, a phone is connected or the button's time, a switch, a scan or "provisioned" runs. And the end of the phone's wait is a wait's end for web.c's I2: `web_quiet()` (main.c `improv_attention(0)`) puts `strangers_from` at least `QUIET_MS` after it, pressed or not |
| "provisioned" never ended while a link stayed; with the new network gone again the button was dead for Improv and every switch refused | `provisioned` ends `PROVISIONED_SECS` after the switch whatever is connected, and that link is ended |
| `back_since = 0` whenever the network was gone: a network away for a moment once per `ONLINE_SECS`, with an unauthorized central, kept `M_LOST` and logins "busy" for good | `on_ms`: time on a network counted together over all its returns, begun anew only by what the button paid for (the press, a scan asked while authorized, a switch's end). The same bound in `M_OOBE` (a network by other means, a central connected from the offline time: it was served for as long as it stayed) |
| `plugin_ask` could hand `strcmp` a NULL (`hold_flush(!strcmp(r, "waiting"))`) | guarded. The other callers (`adb_ask`, `login_ask`, `syn_ask`, the expiry thread, `web_start`) cannot have one |
| The worker's timed wait was on the wall clock, which root sets as the network returns: a step back stalled every time-driven end | `cv` on `CLOCK_MONOTONIC` (as web.c's `hold_cv`) |
| `command()` called `wifi_join`/`wifi_scan` (a state file: two fsyncs; a password's PBKDF2) under `mu`, behind which wait the controller's thread (Bluetooth proxy, A2DP), the button's thread, and every `/api/login` through `improv_waits()` under web.c's `lk` | both: the command is decided under `mu` and what it asks of wifi.c or the core is done after (`command` -> `ask` -> `asked`; `cmdn` stays set, so the next command is told "still being handled" and nothing else starts a switch); and `improv_waits()`/`improv_holds()` read atomics that `show()` keeps, so web.c takes no lock of improv.c's at all, which is also what lets improv.c call `web_quiet` under `mu` |
| Words that said more than the code did (main.c at `on_action`, improv.c's header, SECURITY.md, GUIDE.md) | rewritten; they now say what is not closed |

Not closed, and said so (improv.c's header, docs/SECURITY.md "Bluetooth", docs/GUIDE.md "Setting up from a
phone"): **the ring does not tell whose phone waits.** Improv has no pairing and a link has no identity; one
central can be connected at a time. Somebody in Bluetooth range who connects first and waits gets the press the
owner makes for his own phone, and may then switch the Wi-Fi once within `AUTH_SECS`. (The fourteenth read,
below: what follows narrowed nothing, and the address held back is gone.) `WAIT_SECS` and the
address held back narrow it (the link is not his for good, the owner's phone gets its turn at the advertising),
they do not close it: addresses are the central's to choose, and one that comes back under a new one is served
for another `WAIT_SECS`. Closing it needs something Improv does not have (a code shown and typed, a pairing).

Decided. *`WAIT_SECS` is 120*: two of the button's minutes; a client first connects and shows "press the button",
then the owner walks over as for a login (`LOGIN_MS`, a minute), and the app does not ask again by itself as the
page does. Counted from when the link was last without authorization, so a phone whose minute ran out unused has
the time again. *`SHUN_SECS` is 60*, by address, in `M_LOST` only and not while the button's time runs (a press
means anyone). An owner whose own phone waited two minutes without a press waits that minute too. *Identify over
the count is answered as done* (no error: the spec has none for it, and the rainbow of the one before still
shows). *The quiet time after the phone's wait is told by the ring's hook*, not by `improv_authorize`: every end
of that wait goes through `show()`, the press and the ends without one (the phone left, was let go, the network
came back), and the owner sent to the Echo by his app is on his way after each of them. *A central connected from
the offline time on an Echo that is not set up* is let go by the same `ONLINE_SECS` rule (one condition, not two
rules); a scan begins that time anew there, which is within what any phone may do to an Echo that is not set up.
*Both halves of the lock finding*, because they are for different waiters.

Tests of it: `tests/fake_improv.py`, 110 checks (83 before): a press with no phone connected (the assistant's,
nothing authorized, the advertising unchanged), with one waiting (authorized; within the time a phone that
connects again; none after); a login asked right after the press and under "provisioned" (busy, not armed), the
second press (nobody's); "provisioned" ended with the network gone and the phone connected, the button working
again; the network back 1.9 s at a time; a phone that stays without a press on an Echo with no network (ended,
advertising on again, its address turned away, another's served, any served within the button's time); four
identifies within a second (one done); not set up and on a network (let go). Against 5b05d5b's build 17 of them
fail. Not tested: the clock set back (needs the wall clock), the NULL in `plugin_ask` (the slot is held in
`CHECKING` across the call: by reading, not reachable today), the quiet time after a wait that ended without a
press (the test's quiet time is 0.3 s), what `mu` no longer waits for (by reading).

A fourteenth read (2026-11-20), of the thirteenth's commit (860a3d0), one reviewer; five sequences run on the PC
build (a, b, c, d, f: `tests/fake_improv.py`'s controller and phone), one finding by reading, one older and
unreproduced. All of it Improv and the button again, and half of it against the thirteenth's own fixes.

| Finding | What changed |
|---|---|
| **A write to the flash per Bluetooth link.** Every end of a phone's wait called `web_quiet`, which moved `strangers_from` and had `state/web_wait` written: a central connecting and leaving 118 times in 20 s, 118 statefile commits. And `web_quiet` took web.c's `lk` under improv.c's `mu` on the controller's thread, while `lk` is held over `save_clients`/`save_button` (fsyncs) | `web_quiet` only leaves the time in an atomic (`phone_off`) and signals the expiry thread; `phone_over`, first in `login_expire`, applies it under `lk` wherever the waits are looked at (asks, the press, the hold, the expiry thread). Nothing of web.c's is taken under `mu`. The time is still kept for a restart, as a contest's is: `QUIET_MS` to spare, written again only when it moved past what is written (`hold_save` `moving`): one write per `QUIET_MS` |
| **The owner's own client was the one the time and the shun hit.** `WAIT_SECS` ran from the connection; Home Assistant's flow connects first, then shows its menu, identify, the SSID and password form, and only then asks for the button, and its adapter or proxy has one address, which the shun turned away for a minute: "cannot connect" | the shun is removed. An unauthorized `M_LOST` link is ended once nothing came from it for `WAIT_SECS` (ble.c's new `heard` hook: any ATT PDU), counted from the last of that or the end of what a press paid for |
| **The shun and the time bounded nothing** against a central that means to stay: leaving a second early and returning, or a new address each time, held link and ring 20.0 of 20.0 s | nothing: it cannot be bounded without a link identity. The words that claimed a bound are gone (improv.c's header, SECURITY.md, GUIDE.md, CHANGELOG.md), replaced by what is true and by the owner's rule |
| **A login armed before a phone's wait stayed armed when that wait went off without a press**; the press half a second later, plausibly for the phone, approved the browser | `phone_over` ends an armed `W_LOGIN` as "busy" (`wait_over`; its page asks again and is armed after the quiet time). Approved browsers' waits (adb, plugin, note) stay: below |
| `on_ms` counted only between two looks that both found the network: returns shorter than the worker's second counted nothing | every look that finds the network counts the time since the look before; after a look that found none, a second of it at most |
| (older, by reading) `offline_since` reset by any look that found a carrier: a failed switch's momentary carrier sent the Echo to `M_OFF` for `OFFLINE_LOST` once the phone left, mid-setup | real, reproduced on the PC with the test's stand-in carrier. No look counts while a switch runs, nor within `SETTLE_SECS` 5 of a failed one until a look finds the carrier gone |

Not closed, not bounded, and said so: **a device in Bluetooth range can hold the Echo's one setup link, and the
ring's wait with it, for as long as the Echo is without its network**; the owner's own app then cannot find or
connect to the Echo. A press while that device is on the link is its. The rule for the owner (SECURITY.md,
GUIDE.md, CHANGELOG.md): press only when your own app has asked for the button; if the app cannot find or
connect to the Echo while the ring shows the wait, do not press.

Decided. *`WAIT_SECS` stays 120, from the last thing the link sent*: py-improv-ble-client ends its own link 90 s
after it last asked or heard anything (`DISCONNECT_DELAY`) and connects anew at its next step, so the Echo's end
never comes first for it; what it ends is a phone that was forgotten. It claims nothing about an attacker.
*The hold after a phone's wait is kept over a restart*, with slack, not dropped: without it a restart within those
`QUIET_MS` arms the first stranger to ask while whoever the phone's app sent to the Echo is on his way, the case
`state/web_wait` exists for; the cost is one write per `QUIET_MS` and strangers kept back up to `QUIET_MS` longer
after such a restart. *Approved browsers' waits are not ended with the phone's wait*: the late press for the
phone then opens adb or accepts a plugin for a browser the owner let in, as a press meant for the assistant
would, which web.c already accepts for a login's late press; ended, anyone in Bluetooth range of an offline Echo
could take every install and adb request away by connecting and leaving. *No sound when a login ends with the
phone's wait*: nobody pressed. *A look after a look that found no network counts a second at most*, not the time
since: a command's PBKDF2 between two looks is not time on a network. *`SETTLE_SECS` 5*: root writes "failed" as it
lets go of the network; where it got back onto the old one the carrier stays and counts from then.
`HASSMIC_IMPROV_TIMES` lost the shun's place: `<oobe> <lost> <auth> <provisioned> [<online> [<wait> [<identify>]]]`.

Tests of it: `tests/fake_improv.py` 127 checks (110 before), the new ones on a second Echo that is set up and
offline from its start (`more()`: lost 6 s, nothing from a phone for 5 s, quiet time 3 s): an approved browser's
adb wait through a phone's wait that comes and goes (still armed, the press opens adb); a stranger's login the
same way (ended, "busy", the press the assistant's, armed again after the quiet time); 39 links made and ended in
6.5 s (`state/web_wait` written 3 times: once for the login the first ended, then one per quiet time; 39 on
860a3d0; the time in the file 4.3 s ahead); a phone that reads at 3 s (still connected at 6.6 s, let go 5 s after the read), the same address served
again at once; a switch that fails with the carrier up, the phone leaving (still advertising, served again); the
network back 0.8 s of every 1.95 s (let go). Against 860a3d0's build 13 of the 18 of `more()`'s checks run there fail; with only the
last two fixes taken out of this tree, their 3. The reviewer's sequences on the fixed build: PLAN.md. Not tested:
`lk` no longer taken under `mu` (by reading: `web_quiet` is two lines), the restart within the quiet time after a
phone's wait (the file's time is checked by its count only), a real phone's app against any of it.

A fifteenth read (2026-11-20), of the fourteenth's commit (807c103), one reviewer; seven sequences on the PC build
(A, A2, B, B2, C, D, E, on `tests/fake_improv.py`'s controller and phone). Improv and the button once more; the
first finding is a way into "a phone's wait" that every argument since the twelfth read took not to exist.

| Finding | What changed |
|---|---|
| **A phone's wait on an Echo that is online and was just set up.** `want = core_oobe() ? mode : M_LOST` turned a link held in `M_OOBE` into an `M_LOST` one the moment Home Assistant adopted the Echo, with no ten minutes offline; and in `M_OOBE` every scan reset `on_ms`, so a central kept such a link for as long as it scanned (20 s of 20 with `ONLINE_SECS` 12). The ring showed a wait; the owner's press for his own login (armed: `improv_holds` is 0 in `M_OOBE`) was refused for the browser and taken by `improv_authorize`; the central joined and saved "Cafe". Without a press, the central leaving ended the owner's login (`phone_over`) | `M_LOST` is entered only by "set up and `OFFLINE_LOST` without a network" (`offline_since` begins anew at the adoption); a held mode stays what it is (`want = mode`). Adopted in `M_OOBE`: a switch under way and its "provisioned" end as they would have (the phone hears the result and reads the address; `may()` false, so nothing else; no wait, no press, `improv_holds` 0), anything else is `stale()`: the link ended (worker, `on_connection`), a scan's answer dropped, the mode by the network alone. In `M_OOBE` a scan no longer resets `on_ms` (`a->paid = mode == M_LOST`): on a network a link lasts `ONLINE_SECS` from the last switch's end at the most |
| **A restart with a phone's wait showing forgot it**: `hold_save` had no term for it, `state/web_wait` was written only once the wait had gone off; the first stranger to ask after the start was armed | `hold_save`: while `improv_holds()`, `QUIET_MS` from now, as a moving time (`QUIET_MS` to spare, written again once now passed it: one write per `QUIET_MS` while a phone is connected to an offline Echo); `web_phone` (main.c `improv_attention(1)`) wakes the expiry thread as the wait begins |
| **A scan taken after the button's time**: `!may(now) && !join_id` let a scan through to wifi.c while `join_id` stood, and `join_id` stands until the worker reads root's answer, which it did after the command | not authorized and a switch running: `E_UNKNOWN` from `command()`, nothing asked of wifi.c. (Not by reading the result first: root can answer between that read and the command) |
| "two minutes to reach the Echo" (improv.c's header, GUIDE.md) | words: Home Assistant's client ends its own link after 90 s in the authorize step too; the ring is dark until it connects anew, a press in the gap is the assistant's, each round is a `web_quiet` (header, GUIDE.md, SECURITY.md, CHANGELOG.md) |
| `hold_save`'s "more than 2 `QUIET_MS` too late" rewrite never fired at the shipped `LOGIN_MS` = 2 `QUIET_MS`; the test measured a branch production does not take | the threshold is the spare `QUIET_MS` itself: a login ended by a phone's wait within `QUIET_MS` of its arming has the file rewritten; later than that the file is at most `QUIET_MS` past what is needed, as everything else here |
| web.c's "armed before that wait began": `show()` called the hook and stored `a_waits`/`a_holds` after | `show()` raises both before the hook and lowers them after it; the words say "or as it did" (`login_ask` reads the atomics without `mu`), and why never after the wait's end (the mark is left before they go down; that every ask "takes the mark first" was the wrong half of it: since the sixteenth read `login_ask` and `web_approve` read the atomic first and take the mark after) |
| `settle_until` not cleared by a new switch | cleared in `asked()` |

Decided. *The link is ended at adoption, not kept under `M_LOST`'s rules or under a third mode.* What survives is
what the phone that set the Echo up still needs, and it needs no authorization: the notification of the switch's
end, the result to read. A phone that wanted to scan or switch again after the adoption is asked to wait the ten
minutes and press, like any phone at an Echo that is set up; a first setup never needs that. *`offline_since`
begins anew at the adoption*: an Echo cannot be adopted without a network in practice, but the rule is then true
as worded: ten minutes without a network as an Echo that is set up. *A failed switch's end still resets `on_ms`
in `M_OOBE`*: a switch is what any phone may ask of such an Echo, and each one moves its network; the adoption
ends that. *The wait is not kept showing over Home Assistant's 90 s gap*: a ring with no central connected is the
press with nobody there, which the thirteenth read closed. *The phone's hold over a restart is `QUIET_MS` from
now, renewed*, not `LOGIN_MS + QUIET_MS` once: the wait has no arming time a page promised a minute from, and
what its end leaves in a running hassmic is `QUIET_MS`; the price is the write per `QUIET_MS` and, where the
phone's business ended with no wait going off, up to 2 `QUIET_MS` kept after a restart for nothing. *The 2
`QUIET_MS` rule was changed, not removed*: at the shipped times it now does what it was added for whenever a
phone's wait ends a login in its first half minute; a restart otherwise keeps strangers back at most `QUIET_MS`
longer than need be.

Tests of it: `tests/fake_improv.py`, 155 checks (127 before): the end of `more()` (a phone's wait showing: the
file's time ahead, still ahead 3.6 s later with the phone asking, hassmic killed and started: a stranger "busy")
and `taken()`, two Echos that are not set up: a scan a second on an Echo with its network (let go at
`ONLINE_SECS`); adopted with a central connected and the owner's login armed (link ended, no wait shown or ended,
nothing advertised, the press approves the browser, a central connecting again not served); a login of 6 s with
the quiet time 3 ended by a phone's wait (the file from 9.0 s to 5.4 s ahead; 7.9 before); 24 scans beside a
failed switch after the button's 1 s (none answered, root asked once); adopted while the switch runs (root's
stand-in 2 s late: "provisioned" and the address still arrive, a login beside it armed and approved by the
press, the phone connecting again reads the address and is refused a switch and a scan, ended after
`PROVISIONED_SECS`, no wait ever). Against 807c103's build 11 of them fail. Not tested: `settle_until` cleared
(nothing outside sees it: "provisioned" holds the mode meanwhile), `show()`'s order (by reading), the 90 s gap of
the real client.

A sixteenth read (2026-11-20), of the fifteenth's commit (ef23000), one reviewer, sequences on the PC build. It
judged the commit clean against its threats (a phone's wait only on an Echo ten minutes without its network; no
login armed beside one; the restart) and left these:

| Finding | What changed |
|---|---|
| Low, reproduced. Not set up, the switch worked, the central asks for a scan, Home Assistant adopts: `stale()` does not look while "provisioned" stands and "provisioned" did not end while `scan_id` did, so state, mode and link were kept until root answered or `SCAN_SECS` (19.3 s with `PROVISIONED_SECS` 3), and the list of networks was notified after the adoption | improv.c's worker: in `M_OOBE` and taken on, a scan that is out is dropped before its answer is looked for, whatever else stands; "provisioned" ends at `PROVISIONED_SECS` whether or not a scan is out (not taken on, the scan then holds the mode alone until answered) |
| Order, not reproduced. `login_ask` and `web_approve` ran `login_expire` (which takes the mark of a phone's wait's end) and read `improv_holds()` / `improv_waits()` after: a wait ending between the two was missed for that call (a login armed after the wait's end until the next look; a press approving a login armed before the phone's wait as that wait ended) | both read the atomic first and call `login_expire` after: a 0 read means the mark is there to be taken or no wait had begun (`show()` leaves the mark before it lowers them); a 1 read at the press ends the login unapproved, also where the wait ended in that instant |
| Words: what is served under "provisioned" after the adoption; "a minute at most once the network is back"; "only the phone that just gave the network"; `hold_write`'s "one write per `QUIET_MS` whatever a Bluetooth device does" | device info and identify once per `IDENT_SECS` still are (improv.c's header, GUIDE.md); the minute holds without a press, which with a scan in its time begins it anew (SECURITY.md); whatever is connected or connects in that minute is served, the Echo tells no phone from another (CHANGELOG.md, SECURITY.md, GUIDE.md); a Bluetooth device alone: one per `QUIET_MS`; a login a LAN asker has armed, ended by a link, costs two, at most two more per `QUIET_MS` (web.c, CLAUDE.md) |

Decided. *The scan is dropped in every `M_OOBE` look once taken on*, ahead of the look for its answer, not only at
the adoption's edge: one whose request to root was under way as Home Assistant adopted comes back with its id
and goes at the same look. Root may still scan for it (a request already written is not taken back); its answer
is read by nobody. *"two more per `QUIET_MS`", not "two"*: the phone's own moving write can fall in the same
`QUIET_MS` as a login's two (by reading; the reviewer's count was of the login's).

Tests of it: `tests/fake_improv.py`, 161 checks (155): `taken()`'s fifth Echo, not set up: the switch works
("provisioned" 4 s), a scan asked with root's stand-in 14 s late, adopted: the link ended 4.2 s after
"provisioned", nothing advertised, no list notified. With the two lines of improv.c as ef23000 had them: 15.0 s
and the list notified, 2 of the 6 fail. Not tested: the order in `login_ask` and `web_approve` (by reading: the
window is between two statements under `lk`, and nothing on the PC ends a phone's wait inside it on purpose).

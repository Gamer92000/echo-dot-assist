# The hassmic plugin protocol

What a plugin and hassmic (the broker) say to each other, complete enough to write a client from in any language.
The C client in this folder (`hassmic_plugin.h`, `hmplugin.c`) is one implementation of it; `plugmsg.h` is the wire
format alone. The design and its reasons are in [docs/plugins.md](../docs/plugins.md).

## Versions

The protocol is **scopes**, each with a version of its own, a whole number from 1: `core` (what every plugin speaks:
the greeting, `CONFIG`, `READY`, `PING`, `STOP`, `TIME`, `OK`, `ERR`) and one scope per capability, named as the
capability (`files`, `storage`, `net.lan`, `ha.entities`, `button` ...). Every message is part of a scope (one shared
by several capabilities, `CONNECT`, `LOG`, `NET`, `UDP_OPEN`, is part of each of theirs), from the version that
brought it; so is every field, every value of an enum, every constant, and every key a manifest has for a capability.
The tables at the end ("Messages") name each one's scope and version; this hassmic's versions are at their top.

**Strict backwards compatibility.** A scope's next version only adds: a message, a field appended after every field
of the versions before it (in a list's element too), an enum value, a constant, a key of the manifest. Nothing that a
version had ever changes its meaning, type, bound, number or direction, and nothing goes. What has to break becomes a
new scope, under a new name. A message of several scopes, and the greeting (`HELLO`, `WELCOME`, `REFUSED`, read
before a version is known), never grow; the greeting's lists of scopes hold 255, every scope there will ever be. A
capability more is a scope more: its `CAP_*` number is its own scope's, and core stays as it was.

**Widening.** Every version above 1 says, in `messages.def`, whether it **widens** what its capability lets a plugin
do beyond the words of the install line the owner accepted, or stays **within** them (more of the same: another
field, a value), in words of its own (300 bytes at most). An update that comes to require a widening version is
wider than what was accepted and asks for the action button, as a capability more does; one within installs without
it. The dialog says every version between the two, each in its words, and for a first install or a capability asked
for anew the words of every widening version up to the one required, beside the capability's line (which is version
1's). The mark is recorded with the version and never changes.

**The record.** In hassmic's tree `tools/mkplugmsg.py` refuses what breaks these rules, and `sdk/scopes.lock` records
what every version had, its mark among it: a `messages.def` that would make one otherwise fails the build. Every
version any release carried (a tag `v*`: the betas of `main` and the releases of `release` alike, whichever is newer
and whether or not one branch has the other's) stays exactly as that release's lock had it, in the lock and in
`messages.def`: nothing taken out, nothing added; what is new goes into a version of its own. Only a version no
release carried may be deleted from the lock and recorded anew. A version a release that the tree descends from
carried may not go; one only a release elsewhere carried (a beta of `main` that the release branch lacks, or the
reverse) may be missing, and is held to that release wherever the tree has it. Two releases that carried one version
otherwise fail as well (`tools/mkplugmsg.py --released`, which CI runs with every tag; `make unit-plugmsg` the same
wherever git and a tag with a lock are, and says it skipped where not).

**What a plugin speaks** is what its manifest's `"requires"` names: the least version of each scope it uses, core
and the scope of each capability it asks for, and nothing else (`{"core": 1, "files": 1, "wake": 1}`). A plugin that
uses what `ha.entities` 2 brought requires `"ha.entities": 2`. hassmic runs it where it has every scope named at that
version or a later one, and speaks exactly the versions required with it: nothing of a later version reaches the
plugin (no message, no appended field, no enum value), and nothing of one is taken from it (answered `ERR_UNKNOWN`,
as by a hassmic of that version). A hassmic update never breaks a plugin; a hassmic older than what a plugin
requires (one an Echo fell back to) refuses it at install and at every start, "needs a newer hassmic: `<scope>
<version>`", keeps it, and runs it again under one that has it. A scope name hassmic does not know is a later
hassmic's, said the same way. A plugin's code declares nothing: its client says in `HELLO` what it can speak, and
hassmic answers in `WELCOME` with what will be spoken, which is also what was granted (core and the scope of each
capability the owner accepted: there is no set of bits for it).

**Values, constants and keys.** A field that holds an enum's values (its type in "Messages" names the enum: `u8,
MUSIC_*`) holds one the peer speaks: a value of a later version of the enum's scope, or one no version has, makes the
packet malformed (`ERR_BAD`), and no client or hassmic sends one. Every value a version above 1 brought names the
value of an earlier version that a peer of an earlier version gets in its place (the reference below: "from `call` 2;
to a peer of an earlier version `STOPPED`"; `messages.def` `ELSEWHERE=7@2>STOPPED`, recorded in the lock with it):
a sender maps every such value to what the peer speaks before the packet goes, down the chain to a value the peer
has (`plugmsg.h` `pm_<enum>_for`, and `pm_fit` over a whole packet, which hassmic's `psend` and the C client's
`put()` run on every packet; the test vectors' `fit` lines hold the packet before and after), so no message is
dropped for a value. The fallback is the value a peer of the earlier version should act on, as it understands it:
the one the new value is a case of (a call that ended "elsewhere" ended: `STOPPED`; a failure of a new kind:
`FAILED`). Never a failure or a refusal mapped to a success, never a command to another command, never an event to
one that did not happen. Where no value of an earlier version is right, the value names none (`messages.def`
`MIC=4@2>-`; the reference: "not sent to a peer of an earlier version"): a packet with it is **not sent** to a peer
of an earlier version at all (`pm_<enum>_for` answers `PM_NOT_SENT`, `pm_fit` 1; the vectors' `unsent` lines). That
is only ever where the enum is what the message is about: an event whose whole point is the new value, on a message
marked `drop` (the reference: "Marked `drop`"; `plugmsg.h` `pm_droppable`; recorded in the lock with the message).
Only `BUTTON` is: a new key of the buttons, say, never reaches a plugin that knows only the action button as a press
of it. Never on an answer (`ERR`), an end (`CALL_END`, `CALL_HANGUP`, `REQ_END`), a state (`STATE`, `MUSIC_STATE`)
or a reason or qualifier of something else (`MUSIC_CMD`'s `why`, `ACTION`'s `from`, `REQ_START`'s `source`): what
those say must arrive, so a new value there always names the earlier value it is a case of, and one that is a case
of none, a new command among them, is a new message (`tools/mkplugmsg.py` refuses `>-` on an enum that a message not
marked `drop` holds). So a plugin's own messages are never left unsent for a value: every one it writes goes, its
later values as their fallbacks; and whatever hassmic tells a plugin that must arrive does. Its sender drops a
`drop` message without a word, as what that peer cannot be told: hassmic does not count it as its error (and counts
none of a plugin's budget for it: a press of a key it does not hear is not one of its five a second); a client that
reads `BUTTON` never sees one. Only a message hassmic sends (`<`) is ever marked `drop` (`tools/mkplugmsg.py`
refuses the mark on a plugin's own, `>`, and on the greeting and the answers). A request a client does not send
because its message is of a scope, or a version of one, not spoken with hassmic (a request of a capability its
manifest does not ask for) is refused by the client itself with a code of its own, `0xffff` (the C client's
`HM_ERR_NOT_SPOKEN`, the Rust client's `ERR_NOT_SPOKEN`; never an `ERR_*` on the wire, which `messages.def` keeps
below it); a request with a value of no version is wrong (`ERR_BAD`), as any other. `pm_fit` rewrites the packet in place for the one peer it goes to: encode a packet
per peer, never one buffer for two (hassmic's `psend` and the C client's `put()` refuse a buffer sent once). A
value no version has is refused by the sender (the C client's writers: `-1`). An enum of a
message of several scopes is core's or `CAP`: a peer of one of its scopes may not speak another's. A `CAP_*` value
(`CONNECT`'s and `UDP_OPEN`'s `kind`) is any capability hassmic has: one the plugin was not granted is
`ERR_DENIED`. A constant of a later version
applies only to a peer that speaks it. A key of the manifest (the table "The manifest's keys" below: where it stands
and the version of its scope that brought it) that a later version brought than the manifest requires refuses the
manifest, at the Echo and at the plugin index alike; a key no version has is refused in a capability's entries and
left alone elsewhere.

## What a plugin is

A static program for 32-bit ARM Linux (EABI5, ARMv7-A with VFP) in any language: no interpreter (`PT_INTERP`), a
stack that is marked and not executable (`PT_GNU_STACK` without `PF_X`), no segment both writable and executable. It
is started by root's launcher as a user of its own, under a system call filter (`filter.def` in this folder is the
table: about 70 calls, the rest answer `EPERM`, numbers above 386 `ENOSYS`), with limits on memory, threads and
descriptors. A static **bionic** program (the Android NDK's) does not start under it; musl's, Rust's and Go's do.

What it finds:

| | |
|---|---|
| Descriptors 0, 1 | `/dev/null` |
| Descriptor 2 | a pipe into hassmic's log, shown as `plugin <id>: ...`; 50 lines a minute are kept |
| Descriptor 3 | the control socket: `SOCK_SEQPACKET`, one message per packet |
| `argv` | `plugin`; the environment: `HASSMIC_PLUGIN=1` |
| No file system | no `open`, no directory. What it keeps and reads, the broker keeps for it |
| No sockets of its own making | no `socket`, `connect`, `bind`, `accept`, `sendmsg`. Sockets arrive over the control socket (`CONNECT`, below) |
| Time, randomness | `clock_gettime`, `getrandom`. Not `/dev/urandom` (libc++'s `std::random_device` opens it and aborts) |
| Memory | anonymous private mappings, never executable; `brk`. musl reports a refused `mmap` as `ENOMEM` |

A second `execve`, `fork`, `kill` of another process, `ptrace`, `F_SETFL`, `FIONBIO`, `mmap` of a descriptor: refused.

## Framing

A packet is `u16 type, u16 flags (0), u32 id`, then the type's fields in order, little-endian, 32768 bytes at most.

- A **string** is `u16 length` + that many bytes, no NUL, at most the field's limit.
- A **list** is a `u16 count` where the field stands; its elements follow after the last field of the struct the
  list is in (of several lists: in their order). An element is a struct by the same rule.
- `id` ties an answer to its request: the asker picks it, never 0. Events have 0, but for `LOG`, which carries an id
  the plugin answers (`OK`, flow control: "The log" below).
- A packet is exactly its layout at the version spoken (both ends know it, from `WELCOME`): bytes after its last
  field make it malformed, as a field cut short does (answered `ERR_BAD`).
- A type a reader does not know, or one of a scope or a version the plugin does not speak, is answered `ERR` with
  `ERR_UNKNOWN` (when it had an id), never a reason to end: by hassmic, and by both clients (the C client dropped it
  unanswered before a review of 6631974, and hassmic's request waited for its time).
- A message of a capability the owner did not accept is answered `ERR_DENIED`.
- A value of an enum the peer does not speak (a later version's, or none) makes the packet malformed: `ERR_BAD`.
- Descriptors travel from the broker to the plugin only (`SCM_RIGHTS`, one per packet).

hassmic never waits on a plugin: what it sends is sent without blocking, and a plugin that does not read loses
messages. More than 200 messages a second from a plugin are dropped; five times that, and it is stopped.

## The start

1. The plugin sends `HELLO`: every scope its client can speak, each at the highest version it has (the C client:
   every scope of its `messages.def`), each once.
2. hassmic answers `WELCOME`: where it runs, and the version of each scope spoken from here on, exactly the
   manifest's `requires`, which is also what was granted. A client that names a scope lower than its manifest requires, or not at all
   (an SDK older than the manifest), gets `REFUSED` (`REFUSED_SCOPES`), and the plugin ends; its card says which. A
   client keeps to what `WELCOME` says: it reads and writes each message as that version has it.
3. hassmic sends `FILES` for each file list of the manifest, then `CONFIG` with every setting.
4. The plugin answers the first `CONFIG` with `READY`. It has 5 s from its start to `HELLO` and 5 s from `WELCOME`
   to `READY`. Before `READY` it may already send what is answered at once: `FILE_OPEN`, `WAKE_WORDS`, `STORE_GET`,
   `STORE_PUT`, `STORE_DEL` and `LOG_FOLLOW`. Not `CONNECT`, nor anything else: answered `ERR BAD`
   there (a connect may
   take 10 s, longer than `READY` may). The C client's `ready` callback runs before `READY` goes out; it connects
   from `tick`.

From then on: `PING` every 10 s (answer `PONG` with its id; three unanswered and the plugin is killed, as is one
whose control socket's queue stays full for 5 s because it does not read, and one, unless it is the wake word
engine, that uses more than a whole core for a minute; a `PING` with id 0 is answered like any), `CONFIG` and
`FILES` again whenever the owner changes something, `STOP` when it should end (2 s, then `SIGTERM`, then
`SIGKILL` 2 s after that). Between `STOP` and the plugin's end (until it exits, or is killed: up to 4 s) `STORE_PUT`
and `STORE_DEL` are still done and answered (a forwarder keeps its cursor as it ends), also when `STOP` came for an
update of the plugin: its storage is the next version's (unless that version drops `wake`: see Storage); any other
request is answered `ERR FAILED` at once, also one that was under way when `STOP` came (a `CONNECT` a worker was
still busy with gets `FAILED`, never its socket). A client stops waiting for an answer when `STOP` comes, but for a
storage write's, and runs its stop code: the C client fails the request (`HM_ERR_FAILED`), runs the `stop` callback
as soon as the callback that asked returns, writes then what it kept for later, and `hm_run` returns. A plugin that
ends by itself is started again after 1, 5 and 30 s, then every 5 minutes; five ends in ten minutes switch it off,
unless it is the Echo's wake word engine.

`STOP` comes when the owner switches the plugin off, removes or updates it, and when hassmic itself ends in order:
for an update of hassmic, a restart it asks for, `SIGTERM`. hassmic then waits for its plugins, 2 s at most, before it
ends; after that they are killed. Not on a path hassmic does not see: a crash, a power cut, `SIGKILL` (Android's own
stop and restart of the service) end the plugin with no `STOP` and no stop code. So what a plugin must not lose it
writes as it goes too (a forwarder's cursor every few seconds, as the bucket allows), not only as it stops.

## Files (capability `files`)

The owner uploads items on the plugin's card of the settings page; the plugin only reads. A manifest setting of type
`files` says what an item is made of (files by suffix, grouped by the stem of their names) and which fields the
owner can set per item. `FILE_OPEN` answers `OK` with a read-only descriptor of a regular file: `read` and `lseek`
work, it cannot be mapped.

A new upload is first put to the plugin: `FILE_CHECK { list, item }`, and the plugin opens its files as any item's
and answers with the same id, within 5 s: `FILE_OK` with a title for its row (letters, digits and spaces; 40 are
kept) and start values for its fields, or `FILE_BAD` with a reason that is shown to the owner as text. Only then is
the item kept. Uploaded while the plugin was down, an item is listed with `checked` 0 and put to the plugin when it
next runs.

## The wake word stream (capability `wake`)

A plugin with `wake` is a wake word engine. It may have, besides it, only `files` and `storage`: no way out of the
Echo.

- `WAKE_WORDS { list, ids }`: the items it can listen for. hassmic names them to Home Assistant by the titles they
  got at upload.
- `WAKE_OPEN { slot, word }`: listen for this word in slot 0 or 1. The first one carries a descriptor: the stream.
  Answer `WAKE_READY` or `WAKE_FAILED` within 5 s.
- The stream is a `SOCK_SEQPACKET` socket. Each packet: `u64` position (samples since hassmic's start), then one
  block of 16 kHz mono signed 16-bit little-endian samples, as hassmic's own engine gets it (after Amazon's front
  end, no gain), `WAKE_SAMPLES_MAX` (4096) samples at most: hassmic cuts a longer capture block into several, each
  with its own position, so a reader's buffer of `8 + 2 * WAKE_SAMPLES_MAX` bytes takes every packet whole (a
  `SOCK_SEQPACKET` read into less cuts the packet and loses its rest). A position that is not the last one plus its
  count means blocks were lost (the plugin fell behind: the socket holds about 2 s) or the Echo was muted: start the
  detector anew.
- `WAKE_HEARD { slot, score, begin, end }`: the word lies between these positions. hassmic drops a detection whose
  slot is not open, whose `end` is ahead of the stream or more than 300 ms behind it, that is shorter than 0.3 s or
  longer than 1.5 s, or that comes less than a second after the slot's last one. Twenty accepted in ten minutes and
  the plugin is taken as broken and started anew.

## Storage (capability `storage`)

What the plugin keeps on the Echo for itself: a value per key, the broker writes it. Keys are 1 to 48 of `a-z`,
`0-9`, `.`, `_`, `-`, not beginning with a dot and not ending in `.tmp` and digits; a value is up to 16384 bytes, any
bytes (`bytes` on the wire: a `u16` length and the bytes, NUL among them). All values together stay within the kB the
manifest asks for (`"storage": <kB>`, 1 to 1024) and 128 keys.

- `STORE_GET { key }`: answered `STORE_VALUE { value }` with the request's id, or `ERR NOT_FOUND`.
- `STORE_PUT { key, value }`: `OK`; `ERR LIMIT` over the kB or the keys (nothing written); `ERR BUSY`: written too
  often. The Echo's flash takes ten writes, then one a minute (a write is a file and its folder synced). The C
  client keeps the latest value of a key it was told BUSY for, up to 8 keys, and writes it when it may
  (`hm_store_put` returns 1; -1 with `HM_ERR_BUSY` when it has no room to keep it; a kept value the Echo then
  refuses for good is said in the plugin's log). After `STOP` it writes at once or not at all (-1 `HM_ERR_BUSY`),
  and what it kept for later is tried once more as `hm_run` ends, the rest said in the log.
- `STORE_DEL { key }`: `OK` or `ERR NOT_FOUND`; a write as `STORE_PUT` is.

It stays when the plugin is updated, goes when it is removed (and when an update drops `wake`: once no process of
the version with `wake` runs, before the new one starts), and is copied to another Echo only when the owner picks it
on "Copy plugins" (sealed for the way, and never from a wake word plugin to one that is not). These may be asked
before `READY` too. A process still on its way out after an update writes its plugin's storage until it is gone (what
it writes in its stop code is what the next version starts with; where the update drops `wake`, the wipe comes after
it, before the next version starts). Once the plugin was removed, a process still on its way out is refused (`ERR
DENIED` for storage, `ERR FAILED` for `FILE_OPEN`): the same plugin installed again a moment later starts empty.

## The log (capabilities `log.read`, `log.read.full`)

`LOG_FOLLOW { from }` (`OK`, or `ERR BAD` for a cursor that is none) and from then on `LOG { cursor, gap, lines }`
messages with the log's lines, whole and in order, each with its time in front (`2026-11-21 10:00:00.123Z`, or
`boot+123.456` while the clock is not set) and no line end. A `LOG` that arrives before the `OK` is of the place
followed before (the broker sends both on one thread): a client drops those once the `OK` came, and hands them on
as before when the answer is `ERR` (the old place goes on), as the C client does. `from`: a cursor of an earlier
`LOG` (keep it in storage: after a restart the plugin gets what was written meanwhile), `start` (the oldest line
kept) or empty (from now). `gap` 1: the log was rotated twice since that cursor, the lines between are gone (or may
be: a cursor taken in the Echo's very first log before it was 64 bytes long); also: the cursor did not point at a
line's start, and the lines begin at the next one (a cursor is read only from where a line begins: hassmic's own
always do). Up to 64 lines a message, about 24 kB.

Flow control: every `LOG` has an id, and the plugin must answer `OK` with that id once it has handled its lines (the
C client: after the `log` callback returned). The next `LOG` comes only after that `OK`. It is required: a client
written without flow control gets one `LOG` and no
more; hassmic says so in its log and on the plugin's card when a `LOG` has had no `OK` for 10 s ("does not answer
LOG"). So what a slow plugin has not
handled yet waits in the log on the Echo, never in the plugin's memory: a client that keeps what comes while one of
its requests waits holds one `LOG` at most. A plugin that never answers gets no second `LOG` (and loses nothing: the
place stays where it was). A `LOG_FOLLOW` begins anew: no `OK` of the old place's `LOG` is waited for. A plugin that
does not read at all loses nothing that is still kept either: the broker sends from the same place again once it
reads, and holds `LOG` back while a `CONNECT` of the plugin waits for its answer.

What a plugin gets is less than the page shows: never a line hassmic marks as private (what was said to the Echo,
what it answered, names of networks, Bluetooth devices, timers and servers, media addresses, Home Assistant's error
texts) or as sealed (a wake word plugin's), of the system's own lines only its events (updates, resets, restarts,
the firewall's alarms), MAC and Bluetooth addresses blanked (`**:**:**:**:**:**`), the Sendspin pairing token
blanked. The marks are put where a line is written, and a line with one anywhere in it is left out whole. Its own lines come back to it: a
forwarder leaves out lines that begin `plugin <its id>: ` or makes a loop.

`log.read.full` is the same messages with what the settings page shows: every line as it is, the private
ones with their mark (`(private) transcript: ...`), the system's lines whatever they say, addresses as they are; the
Sendspin pairing token blanked, and still never a wake word plugin's line (sealed, or the lines that name a wake
word plugin installed now). Only a plugin the plugin index lists as official may have it: hassmic refuses any other
at install and does not start one that is no longer listed so (the index refuses a community plugin that asks for
it). `LOG_FOLLOW` takes either capability; a plugin with both gets the whole log.

## The network (capabilities `net.server`, `net.lan`, `net.internet`)

A plugin makes no socket. The manifest names the servers it may reach, each a host row the owner fills in:

```json
"capabilities": { "net.server": [ { "setting": "server", "proto": "udp", "ports": "setting:port" },
                                  { "setting": "server", "proto": "tcp", "ports": [ 6514 ] } ] },
"settings": [ { "name": "server", "label": "Syslog server", "type": "host" },
              { "name": "port", "label": "Port", "type": "int", "min": 1, "max": 65535 } ]
```

One to four entries, each with exactly `setting` (a row of type `host`), `proto` (`tcp` or `udp`) and `ports` (a
list of 1 to 8 ports, never 5555, or `"setting:<row>"` naming an `int` row whose `min` is at least 1 and `max` at
most 65535). Neither row has a `default`: a manifest with one there is refused, and hassmic connects only to what
the owner stored (a server named by the plugin's author is no server the owner entered). The install line names
them: "Connects to the server you enter in "Syslog server" (UDP, the port you enter in "Port")".

`CONNECT { kind, entry, host, port }`: `kind` the capability's bit (5, `CAP_NET_SERVER`), `entry` the entry's place
from 0, `host` empty (or exactly what the owner entered), `port` 0 (the entry's only port, or the one entered) or one
the entry lists. Only once the plugin runs (after `READY`). hassmic resolves the owner's host (no lookup of the
plugin's own: there is none), refuses loopback, the Echo's own addresses, an address the Echo routes to itself or
not out of an interface, 0.0.0.0/8, multicast, broadcast and port 5555 whatever the entry says, connects (10 s for all
of it, the lookup and every address the name has: a lookup that does not answer in that time is given up, `FAILED`;
UDP connected, so `send` reaches that server only), and answers
`OK` with the descriptor, which is the plugin's to keep and close: hassmic keeps no copy. It arrives non-blocking (and `F_SETFL` is refused: poll before reading or
writing). Errors: `DENIED` (not that entry's host or port, or an address no plugin may reach), `UNSET` (no server
entered), `NOT_FOUND` (its name has no address), `FAILED` (no connection), `BUSY` (two under way, or thirty in the
last minute, or its name's lookup not begun: two of the plugin's own still under way, which a name nobody answers
holds until the resolver gives up, 20 s and more; or the Echo's at its bound, which keeps places for the servers
owners entered), `BAD` (before `READY`). A plugin's requests are worked on one at a time, each beside every other
plugin's: its own slow server delays its own next request, nobody else's. A refusal of what the plugin may not do (`DENIED`) is said in hassmic's log
once in 10 s, the others counted into the next line. A server outside the owner's network is reached because the owner entered it.
When the owner changes or clears a row an entry names, hassmic starts the plugin again: what it held goes with it. On the Echo the plugin's own
user owns no socket at all: the firewall drops whatever it would send.

`net.lan` and `net.internet` are where the plugin names the address itself, at the ports its manifest
lists:

```json
"capabilities": { "net.lan": [ { "proto": "tcp", "ports": [ 80, 443 ] }, { "proto": "udp", "ports": "any" } ],
                  "net.internet": [ { "proto": "tcp", "ports": [ 443 ] } ] }
```

One to four entries each, with exactly `proto` (`tcp` or `udp`) and `ports` (a list of 1 to 8 ports, never 5555, or
`"any"`: every port but 5555; the plugin index flags a community plugin with it). `net.lan` never at DNS's ports 53
and 853, TCP or UDP, listed or under `"any"`: the home's router answers there by asking the internet for whatever
name it is sent, a way out of "your network" (the manifest is refused, a `CONNECT` there `DENIED`; `UDP_PEER`,
`SENDTO` and `net.discover`'s port the same). A device on the network that passes traffic on by other means (a
proxy, a router's own services) can still carry what a plugin sends further: the owner is told so where it matters
(the second press of a plugin that hears requests). The install lines name them:
"Connects to any device on your network (TCP, ports 80, 443; UDP, any port)", "Connects to the internet (TCP, port
443)". `CONNECT { kind, entry, host, port }` with `kind` 6 (`CAP_NET_LAN`) or 7 (`CAP_NET_INTERNET`), `entry` that
capability's entry from 0, `port` 0 only for an entry of exactly one port. `host`, for `net.lan`: an IPv4 address
written `a.b.c.d`, private or link-local (10/8, 172.16/12, 192.168/16, 169.254/16), never a name: hassmic looks
nothing up for it, so nothing a plugin of the network chooses goes into a DNS query; anything else is `DENIED`
before a socket is made. For `net.internet`: a host name or an IPv4 address on the internet; hassmic resolves it (10
s for all of it, as above), and an address on the local network (private or link-local), written so or what the name
resolves to, is `DENIED`: the network is `net.lan`'s, asked for and shown on a line of its own, and a plugin that
wants both asks for both. Whatever the kind: never loopback, the Echo's own addresses, one the Echo routes to itself,
0.0.0.0/8, multicast, broadcast or port 5555 (`DENIED`). `BAD`: no host, a host that is no name or address, port 0
for an entry of several. `net.lan` gets a socket the Echo's firewall holds to the local network, `net.internet` one
that may leave it. The same bounds as `net.server`'s: two under way, thirty in a minute, `BUSY` past them. A
plugin with `request.audio` and either of these is installed only with two presses of the action button (below),
and the plugin index lists none with `request.audio` and `net.internet` but an official one. hassmic's lines about
where a plugin that hears requests or calls (`call`) connects are private (no `log.read` plugin gets them).

## Music (capabilities `audio.music`, `audio.volume`)

A plugin with `audio.music` is a music source beside Sendspin and a phone over Bluetooth: the newest one plays, and
the others are told to pause.

- `MUSIC_OPEN { rate, channels }` (8000, 11025, 16000, 22050, 24000, 32000, 44100 or 48000 Hz; 1 or 2 channels): `OK`
  with a stream socket (`SOCK_STREAM`, non-blocking; `F_SETFL` is refused, so poll before writing). What is written
  into it, s16le at that rate, plays on the Echo's speaker as music: made stereo, at the music's volume, turned down
  while the voice assistant listens and answers (the setting "Music under the assistant"), into the light ring's music
  effects. hassmic reads it at playback speed, 40 ms at a time: the socket's buffer (32 kB) is the plugin's
  back-pressure ("would block" is full). Opening one makes the plugin the newest source. `ERR BUSY`: one is open;
  `LIMIT`: ten in a minute, or no room for another music plugin (four at once); `BAD`: a rate or channel count it
  does not take, or before `READY`. A second plugin that opens a stream takes the speaker: the first one's stream
  ends (its writes fail with `EPIPE`).
- `MUSIC_STATE { state, title, artist }` (`PLAYING`, `PAUSED`, `STOPPED`; title and artist are shown on its card):
  what its player does now. The action button, "<wake word>, stop" and Home Assistant's media player go by what it
  said last; `PLAYING` after something else makes it the newest source again, and what it writes while it says
  anything else, or after another source took over, is read at the stream's rate and dropped (write no faster than
  that: the socket fills as while it plays). Write it in pieces of 5 ms or more: after a smaller one the stream is
  read again 5 ms later at the earliest, playing or not. An open stream counts as playing until it says otherwise. Title and
  artist are cut where they stop being UTF-8. No answer unless it has an id; five a second.
- `MUSIC_CMD { cmd, why }`: `PAUSE`, `RESUME`, `STOP` (Home Assistant's media player), `NEXT`, `PREVIOUS` (the button
  pressed twice or three times), because of `BUTTON`, `STOP_WORD`, `OTHER_SOURCE` (another source started, or a Drop
  In call) or `HOME_ASSISTANT`. The player does it and says so with `MUSIC_STATE`. The button pauses what plays and
  resumes what it or a stop paused (within 30 minutes), as for Sendspin. hassmic does not wait for it: from a
  `PAUSE` or `STOP` on, what it writes is not heard, whether it obeys or goes on saying `PLAYING`, until hassmic
  resumes it (`RESUME`: the button's second press, Home Assistant's play), it starts anew (`PAUSED` or `STOPPED`, then
  `PLAYING`), or, for one paused for another source, every other source is off again.
- `MUSIC_POS { frames, at, queued }`, five a second while a stream is open: frames that reached the speaker (written,
  less what is queued and the output's own latency), when (`CLOCK_MONOTONIC`, µs), and what is queued (µs).
- `MUSIC_CLOSE`: the stream ends once what was written has played. `OK`.
- With `audio.volume`: `VOLUME_SET { percent }` sets the Echo's volume as its keys do (five a second and twenty a
  minute, then `BUSY`; a burst sets it at once and then to its last value, 500 ms apart at most, unless the volume
  is set elsewhere in between: then that stands, and `VOLUME` says so);
  `VOLUME { percent }` after `READY` and whenever the volume moves, from wherever.

Home Assistant's media player for the Echo offers pause and play while a music plugin is installed and switched on,
and its state is the newest plugin source's (playing, paused) while Home Assistant's own media does not play.

## The Echo's state (capability `state`)

`STATE { state, muted, volume }` after `READY` and whenever any of it changes: `IDLE`, `LISTENING`, `THINKING`,
`SPEAKING`; whether the microphones are off (the button's latch or Home Assistant's switch); the volume. Never what
was said.

## UDP of its own, the search, proposals (capabilities `net.udp`, `net.discover`)

```json
"capabilities": { "net.udp": 2, "net.discover": { "udp": 3483, "to": "broadcast" },
                  "net.server": [ { "setting": "server", "proto": "tcp", "ports": [ 3483 ] } ] }
```

`net.udp` is a count of UDP ports (1 to 4), `net.discover` the port its search goes to and where: `"broadcast"`
(every network the Echo is on) or a multicast group (224.0.0.0/4, written a.b.c.d). The plugin's ports are in
28940-28999, the same ones at every start. The search's port is none of hassmic's own (2330, 28928-28999: refused at
install), and while the plugin runs its socket is reachable from the network as a net.udp port is (its install's
line says so). A multicast search does not come back to the Echo's own sockets; a broadcast comes back to whatever
the Echo itself has on that port, which the firewall drops (from a plugin's port and the Echo's own address).

- `UDP_OPEN { kind, entry }`: `kind` 8 (`CAP_NET_UDP`, `entry` 0 to its count - 1) or 9 (`CAP_NET_DISCOVER`, entry
  0, the search's socket); answered `UDP_OPENED { handle, port }` with the socket. Only private and link-local
  senders reach it: the socket drops anything else itself, and on an Echo the firewall does before. `ERR DENIED`: a
  capability or an entry it has not; `BUSY`: open already; `LIMIT`: no port left; `BAD`: before `READY`.
- `UDP_PEER { handle, address, port }` (a net.udp socket; `address` an IPv4 as `a << 24 | b << 16 | c << 8 | d`):
  `send` reaches that address and port only, and only it is heard. A private or link-local unicast address that the
  Echo routes out of its network interface, never the Echo's own (by an interface or a "local" route), a network's
  broadcast, loopback, port 5555 or DNS's 53 and 853 (`DENIED`). A new peer once a second, two at once (`BUSY`).
- `SENDTO { handle, address, port, data }`: one datagram from a net.udp socket to such an address, not waited for,
  50 a second (a peer fixed with `UDP_PEER` sends past hassmic). `sendto` with an address of the plugin's own is
  refused by the filter.
- `BROADCAST { handle, data }`: the search, from its socket to the manifest's port at each network's broadcast
  address or the group, once a second. The answers come to that socket.
- `PROPOSE { setting, value }` (with `net.server`): what it found, for a host row one of its net.server entries
  names, a private or link-local IPv4 address as `UDP_PEER` takes them: shown on its card ("Found 192.168.1.20 ...
  Use it?"), written only by the owner's click (the plugin then starts again, as for any change of its server; the
  click asks the same again first). One a row a minute (`BUSY`); `DENIED` for another row or address.
- `NET { address, mask, up }` to a plugin with any net capability, after `READY` and when the Echo's address changes.

## Home Assistant (capabilities `ha.entities`, `ha.actions`)

```json
"ha.entities": [ { "name": "position", "kind": "sensor", "label": "Position", "unit": "s", "icon": "mdi:timer" },
                 { "name": "shuffle", "kind": "switch", "label": "Shuffle" },
                 { "name": "level", "kind": "number", "label": "Level", "min": 0, "max": 10, "step": 1 },
                 { "name": "mode", "kind": "select", "label": "Mode", "options": [ "a", "b" ] },
                 { "name": "remote", "kind": "event", "label": "Remote", "types": [ "press", "hold" ] } ],
"ha.actions": [ { "name": "search", "label": "Search for the server", "page": true },
                { "name": "play", "label": "Play", "args": [ { "name": "url", "type": "string" } ] } ]
```

Kinds: `binary_sensor`, `sensor` (`unit`, `decimals` 0..6), `text_sensor`, `switch`, `button`, `number` (`unit`,
`min`, `max`, `step`), `select` (`options`), `event` (`types`); `name` `[a-z0-9_]{1,32}`, `label`, `icon` ("mdi:..."),
`page` (below), and no other key; no two entities and no two actions with the same label. 16 entities and 8 actions, 4 arguments
each (`string` or `int`); `page`: a button on the plugin's card too, for an action without arguments. In Home
Assistant an entity is named "Plugin <length of the plugin id> <plugin id>: <label>" ("Plugin 9 testmusic:
Position"; Home Assistant keeps it by that name, and its entity id, `sensor.<node>_plugin_9_testmusic_position`, is
never one of hassmic's own or another plugin's), object id `p<length of the plugin id>_<plugin id, - as _>_<name>`;
an action is `esphome.<node>_p<length>_<plugin id, - as _>_<name>` (`p9_testmusic_search`). Its key is given once
and kept, so Home Assistant keeps the entity over restarts and updates. Listed while the plugin is switched on and
plugins can run, whether it runs or not; while it does not, its states are unavailable. Two labels of one plugin
that Home Assistant makes one entity id of ("Temp", "temp!") get it as any device's do: the second with "_2".

- `ENTITY_STATE { name, value }`: an entity's state as text (`on`/`off`, a number, within a number's limits, one of
  the options, a text of up to 255 bytes of strict UTF-8: no overlong form, no half of a surrogate pair, nothing past
  U+10FFFF); `EVENT { name, type }`: an event entity fires one of its types. Ten a second for all of them; no answer
  unless it has an id; a value it cannot take is said in hassmic's log.
- `ENTITY_SET { name, value }`: Home Assistant set a switch (`on`/`off`), a number, a select, or pressed a button
  (empty). Answer with `ENTITY_STATE` once it is so.
- `ACTION { name, from, token, args }`: one of its actions, from Home Assistant (1) or the card's button (2), its
  arguments in the manifest's order (an int as its digits, a string as given: an empty one is empty).  `token`: a
  call token where the manifest marks the action `"calls": true` (it needs the capability `call`), else
  empty; it places a call going out and nothing else ("Calls" below).

Home Assistant's commands and actions come only over the keyed link (the key Home Assistant sets at adoption).

**Rows of its own on its card.** An entity marked `"page": true` (true or false, nothing else; any kind with a
state, not a `button` or an `event`) is also a row on the plugin's card on the settings page: its label, and its state
as Home Assistant has it (a switch or a binary sensor "On"/"Off", a sensor to its `decimals` with its `unit`, a number
with its `unit`, an option or a text as it is; "Nothing said yet" before the first `ENTITY_STATE`), read-only, updated as
the card is (every few seconds while the page shows it).  Shown only while the plugin runs and Home Assistant lists
it.  No message of its own: `ENTITY_STATE` is the row's too.  It shows the owner what Home Assistant is told anyway, so
`page` asks for nothing at install and an update that adds or removes it needs no button.  It is part of
`ha.entities` 1.

Everything a plugin says that is shown (states, titles, a reason it refuses a file, a field's value, its stderr in
the log) is held to strict UTF-8: a state or a field that is not is refused, a title cut, a log line or a reason
mended ("?" for each byte that is no character).

## Calls (capability `call`)

```json
"call": true,
"ha.actions": [ { "name": "call", "label": "Call", "calls": true, "args": [ { "name": "number", "type": "string" } ] },
                { "name": "hang_up", "label": "Hang up", "page": true } ]
```

The line at install: "Makes and takes calls: a call coming in is answered only at the Echo, one goes out only when
Home Assistant or the settings page asks. Hears the room during a call"; an action marked `calls` is said with
"(and have it place a call)".  One call on the Echo at a time, the plugin's or a Drop In between two Echos: whoever
finds it taken is answered busy.  hassmic shows the call as it shows a Drop In (the ring's light, the ringtone, the
comms sounds, on a Show the plugin's name beside the other side's, the music paused), and the person here answers and
ends it as one (the action button, "<wake word>, stop", "hang up" said, the screen).  Home Assistant sees a plugin's
calls only through the plugin's own entities.

- **Going out.** A call needs a person's word: a **token**, which hassmic puts into an `ACTION` the manifest marks
  `calls` (both ways an action arrives are a person's: Home Assistant's keyed link, an approved browser's button on the
  card).  32 hex digits, good once, within 10 s.  `CALL_OUT { token, who }`: `OK`, or `ERR DENIED` (no such token,
  spent, too old; the microphones are off), `BUSY`.  `CALL_RINGING_THERE` when the far side rings (for the log), then
  `CALL_LIVE {}` once the far side took it.  A token places a call going out and nothing else; the Echo shows that
  call (the ring's light, the screen) from `CALL_OUT` to its end.
- **Coming in.** `CALL_INCOMING { who }`: the Echo rings, 30 s at most.  `ERR BUSY` (another call), `DENIED` (do not
  disturb, the microphones off), `LIMIT` (three rings of this plugin unanswered in the last ten minutes: no more
  until the window has moved on).  A ring refused `BUSY` costs a look of its own, one a second: asked again sooner it
  is `BUSY` without a look at the line.  It goes live only once a person answered it at the Echo: hassmic sends
  `CALL_ANSWER` (the button, the screen; the Echo's ringtone stops then), the plugin answers on the far side and says
  `CALL_LIVE {}` within 10 s.  Nothing else answers it: no token (`CALL_LIVE`'s token field is not looked at: send it
  empty), not Home Assistant, not the card.  `CALL_LIVE` for a call that still rings: `ERR DENIED`.  There is no
  automatic answer.
- **Live.** `CALL_LIVE`'s `OK` brings the call's audio socket (`SOCK_SEQPACKET`).  Each packet is 644 bytes: a `u32`
  sequence number (little-endian, from 0), then 20 ms of 16 kHz mono s16le.  From hassmic: the room's microphone
  (Amazon's call pipeline where the Echo has one, else the assistant's, after hassmic's gate, echo suppressor and
  gain), one packet every 20 ms, sent without waiting: a plugin that does not read loses packets (the numbers show
  the gap).  To hassmic: the far side's voice, at the call's pace (more than 100 packets a second are left in the
  socket; every packet counts, whatever its size); it plays on the Echo's call stream through a jitter buffer by the
  numbers, a missing frame the last one faded.  Number the frames from anything and one up each, as a `u32`: a frame
  more than 64 away from the newest, either way, is dropped, unless three such come in a row, each one after the
  other (within 8): then the buffer starts anew from there (a numbering begun again, a 16-bit RTP number's wrap: it
  costs ~100 ms).  A frame 32 or more behind the newest is too late and dropped.  A packet of another size is
  dropped.  No codec on either side: G.711, Opus, RTP and SIP are the plugin's.
- **The end.** `CALL_END { result }` from the plugin (any state: hung up, busy, declined, not answered, failed on the
  far side), or `CALL_HANGUP { why }` from hassmic (the button, spoken, the screen, not answered in time, no
  `CALL_LIVE` in time after an answer, the plugin stopped), after which the audio socket is at its end.  The plugin's
  process ending ends its call.
- Five call messages a second; none before `READY`.

The room reaches a plugin between `CALL_LIVE` and the end and at no other time.

## A request's copy (capabilities `request.audio`, `request.text`)

```json
"request.audio": true, "request.text": true
```

The lines at install: "**Hears everything said to this Echo after the wake word or the button**" and "Reads what was
said to this Echo and what it answered".  A request is Home Assistant's: a plugin gets a copy, one way, and nothing it
does can delay, change, end or start one, put audio or text into one, or answer it (the protocol has no message for
any of that: `REQ_START`, `REQ_END` and `REQ_TEXT` are hassmic's, and a plugin that sends one is answered `ERR BAD`).

- **`REQ_START { run, source, slot, phrase }`** (`request.audio`): a request begins on this Echo, as hassmic opens the
  microphone for it: the wake word heard here (`source` 1, its `slot`), the action button (2), a follow-up Home
  Assistant asked for (3), Home Assistant's own wake word with `-w remote` (4); `phrase` is the wake word's phrase
  Home Assistant gets.  `run` numbers it.  It comes with **the request's own audio socket** (`SOCK_SEQPACKET`, one per
  request: the plugin closes it once it read its end).  Each packet: a `u32` run, a `u32` offset (samples since the
  run's first), then one block exactly as Home Assistant got it, 16 kHz mono s16le, `REQ_SAMPLES_MAX` (2048) samples
  at most: the same bytes, in the same order, the lead-in's 800 ms of zeros first, after the same processing (the
  wake sound's removal, noise reduction, the gain).  A request's start comes at once (the lead-in and the replay from
  the wake word on, up to 4.8 s); the socket holds about 6 s (less where the kernel's `net.core.wmem_max` is lower:
  hassmic's log says so), so a plugin that reads as it gets `REQ_START` loses nothing.  Sent without waiting: a
  plugin that does not read loses blocks, the next offset shows the gap, and Home Assistant loses nothing.  hassmic closes its
  end after the last block: what is in the socket is the plugin's to read, then its end.
- **`REQ_END { run, why, samples, dropped }`**: the request's audio ended (the microphone off): Home Assistant ended it
  (1: its VAD, or its run), the end heard on the Echo (2), cancelled (3: the button, "stop", a new wake word), the
  pipeline's 30 s (4), an error (5), the link lost (6), the microphones muted (7: the button's latch or the soft mute).
  `samples` is what Home Assistant got of it, `dropped` what this plugin did not.  Every block of the run was sent
  before it.
- **`REQ_TEXT { run, kind, text }`** (`request.text`): what speech to text understood (`kind` 1) or the assistant's
  answer (2), as Home Assistant reports them (Wyoming tells only the words), UTF-8, 1024 bytes at most, with the number
  of the request it belongs to (the last one begun: the reply comes after the microphone went off).  Only a
  request's: with `-w remote` a run streams for Home Assistant's wake word before it is one, and what Home Assistant
  says of such a run is passed to no plugin.  Also without `request.audio`: then there is no `REQ_START` and no
  socket.  A "stop" or a bare wake phrase that the Echo acted on itself is not passed on.

Only the Echo that streams a request has one: of several Echos that heard the wake word only the one that answers.
With `-w remote` hassmic streams to Home Assistant all the time; the copy starts only at Home Assistant's wake word.
Nothing comes outside a request, while the microphones are muted (no request begins then, and none that was asked for
before the mute), or to a plugin that was not running (`READY`) when the request began.  A plugin with `request.audio` and any `net.*` is installed only with **two presses** of the
action button, the second on a screen of its own ("This plugin hears every request and can send what it hears to
...").  No plugin with `request.audio` runs while the Echo's wake word engine is a plugin the index does not list as
official, or one it withdrew.  A plugin's lines in the log, and hassmic's lines about it, are private while it has
either capability: no `log.read` plugin gets them.

## The buttons (capability `button`)

```json
"button": true
```

The line at install: "Hears presses of this Echo's buttons, as Home Assistant's "Button" does".  `BUTTON { key, how }`
comes for every press and hold Home Assistant's "Button" event entity hears, at the same moment: `key` the action button
(1), volume up (2), volume down (3); `how` pressed (1: let go within a second) or held a second (2: still down; a key
held longer is one hold).  The same exclusions as Home Assistant's: no press that served a wait for the button (a login
of the settings page, adb, a plugin's install, a withdrawn plugin run anyway, the community switch, a phone's setup
over Bluetooth), no hold that served the settings page's note, no skip of a double or triple press (the multi-press
setting).  The plugin only hears: the press does on the Echo what it does anyway (the assistant, pause, answer a
call), whatever the plugin does, and nothing it sends changes that; there is no answer.  Five a second, ten at once:
more are dropped and counted in hassmic's log.  Nothing before `READY`.  Not with `wake`: a wake word plugin has
`files` and `storage` beside it and nothing else (docs/plugins.md 3.3, the seal).

## The development flag

On a plugin's card the owner (or its author, on his own Echo) can switch on the development flag: a system call the
filter refuses then raises `SIGSYS` instead of answering `EPERM`, and the C client's handler writes "the sandbox
refused system call <number>" into the log before the plugin ends; the card says the sandbox stopped it. It allows
nothing more. On a runtime that makes refused calls at its start and lives with the refusal (Go's runtime probes
files) the plugin cannot start with it. `plugrun -T -D <program>` does the same on an Echo without hassmic.

## The manifest and the package

```json
{ "id": "microwakeword", "name": "microWakeWord", "version": 8, "version_name": "8",
  "requires": { "core": 1, "wake": 1, "files": 1 },
  "author": "...", "source": "https://...", "license": "...", "description": "...",
  "models": ["*"], "memory_mb": 16, "threads": 2,
  "capabilities": { "wake": true, "files": 4 },
  "settings": [ { "name": "models", "label": "Wake word models", "type": "files",
                  "item": { "by": "stem", "files": [ { "suffix": ".tflite", "max_kb": 1024, "as": "model.tflite" },
                                                     { "suffix": ".json", "max_kb": 16, "optional": true } ] },
                  "max_items": 16,
                  "fields": [ { "name": "cutoff", "label": "Threshold", "type": "number", "min": 0.5, "max": 0.99, "step": 0.01 } ],
                  "catalog": { "label": "...", "base": "https://.../", "items": [ { "id": "okay_nabu", "title": "Okay Nabu" } ] } } ] }
```

- `id`: 3 to 24 of `a-z0-9-`. `version`: a whole number that only grows. `models`: codenames, or `"*"`.
- `requires` ("Versions" above): core and the scope of each capability, each with the least version the plugin
  speaks, and nothing else; read before anything a scope's later version may bring. A key a later version of a
  scope brought (the table "The manifest's keys" in "Messages") needs that version required. `plugcheck.py requires
  manifest.json` prints the one a manifest's capabilities start from (every scope at 1), and `pack` refuses a
  manifest whose `requires` is not of that form.
- `capabilities`: the names of `CAP_*` below in the manifest's spelling (`net.server`, `log.read` ...). `files`:
  megabytes; `storage`: kilobytes.
- `memory_mb` (8 to 96, 32 when left out) and `threads` (1 to 16, 4): the limits the launcher holds the plugin to. Up
  to 8 plugins run at once, and their `memory_mb` together within 128 MB (a quarter of the smallest Echo's memory, less
  where an Echo has less): one that finds no place waits for one, says so on its card ("Waits for a place") and starts
  when another stops. Ask for what the plugin needs, not for the most: it is what its place counts.
- `settings`: up to 32, of type `bool`, `int` (`min`, `max`, `unit`), `select` (`options`), `text`, `host`, `secret`
  or `files`; each with `name` (`a-z0-9_`), `label`, optional `help` and `default`. The owner writes them, the plugin
  reads them (`CONFIG`). A `secret` is never shown again once set.
- A `files` setting: `item.files` (suffix, `max_kb`, `optional`, `as`: the name it is kept under), `max_items`,
  `fields` (`text`, `number`, `int`), and an optional `catalog` of items the owner's *browser* can fetch as
  `<base><path or id><suffix>`. The Echo fetches nothing.
- `author_key` and `digest` are written by `plugcheck.py pack`.

A package (`.hmp`) is a plain tar of `manifest.json`, `manifest.sig`, `plugin`, `LICENSE` and, for a plugin of this
project's own, `listing.sig`. `python3 sdk/plugcheck.py pack manifest.json program author.key out.hmp` makes one;
`plugcheck.py elf program` checks the binary rules a file shows; `plugcheck.py keygen` makes an author's key. The
Echo checks everything again, pins the author's key at the first install, and asks for the action button for
a new plugin and for an update that asks for more than the owner accepted: a capability more, a larger number,
another entry in a capability's list or any other change of one (another port of `net.server`, `net.lan` or
`net.internet`, an entry gone or in another order), a new setting
of type `secret`, `host` or `text` or another label for one,
another `name` or `author`, a `requires` raised into a version that widens ("Versions"). The same or less installs without it, and so does an entity's `page` added or taken away (a row on the card is no power). `plugcheck.py show` and `pack` print the
author key's fingerprint (16 hex digits in fours): the settings page names it for a package without a listing,
so publish it where your users find the package.

## Building and trying

Any toolchain that makes such a binary: `zig cc -target arm-linux-musleabihf -static`, a musl cross gcc, Rust's
`armv7-unknown-linux-musleabihf` with `crt-static`, Go with `GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0`. In C,
compile `hmplugin.c` with your sources and include `hassmic_plugin.h`.

On a PC, hassmic's own tests run plugins built for the PC (`cc -static`) under `build/plugrun-host`, which applies
the filter's x86_64 translation: `tests/fake_plugins.py` and `tests/plugins/testwake.c` are a small plugin and
everything around it, `tests/fake_plugins_net.py` and `tests/plugins/testnet.c` a forwarder with storage, the log
and a server, `tests/fake_plugins_music.py` and `tests/plugins/testmusic.c` a music player with Home Assistant's
entities, a UDP port and a search, `tests/fake_plugins_button.py` and `tests/plugins/testbutton.c` the buttons and rows of
its own on the card. That shows what a runtime calls, not how it takes a refusal on an Echo's kernel: the run on a
device is the proof.

## Test vectors

`sdk/vectors/<MESSAGE>.vec`, one file a message, made by `tools/mkplugmsg.py` from `messages.def` and held to
`plugmsg.h`'s encoder and decoder by `make unit-plugmsg`: cases of every field and the packet each is, and packets a
decoder must refuse. A client in another language reads them to check its own encoder and decoder. Lines (`#` to
the end of a line is for people):

- `message <NAME> <number>`, `id <the header's id>` (the header's flags are 0);
- `<path> <type> <value>` per field, in the definition's order: `u8` to `u64` in decimal, `str` and `bytes` as hex
  (`-` for none; a `str`'s text follows after `#`), `list <count>`; an element's fields under `<list>[<i>].`, right
  after its count (on the wire elements come after the last field of the struct the list is in, in order);
- `packet <hex>`: the whole packet, header included. The first case, from `message` to its `packet`, is the sample:
  every field with a value of its own.
- `case <name>` begins another case, with its own `id`, fields and `packet`, for a message that has fields: `empty`
  (numbers 0, every string, bytes field and list empty), `long` (numbers at their largest, every string and bytes
  field at its bound in bytes, every list one element of the same), `full` (every list at its bound, its elements
  `full` again where the packet still fits in 32768 bytes, else `empty`; numbers 1, strings and bytes empty).
- `refuse <what> <hex>`: a packet the decoder must refuse, whole, header included. `what` says how it is wrong:
  `short` (seven bytes: no header), `truncated` (the sample less its last byte), `over:<path>` (that string or bytes
  field one byte over its bound), `nul:<path>` (that string with a NUL in it), `count:<path>` (that list one element
  over its bound, its elements `empty`); a path into a list is its first element's. All other fields as in the
  sample; and `trailing` (the sample and one byte more: a packet is exactly its layout). Refused means: the header
  does not read, or the fields do not.
- A message that grew (a field of a later version of its scope) has a case `v<N>` per earlier version, after the
  others: a line `version <scope> <N>`, then its fields and packet as a peer that speaks version N writes it.
- A field that holds an enum's values has one of version 1's in every case (`empty` its smallest, `long` its
  largest), and two more refusals: `enum:<path>` (a value the enum has in no version) and, for a value a later
  version brought, `later:<path>:<scope>:<N>` (refused by a peer that speaks that scope at N, the others at this
  hassmic's versions).
- `fit <path>:<scope>:<N> <hex> <hex>`: for a value a later version brought, per earlier version N a peer may speak
  with the message: the packet with that value (the `later:` refusal's), and the packet a sender sends that peer, the
  value's fallback for version N in its place and nothing else changed (`pm_fit`). A client that writes the message
  maps the value the same way; one that reads it never sees the first.
- `unsent <path>:<scope>:<N> <hex>`: the same for a value whose chain ends in none (`>-`) before version N: the
  packet with that value, which a sender does not send a peer of version N at all (`pm_fit` 1).

## Messages

<!-- messages: made by tools/mkplugmsg.py from messages.def -->

Scopes, with the version this hassmic has of each:

| Scope | Version | |
|---|---|---|
| `core` | 1 | what every plugin speaks: the greeting, its settings, READY, PING, STOP, the time, OK and ERR |
| `storage` | 1 | data of its own on the Echo |
| `files` | 1 | the files the owner uploads on its card |
| `state` | 1 | what the Echo does: idle, listening, thinking, speaking; mute; volume |
| `log.read` | 1 | hassmic's log without what was said, names and addresses |
| `log.read.full` | 1 | the log as the settings page shows it (official plugins only) |
| `net.server` | 1 | sockets to the server the owner enters |
| `net.lan` | 1 | sockets to addresses on the local network |
| `net.internet` | 1 | sockets to the internet |
| `net.udp` | 1 | UDP ports of its own |
| `net.discover` | 1 | a search of the network |
| `audio.music` | 1 | a music source |
| `audio.volume` | 1 | the Echo's volume |
| `call` | 1 | calls: a ring, the room's audio while one is live |
| `wake` | 1 | the microphone stream all the time; wake words |
| `request.audio` | 1 | a copy of each request's audio |
| `request.text` | 1 | each request's transcript and reply |
| `ha.entities` | 1 | entities in Home Assistant |
| `ha.actions` | 1 | actions Home Assistant calls |
| `button` | 1 | presses and holds of the Echo's buttons |

Constants: `MAX_PACKET` = 32768 (core), `WAKE_SAMPLES_MAX` = 4096 (wake), `REQ_SAMPLES_MAX` = 2048 (request.audio).

`ERR_*` (core): `UNKNOWN` = 1, `DENIED` = 2, `BAD` = 3, `BUSY` = 4, `LIMIT` = 5, `NOT_FOUND` = 6, `FAILED` = 7, `UNSET` = 8.

`MUSIC_*` (audio.music): `PLAYING` = 1, `PAUSED` = 2, `STOPPED` = 3.

`MCMD_*` (audio.music): `PAUSE` = 1, `RESUME` = 2, `STOP` = 3, `NEXT` = 4, `PREVIOUS` = 5.

`MWHY_*` (audio.music): `BUTTON` = 1, `STOP_WORD` = 2, `OTHER_SOURCE` = 3, `HOME_ASSISTANT` = 4.

`STATE_*` (state): `IDLE` = 0, `LISTENING` = 1, `THINKING` = 2, `SPEAKING` = 3.

`FROM_*` (ha.actions): `HOME_ASSISTANT` = 1, `PAGE` = 2.

`HANGUP_*` (call): `BUTTON` = 1, `SPOKEN` = 2, `SCREEN` = 3, `NOT_ANSWERED` = 4, `NOT_LIVE` = 5, `STOPPED` = 6.

`RESULT_*` (call): `HUNG_UP` = 1, `BUSY` = 2, `DECLINED` = 3, `NO_ANSWER` = 4, `FAILED` = 5.

`REFUSED_*` (core): `SCOPES` = 1, `STATE` = 2.

`REQSRC_*` (request.audio): `WAKE_WORD` = 1, `BUTTON` = 2, `FOLLOW_UP` = 3, `HA_WAKE` = 4.

`REQEND_*` (request.audio): `HA` = 1, `HEARD_HERE` = 2, `CANCELLED` = 3, `TIMEOUT` = 4, `ERROR` = 5, `LINK_LOST` = 6, `MUTED` = 7.

`REQTEXT_*` (request.text): `HEARD` = 1, `REPLY` = 2.

`WAKE_FAIL_*` (wake): `NO_MODEL` = 1, `BAD_MODEL` = 2, `MEMORY` = 3, `UNSUPPORTED` = 4, `OTHER` = 5.

`FILE_BAD_*` (files): `FORMAT` = 1, `UNSUPPORTED` = 2, `TOO_LARGE` = 3, `MISSING` = 4, `OTHER` = 5.

`BUTTON_KEY_*` (button): `ACTION` = 1, `VOLUME_UP` = 2, `VOLUME_DOWN` = 3.

`BUTTON_HOW_*` (button): `PRESS` = 1, `HOLD` = 2.

`CAP_*` (each the capability's own scope, from its version 1): `STORAGE` = 0, `FILES` = 1, `STATE` = 2, `LOG_READ` = 3, `LOG_READ_FULL` = 4, `NET_SERVER` = 5, `NET_LAN` = 6, `NET_INTERNET` = 7, `NET_UDP` = 8, `NET_DISCOVER` = 9, `AUDIO_MUSIC` = 10, `AUDIO_VOLUME` = 11, `CALL` = 12, `WAKE` = 13, `REQUEST_AUDIO` = 14, `REQUEST_TEXT` = 15, `HA_ENTITIES` = 16, `HA_ACTIONS` = 17, `BUTTON` = 18.

The manifest's keys, by where they stand (a key of a later version than the manifest requires refuses it):

| Where | Keys |
|---|---|
| `manifest` | `id` (core 1), `name`, `author`, `version`, `version_name`, `source`, `license`, `description`, `requires`, `author_key`, `digest`, `memory_mb`, `threads`, `models`, `capabilities`, `settings` |
| `setting` | `name` (core 1), `label`, `type`, `help`, `unit`, `min`, `max`, `options`, `default`, `item`, `max_items`, `fields`, `catalog` |
| `setting.item` | `by` (files 1), `files` |
| `setting.item.file` | `suffix` (files 1), `as`, `max_kb`, `optional` |
| `setting.field` | `name` (files 1), `label`, `type`, `min`, `max`, `step` |
| `net.server` | `setting` (net.server 1), `proto`, `ports` |
| `net.discover` | `udp` (net.discover 1), `to` |
| `net.lan` | `proto` (net.lan 1), `ports` |
| `net.internet` | `proto` (net.internet 1), `ports` |
| `ha.entities.binary_sensor` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page` |
| `ha.entities.sensor` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page`, `unit`, `decimals` |
| `ha.entities.text_sensor` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page` |
| `ha.entities.switch` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page` |
| `ha.entities.button` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page` |
| `ha.entities.number` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page`, `unit`, `min`, `max`, `step` |
| `ha.entities.select` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page`, `options` |
| `ha.entities.event` | `name` (ha.entities 1), `kind`, `label`, `icon`, `page`, `types` |
| `ha.actions` | `name` (ha.actions 1), `label`, `args`, `page`, `calls` |
| `ha.actions.arg` | `name` (ha.actions 1), `type` |

#### 1 `HELLO` (plugin → broker, `core` 1)

The first message, and the plugin's: every scope its client speaks, each at the highest version it has (its sdk/messages.def's).  The greeting never grows.

| Field | Type | |
|---|---|---|
| `sdk` | u32 | the client library's version, for the log |
| `scopes` | list (≤ 255) | the scopes, each once |
| &nbsp;&nbsp;↳ `name` | str (≤ 24) |  |
| &nbsp;&nbsp;↳ `version` | u16 | 1 or more |

#### 2 `WELCOME` (broker → plugin, `core` 1)

The answer to HELLO: where it runs, and the version of each scope spoken with it from here on: exactly its manifest's requires (hassmic sends it nothing of a later version, and takes nothing of one), which is also what it was granted (core and the scope of each capability the owner accepted).  The greeting never grows.

| Field | Type | |
|---|---|---|
| `clock_set` | u32 | 1: the wall clock is set (it comes from Home Assistant) |
| `model` | str (≤ 48) | the Echo's model, as Home Assistant shows it |
| `node` | str (≤ 64) | its node name |
| `name` | str (≤ 64) | its name |
| `zone` | str (≤ 64) | the time zone: a name or a POSIX rule, empty while unknown |
| `scopes` | list (≤ 255) | the scopes spoken, each once: core and those of its capabilities (what the owner accepted) |
| &nbsp;&nbsp;↳ `name` | str (≤ 24) |  |
| &nbsp;&nbsp;↳ `version` | u16 |  |

#### 3 `REFUSED` (broker → plugin, `core` 1)

The answer to a HELLO hassmic cannot serve; the plugin ends.  SCOPES: its client does not speak a scope at the version its manifest requires (HELLO named it lower, or not at all); STATE: a HELLO out of turn.  The greeting never grows.

| Field | Type | |
|---|---|---|
| `why` | u16, `REFUSED_*` | REFUSED_* |

#### 4 `CONFIG` (broker → plugin, `core` 1)

The plugin's settings, all of them: after WELCOME and whenever the owner changes one.

| Field | Type | |
|---|---|---|
| `settings` | list (≤ 32) |  |
| &nbsp;&nbsp;↳ `name` | str (≤ 32) |  |
| &nbsp;&nbsp;↳ `value` | str (≤ 512) | as text: a bool is 0 or 1 |

#### 5 `READY` (plugin → broker, `core` 1)

The plugin has its settings and runs.  Until then hassmic sends it nothing but CONFIG and FILES.

No fields.

#### 6 `PING` (broker → plugin, `core` 1)

Every 10 s; three unanswered and the plugin is killed.

No fields.

#### 7 `PONG` (plugin → broker, `core` 1)

The answer, with PING's id.

No fields.

#### 8 `STOP` (broker → plugin, `core` 1)

End now: 2 s, then SIGTERM.

No fields.

#### 9 `TIME` (broker → plugin, `core` 1)

The clock was set, or the time zone changed.

| Field | Type | |
|---|---|---|
| `clock_set` | u32 |  |
| `zone` | str (≤ 64) |  |

#### 10 `OK` (either way, `core` 1)

A request went through (the request's id).  A descriptor the request asked for comes with it.  From the plugin: a LOG handled (the LOG's id).

No fields.

#### 11 `ERR` (either way, `core` 1)

A request did not (the request's id).

| Field | Type | |
|---|---|---|
| `code` | u16, `ERR_*` | ERR_* |

#### 20 `FILES` (broker → plugin, `files` 1)

One list of uploaded items: after CONFIG and whenever the owner adds, edits or deletes one.

| Field | Type | |
|---|---|---|
| `list` | str (≤ 32) | the files setting's name |
| `items` | list (≤ 64) |  |
| &nbsp;&nbsp;↳ `id` | str (≤ 40) |  |
| &nbsp;&nbsp;↳ `checked` | u8 | 1: the plugin said FILE_OK to it |
| &nbsp;&nbsp;↳ `title` | str (≤ 64) | what FILE_OK named it |
| &nbsp;&nbsp;↳ `fields` | list (≤ 16) | the owner's values of the item's fields |
| &nbsp;&nbsp;↳ &nbsp;&nbsp;↳ `name` | str (≤ 32) |  |
| &nbsp;&nbsp;↳ &nbsp;&nbsp;↳ `value` | str (≤ 256) |  |
| &nbsp;&nbsp;↳ `files` | list (≤ 8) |  |
| &nbsp;&nbsp;↳ &nbsp;&nbsp;↳ `name` | str (≤ 64) | the file's suffix, as the manifest names it |
| &nbsp;&nbsp;↳ &nbsp;&nbsp;↳ `size` | u32 |  |

#### 21 `FILE_OPEN` (plugin → broker, `files` 1)

One file of an item: OK comes with a read-only descriptor of it.

| Field | Type | |
|---|---|---|
| `list` | str (≤ 32) |  |
| `item` | str (≤ 40) |  |
| `file` | str (≤ 64) | its suffix |

#### 22 `FILE_CHECK` (broker → plugin, `files` 1)

The owner uploaded this item: can the plugin use it?  5 s for FILE_OK or FILE_BAD (this id).

| Field | Type | |
|---|---|---|
| `list` | str (≤ 32) |  |
| `item` | str (≤ 40) |  |

#### 23 `FILE_OK` (plugin → broker, `files` 1)

The item is good.

| Field | Type | |
|---|---|---|
| `title` | str (≤ 64) | for its row on the page (a wake word model: the phrase) |
| `fields` | list (≤ 16) | start values of its fields, read out of the file |
| &nbsp;&nbsp;↳ `name` | str (≤ 32) |  |
| &nbsp;&nbsp;↳ `value` | str (≤ 256) |  |

#### 24 `FILE_BAD` (plugin → broker, `files` 1)

The item cannot be used; it is not kept.

| Field | Type | |
|---|---|---|
| `code` | u16, `FILE_BAD_*` | FILE_BAD_* |
| `why` | str (≤ 200) | shown as text |

#### 30 `WAKE_WORDS` (plugin → broker, `wake` 1)

The items the plugin can listen for: at start and whenever they change.  Ids only: the phrases are hassmic's, fixed when the owner uploaded each item.

| Field | Type | |
|---|---|---|
| `list` | str (≤ 32) | the files setting they are items of |
| `words` | list (≤ 16) |  |
| &nbsp;&nbsp;↳ `id` | str (≤ 40) |  |

#### 31 `WAKE_OPEN` (broker → plugin, `wake` 1)

Listen for this word in this slot.  The first one comes with the stream's descriptor.  5 s for WAKE_READY or WAKE_FAILED.

| Field | Type | |
|---|---|---|
| `slot` | u8 | 0 or 1 |
| `word` | str (≤ 40) |  |

#### 32 `WAKE_READY` (plugin → broker, `wake` 1)

The slot's word is loaded and listening.

| Field | Type | |
|---|---|---|
| `slot` | u8 |  |

#### 33 `WAKE_FAILED` (plugin → broker, `wake` 1)

The slot's word could not be loaded.

| Field | Type | |
|---|---|---|
| `slot` | u8 |  |
| `why` | u16, `WAKE_FAIL_*` | WAKE_FAIL_* |

#### 34 `WAKE_CLOSE` (broker → plugin, `wake` 1)

Stop listening in this slot.

| Field | Type | |
|---|---|---|
| `slot` | u8 |  |

#### 35 `WAKE_RESET` (broker → plugin, `wake` 1)

The audio has a gap (hassmic played something, the capture restarted): forget what was heard.

No fields.

#### 36 `WAKE_HEARD` (plugin → broker, `wake` 1)

The slot's word, between two positions of the stream.  Checked by hassmic: begin < end, end not ahead of the stream and at most 300 ms behind it, 0.3 to 1.5 s long, one a second.

| Field | Type | |
|---|---|---|
| `slot` | u8 |  |
| `score` | u8 | how sure, 0..255 |
| `begin` | u64 | samples, on the stream's clock |
| `end` | u64 |  |

#### 40 `STORE_GET` (plugin → broker, `storage` 1)

The value kept under a key: answered STORE_VALUE (this id), or ERR NOT_FOUND.

| Field | Type | |
|---|---|---|
| `key` | str (≤ 48) | [a-z0-9._-], 1 to 48, not beginning with a dot, not ending in .tmp and digits |

#### 41 `STORE_VALUE` (broker → plugin, `storage` 1)

The answer to STORE_GET.

| Field | Type | |
|---|---|---|
| `value` | bytes (≤ 16384) |  |

#### 42 `STORE_PUT` (plugin → broker, `storage` 1)

Keep this value under this key, in place of what was there.  OK; ERR LIMIT: over the accepted kB (or 128 keys); ERR BUSY: written too often (10 writes, then one a minute: the Echo's flash), try again later.

| Field | Type | |
|---|---|---|
| `key` | str (≤ 48) |  |
| `value` | bytes (≤ 16384) |  |

#### 43 `STORE_DEL` (plugin → broker, `storage` 1)

The key and its value away (a write as STORE_PUT is).  OK, or ERR NOT_FOUND.

| Field | Type | |
|---|---|---|
| `key` | str (≤ 48) |  |

#### 50 `LOG_FOLLOW` (plugin → broker, `log.read` 1 or `log.read.full` 1)

Send the log from here on (LOG events) until the plugin ends.  OK, or ERR BAD for a cursor that is none.  Asked again: from there instead.

| Field | Type | |
|---|---|---|
| `from` | str (≤ 40) | a cursor of a LOG before (after a restart: what was written meanwhile); start: the oldest line kept; empty: from now |

#### 51 `LOG` (broker → plugin, `log.read` 1 or `log.read.full` 1)

Lines of hassmic's log, whole and in order: under log.read without what was said, names and addresses; under log.read.full (official plugins only) as the settings page shows them, what was said included, but for a wake word plugin's lines and the Sendspin pairing token (sdk/PROTOCOL.md).  It has an id: the plugin must answer OK (that id) once it has handled the lines, and the next LOG comes only after that OK (flow control; unanswered for 10 s, hassmic says so).  A plugin that falls behind loses nothing that is still kept.

| Field | Type | |
|---|---|---|
| `cursor` | str (≤ 40) | where the next LOG begins: keep it to go on from there |
| `gap` | u8 | 1: lines between the cursor asked for and these are gone (the log was rotated twice meanwhile) |
| `lines` | list (≤ 64) |  |
| &nbsp;&nbsp;↳ `line` | str (≤ 1024) | one line, with its time in front; no line end |

#### 60 `CONNECT` (plugin → broker, `net.server` 1 or `net.lan` 1 or `net.internet` 1)

A socket by one of the capability's entries: OK comes with it, connected and non-blocking (TCP), or connected (UDP: send() reaches that address only).  The plugin's to keep and close; hassmic keeps no copy.  net.server: the server the owner entered; net.lan: an address on the local network the plugin names (private or link-local, a.b.c.d, never a name), never at DNS's ports 53 and 853; net.internet: an address or name on the internet the plugin names, never one on the local network (also not what a name resolves to).  Never loopback, the Echo's own addresses, multicast, broadcast, 0.0.0.0/8 or port 5555.  ERR DENIED: not that entry's host or port, not the kind's address, not the plugin's capability; UNSET: the owner has not entered the server; NOT_FOUND: its name has no address; FAILED: no connection (10 s for all of it); BUSY: two under way, or 30 in the last minute, or its name's lookup not begun (two of the plugin's own still under way, or the Echo's at its bound); BAD: before READY, no host for net.lan or net.internet, a host that is no name or address, port 0 where the entry has more than one.

| Field | Type | |
|---|---|---|
| `kind` | u8, `CAP_*` | the capability (CAP_NET_SERVER, CAP_NET_LAN, CAP_NET_INTERNET) |
| `entry` | u8 | the entry's place in that capability's list, from 0 |
| `host` | str (≤ 253) | net.server: empty, the server the owner entered (anything else must be that, as written).  net.lan: an IPv4 address, a.b.c.d.  net.internet: a host name or an IPv4 address |
| `port` | u16 | 0: the entry's (its only port, or for net.server the one in its port setting); else one the entry allows (any but 5555 for \"any\", for net.lan also but 53 and 853) |

#### 61 `UDP_OPEN` (plugin → broker, `net.udp` 1 or `net.discover` 1)

A UDP socket bound to a port of the plugin's own (28940 to 28999, the same at every start): answered UDP_OPENED (this id) with its descriptor.  It hears only private and link-local senders.  kind CAP_NET_UDP: one of the manifest's net.udp count (entry from 0); CAP_NET_DISCOVER: the socket that searches (entry 0), BROADCAST's.  ERR DENIED: not the plugin's capability or entry; BUSY: that one is open; LIMIT: no port left; BAD: before READY.

| Field | Type | |
|---|---|---|
| `kind` | u8, `CAP_*` | CAP_NET_UDP or CAP_NET_DISCOVER |
| `entry` | u8 |  |

#### 62 `UDP_OPENED` (broker → plugin, `net.udp` 1 or `net.discover` 1)

The answer to UDP_OPEN, with the socket (receive with recv or recvfrom; send with send once UDP_PEER fixed its peer, or by SENDTO).

| Field | Type | |
|---|---|---|
| `handle` | u8 | what UDP_PEER, SENDTO and BROADCAST name it by |
| `port` | u16 | the port it is bound to |

#### 63 `UDP_PEER` (plugin → broker, `net.udp` 1)

Fix the peer of a net.udp socket: send() reaches that address and port only, and only it is heard.  A private or link-local unicast address the Echo routes out of its network interface, never the Echo's own, loopback, port 5555 or DNS's 53 and 853.  A new peer once a second, two at once.  OK; ERR DENIED (the address), BAD (no such handle), BUSY (over the rate).

| Field | Type | |
|---|---|---|
| `handle` | u8 |  |
| `address` | u32 | IPv4: a.b.c.d as (a << 24) | (b << 16) | (c << 8) | d |
| `port` | u16 |  |

#### 64 `SENDTO` (plugin → broker, `net.udp` 1)

One datagram from a net.udp socket to an address as UDP_PEER allows them; sent without waiting (dropped when the socket's queue is full).  50 a second.  An answer only to an id that is not 0: OK, or ERR DENIED, BAD, BUSY (over the rate).

| Field | Type | |
|---|---|---|
| `handle` | u8 |  |
| `address` | u32 |  |
| `port` | u16 |  |
| `data` | bytes (≤ 1472) |  |

#### 65 `BROADCAST` (plugin → broker, `net.discover` 1)

The search: one datagram from the net.discover socket to the manifest's port, on every network the Echo is on (each one's broadcast address) or to the manifest's multicast group.  Once a second.  Answers come to that socket.  An answer only to an id that is not 0: OK, or ERR BAD, BUSY.

| Field | Type | |
|---|---|---|
| `handle` | u8 |  |
| `data` | bytes (≤ 1472) |  |

#### 66 `PROPOSE` (plugin → broker, `net.server` 1)

What the plugin found, offered to the owner for a host row its net.server names: shown on its card ('Found 192.168.1.20. Use it?'), written only by the owner's click.  A private or link-local IPv4 address as text; one per row at a time, a minute apart.  OK; ERR DENIED (not such a row, not such an address), BUSY (within the minute), BAD.

| Field | Type | |
|---|---|---|
| `setting` | str (≤ 32) |  |
| `value` | str (≤ 15) | a.b.c.d |

#### 67 `NET` (broker → plugin, `net.server` 1 or `net.lan` 1 or `net.internet` 1 or `net.udp` 1 or `net.discover` 1)

The Echo's address on its network, to a plugin with a net capability: after READY and whenever it changes.

| Field | Type | |
|---|---|---|
| `address` | u32 | as UDP_PEER's; 0 while it has none |
| `mask` | u32 |  |
| `up` | u8 | 1: on a network |

#### 70 `MUSIC_OPEN` (plugin → broker, `audio.music` 1)

Play: OK comes with a stream socket (SOCK_STREAM, non-blocking) into which the plugin writes s16le at this rate and these channels; what it writes plays on the Echo's speaker as music (ducked under the voice assistant, at the music's volume).  The newest music source wins: Sendspin and a phone over Bluetooth are paused.  Rates 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000; 1 or 2 channels.  ERR BAD: a format it does not take, BUSY: open already (MUSIC_CLOSE first), LIMIT: ten in a minute.

| Field | Type | |
|---|---|---|
| `rate` | u32 |  |
| `channels` | u8 |  |

#### 71 `MUSIC_STATE` (plugin → broker, `audio.music` 1)

What the plugin's player does now: the button, the stop word and Home Assistant's media player go by it.  PLAYING with a stream open makes it the newest source; title and artist go to its card.  An answer only to an id that is not 0.

| Field | Type | |
|---|---|---|
| `state` | u8, `MUSIC_*` | MUSIC_* |
| `title` | str (≤ 128) |  |
| `artist` | str (≤ 128) |  |

#### 72 `MUSIC_CLOSE` (plugin → broker, `audio.music` 1)

The stream ends: what was written plays out, then the output closes.  OK.

No fields.

#### 73 `MUSIC_POS` (broker → plugin, `audio.music` 1)

Where the stream is, five times a second while it is open: what a player needs to say where it is (and to keep a group in step).

| Field | Type | |
|---|---|---|
| `frames` | u64 | frames that reached the speaker |
| `at` | u64 | when, on the monotonic clock (CLOCK_MONOTONIC, microseconds) |
| `queued` | u32 | microseconds written and not played yet |

#### 74 `MUSIC_CMD` (broker → plugin, `audio.music` 1)

Do this to what plays: from the action button, the stop word, another source that started or a Drop In call (OTHER_SOURCE), Home Assistant's media player.

| Field | Type | |
|---|---|---|
| `cmd` | u8, `MCMD_*` | MCMD_* |
| `why` | u8, `MWHY_*` | MWHY_* |

#### 75 `VOLUME_SET` (plugin → broker, `audio.volume` 1)

Set the Echo's volume (as the keys do; a burst sets it at once and then to its last value, 500 ms apart at most; the last value is dropped if the volume is set elsewhere before it, and VOLUME says what it is): OK; ERR BUSY over five a second or twenty a minute.

| Field | Type | |
|---|---|---|
| `percent` | u8 | 0..100 |

#### 76 `VOLUME` (broker → plugin, `audio.volume` 1)

The Echo's volume moved, from wherever: after READY and on every change.

| Field | Type | |
|---|---|---|
| `percent` | u8 |  |

#### 80 `STATE` (broker → plugin, `state` 1)

What the Echo does: after READY and on every change of any of it.  Never what was said.

| Field | Type | |
|---|---|---|
| `state` | u8, `STATE_*` | STATE_* |
| `muted` | u8 | 1: the microphones are off (the button's latch or Home Assistant's switch) |
| `volume` | u8 | 0..100 |

#### 90 `ENTITY_STATE` (plugin → broker, `ha.entities` 1)

An entity's state, as text: a sensor's number, a binary sensor's or switch's on/off, a number within its limits, a select's option, a text sensor's text (255 bytes).  Ten a second for all of the plugin's.  An answer only to an id that is not 0: OK, or ERR BAD (no such entity, a value it cannot take), BUSY.

| Field | Type | |
|---|---|---|
| `name` | str (≤ 32) | the manifest's |
| `value` | str (≤ 255) |  |

#### 91 `EVENT` (plugin → broker, `ha.entities` 1)

An event entity fires one of its types.  Counted with ENTITY_STATE.

| Field | Type | |
|---|---|---|
| `name` | str (≤ 32) |  |
| `type` | str (≤ 32) |  |

#### 92 `ENTITY_SET` (broker → plugin, `ha.entities` 1)

Home Assistant set a switch, number or select of the plugin's, or pressed its button (value empty).  The plugin answers with ENTITY_STATE once it is so.

| Field | Type | |
|---|---|---|
| `name` | str (≤ 32) |  |
| `value` | str (≤ 255) |  |

#### 93 `ACTION` (broker → plugin, `ha.actions` 1)

One of the plugin's actions, called by Home Assistant (its keyed link) or by the owner's button on its card.

| Field | Type | |
|---|---|---|
| `name` | str (≤ 32) |  |
| `from` | u8, `FROM_*` | FROM_* |
| `token` | str (≤ 32) | call: a call token (32 hex digits) when the manifest marks the action \"calls\" (both of its ways in are a person's: Home Assistant's keyed link, an approved browser); else empty.  One use, for CALL_OUT only (a call going out), within 10 s; it answers no call coming in |
| `args` | list (≤ 4) | in the manifest's order |
| &nbsp;&nbsp;↳ `name` | str (≤ 32) |  |
| &nbsp;&nbsp;↳ `value` | str (≤ 255) | an int as its digits |

#### 100 `CALL_INCOMING` (plugin → broker, `call` 1)

A call comes in: the Echo rings (its ringtone, its light, on a Show the plugin's name beside WHO) until a person answers here (CALL_ANSWER) or it ends.  OK: it rings; ERR BUSY: another call is on (a Drop In, another plugin's, this one's), or a ring asked faster than its looks at the line allow: each ring refused busy costs one, one a second come back and two at most kept, and with none left it is refused busy without a look; DENIED: do not disturb, or the microphones are off; LIMIT: three rings of this plugin unanswered in the last ten minutes; BAD: before READY.

| Field | Type | |
|---|---|---|
| `who` | str (≤ 64) | the caller as the plugin names it (a number, a name): shown, never interpreted |

#### 101 `CALL_ANSWER` (broker → plugin, `call` 1)

A person here answered the ringing call (the action button, the Show's screen): answer it on the far side, then CALL_LIVE.

No fields.

#### 102 `CALL_HANGUP` (broker → plugin, `call` 1)

hassmic ended the call (any state): end it on the far side.  No CALL_END is needed after it.

| Field | Type | |
|---|---|---|
| `why` | u8, `HANGUP_*` | HANGUP_* |

#### 103 `CALL_OUT` (plugin → broker, `call` 1)

Place a call: only with a token an ACTION marked calls just brought (a person asked for it).  The Echo shows the call going out.  OK; ERR DENIED: no such token, used, older than 10 s, or the microphones are off; BUSY: another call is on; BAD.

| Field | Type | |
|---|---|---|
| `token` | str (≤ 32) |  |
| `who` | str (≤ 64) | whom it calls, as shown |

#### 104 `CALL_RINGING_THERE` (plugin → broker, `call` 1)

The far side rings (a call going out).  For the log.  An answer only to an id that is not 0.

No fields.

#### 105 `CALL_LIVE` (plugin → broker, `call` 1)

The call is connected on the far side: OK comes with the call's audio socket (SOCK_SEQPACKET; each packet a u32 sequence number, then 20 ms of 16 kHz mono s16le, 644 bytes: the room's microphone from hassmic, the far side's voice to hassmic).  An incoming call only once a person answered it here, at the Echo (CALL_ANSWER); no token answers one.  ERR DENIED: not answered here; BAD: no call of this plugin's in that state.

| Field | Type | |
|---|---|---|
| `token` | str (≤ 32) | not looked at (a token is for CALL_OUT only): send it empty.  Kept: a field of call 1's layout; read as every text field is, so one that is not a string (a NUL inside, longer than 32 bytes) is BAD |

#### 106 `CALL_END` (plugin → broker, `call` 1)

The call ended on the far side, or the plugin ends it: in any state.  OK.

| Field | Type | |
|---|---|---|
| `result` | u8, `RESULT_*` | RESULT_* |

#### 110 `REQ_START` (broker → plugin, `request.audio` 1)

A request begins: a copy of what Home Assistant is sent comes on the socket that comes with this message, until REQ_END.  Only the Echo that streams the request has one; with the wake word in Home Assistant (-w remote) from its wake word on.  Nothing can be sent back: the plugin hears, it never takes part.

| Field | Type | |
|---|---|---|
| `run` | u32 | the request's number, new with each (the audio's packets carry it) |
| `source` | u8, `REQSRC_*` | REQSRC_*: the wake word heard here, the action button, a follow-up Home Assistant asked for, the wake word heard in Home Assistant |
| `slot` | u8 | the wake word's slot (0 or 1) for REQSRC_WAKE_WORD, else 0 |
| `phrase` | str (≤ 64) | the wake word's phrase that goes to Home Assistant with the request |

#### 111 `REQ_END` (broker → plugin, `request.audio` 1)

The request's audio ended (the microphone off): the socket has its last block and then its end.

| Field | Type | |
|---|---|---|
| `run` | u32 |  |
| `why` | u8, `REQEND_*` | REQEND_*: Home Assistant ended it (it heard enough, or ended its run), the end heard here, cancelled (the button, stop, a new wake word), the pipeline's timeout, an error, the link lost, the microphones muted (the button's latch or Home Assistant's switch) |
| `samples` | u32 | samples the copy had (what Home Assistant got of it) |
| `dropped` | u32 | of them not sent to the plugin: its socket was full (it did not read in time) |

#### 112 `REQ_TEXT` (broker → plugin, `request.text` 1)

What speech to text understood of a request, or the assistant's answer, as Home Assistant reports them (Wyoming: the transcript only).  A bare stop or wake word that the Echo acts on itself is not passed on.

| Field | Type | |
|---|---|---|
| `run` | u32 | the request's number, as REQ_START's (a plugin without request.audio gets only this) |
| `kind` | u8, `REQTEXT_*` | REQTEXT_* |
| `text` | str (≤ 1024) |  |

#### 120 `BUTTON` (broker → plugin, `button` 1)

A button of the Echo was pressed or held a second: exactly what Home Assistant's \"Button\" event entity hears (the same moment, the same exclusions): never a press that served a wait for the button (a login of the settings page, adb, a plugin's install, a withdrawn plugin run anyway, a phone's setup over Bluetooth), never a hold that served the settings page's note, never a skip of a double or triple press.  The plugin only hears: what the button does on the Echo happens all the same, and nothing it answers changes that.  Five a second, ten at once: more are dropped, and the log counts them.

Marked `drop`: a peer whose version has no word for a value it holds (a value "not sent to a peer of an earlier version") gets none of it, not this message at all.  The only kind of message that is ever left unsent so.

| Field | Type | |
|---|---|---|
| `key` | u8, `BUTTON_KEY_*` | BUTTON_KEY_*: the action button, volume up, volume down |
| `how` | u8, `BUTTON_HOW_*` | BUTTON_HOW_*: pressed (let go within a second), held a second (still down) |

<!-- messages: end -->

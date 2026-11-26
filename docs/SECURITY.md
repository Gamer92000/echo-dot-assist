# Security

What protects an Echo running hassmic, and where that ends. Nobody has audited any of it; see the caution in the
[README](../README.md).

## Network

| Direction | What gets through |
|---|---|
| **Out** | Amazon's daemons: local addresses only, plus DNS to the servers DHCP hands out (so Amazon's names may still be looked up; on the Android models Android's captive portal test, which looked two of them up every few minutes, is set to off: measured on the Echo Show 5, not yet on the others, issue #189). `otad` and `ace_otad` (firmware updates): never. hassmic itself: anywhere, to fetch replies and music from where Home Assistant or Music Assistant point it. On the screen models the Kiosk Satellite app too, while "Kiosk Satellite internet access" is on (default; one rule for that app's uid, nothing else of Amazon's or the system's gets out through it). |
| **In, TCP** | 16384–32767 only: 26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates, 28931 settings page; on the screen models that name a Kiosk Satellite package (`KIOSK_PACKAGE`), also 2324 (the app's remote administration) and 6053 (its ESPHome API), from private addresses only; with "Kiosk Satellite DLNA renderer" on (off by default), also its DLNA renderer's port (the one the app's own setting names, so any the app picks), from private addresses only and only while the app listens there; the renderer has no login (DLNA has none) |
| **In, UDP** | 16384–32767 only (28930: arbitration between Echos, 28932: Drop In audio), and 2330 only while arbitration is in Kiosk Satellite mode (their fixed port); with "Kiosk Satellite DLNA renderer" on, 1900 (SSDP searches to 239.255.255.250, from private addresses) while the Kiosk Satellite app holds that port and nothing else on the Echo does (looked at every 5 s) |
| **In, ICMP** | ping (echo request) from private and link-local addresses only; answers to the Echo's own traffic |

**Kiosk Satellite**, where you install it, is an app you trust like adb. Its update helper runs the app's own code as the shell user, every few minutes, so the app and each update of it can do what an adb shell can, `pm grant` included. It also gets storage and location without asking. The "microphone" and "camera" switches are Android's permissions, no wall against the app itself. Setup and `scripts/kiosk-install.sh` check who signed the APK against a pinned certificate on the PC, before it reaches the Echo; a copy you install by hand and the app's own updates are not checked by this project. The helper's provider is exported with no manifest permission, so another app on the Echo might reach it: install nothing else you do not trust. Ports 2324 and 6053 are open only while the app is installed, its DLNA renderer's only while you switch that on and the renderer listens. Its page on 2324 asks for a login once the app's remote admin password is set (seen on 2026.10.45): set one. Beyond that its pages are the app's own, and this project has not reviewed them.

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
- A browser gets in only by a press of the Echo's action button while its login waits (or through another Echo of
  the same network it is approved on).
- **One thing waits for the button at a time, and a press is not turned from the wait it was armed for to
  another.** The same button lets a browser in, opens adb, accepts a plugin and (held for a second, not
  pressed) takes the note of a copied plugin off. Whatever waits for it was armed by one request, the ring shows
  which kind (a login or adb: the login's ring; a plugin or a note: its own), the page that asked says what the
  button will do, and no request puts another wait in its place. It ends by the press (a note's: the hold), by
  its minute, or by its own browser's signed word (adb's "off" withdraws only the wait of the browser that says
  it); a login also ends when a second browser that is not approved asks (below). And when a wait has ended, in
  whatever way, **no other browser that is not approved is taken for a login until whoever that wait sent to
  the Echo has pressed in vain**: 30 seconds after the press that ended it, or 30 seconds after the end of the
  minute its page promised; and 30 seconds after every press in that time that found nothing to serve (you
  pressed for a login that was contested meanwhile, or a moment too late, and press again). That
  goes for the browser whose own login just ran out as well, once somebody pressed: it is taken again at once
  only while nobody has. This holds over a
  restart of hassmic too (an update, a crash): the Echo keeps that time on its flash for as long as it has not
  been rebooted (told by the kernel's boot id; on an Echo where that cannot be read, by the clock alone, which
  after a reboot can keep new browsers waiting for up to two minutes for nothing). What **someone on your network who is not approved** can and cannot do with requests to log
  in:
  - *cannot* be approved by a press you meant for something else that waited. While adb, a plugin's install or a
    note (asked by an approved browser) waits, a stranger's login is not taken, nor in the time after it named
    above: a press that comes late, or twice, finds nothing of his. While a phone's setup over Bluetooth waits for
    the button, the same; and a login that was already waiting when the phone's wait began is not approved by the
    press either: that press is the phone's, the login ends and its page asks again.
  - *cannot* get the press that was on its way to another browser's login. Two browsers that are both not
    approved yet look alike to the Echo, so when a second one asks while the first one's login waits, the login is
    **contested**: the ring goes dark, the press approves nobody (it is an ordinary press), both pages say so, and
    no login is taken until 30 seconds after the later of the end of the first one's minute and the last time the
    two asked against each other. The same after a login ran out or was approved: nobody else's login for the
    next 30 seconds (the browser whose own login ran out may ask again at once). Your page keeps asking by itself
    while it is told to wait, so an open page of yours stays in the way of a stranger's: its asks contest a
    stranger's waiting login also when the count of requests (below) is used up.
  - *can* delay logins: a contested one for as long as he keeps asking and 30 seconds more (a minute and a half
    at least: longer than before 2026-11-20's second fix, which is the price of the rule above), and any new login
    by keeping one of his own waiting, minute after minute. He can hold up adb or a plugin's install once, for the
    minute his own login waits; the page that was told "a login waits first" asks again by itself when that login
    is over, and the Echo takes no stranger's login in the 30 seconds between.
  - *can* make the ring show a login that nobody at home asked for, whenever nothing else waits and none of the
    quiet times above runs: it is the same ring as for your own login, and **a press on it lets him in**. The
    Echo cannot know who stands in front of it. **Press for a login only while your own page says "Waiting…
    press the action button"**, and never for a ring you did not ask for. This is not closed by anything above.
  - *can* learn that something waits for the button ("busy"), not what.
  - His requests are counted (40 in 10 seconds from one address, 120 from all); beyond that they are refused and
    no login is taken for them. So three addresses that ask as fast as they may use up the count of all, and
    **no new login is taken while they keep that up**. The requests of the browser whose login waits are not
    counted (its page is told "waiting" and nothing changes), approved browsers are not counted, and a request
    beyond the count still does one thing: it makes a waiting login of another browser contested, so nobody
    keeps a login of his own waiting beside your open page by using the count up. Requests write nothing to the
    flash but a log line and the time kept for a restart (one small file) when a login starts to wait or is
    contested.
  - *can*, with a phone's setup over Bluetooth: a phone (or anything that speaks Improv) in Bluetooth range
    that connects while the Echo has been without a network for ten minutes shows the wait for the button and
    keeps logins of unknown browsers back, for as long as it is connected and for half a minute after; a login
    that was already waiting when it connected ends when the phone's wait does, and its page asks again.
    Only then: the wait for a phone begins on no Echo that has its network or lost it less than ten minutes
    ago, counted from when Home Assistant added the Echo at the earliest, and once the network is back it
    lasts a minute at most unless somebody presses: a press, and a scan for networks asked for within the
    minute that press opens, begin the minute anew. (Until 2026-11-20's fifteenth
    review's fix there was another way in: a device that connected while the Echo was not set up, which needs
    no button, and kept its connection by asking for the list of networks was, the moment Home Assistant added
    the Echo, "a phone that waits" on an Echo that was online. The ring showed it, your press for your own
    browser's login was refused for the browser and went to that device, and it could then move the Echo to a
    network of its own. Now asking for the list keeps nobody connected to an Echo that has a network, and when
    Home Assistant adds the Echo, whatever is still connected from the setup is disconnected, unless a network
    was given over that setup a moment before: then, for a minute from when that change worked, whatever is
    connected or connects (the Echo cannot tell one phone from another) can read the result, and ask for what
    needs no button anywhere: the Echo's name and version, and the identify sound and rainbow once. No
    network, no list of networks. Never released.)
    **Such a device can hold the Echo's one setup connection, and the ring's wait with it, for as long as the
    Echo is without its network**: it only has to ask the Echo for something now and then, or leave and
    connect again. Nothing bounds that, and while it lasts your own app cannot find or connect to the Echo.
    (The Echo only ends a connection that has been silent for two minutes without a press, which is for a
    phone that was forgotten, not for one that means to stay. Until 2026-11-20's last fix it ended every
    connection two minutes after it began and turned that phone's address away for a minute after; that kept
    nobody out who took another address or came back a second early, and it broke Home Assistant's own setup,
    which connects before it asks you for the network and its password.) What does end the connection,
    whatever the device does, is the Echo's network coming back: one minute in all on it, counted together however often the network goes
    and returns and however often the phone connects again. On an Echo that is set up a scan for networks needs the
    button as a new network does, and only what the button paid for (the press, a scan or a switch within its
    minute) begins that minute anew. It did not end before 2026-11-20's third fix; until the fifth a phone that
    asked for a scan every few seconds was never let go, each scan a request to root and a small file written;
    until the sixth each connection had a minute of its own; and until the seventh a network that went away
    for a moment once a minute began the minute anew.
    The press itself is the phone's only while a phone is connected and the ring shows its wait: until the
    seventh fix an Echo that had been without its network for ten minutes took every press of the action
    button, with no phone there and nothing on the ring, away from the music and the assistant. And no unknown
    browser's login starts to wait while a phone is connected or its press's minute, a switch or "provisioned"
    runs: until then a login asked for right after the press for the phone showed the same ring, and a second
    press let that browser in.
  - What it costs you: after a restart of hassmic while something waited, or within the half minute after, a
    browser that is not approved yet is told to wait until that time is over (a minute and a half at most).
    A phone's setup that was waiting for the button counts as something that waited: the restart ends it (the
    Echo offers the setup again only after ten more minutes without its network), and unknown browsers are
    kept back for up to a minute after such a restart, so the press you were on your way to make for the phone
    lets nobody in. While a phone is connected to an Echo without its network the Echo writes that time down
    once every half minute (one small file).
  - What an **approved** browser can do with the button: its adb, plugin or note wait is taken whenever nothing
    else waits, also right after a login ended. A press you meant for that login, coming late, then serves the
    approved browser's request (adb opens, a plugin is accepted; never a note: that takes the button held for a
    second). An approved browser could change every setting
    before; the press adds adb or a plugin for it. Revoke browsers you do not know (System, "Approved browsers").

  Before 2026-11-20 a second request to log in ended whatever waited, a login, adb or a plugin's install, and the
  one after it was taken as a login of its own: a press meant for the first approved the stranger's browser, and an
  approved browser sets everything on the Echo. Released builds before that date have this. A first fix that day
  covered adb and plugins only: after a login had ended (contested for ten seconds, run out, or approved) the
  next browser to ask was taken at once and got the press that was still on its way; a second counted the half
  minute from the wait's end only, so a press into a contested login or into the quiet time, and the press after
  it, could still let in a stranger armed in between, as could a press after hassmic restarted; neither build was
  released.
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

While it is open, anyone on the network has root; nothing on the Echo but root does: the firewall drops every other
user's connections to adb's port, so a hassmic taken over cannot open adb and use it. USB always works: physical
access is root access anyway. Without `hassmic.conf` (stock behaviour, or before the install) it is open, as stock
leaves it.

## hassmic and root

hassmic runs as a user of its own, not as root, so that a flaw in it does not hand over the whole Echo. Root acts on its
requests (updates, models, Wi-Fi and the network address, the light ring, adb, restarts, the clock) only through files in hassmic's folder and
treats them as hostile. It moves each request into a folder of its own before it looks at it, and takes it only as a
plain file with no other name (no link, no FIFO, no hard link to a file of root's); models are read as hassmic's user.
The update and model folders are checked to be hassmic's own, not a link to elsewhere. Root writes its answers in its
own folder and then moves them over, or writes them as hassmic's user, never through a name hassmic could have made a
link. The settings file it reads in place only switches things on or off and sets numbers. Until 2026-11-12 several
of these went through such names (issue #130): code running as hassmic could have had root write a file of root's and
give it to hassmic, its start-up configuration included.

hassmic also runs in an SELinux domain of its own (`u:r:hassmic:s0`), not in root's `su`, which the unlock leaves
permissive: there any process may set any property, adb's port and the start of any service included, and hassmic
could (checked on a Dot 2). Its domain may do what su may, but set no property, enter no domain but the few su
enters on exec (Amazon's own, enforcing), ptrace no process, relabel no file, and register no binder service. An Echo installed before 2026-11-12 gets it only by running `scripts/install-system.sh` again (updates do
not change the policy); until then hassmic's log says "runas: no SELinux domain" and it runs in `su` as before, able
to set properties. The firewall lets no process on the Echo but root's connect to adb's port, so no process acting
for hassmic can use an open adb.

On an Echo Dot 5 "Light ring live" needs nothing new from root: its frames go through the same socket into Amazon's
ring program as Home Assistant's light already does (only animations of hassmic's own, checked there), at the
brightness Amazon's program sets. Through it hassmic can also read what the LEDs show, nothing more.

On an Echo Dot 3, an Echo Dot 2 and an Echo 2 root also gives hassmic one file of the LED driver at each start, the ring's frame
(the colours of its 12 LEDs, for "Light ring live"), through a group only hassmic has. Nothing else of the driver: the
LEDs' current, their calibration, the start-up ring and the Dot 3's mute button light stay Amazon's ring program's
and root's. Root also copies the ring's brightness table (Amazon's, read-only for hassmic on the Dot 2 and Echo 2) into its own
folder, where hassmic may read it and not write. On the Dot 3 the driver scales and bounds every value by the unit's
calibration; the Dot 2's and the Echo 2's take what they are given, so a hassmic taken over could drive its LEDs up to FF where Amazon's
brightest is DF, at the current Amazon's program set. Either way the worst it could do is show wrong colours on the
ring, mute's red ring included, which it could already through the ring's own patterns; the hardware mute switch
does not depend on any of it.

What stays: hassmic is what sees the action button and checks the signed challenge, so code that took hassmic over
can open adb over Wi-Fi for someone else on the network, though not use it itself. And it can name a key of
[Sources](#updates) for root to trust (state/update-key), and so get a bundle of its own installed as root: the one
request root takes on hassmic's word alone, for the page's signing key of your own or no signature check.

## Plugins

Experimental, off by default; [plugins.md](plugins.md) has the whole design and its threat table. A plugin index of
your own ([Sources](#updates)) changes only what the index decides: with a key of yours, what that index lists as
official is official here; with a key of yours or no signature check, what it withdraws (or does not) is what is
withdrawn here (an approved browser and the action button, and the page warns in red). Everything below holds for its plugins as for any. A plugin is
someone else's native code on the Echo, so what holds it matters:

- **Its own process and user.** Root's launcher (`plugrun`, started in front of hassmic) starts each plugin as a
  user of its own (6900 to 6907), with limits on memory, threads and descriptors, and takes nothing from hassmic but
  descriptors: it opens no file hassmic names. Whatever hassmic asks it to start, it starts in the sandbox; a hassmic
  that was taken over gets a process that can do less than hassmic itself.
  Up to eight run at once (one per user), and only as many as fit 128 MB of their memory limits together (a quarter
  of the smallest Echo's memory; since 2026-11-22, two before): plugins cannot together take more of the Echo's memory
  than that, however many are installed; one that does not fit waits. A plugin may name less memory than it needs
  and fail; it cannot get more than it named.
- **A system call filter** (seccomp) of about 70 calls: no file system, no sockets of its own making, no other
  program, no executable memory. Everything else a plugin has comes from hassmic as a descriptor or a message.
- **Only what the owner accepted.** A package's manifest names each capability; the Echo checks the package, the
  page lists the capabilities, and nothing is installed before the action button is pressed. A wake word plugin
  hears the microphone all the time and may have nothing else but files the owner gives it: no way out of the Echo.
  An update that drops that capability is a new install: it waits for the button, and once it runs, the files and
  storage it had while it heard the microphone are wiped (since 2026-11-21; before, it only asked for the button).
  The wipe comes after the old version's process is gone and before the new one starts, so whatever the old
  process writes on its way out (it may, for a few seconds: its stop code keeps its data, which an ordinary update
  carries over) goes with it (until a second review on 2026-11-21 the wipe came first, and what the old process
  wrote after it stayed for the new version; a removed plugin's process could also write into the same plugin
  installed again at once, which it cannot since).
  What the seal does not close, named: when a wake word plugin reports a detection, how much CPU it uses, and where
  on the stream it says the word lay, which hassmic's own log lines about the request show (the replay's start, the
  wake word's tail): about 12 bits a detection, a detection a second at most and twenty in ten minutes, that a
  `log.read` plugin could read. The same order as the timing, which every run that starts shows anyway.
- **Storage, the log and a server (2026-11-21).** What each gives, and so what a plugin that has it
  can do with it, wrong or hostile:
  - `storage`: it keeps data of its own on the Echo, within the kB the owner accepted and 128 keys, in its own folder
    (it names keys, never paths); the Echo takes ten writes and then one a minute, so it cannot wear the flash out
    faster than that. The data outlives its runs, and goes with the plugin when it is removed. It is copied to another
    Echo only when the owner picks it on "Copy plugins"; whoever is let in on both Echos' pages can do that. On its way
    through the browser it is sealed for each Echo (it may hold a token the plugin was given; the page is plain HTTP),
    and a wake word plugin's data never goes into a plugin of that id that is not one (what it kept while it heard the
    microphone stays sealed); the files of a wake word plugin and what it said of them neither, by the page's word.
    The source Echo seals its answer under a nonce of its own, new with every answer, and takes each request for it
    once, over its restarts too (until a second review on 2026-11-21 a request captured on the network and replayed
    after hassmic restarted was answered under the same key stream as before, and the two answers together gave what
    both held). Whether the data is a wake word plugin's is the page's word: the browser that copies is let in on both
    Echos and could post any data as the plugin's on the target, so a signature by the source would add nothing
    against it; the rule keeps the owner's own copy from carrying a wake plugin's data where it must not go. What the
    page seals for an Echo (a Wi-Fi password, a secret, the Music Assistant token, the copy's pieces) is sealed under
    the browser's own counter, new with every request, its last three digits at random so that two tabs of one
    browser do not pick the same in the same millisecond.
  - `log.read`: it reads hassmic's log, which says a great deal about the household's Echo (when it is used, errors,
    the network's addresses, which Home Assistant connects). It does not get what was said to the Echo, the replies,
    the names of networks, Bluetooth devices, timers, light ring designs or the Music Assistant server, media
    addresses (a reply's address fetches what was answered), Amazon's login code, the country the home is in, Home
    Assistant's error texts (hassmic marks those lines where it writes them, one line in one write, line breaks taken
    out; a line with a mark anywhere goes to no plugin; a line written without the mark that should have it is a bug
    that would let it through: every one that prints a value was looked at on 2026-11-21), root's lines but its
    events, a wake word plugin's lines (sealed when written, so also after it is removed), MAC and Bluetooth
    addresses, or the Sendspin pairing token. Its place in the log (a cursor, which it keeps and hands back) is
    read only from a line's start: one made up to point into a marked line moves to the next line (until a second
    review on 2026-11-21 it read the rest of that line as a line of its own). Cursors are not signed: from a line's
    start a plugin reads nothing it could not read from the log's beginning. A slow plugin holds the log back on
    the Echo (it gets the next lines once it has handled the last ones) and loses nothing that is still kept.
  - `net.server`: it connects to the server the owner entered in its settings, at the ports its install named, and to
    nothing else: hassmic makes the connection after checking the address (never loopback, where hassmic's own ports
    and Amazon's daemons are, never the Echo's own addresses or one its routes deliver to itself, never adbd's port)
    and hands it over. The server's and the port's rows have no value of the author's: a plugin that brings one is
    refused, and only what the owner typed is connected to (until 2026-11-21 a default in those rows was taken for the
    owner's, and a plugin could reach its author's server with nothing typed). A server outside the home network is
    reached because the owner typed it in; whoever can change the plugin's settings (an approved browser) can point it
    elsewhere. When the server is changed or cleared the plugin is started again, so what it held to the old one goes
    with it. A name's lookup is given up with the connect after 10 s, and a plugin asking for what it may not is said
    in the log once in 10 seconds with a count, so it cannot fill the log or wear the flash that way. Together with `log.read` that is the point of a log forwarder, and also the risk: what it reads, it can
    send to that server.
  - `net.lan`, `net.internet` (2026-11-22): the plugin names the address itself, at the ports its
    install named (or any port but 5555 where it said "any port"). `net.lan` takes a private or link-local address
    written as one, never a name: hassmic looks nothing up for it, and its socket is one the firewall holds to local
    addresses. So a plugin with it can talk to any device of the home network, which is what it says at install. It
    is not a wall against the internet: a device of the home that passes traffic on carries what the plugin sends
    further. The home's router does so for DNS (port 53, and 853 for DNS over TLS): sent a query for a name under a
    domain of its author's, it asks the internet, and the name can spell whatever the plugin heard. Those two ports
    are refused for `net.lan`, `net.udp` and `net.discover` as adbd's is for all, in the manifest, under "any port"
    and at every connect and datagram (until the review of 7be9343 they were not, and this document said a plugin
    with `net.lan` "cannot spell data into the home's DNS queries", which only held for the lookups hassmic itself
    makes). Other devices that relay remain: a proxy on the network, a router's own services (UPnP, a VPN, a
    forwarder on another port), a printer or NAS that fetches what it is told to. Not closed, and not closable from
    the Echo; it is why a plugin that hears every request and reaches "your network" is installed only with the
    second press, whose screen says so. `net.internet` takes a name or an address on the internet, its socket one
    the firewall lets out: what such a plugin reads or hears can go anywhere, which is what "Connects to the
    internet" says. It never takes an address of the home network: one written or what a name resolves to (a name
    its author points at a device of the home) is refused; a plugin that wants both asks for both (until the review
    of 7be9343 it reached every device of the home too). The router's own public (WAN) address is an address on the
    internet like any other and answers like any server there, from the LAN side: often with the router's admin page
    (some show it only to the LAN), and through a port it forwards into the home (hairpin NAT) with that device. So
    `net.internet` reaches whatever the router answers on its public address, which is more than "not the home
    network" (a review of 5aa32ee: this document said it "does not reach the home network"). Not closable from the
    Echo: it does not know which public address is its router's. Neither reaches loopback, the Echo's own addresses or
    one its routes deliver to itself, multicast, broadcast or adbd's port, whatever the plugin names; the same bounds
    as `net.server` (two under way, thirty a minute, 10 s each), and each plugin's name lookups bounded by its own
    count (two at once) and the Echo's (keeping places for the servers owners entered), so that one plugin's names
    that never answer do not hold up another's. Where a plugin that hears requests or calls connects is its own
    choice, so hassmic's lines about it are private.
  - `log.read.full` (2026-11-22): the log as the settings page shows it, what was said and answered
    and every name included; never a wake word plugin's lines (the seal holds against it too) and never the pairing
    token. Only for plugins the plugin index lists as official: refused at install for any other, and a plugin whose
    listing is gone (put into `state/` by other means) or was made by an index older than the build takes (its least
    serial, raised after a revocation that mattered; held at every start, as at install) is not started. An official plugin that the index withdrew and
    the owner runs anyway keeps it: the owner pressed for that, after the index's reason.
  - A plugin's own user owns no socket (the filter refuses `socket` and `accept`), and the firewall drops whatever
    those users would send all the same: a sandbox that failed still gets nothing out.
  - The development flag (on a plugin's card) makes a refused call end the plugin instead of failing quietly; it
    allows nothing more.
- **Music, UDP, the search and Home Assistant (2026-11-21).** What each gives:
  - `audio.music`: PCM into a socket hassmic plays as music, at the music's volume and ducked as Sendspin's; the
    newest source wins, so such a plugin can take the speaker from Sendspin or a phone whenever it likes (its
    install says so: "whenever it likes"); ten opens a minute. It hears nothing: the microphone is not part of it.
    What hassmic holds it to: it may start whenever it likes, but it cannot play through a pause. From the moment
    hassmic pauses it (another source started, the action button, "stop", Home Assistant) what it writes is not
    heard, whether it obeys or not, until hassmic resumes it or it starts anew; until 2026-11-21 it was only told,
    and one that went on saying "playing" played on and took every press of the button. What it writes while not
    heard is read no faster than it would play, so a plugin writing in a loop costs hassmic nothing (it cost two
    thirds of a core), and however small its writes, its stream is read at most every 5 ms after a small read,
    playing or not (until 2026-11-21 eight bytes a write into a socket made as small as it goes were read unpaced
    again).
  - `audio.volume`: the Echo's volume as the keys set it, five times a second and twenty a minute at most, a burst
    applied as its last value (each set is a log line, the ring's pattern and a write to the flash). `state`: idle, listening,
    thinking, speaking, mute and volume, never what was said; enough to tell when someone talks to the Echo.
  - `net.udp`: up to four UDP ports of its own in 28940-28999 (the plugin's user still owns no socket: hassmic binds
    them and hands them over). Inbound only from private and link-local addresses, twice: the socket's own filter
    (a socket filter fixed with `SO_LOCK_FILTER`: the plugin cannot take it off) and, on an Echo, the firewall's
    chain `hassmic_in` first in INPUT (that range from anything else dropped; `lockdown.sh` checks it as its other
    rules). Outbound only to private and link-local unicast that the Echo routes out of its network interface
    (never the Echo's own address, also not by a "local" route, a broadcast, loopback, port 5555 or DNS's 53 and 853,
    where the home's router would ask the internet for it: above): a peer fixed
    through hassmic (`UDP_PEER`, one a second, after which the kernel holds the socket to it: what goes to that peer
    is not counted, but a scan of the network's ports is one port a second) or each datagram through hassmic
    (`SENDTO`, 50 a second). The seccomp filter lets `sendto` through only without an
    address, and `connect` and `sendmsg` not at all, so the destination is always one hassmic checked. So a music plugin can talk to any device of the home
    network over UDP, which is what a player needs: it cannot reach the internet that way.
  - `net.discover`: the search, to the port its manifest names (and only that; never one of hassmic's own, 2330 or
    28928-28999, nor DNS's 53 or 853), at each private network's broadcast address or the multicast group its manifest names, once a
    second. Answers come to its own port, and so does anything else a device on the network sends there while the
    plugin runs: an open port, which its install says. Never back to the Echo itself: the kernel hands what the Echo
    sends to a group or a broadcast address to its own sockets of that port too, as if it came from the network, and
    until 2026-11-21 a search to 2330 forged Kiosk Satellite's claims on the Echo's own (its wake word silenced), one to
    28930 reached arbitration. Now the search's multicast is not handed back (`IP_MULTICAST_LOOP` 0), the manifest
    names none of hassmic's ports, and the firewall drops what comes in from a plugin's port and any address of the
    Echo's own, first in INPUT (chain `hassmic_self`, the addresses read again every 5 s: for up to 5 s after a new
    lease a broadcast to a port of Amazon's or avahi's could still come back; until 2026-11-21 the first address of
    wlan0 only, so a second one's broadcast came back past it).
  - Proposals: what a search found goes to the plugin's card; only the owner's click writes it into its server
    row, and only a private or link-local address is taken. So a plugin cannot point its `net.server` at a server
    of its choosing, which is what keeps the server the owner's (3.3).
  - `ha.entities`, `ha.actions`: entities and actions of the plugin's own on the Echo's device in Home Assistant,
    as its manifest names them (16 and 8). No plugin can speak for an entity of hassmic's or another plugin's: keys
    are given by hassmic once and kept, never one another had (until 2026-11-21 a 30-bit hash of id and name, and two
    plugins could share one); Home Assistant keeps an entity by its name and makes its entity id from it, and a plugin's
    is "Plugin <length of its id> <id>: <label>", whose entity id is never one of hassmic's own or another plugin's
    (before, a plugin "Wake" with an entity "sound", and later one "Last" with "· heard", took the entity id of
    hassmic's own entity of that name once that was switched off, history and automations with it; until
    2026-11-21). States are checked against the manifest (a number within its limits, an option, a listed event
    type, text of strict UTF-8), ten a second; a text that is not UTF-8 had Home Assistant drop the link, and again
    on every link after, until 2026-11-21. Home Assistant's commands and actions reach the plugin only over the
    keyed link (until 2026-11-21 also over any link where the Echo had no key yet). What the plugin says in its
    entities goes into Home Assistant's history: a plugin with `log.read` or `state` could publish there what it
    reads.
- **Calls (2026-11-21).** `call` is the second capability after `wake` that hears the room, and
  only during a call:
  - The room's microphone (Drop In's: the call pipeline's or the assistant's, after hassmic's gate, echo suppressor
    and gain) reaches the plugin only between its `CALL_LIVE` and the call's end: the audio socket is made at
    `CALL_LIVE` and shut down at the end, whoever ends it, the plugin's process ending included. A call coming in is
    live only after a person answered it at the Echo (the action button, the screen): not from Home Assistant, not
    from the page, never automatically (until 2026-11-22 an action marked `"calls"` could answer, and with it an
    automation). A call going out needs the token of an action of the plugin's marked `"calls"` (one use, 10 s); a
    token opens nothing else, an incoming call least of all. Those actions come only over Home Assistant's keyed
    link or from an approved browser, so the owner's Home Assistant can have the plugin call out: an automation there
    on such an action opens the room's microphone to the far side as a person's press would, and the call shows on
    the ring and the screen from its first moment to its end. The install says which actions can.
  - "Answer calls by voice" (Voice, off by default, issue #240) lets "<wake word>, answer" answer a ringing plugin's
    call or Drop In too. The words are Home Assistant's: its speech to text turns the request into them. So the
    Echo takes them only in a request its own wake word engine began, from idle or cutting a reply (never one Home
    Assistant opened: a question back, an announcement that listens, Home Assistant's wake word). With the setting
    on, a Home Assistant that put "answer" into such a request's transcript answers the call; and the engine hears
    the wake word from any sound, a TV's, or a reply or media Home Assistant plays on the Echo itself (its echo
    canceller takes most of its own sound out, not all), so a Home Assistant set on it could also start that request.
    That is the owner's choice to make: the settings page (an approved browser), an import or a preset switch it,
    Home Assistant has no entity for it. A phone's call over Bluetooth answers by voice without it, in any request:
    Home Assistant answers those anyway ("Answer phone call").
  - "Answered only at the Echo" holds in hassmic's terms: the Echo's own ring is answered only by a person here. What
    a call going out is on the far side is the plugin's to decide: a SIP plugin can take an action marked `"calls"`
    as "end my ring here, then `CALL_OUT` with the token" and have its server treat that as its accept of a call
    that rang there first. The Echo shows a call going out, then connected, and the room goes to that caller. That is
    an outgoing call the owner allowed (the owner's Home Assistant or approved browser called the action, which the
    install named as placing calls), so an automation of the owner's on such an action can connect a caller with
    nobody at the Echo; hassmic does not and cannot tell it from any other call going out.
  - What is not closed: a call that is live is the plugin's to carry wherever its network capabilities let it (a SIP
    plugin sends the room to its server, which is what it is for), and how long it stays live is the plugin's and the
    far side's until someone here ends it. The Echo shows it the whole time (the ring's call light, on a Show the
    screen) and the action button ends it. While the far side talks, the wake word is left to it, as in a Drop In.
  - Ringing is hassmic's (its ringtone, its light; the caller's name is the plugin's text, shown beside the plugin's
    own name), three unanswered rings in ten minutes and the plugin rings no more meanwhile, do not disturb refuses
    all; a plugin cannot use the ring to pester. One call at a time: Drop In and a plugin are busy for each other,
    and the line comes free only once the Echo shows the call before ended (until 2026-11-22 a plugin could ring in
    the 20 ms after a Drop In ended, unseen and unheard, and the next press answered it). A ring refused busy costs
    the plugin its next look for a second.
  - A plugin's calls are not Home Assistant's Drop In: they never show in its Drop In entities or the voice blueprint.
- **A copy of each request (2026-11-22).** `request.audio` gives a plugin what Home Assistant is
  sent of each request said to that Echo, byte for byte: the voices and the room behind them, for as long as it is
  installed. That is its whole point, and what it is accepted for ("**Hears everything said to this Echo after the
  wake word or the button**", in bold). What holds around it:
  - Only within a request: from where the Echo opens the microphone for one (the wake word, the button, a follow-up;
    with `-w remote` Home Assistant's wake word, never the stream before it) to the microphone off. The audio comes
    on a socket of its own per request, made as the request begins and closed by hassmic after its last block, so a
    plugin holds nothing that could carry the room between requests. A mute, the button's latch or the soft one,
    ends the copy, and while muted no request begins: no microphone audio leaves the Echo, to Home Assistant or a
    plugin, also not for a run asked for before the mute (Home Assistant's question back, an announcement's
    `start_conversation`, a wake word or the button that cut a reply), and with `-w remote` the stream to Home
    Assistant stops. Until 2026-11-22 such a run started after the mute and listened, and Home Assistant could have
    one start on a muted Echo at will with `start_conversation` (an announcement that goes on as a conversation);
    `-w remote` streamed through the soft mute. Only the Echo that streams the request has a copy: of several
    Echos that heard the wake word, the round's winner.
  - One way: the copy is taken after Home Assistant has the block, by a send that never waits. A plugin cannot
    delay, change, end, start or answer a request (there is no message for it; one that sends hassmic's own request
    messages is refused). A plugin that does not read loses its copy's blocks; Home Assistant loses none.
  - Nothing shows it beyond what shows the Echo listening: the ring (or the band, or the Input's LED), which the
    owner can dim to nothing (docs/plugins.md 9.17). What says there is such a plugin is the install dialog and the
    Plugins page, whose top names every plugin that hears requests while it runs.
  - With a way out (any `net.*`: the server the owner enters, the network) such a plugin is a recording of the
    household that can leave the Echo. It is installed only with two presses of the action button, the second on a
    screen of its own that says where it can send what it hears; the Echo holds to that whatever the page shows (the
    store wants both presses as it writes), an update that adds a way out asks for both, and a copy from another Echo
    of the network takes this Echo's own second press: a voucher stands for the first at most. What is not closed: a
    plugin with `net.server` sends what it hears to the server the owner enters, and that server may pass it on; the
    rule stops the direct road only (docs/plugins.md 9.17). With `net.internet` (built since 2026-11-22) it is the
    internet itself, and with `net.lan` any device of the home: the two presses name them ("to the internet and your
    network"); the plugin index does not list a community plugin with `net.internet` beside `request.audio`.
  - A wake word plugin's false detections open the microphone (20 in ten minutes before it is taken as broken). So
    no plugin with `request.audio` runs while the wake word engine is a plugin the index does not list as official,
    or one the index withdrew that the owner runs anyway: the Echo refuses the second of the two (an install, an
    update, choosing the engine) and does not start the listener where both came to be (stops it, where the index
    withdraws the engine while both run).
  - What it hears or reads may reach its own lines in the log: those of a plugin with `request.audio`,
    `request.text` or `call` (it hears the room while a call is live, and the person at the other end; public until
    the review of 7be9343) are written private, and so are hassmic's lines about it (what it did not take of a request, a
    volume it set, its reason for refusing a file: values it chooses), and no plugin with `log.read` gets them (the
    page's log shows them; until 2026-11-22 hassmic's lines about it were public). Left, and accepted: what such a
    plugin can show without the log to another plugin or to Home Assistant, a few bits at a time and never audio:
    the volume it sets (Home Assistant's media player, `STATE` to plugins), when and how much it works (CPU time,
    which another plugin on the Echo can time), and its own entities in Home Assistant where it has them.
  - `request.text` is less (what speech to text understood, the reply; no voices) and is given whether or not "Last
    heard"/"Last reply" are on for Home Assistant (the maintainer's decision: the plugin's capability was accepted on
    its own). A "stop" the Echo acted on itself is not passed on; with Wyoming every transcript of a request is (it is
    not looked at for "stop" there). Only a request's: with `-w remote` from Home Assistant's wake word on (ESPHome's
    wake word end, Wyoming's detection), and a text of a run no wake word made a request is passed to no plugin
    (until 2026-11-22 Wyoming's went out under the last request's number, and its detection began no copy).
- **The buttons (2026-11-22).** `button` tells a plugin each press and hold of the action and volume
  buttons that Home Assistant's "Button" event entity hears, when it happens: so when a person is at the Echo and
  touches it, and which key, a few bits a press. That is what the owner accepts with its line ("Hears presses of this
  Echo's buttons, as Home Assistant's "Button" does"); with any way out it can carry that further (to its server, its
  entities in Home Assistant), as Home Assistant's own history keeps it. It hears nothing the entity does not: the
  press that served a wait for the button (a login, adb, a plugin's install, running a withdrawn plugin anyway, a
  phone's setup) and the hold that served a copied plugin's note go to neither, because both
  are told from one place (`main.c` `button_event`), which those paths never reach.  So a plugin cannot learn, by
  the timing of a press, when an owner approves a browser or a plugin, beyond what the ring and the page show anyone
  there.  It cannot take a press either: what the button does on the Echo happens whatever the plugin does, and
  there is no answer.  Five a second per plugin, more dropped.  A wake word plugin cannot have it (the seal: only
  `files` and `storage` beside `wake`).  Rows of a plugin's own on its card (an entity marked `"page"`, the same API)
  show the owner what Home Assistant is told anyway, as text, and give the plugin nothing: no button asked for them.
- **A plugin's files are its own.** What the owner gives a plugin is kept in its own folder, whatever its id. Until
  2026-11-21 a plugin installed under the id `microwakeword` had the built-in microWakeWord engine's model folder as
  its list: whoever installed one by that name (a file, another author's key, with the button) got the owner's
  models, and could change or delete them for the built-in engine. Now the move to the microWakeWord plugin copies
  the models, and only to the official plugin (the index's listing) without a question; for any other plugin by that
  name the Wake word page names its author, its author key's fingerprint and where it came from and asks first. An
  Echo whose official plugin used that folder under a build before gets the models it had there copied into its own
  list once, at the first start with a build without the alias, the official plugin with `wake` only, never over a
  model it has: the files and the owner's threshold and window, never the plugin's `checked` mark or the title (the
  plugin checks and names each copy itself). Until a review of 61028a8 that copy ran at every start and took any
  folder with a `checked` file in it, whoever had put it there: a copy of models from another Echo (any approved
  browser, or an Echo of the alias era that listed the plugin's marks with its models) could replace the plugin's
  model with one marked checked that the plugin never looked at, and a title nobody checked went to Home Assistant.
  Copies of microWakeWord models between Echos now carry the model's two files and nothing else.
- **Signatures.** A package is signed by its author, whose key is pinned at the first install. A plugin the plugin
  index lists also carries the index's listing: a signature by the index key over the plugin's id, version, tier
  (official or community) and the digest of the manifest that was looked at. The index is a repository of its own
  (`hassmic-plugins`); every hassmic build carries its public key (`keys/plugins.pub`; during a change of key a second
  beside it, below) and takes no other key's word for a plugin (the release key has no part in plugins any more).
  Each official plugin has a repository and an author key of its own, and its package is that repository's release
  asset, which the index fetches, checks, verifies the attestation of (as a community one's, below) and
  lists. A package without a listing is shown as "From a file, not reviewed", a community one as "not reviewed".
- **Community listings: built by the author's own CI, attested, not reviewed** (2026-11-23; nothing published yet).
  The index builds nothing. Every package it lists, community and official alike, is a release asset of the plugin's
  own GitHub repository, built by that repository's GitHub Actions and carrying a GitHub artifact attestation (build
  provenance, signed with Sigstore). Before it lists a version the index verifies it with GitHub's CLI and reads the
  answer again field by field: the attestation's subject is the package's SHA-256, its source the entry's repository
  and commit, the run on a runner GitHub hosts (not a self-hosted machine), started by the push of the tag the
  release asset is under, and both the workflow that signed it and the workflow the run was for exactly the release
  workflow the entry names, at that tag and commit. A package with no such attestation (uploaded by hand, built on
  the author's machine) is refused, and so is one attested by another workflow of the same repository (one that
  downloads a package and attests it), by the named workflow called from another, by a run started by hand
  (`workflow_dispatch`: a writer of the repository can run any workflow at any ref so), of a branch, or of another
  tag's run. Where gh gives no verdict (GitHub's API, Sigstore or gh itself out of reach) a version the index lists
  already is never dropped: the publish fails instead, and nothing, a revocation neither, goes out until gh answers
  again; a new version waits for a later publish. A version the index lists is verified again with the bundle the
  index's site keeps of it, for the very bytes it held for: a no there is no verdict either (gh, Sigstore's trust
  root or the index's reading changed, not the release), the publish fails, and a listed version leaves the index
  only by a person's step (the revocation list, or the entry without it). The index's gh is one release of GitHub's
  CLI, checked by its SHA-256, never the one the runner happens to have, and a gh that says another version is no
  verdict wherever the index runs. A community entry also names the manifest's digest, the author's signature, the
  `LICENSE`'s SHA-256 and the capabilities it asks for, so every byte listed is the entry's word, whatever the
  release asset is replaced with later. Before a version is first listed the index also reads its release workflow,
  every workflow it calls and every action they use, all the way down, each at the commit it is taken at, and refuses
  it unless every action is pinned by a commit of that action's own repository (on one of its branches or tags, by
  GitHub's own listings and compare: GitHub runs a commit of any fork under the original's name, an impostor commit)
  and every image by its digest: a composite action's steps (the author's own action too, by its own commit, or by
  GitHub's `$/<path>`, read at the same commit), another repository's reusable workflow and a docker action's image,
  or its Dockerfile's `FROM` lines and `--from=`, are held to the same, a mount's `from=` only of an earlier stage;
  a workflow it cannot read as plain YAML, an action's metadata it cannot read so where it counts, or a Dockerfile it
  cannot read exactly as BuildKit does (a continued line, a parser directive or a line BuildKit may take for one, a
  JSON file, a flag with a quote or a backslash) is refused too, honest ones among them (the author then picks
  another action or writes the step).
  Not held by it: what a pinned action's own code does at that commit (a node action's downloads, a Dockerfile's `RUN`
  and `ADD`), the maintainer's to read; a release whose actions need more than 50 files read gets no verdict, a
  person reads it. That holds whatever the plugin's own tests say: they are in the author's repository. A version
  already listed is never taken off by this check (an action's repository deleted since answers 404): the publish
  fails, and a person decides. What that gives an owner: the listed package was made by GitHub's CI from a
  public commit of the plugin's repository, not by anyone's hand, and the bundles beside it on the index's site let
  anyone check that again. What it does **not** give: what the workflow does (it can download a program from anywhere
  and attest it: the maintainer reads the named release workflow and all it calls and runs in the entry's pull
  request, and nobody reads the code), a review of the code (the first entry is merged after a look that it is what
  it says, no more), or more than GitHub's word (the attestation is GitHub's, as the release asset is GitHub's to
  serve). Nothing of what goes into the package may come from outside that commit: no action (`uses:`) not pinned by
  a commit of the action's own repository (one on its tags or branches: GitHub runs a fork's commit under the
  original's name too, an impostor commit, which the index refuses), all the way down (a composite action's own
  steps, a called workflow's, a docker action's image and its Dockerfile's `FROM`, by the same rule), no artifact of
  another job or run (one only to compare a build of its own with), no other job's outputs (`needs.*.outputs`) but a
  SHA-256 to compare with, nothing of the event's payload (`github.event.*`; a tag's annotation is not in the commit)
  but the tag's name, no other repository checked out at a ref that moves (a branch or a tag; a commit's id stays),
  no container image (`container:`, `docker://`) not pinned by its digest, no cache (`actions/cache`, the caches
  `setup-*` actions keep, which other runs write), no `vars` and no secret as an input (the signing key aside, which
  only the release job's environment holds: no other job names an environment, whose secrets it would get), no
  release asset already there, no toolchain resolved online by a channel (rustup's `stable`, `beta` or `nightly`, in
  a `rust-toolchain.toml` or on its command line), and no download not pinned by its hash; and a re-run of an old
  run, which GitHub attests as that tag's push again, has to make the same bytes. In the pull request the index also
  runs the program for 3 s under qemu against a stand-in hassmic and fails one that makes a call the sandbox's table
  refuses: **a smoke test, not a wall** (a program can tell it runs under qemu and behave, or wait the 3 s out); what
  holds on the Echo is the launcher's seccomp filter from the same table, every call. The index refuses
  `log.read.full` for a community plugin and `request.audio` beside `net.internet` (stricter than any Echo), and
  flags broad capabilities (`net.lan` at any port, `net.internet`, `call`, `request.audio`, `request.text`), which
  its page and the Echo's "Browse plugins" show. Not built in the index: an update of a community plugin merging
  itself; a person merges each. (For a day before, 2026-11-22, the index built community plugins from their source
  itself, in bubblewrap with a pinned zig on two runners; gone, never published.)
- **Who decides what is official: the index key.** Its private half is a secret of the index repository's
  protected signing job, as the release key is of this one's (its workflow's actions pinned by commit). Whoever
  holds it can mark any package official, and on every Echo that buys: an update of an installed plugin by the
  author key that Echo pinned, that asks for nothing beyond what the owner accepted there and comes from the same
  tier or a higher one, installs without the button (pushed with the update key, fetched by hassmic for its wake word
  engine, or from the page with one click); the plugin can be copied between the Echos of one network without the
  receiving Echo's button (below); and the whole log (`log.read.full`) may be asked for. It does **not** buy a first
  install without the button (every first install, official or not, waits for the action button and shows what the
  plugin may do), a plugin under another author key in place of one installed, a version older than the one
  installed, anything past the combination rules (a wake word plugin never has a way out of the Echo), nor immunity
  from the revocation list: since 2026-11-21 the index withdraws official plugins as any other (before, an official
  listing was out of the list's reach, and an official plugin's leaked author key had no stop but a hassmic release).
  An update from a lower tier than the version installed (official, then community, then a file) asks for the
  button whoever brings it: before 2026-11-21, whoever had an official plugin's author key replaced it with an
  unlisted package of his own from any approved browser, without one.
  **What the key's holder can do besides**: offer plugins on "Browse plugins"; withdraw any plugin, official ones
  and the one that is an Echo's wake word engine included (that Echo then hears no wake word, Amazon's neither,
  until its owner chooses another engine or runs the plugin anyway with the action button: whoever holds the index
  key can stop the wake word of every Echo whose engine is a plugin, community or official); and, with a list
  dated in the future, keep honest lists from Echos for a day at most (an Echo with its clock set refuses a serial
  more than a day ahead; one without a set clock takes it but holds later lists to no more than its build's time +
  a day). **What it cannot**: install anything without the button where a first install is, take a plugin's place
  under another author key, freeze an Echo's lists for longer than that day, or reach anything of hassmic's own.
  Each hassmic build also carries the least serials of the index and its revocation list it takes (set from the
  published site before a release) and the least version of each official plugin it runs.
- **A new index key** (none planned) needs a hassmic release before the index signs with it: an Echo takes lists
  and listings by the keys its build carries only. The way (docs/plugins.md 5.2 has each step): a build that takes
  both keys, time for it to reach the Echos, the index signing with the new one (its tool reads the published site
  with the old key as `keys/index-previous.pub` for that one publish), a build with the new key alone; meanwhile
  either key holds the index. For a leaked key there is no meanwhile, and Echos on older builds trust it until they
  update. *Built (2026-11-22):* a build takes a second key where `keys/plugins.conf` names one (`PLUGINS_PUB2`, with
  `PLUGINS_PUB2_WITH`, the hex of the first key it belongs with; empty in this tree and in every release so far: one
  key is trusted today), for every list and listing alike (the index, the revocation list, from the site or another
  Echo, and each package's listing; `plugin_index.c` `pi_keys`); a build whose first key is another one (a fork's
  own, however it names it) never takes that second key beside it (the Makefile's rule, by the keys' bytes). Whoever
  puts a second key in a release gives its holder the whole index on every Echo of that release, as the first key's
  holder has it.
  What it does to what is installed: a listing holds only under a build with its key. Under a build of one key, a
  plugin only the other key listed is as from a file: kept, but on an Echo with community plugins off (the default)
  not started, the wake word engine's plugin neither (its words say "from a file"). That is the last step for an
  Echo still holding the old key's listings, and any step back to a build of the old key alone (an older online
  build, a fallback after a failed update, the factory copy) for one holding the new key's. The remedy is the
  index's listing of the same version, which the Echo takes in place of the installed one, without the button and
  without an install (the same manifest, so the same program; settings, storage and files stay), wherever it is by
  the other index key, of a higher tier, or under the build's least index serial; never a lower tier. It does so
  after every look at the index from the Plugins page ("Look again" too), and by itself: at its daily look (the
  first 5 to 65 minutes after a start) wherever the listings installed say there may be one (one by the first of
  two keys, one under the least, or one that failed to be taken at an earlier look; the daily look then reads the
  index as well as the revocation list: until 2026-11-22 only the page's look did, a review of 3c5d7ff), and where a
  listing installed does not hold under this build, within a minute of the start or of the network's return and
  then hourly until it is taken (the daily look alone left such a plugin, the wake word engine's too, stopped for
  up to a day after one look that failed: a review of 3fcd319), at once when the gap of 5 minutes after a look is
  over where the network came back within it. A version the last look found not listed, or listed with a listing
  this Echo cannot take, is no reason for the hourly look; the daily look still reads the index for it, in case it
  is listed again (a review of 9cd7a77). So every Echo that reaches the index between the index's switch and the build of
  the new key alone moves its listings to the new key, whether or not anyone opens its Plugins page; one that did
  not is brought back by the new build's own look about a minute after its start. A version the index no longer
  lists cannot be brought back: under that build it is a file's, with community plugins off stopped. An update
  whose self test finds the wake word engine's plugin stopped that way (a listing it does not read, community
  plugins off, where the plugin ran as listed before: by an official listing) does not become the factory copy, the
  one the Echo falls back to, while it is so: that is the new build's doing, not the owner's choice. Nothing goes
  back: the build runs on, and its other plugins wait for the self test, stopped. The self test asks again every
  10 s for as long as hassmic runs; once the build's look has restored the plugin and it listens, the self test
  passes, the build becomes the factory copy and the other plugins start (until a review of 9cd7a77 the self test
  ended at 30 s: the other plugins stayed stopped for the whole run, and the build became the factory copy only at
  its next start). Under a build of the old key alone after the switch
  there is no index it reads: those plugins wait for the update back, or for community plugins switched on.
  The revocation list kept is kept as the newest key signed it: the index's switch publishes it signed by the new
  key under a serial one higher, and an Echo of both keys that is handed the same list under the same serial by
  the second key (the new one) keeps that signature too; the first key's copy of it after that changes nothing (a
  member handing it over turned the file back to the old key: a review of 3fcd319). Kept as the old key signed it, the build of the new key alone read it as
  no list, and what it withdrew ran there until the next look reached the site (a review of 3c5d7ff).
- **Keys under which anything is signed.** A public key of small order (all zero, the identity, the six others) takes
  the null signature, R the identity and s = 0, for every text (Monocypher's check is cofactored). No key of hassmic's
  or its tools' is one: the Makefile refuses to build in such an index or release key, the Echo takes none as the
  index's (no list then), as the release or update key (no update) or as a plugin author's (no install), and the PC's
  `otatool.py`, `plugcheck.py` and the index's tool refuse them as well (`src/include/edkey.h`), in any of their
  encodings: `otatool.py` `weak()` also refuses every encoding it does not read as a point, six of which Monocypher
  reads as points of small order (until 2026-11-22 the build and the index's tool took those, and the Echo refused
  them at run time). The build refuses every key where it cannot ask (no Python), and a second index key named
  with another first key than the tree's own (a typo in `keys/plugins.conf`) stops it, where it was left out without
  a word; a fork's own first key leaves it out as before.
- **What the Echo fetches itself.** The plugin index's site (GitHub Pages, its address built in: `keys/plugins.conf`;
  over HTTPS checked against the system's CAs, through the same code as online updates, redirects followed to HTTPS
  only): four files for the list (`index.json`, `revoked.json` and their signatures), and a package by its id and
  version. Those requests carry the Echo's address, the file names and "hassmic" as what asks: no version of
  hassmic's (they did until 2026-11-21) and nothing about what is installed. They are made when the owner opens
  "Browse plugins", opens the Plugins page with plugins switched on, presses "Look", "Install" or "Update", and,
  while "Online updates" is on, for the wake word engine's plugin after a hassmic update it needs another build for,
  and for every plugin installed before an update that needs other builds of them (an online update the owner
  installs, a push while online updates are on): its id and version, as for "Install".
  **And once a day without asking**: while plugins are on and one is installed, `revoked.json` and its signature
  alone, at a random time (the Plugins page's help says so; off with plugins). How often is the Echo's own rule,
  whatever a page asks of it: for the list a page shows by itself, at most one look an hour (four requests),
  whether that look found the list or failed; for a press of "Look again", "Install" or "Update", at most one every
  30 seconds. One fetch, its look and download together, is given up after five minutes. An Echo with plugins off
  and nobody on those pages asks nothing for plugins.
  Nothing on the site is trusted but what the index key signed: both lists are taken only with their signatures,
  at least as high as its build's least serials, and an index only beside a revocation list at least as new as the
  one it was made with. A revocation list is never taken older than the one the Echo keeps, clock or no clock (it
  was, until 2026-11-21's second review, once the clock was set after a list taken without it), with one exception:
  a kept list whose serial the clock, once set, shows to be more than a day in the future; then any list from the
  floor line it was kept with replaces it. That line is the kept serial wherever a set clock vouched for it, and
  never falls: a list taken later without a clock keeps it (it fell to the build's time + a day, and with Home
  Assistant's clock more than a day behind the new list, a list older than the vouched one was taken: a review of
  6f39dd8); only the first list ever taken without a clock has the build's time + a day as its line. An Echo whose clock is never set
  (Wyoming, no time from Home Assistant) keeps such a list until its clock is set: a serial from the far future only
  the index key can sign, and the index's tool refuses to make one. An index is never taken older than the newest
  the Echo took once its clock vouches for that one; while the clock is not set, an index older than the newest but
  younger than the build's time + a day can still be handed to it, and what it offers is held to the revocation list
  kept, which never goes back. So whoever can serve the site's files cannot hand an Echo an older revocation list,
  nor, with its clock set, an older index that still offers a version since taken out, nor a new index beside an
  old revocation list (they could until 2026-11-21), nor an old list to an Echo after a factory reset (older than
  its build takes). A site's revocation list older than the one the Echo has (one a member handed over first) is
  refused and said, and the look goes on with the Echo's own. A package is downloaded from the address built of the site's base,
  the id and the version (never a path the list names), no more than the size the list names, and refused unless
  its size and SHA-256 are the list's (where they are not, the Echo looks at the index once more, since every
  publish lists every package anew, and holds the package to what that list says); then it goes through the same check as an uploaded file, and has to carry
  the index key's listing for exactly the manifest, id, version and tier the list named. No sideload comes in
  through this path. The action button is asked for exactly as for a file; hassmic installs by itself only an update
  of a plugin the owner accepted, by the pinned author key, that asks for nothing beyond what was accepted. What the
  network can do to this is keep the lists away: a list that cannot be fetched changes nothing installed, and the
  page says how old what it shows is, the revocation list's date and when the Echo last had word of it included,
  and warns after a week without such word while plugins run. Word is the site's answer, now, or a newer list from
  a member, as of that list's serial (the time it was made, all a member's list says: dated "now", a member holding
  an archive of older lists kept the warning away with one a week); a member handing over the list the Echo has is
  none (a member could repeat it at will). The age is kept over restarts: by the clock where it was set, else by the
  Echo's own running time (which misses the time it was off, so it is the least the age can be); a clock behind the
  word's time says nothing, and no word known at all is warned of too (each of these counted as fresh until
  2026-11-21's second review; the record's older form, a wall time alone, counts as no word while the clock is not
  set or behind it). A kept list whose signature no longer holds (a hassmic of other index keys than the one that
  signed it: a step back to a build of one key during a change of the key) is no list and no word: what it withdrew
  runs again there, and the warning, which comes at once (no list read is no word), says why.
- **Withdrawn plugins.** The index's signed revocation list names plugins by id (up to a version) or by author key,
  each with a reason. It applies to every tier: official and community plugins and plugins installed from a file
  (by their id or their author's key). The Echo keeps the newest list it took (`state/plugins/.revoked`, list and
  signature in one file, read and checked again after every start); one older than a later build wants still
  withdraws what it names until a newer one comes (it was dropped at the start: what it withdrew ran again, for good
  where the site is out of reach), and the page says it is old. It refuses to install what the list names, and stops a plugin
  installed there that it names: kept, its card says why, removing it is the owner's, and so is running it anyway
  with the action button, which holds until a newer list names it again (what that press does is fixed when the
  page asks: that plugin, for that list; it is one wait for the button like an install's, with the same weight: an
  approved browser that can make you press, by ringing a Drop In there, can get it as it can an install's press;
  only a press while the plugins' ring shows does it). A list longer than an Echo reads is
  refused whole and said (before 2026-11-21 the entries past 2048 were dropped and what they named ran on). The list
  arrives with a look at the index, with the daily look above, and from the other Echos of the arbitration network:
  each tells the others the serial of its list (a message on the network's own authenticated channel) and hands the
  list out on its settings port to members only (`GET /api/plugins/revoked` naming a ticket of the two the Echo told
  last, a nonce of the asker's, and a MAC over both by a key derived from the network's; a ticket is taken for two
  hours, and a nonce is refused while it is among the last 64 taken with its ticket; to anyone else, and to a request
  sent again, it answers as for a path it does not have, list or not: it was open, and then a request seen on the way
  was answered for ever. The 64 are a ring, not a cap: a cap let a member that spent them refuse every honest
  request. So a request seen on the way is answered again once 64 members' requests came after it with its ticket,
  with the list the site hands anyone); an Echo that
  hears of a newer one fetches it (plain HTTP, no redirect followed) and checks it as it checks the site's. The
  announcement is authenticated, not hidden: the hourly arbitration broadcast (sent only while plugins are on and
  one is installed) shows anyone who sees the network's broadcasts that this Echo has plugins (not which), the
  serial of its list and its settings port, and the ticket, which is no use without the network's key. The bounds
  on what a member's word costs an Echo: a member's identity on that channel is its own word, signed only with the
  shared key, so it can name any number of identities, addresses and ports, though only addresses of the local
  network: a word from any other source address is dropped, and the list is fetched over a socket the egress lock
  keeps on the local network (a review of 6631974: the source address and the message's port were fetched from
  wherever they were). Each such place is asked once in ten
  minutes whatever serial it names, five seconds at most each, no write when it hands back the Echo's own list;
  members' fetches in all are four at once, then three a minute, and the log's lines on them five, then one every
  ten minutes, the rest counted (one member naming a new identity with each message had 496 fetches and as many log
  lines in five seconds); a message under a known identity from another address or port is a place of its own and
  never moves the known one's. These stop a member's mistakes and anyone without the key; **they do not hold
  against a member that has the network's key and means it**: such a member can delay another member's newer list
  for as long as it keeps at it (keeping the places full, a place of its own over the honest member's as that one
  arrives), and can silence a member's messages on the arbitration channel altogether by sending one under that
  member's identity with a counter ahead of it (arbitration's own older limit, below, until the Echo restarts). This
  is a decision, not an oversight: a member with the key can already disturb arbitration, Drop In and ducking, and
  the handover only helps Echos without the internet. What holds: it cannot change a list (only what the index key
  signed is taken), nor reach the site's daily look, so an Echo that reaches the internet still gets a newer list
  within a day; one that does not gets it only through members, and warns after a week without word. With plugins
  off nothing is told or fetched.
- **Plugins copied between Echos.** The Echos of one arbitration network share a key, and with it one Echo can
  tell another "the owner accepted this plugin here, with the button" (a voucher, bound to the receiving Echo, the
  browser, the plugin, its manifest, its author's key and a one-time number). The receiving Echo then installs
  without its own button, but only a package that carries the index key's listing as official (from an index at least as new as its build takes), that passes every check it
  makes of any package, and that is the very one the voucher names. A voucher proves the network's key: **not a
  button press, and not which Echo made it**. Every Echo of the network has the key, so any of them can write a
  voucher in any member's name; the receiving Echo only checks that an Echo of that name is a member. The name is
  therefore written everywhere (the page's note, the log, the plugin's `accepted` file) as "the Echo the voucher
  named", never as where the plugin came from. So an Echo of the network that was taken over (or anyone who has
  the network's key and a browser approved on a member, which such an Echo can arrange) **can**: have official
  plugins installed on the other Echos without their buttons, in versions this project released, and change the
  settings and files of plugins there as any approved browser can (a plugin's secret only by that Echo's own page:
  the Echo takes none sent in clear). It **cannot**: install a program of its own making or any package without
  an official listing by the index key, replace a plugin installed under another author's key, or get a plugin more than
  each Echo's own rules allow (a wake word plugin still has no way out of the Echo). **Versions**: never one
  older than the one installed there, never one older than any that was installed there before (removing a
  plugin takes no button, so the Echo remembers the highest version it ever installed of each, until a factory
  reset), and never one older than the newest that Echo has itself seen listed in
  the plugin index (it remembers that over restarts). An Echo that has never looked at the index (its Plugins
  pages never opened, or the index out of reach) has nothing to compare with: there, a plugin that is
  not installed yet can arrive in any official version this hassmic still runs, an old one included. And nothing
  arrives this way while plugins are switched off on the receiving Echo.
  One press in the kitchen does put a listening official plugin in the bedroom: that is the decision taken, and
  the announcement stands in for the press. The announcement is made so that the one it is about cannot take it
  back alone: the note is written before the install (an Echo that loses power in between keeps the note); it
  shows on every page of that Echo, and on a page of another Echo of the network while that page shows its
  neighbours and the browser is logged in to the Echo the note is on (not on every page everywhere); notes
  are never replaced or pushed out (with sixteen standing, read or not, the next copy needs the button); a
  dismissal covers the notes that showed when it was asked for and none that came meanwhile; the browser that
  brought the plugin cannot dismiss its note by a click, nor can a browser that was not let in with that very
  Echo's own button (the same key that forges a voucher lets in as many browsers as it likes): those need
  that Echo's action button held for a second, and only a different browser that was let in with that Echo's own button
  dismisses by a click. Which browsers those are is kept as a list of the keys the button let in
  (`state/web_button`; a line in it for a browser that is no longer approved is taken out when one leaves and at
  every start); a browser that is not on it, because it came through another Echo, because it was let in
  by a build before the list, or because the list is lost, counts as vouched. After the update that is every
  browser, until it is logged out and let in again with the button of the Echo in question. A note of a build
  that did not keep the browser's key is the button's for everyone. The log line stays, and Home Assistant's
  `synced` event is held until Home Assistant is connected, also over a restart and also when the note was
  dismissed first. "Home Assistant" here is whatever holds the Echo's ESPHome link with its key; **on an Echo
  without an API key any client on the network that subscribes to its states counts as that link**, takes the
  event, and it is not said again. The note names the browser that brought the plugin by the name that browser
  gave itself and by the first digits of its key; the name is free text, the key's digits are what to compare
  with the list of browsers (System). **Taking a note off at the Echo is the button held for a second, never a
  press**: the browser that asks for it could make you press (it can ring a Drop In on that Echo, start a timer,
  play music), and a press while the wait shows does what it always does. Only a hold that begins after the wait
  was asked for counts: a button already held down when the browser asks takes nothing off. While the wait
  shows (a minute, the plugins' ring), a hold of a second is the note's and Home Assistant's "Button" entity
  does not hear `action_hold` for it: an automation on that hold misses it then. On an Echo Show the hold is
  both volume keys for one to two seconds; held two seconds or more they pair Echos and take no note off. That wait is bounded per Echo: three
  times an hour, whichever notes and whoever asks, also over restarts and reboots, so the ring cannot be kept
  showing it. **What that costs**: a browser that is approved on the Echo only through another Echo can use
  the three up, and then the browser that brought the plugin (in the ordinary case your only one) is told
  "later" for up to an hour. The notes stay on every page meanwhile, with sixteen standing the next copy needs
  the button, and a browser you let in with that Echo's own button, which did not bring the plugin, dismisses
  at once. A plugin's files are put in place whole or not at all, and a program that is not the one its manifest
  names is not started. If the Echo cannot read its file of notes (or of installed versions) at start, it says
  so in its log and takes no plugin without its button until it can. Secrets of a plugin never leave an Echo.
  The "never older than one it has seen listed" rule stands on a file that is only ever raised
  (`state/plugins/.listed`): an index that once listed a plugin with a wrongly high version number makes every
  copy of that plugin ask for the button on that Echo from then on (installing with the button still works), and
  nothing but a factory reset of the Echo clears it; and it keeps 4096 plugins, so one beyond those has no such
  floor.
- **Pushed plugins** (`ota-push.sh --plugin`): the push port takes a listed plugin's package from whoever
  answers its challenge with the owner's update key, and installs it under that same last rule and no other. The
  update key only opens the port (without it anyone on the network could make the Echo restart its wake word
  engine or choose when it updates); it does not make a package installable. That is the index key's listing.
- **One install at a time.** A press of the action button accepts exactly the package whose list the dialog of
  the browser that asked showed: the Echo remembers that browser, that package's manifest digest and which
  install it was, and installs only if all three still hold when the press comes. Whether a press is needed at all
  is decided again at the moment the files are written, under the lock a removal takes too: an update that needed
  no button when it was checked is not installed without one if the plugin was removed in between (before
  2026-11-20 it was, as a first install nobody had accepted). While one browser's dialog is
  open (two minutes) or its install waits for the button, no other browser, no push and not hassmic itself can
  put another package in its place; a press that finds anything else installs nothing.
- **The push port and someone on the network without the key** (port 28929: updates, adb over Wi-Fi by the
  update key, pushed plugins). Nothing is installed or opened without a signature of the owner's update key. What
  such a neighbour can do is keep the port busy, and that is bounded per address, so that one machine alone
  keeps nobody else out:
  - time: the request line has 5 seconds as a whole, the answer to a challenge 10; a bundle has to bring 64 kB in
    every 10 seconds, counted anew each time (a burst buys no silence afterwards), and all of it within 10 seconds
    plus its size at 32 kB/s;
  - connections: before it has signed, one address has two and all addresses together four; a newcomer that finds
    the four taken ends the one that has waited longest among those that are not receiving a bundle (a bundle
    cannot sign before all of it has arrived, seconds for a few megabytes, and is not ended for a connection
    that only sits there; "receiving a bundle" means holding one of the two places below at that moment, not
    having held one). A connection that is refused or ended is told so ("FAILED busy ..."), and `otatool`
    prints it;
  - memory: a bundle's signature is over the whole bundle, so it cannot be checked before it has arrived, and it
    is never written to the flash before. Two such bundles are held at a time (32 MB at most), one per address.
    When both places are taken, a new push takes the place of the one that came in slowest over the last second
    or two, unless that one too is faster than 256 kB/s, or took its place less than two seconds ago (once per
    address: not when that address's last bundle, in the ten seconds before, ended without a good signature
    or lost its place while nothing was coming from it; a bundle that was arriving when it lost its place does
    not count against its address).

  A bundle that has arrived whole keeps its place while its signature is checked (it could lose it then, and was
  installed all the same while the PC read "busy").

  What is left: someone with several addresses who keeps sending junk bundles at that speed (two addresses), or
  takes turns with fresh bundles from some ten addresses, or opens four connections in the moment between your
  connect and your signature (milliseconds for adb and a plugin; for a bundle, between its connect and its
  place), can keep a push or adb from getting through while they do, as anyone who floods the network can; and a push over a link slower than 256 kB/s can lose its place to such a sender (it
  is told, and you push again). USB and the settings page's own adb switch do not go through this port. Before
  2026-11-20 one sender could hold the port without that effort: 16 MB sent at once and then silence kept every
  push out for almost nine minutes, and four connections that asked for adb and never signed, opened again as
  they ended, kept everyone else out for as long as they liked. The first fix of that day still let idle
  connections from two addresses, three a second, end every bundle on its way (a bundle was "the one that has
  waited longest" after a moment); the second let four addresses that each announced a bundle and sent nothing
  keep everyone out again (a connection whose place had been taken still counted as receiving). Neither build
  was released.
- **What it is not.** The Echos' kernels (3.18 to 5.4) get no fixes. A plugin with a kernel exploit that works
  through one of the allowed calls is root. The sandbox limits mistakes and makes abuse expensive; it does not make
  an unknown author's code safe. Install what you would run as a program on your own computer.
- **SELinux** is a second wall only where the system install is new enough. A system install from 2026-11-22 on
  adds the plugins' own domain, `hassmic_plugin` (scripts/system/sepolicy-hassmic.rules): it may use the descriptors
  it was handed and read and write the sockets and pipes behind them and pipes it makes itself, execute its own program (a file type of its
  own, which hassmic gives the program before each start and which nothing else has) and run its threads; no file it
  could open, no socket of its own, no property, no binder, no process outside it, and only root's launcher enters it.
  It is one domain for every plugin: SELinux does not keep one plugin from signalling another or looking into its
  `/proc` directory. What keeps plugins apart from each other is that each runs as a user of its own (the kernel
  lets no process signal another user's) and the system call filter lets it signal only its own process with
  `tgkill`; `tkill`, which musl's `raise()` and `abort()` use and which names a thread alone, is held to the plugin's
  own threads by its user only (no rule of the filter can tell a thread id apart from another process's).
  `tools/sepolicy-check.sh` compiles the rules into every model's policy as the install does and checks that this is
  all it may do; it is **not tried on an Echo yet**. Older system installs: plugins run in hassmic's domain (from
  2026-11-12), or, before that, in the permissive one hassmic itself runs in there, where SELinux stops nothing. An
  update never changes the policy; running the system install again does. The page says which of the three this Echo
  has, at all times.
- **Community plugins are off until the owner switches them on** (since 2026-11-22). Anything the plugin index does
  not list as official (its community listings, a file) is refused at install and not started while the switch is
  off, however it comes (a file, the index, another Echo's copy, a push); those installed are kept and stopped. The
  switch is a setting of the page as any other: on and off from an approved browser (whose every request is signed),
  with no press of the action button. Where the sandbox lacks its SELinux wall the page says in full what is missing
  and how to close the gap: in the question before the switch goes on from the Plugins page, and in the install
  dialog of every plugin that is not official, the step that takes the action button. It goes into exports and is
  copied with the other settings (Compare & copy, "make like this Echo", an import, a preset), which switch it
  without that question: there the warning comes at each plugin's install dialog only. Home Assistant cannot
  switch it. The press is where it always was: each community plugin is
  installed only with the Echo's action button, and gets only what that press accepted. So whoever has an approved
  browser, or a preset or an export the owner applies, can switch the switch, but nothing comes in by it without a
  person at that Echo pressing for that plugin. What it does not do: make a community plugin safe once it is on.
  Off unless set: no Echo starts with it on by itself.
- **hassmic's own risk**: it now reads messages from plugins, in the process that holds its keys. Every message is
  checked for form, state, capability and rate before it is acted on, and the format's reader is fuzzed.

Not seen on an Echo yet (2026-11-18): the launcher, the users, the filter's ARM program.

## Drop In

A call opens the called Echo's microphone, at once by default. Only a member of the Echo network can ask for one (the
calls travel signed with the network's key), Home Assistant only over its encrypted link, the settings page only from
an approved browser. Each call gets its own key from a fresh key exchange between the two Echos, so a recording stays
closed even to someone who gets the network key later. The audio goes straight between the two Echos and nowhere else.
Do not disturb and the microphone switches refuse calls; "Drop In answers: after the action button" makes every call
wait for a press on the Echo called (or, where "Answer calls by voice" is on, "<wake word>, answer" in a request
that Echo's own wake word began: the words are Home Assistant's, see Plugins, calls); "Drop In" off stops calls both ways.
A packet of a call sent again by someone else on the network decrypts as it did the first time, so each frame is
taken once, and only a frame newer than all before it can move where the Echo sends its microphone or keep the call
open (until 2026-11-22 a replayed packet could cut short bits of the other side's voice, have the Echo send its own
room, still encrypted, to the replaying host instead of the other Echo, and keep it in a call whose other Echo had
gone; a build never published, between two reviews, also played the other side's earlier words again).

## Music Assistant (Sendspin)

Only a Sendspin server paired with the Echo's token plays on it (unless "Music Assistant without pairing" is on): an
unpaired connection is encrypted under a key everyone knows. Whoever has the token can pair a server. Over the ESPHome
API the Echo gives the token only over the connection with the key, never over plaintext nor before Home Assistant has
set its key (it does so when you adopt the Echo): both the pairing-token request (what Home Assistant asks for and
hands to Music Assistant, which then pairs by itself) and the older "Sendspin pairing token" text sensor, which has no
state until then. Before adoption someone on the network could set a key first and then ask, but that takes the Echo
from Home Assistant, which you would notice. The token is also in the Echo's log, read over adb, and at the end of
`scripts/setup.sh`. The settings page never shows it, and its log viewer blanks it.

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

Every member is trusted with all of it: a member's identity on the channel is its own word, signed only with the shared
key. An Echo (or anything else) that has the network's key can claim wake words under any member's name, keep others
from answering, ring Drop In, duck the music, and, with one message under a member's identity and a counter ahead of
that member's, have the other Echos drop that member's messages until they restart.

**Kiosk Satellite mode** has none of this: its claims carry no key, so any device on the network can silence the Echo
while that mode is on.

## Updates

Installed only when signed with your `secrets/update.key` (pushed from your PC) or with the project's release key
(`keys/release.pub`; downloaded by hassmic itself, only once a channel is picked under "Online updates"). Root checks
the signature, with the tool and keys from the system partition or the installed copy, before anything is unpacked.
Your key also opens adb over Wi-Fi; the release key does not.

**Sources** (issue #175: the settings page's "Where software comes from"). Online updates may come from another server
than GitHub, set from an approved browser; that alone trusts no more, since root still checks the release key and
your update key. For a server of your own you may also name a signing key of yours, or switch the signature check off
(then hassmic signs what it downloads with a key of its own). Either is taken only from an approved browser plus a
press of the action button for that very choice, and every page warns in red while it stands. Root then trusts a third
key, which hassmic named (`state/update-key`, kept by root as `/data/local/hassmic/page-update.pub`). That is the one
place where root takes hassmic's word on what it installs: **code that took hassmic over can name its own key there
and run as root**, which without it it cannot (below, "hassmic and root"). The maintainer chose that over a key set
through adb only (2026-11-24): the button bounds it as it bounds adb over Wi-Fi, against everyone but a hassmic
already taken over. With no check, whoever controls the server, and on plain HTTP anyone on the network between, can
install anything. The push port is not affected: it takes your update key only. A factory reset takes the key away,
but not what was installed under it: that ran as root, and an update brings the release key the Echo trusts next
(the factory copy's included), so a server that was trusted once can stay trusted. Reflash the Echo if that matters.
A plugin index of your own with no check takes its lists unchecked, never a package's listing: a file uploaded or
copied from another Echo cannot pass for official by it, and a listing by the project's key keeps this build's least
serial whatever index is set.

Online updates mean trusting the project's releases: GitHub Actions builds and signs them
(`.github/workflows/build.yml`) in a job that runs only for `main` and `release`, and its secret is the only copy of the
release key besides the maintainer's. Only a browser approved on the settings page (with the action button) may pick
the channel, and only Home Assistant's encrypted link may install.

Plugins never hold a hassmic update back and nothing is fetched for one: a plugin's `requires` names a version of
each scope it uses, and a later hassmic only adds to each (docs/plugins.md 3.1, held by `tools/mkplugmsg.py` and
`sdk/scopes.lock`), so what ran before runs after.  hassmic speaks to each plugin exactly the versions its manifest
requires, both ways: a message, a field or an enum's value of a later version is neither sent nor taken (every packet
is held to what the plugin speaks before it goes, by hassmic and by the C client alike, a value of a later version
turned into the earlier one `messages.def` names for it, or, where it names none (`>-`), the packet not sent to that
plugin at all: an older plugin is never told of an event as one it knows that did not happen, such as a new button as
a press of the action button; and every packet the C client takes, answers included).  Only an event whose whole
point is the new value may go unsent so (`BUTTON`, marked `drop`; `tools/mkplugmsg.py` refuses it on any other): an
answer, the end of a call, a state or a reason always arrives, so a plugin is never left waiting, a call's end never
lost, and what hassmic tells Home Assistant went to a plugin went.  So a plugin cannot reach
anything its manifest did not name; and a manifest that uses a key a later version brought
than it requires is refused, at the Echo and at the plugin index alike.  A later version that lets a plugin do more
than its capability's line at install said is marked so (`messages.def`, recorded in the lock), and an update that
comes to require one asks for the action button as a capability more does, with the words of every version between;
an install shows the words of every widening version it requires.  CI holds the lock and `messages.def` to the lock
of every release (each tag `v*`, betas of `main` and releases of `release` alike): every version a release carried is
exactly what it carried, nothing taken out and nothing added, so a released version cannot be changed by deleting its
record first, by a line added to it, nor by a newer release of the other branch that lacks it; a version that a
release this tree descends from carried cannot go.  What this does not hold: a version only a release of the other
branch carried may be missing here (a push to `release` that lacks `main`'s newest beta), as long as nothing here
says it otherwise; a shallow clone cannot tell which releases it descends from, so CI fails there and a PC's `make
unit-plugmsg` skips the check and says so; and a push to `main` and one to `release` build at once, so the release
job asks again right before it publishes, which leaves the seconds between that and the release (two releases that
carried one version otherwise then fail every build until one is deleted, DEVELOPMENT.md "CI and releases").  A plugin that needs more than the running hassmic has (after a fall back to an older one) is
kept, never started, and its card says why.  The new version's self test does not wait for the wake word plugin that
listened before only where the owner chose that it does not run (plugins off, `PLUGINS=0`, the plugin switched off,
a version the index withdrew) or where this hassmic cannot run it (it needs a newer one): before a review of 61028a8
an update installed with plugins or the plugin switched off waited for a plugin that never started, and never became
the fallback. Anything else that keeps it down under the new version (no launcher or a silent one, no sandbox, a
manifest or acceptance it does not read, a program not the one accepted) keeps the update from becoming the fallback:
913ec8d excused those too, and a version whose launcher never answered became the fallback while Amazon's wake word
listened, with no plugin running (a review of 913ec8d).  (Until 2026-11-23, never released: builds of each plugin
for another plugin API fetched before an update, held updates, "Install it all the same".)

## Bluetooth

- Keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker, and phones for calls), under `/data/local/hassmic/`.
- **Phone calls over Bluetooth** ("Phone calls over Bluetooth", off by default): a phone paired with the Echo (pairing
  only in the window Home Assistant's "Bluetooth pairing" switch opens) may send a call's audio to it whenever it
  says it has a call, and the Echo's microphone then goes to that phone. The phone decides where its call talks, as
  with a car kit: a call answered on the phone talks on the Echo when the phone routes it there (the owner's
  decision, 2026-11-25). So **a paired phone can open the Echo's microphone by saying it has a call**, without anyone
  at the Echo; the ring's call animation shows while it does, and Home Assistant's "Phone call" says `connected`.
  Without a call indicator from the phone (its voice assistant, an app's audio) the audio link is refused.
  Answering here, taking a call here, switching to a second call (which answers one that waits) and dialling from
  Home Assistant come only over its keyed connection. Pair only
  your own phones; unpairing is forgetting the Echo on the phone and pairing anew from Home Assistant (a phone the
  Echo no longer has a key for cannot connect).
- **Setup from a phone (Improv)**: offered only without a network: to anyone in range on an Echo that is not set up
  (nothing to lose there), only after a press of the action button on one that is. The Wi-Fi password crosses
  Bluetooth unencrypted, as with every Improv device; the Echo keeps only the network key derived from it.
  What the press is worth on an Echo that is set up: one minute, for whichever phone is connected in it (the one
  that was there or one that comes; not tied to one connection, the clients connect anew between their steps),
  one change of network (the next needs the button again), and only while the Echo advertises for a lost
  network. The press is taken for the setup only while a phone is connected and the ring shows that it waits.
  That wait begins only on an Echo that is set up and has been without its network for ten minutes; an Echo
  that is not set up asks for no press, and one that Home Assistant adds while a phone is still connected from
  the setup does not start asking for one (see "Settings page" above for what that was).
  What you see while Home Assistant waits for your press: it gives you a minute and a half, then ends its own
  connection, and the ring stops asking; when it asks again it connects again and the ring is back. A press
  in the gap between is not the phone's: the Echo starts listening as on any press. Press again once the ring
  shows the wait.
  **The ring does not tell whose phone waits.** Improv has no pairing: the Echo knows that something is
  connected over Bluetooth and asks for the button, not what or whose it is, and only one phone can be connected
  at a time. A press while the ring shows that wait lets whatever is connected within that minute change the
  Wi-Fi once: your phone, or someone else's in Bluetooth range that connected first and waited for you to press
  (it could then put the Echo onto a network of its own). Press only when you are setting it up yourself and
  your own app has asked for the button. **If your app cannot find the Echo or cannot connect to it while the
  ring shows that wait, something else holds the connection: do not press.** A device in range can hold it for
  as long as the Echo is without its network (it need not be let go: the Echo ends only a connection that has
  been silent for two minutes), so do not wait for the ring to go dark either: get the Echo back onto its
  network another way (its router back, or a reset and a fresh setup, which needs no press), or take the
  other device away. This is not closed, and the times above do not narrow it: Improv has nothing to tell one
  phone from another by.
  Before the button's press a connected phone gets the Echo's name and version and can make it play its
  identify sound and rainbow, once a minute at most.
  **Fixed 2026-11-20, in every version before it, released ones included**: with the Echo set up, its network
  gone for ten minutes and then back, a phone in Bluetooth range that had connected meanwhile (no button
  needed for that) could end its connection and connect again at once; the Echo took that connection while it
  was no longer offering the setup, treated it as authorized, and joined and saved any network the phone named,
  with no press. It took someone in Bluetooth range at the moment the Echo's network came back after an outage
  of ten minutes or more (which a jammer or a switched-off router provides). Now a connection that arrives
  while the setup is not offered is ended at once and gets nothing, and "authorized" is only ever one of the two
  facts above.

## Amazon downloads

The settings page's "Download from Amazon" registers the Echo to your Amazon account. Its tokens stay on the Echo
(`state/davs`, readable by hassmic only) and never reach the page. To be let in, the Echo signs with its attestation
key, which lives in the Echo's secure hardware: hassmic can ask it for a signature but never read it. For that the
daemon carries the group that opens the secure hardware (`drmrpc`), on the Echo Dot 3 also the one of its certificate
file (`keystore`). "Sign out" on the page removes the registration again.

## Factory reset

From Home Assistant only over its encrypted link (once it set a key), from the settings page only from an approved
browser, else by holding the button.

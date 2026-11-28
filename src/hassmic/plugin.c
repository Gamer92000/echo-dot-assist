/* plugin.h: the broker.  docs/plugins.md 2.6, 3, 4.13.
 *
 * One thread (broker) polls the launcher's socket and every plugin's control socket and stderr pipe, and keeps the
 * clocks: pings, timeouts, restarts.  Its rules (3.2):
 *  - hassmic never waits on a plugin: every send to one is MSG_DONTWAIT, on a socket whose other end only the plugin
 *    holds; what does not fit is dropped and counted.
 *  - A plugin's messages are hostile input: plugmsg.h checks every length, the broker every state, capability and
 *    rate (200 messages a second, then dropped).
 *  - What a plugin says is shown as text and nowhere interpreted.
 *
 * Locks.  bl guards plugs[] and every descriptor in it, and is only ever held over work that cannot wait: a
 * non-blocking send, a state change.  That is what lets the capture thread take it (wake_plugin's feed: issue #77,
 * the capture thread waits for nobody who can block).  Nothing takes core_lock or the store's lock (flash) while
 * holding bl; the order is core_lock, the store's, bl.  Descriptors are closed by the broker thread alone, under bl.
 *
 * The watchdog (2.6) keeps its time on a clock of the broker's own (btime), which stands still while the broker's
 * thread waits on someone else: a broker held up by core_lock or the flash must not kill the plugins it kept waiting.
 *
 * hassmic's own crashes (2.6): a fatal signal leaves a mark (state/plugins/.fall) once a plugin was started in this
 * run, naming the plugin whose message the broker's thread was reading, if it was.  The next start counts a mark that
 * names one (state/plugins/.falls), holds that plugin back (the wake engine's: for five minutes), and at three holds
 * back all but the wake engine's.  A mark that names nobody is said in the log and forgotten: a crash in a decoder or
 * in one of Amazon's libraries is none of the plugins'.  A second kind of mark ("core <id>") is for a fault in what
 * the core did with a plugin's report, on the broker's thread: never counted towards those three and never a reason
 * to refuse the plugin, but three within ten minutes make its start wait the five minutes (state/plugins/.cores).
 *
 * CPU (2.6): a plugin is another user's process, and Android's /proc shows hassmic none of those (hidepid): root's
 * launcher, its parent, reads its time when asked (PLUGRUN_USAGE) and the broker decides.
 *
 * A plugin as the wake word engine (4.13): wake_plugin forwards wake.h's calls as messages.  The stream is one
 * SOCK_SEQPACKET pair per plugin: each packet a block as wake_feed got it, with its position on wake_fed()'s clock
 * in front.  A detection comes back on the broker's thread, is checked against that clock, and goes to the core as
 * Pryon's does, from a thread of its own. */
#define _GNU_SOURCE
#include "plugin.h"
#include "board.h"
#include "clock.h"
#include "core.h"
#include "json.h"
#include "logread.h"
#include "plugin_net.h"
#include "plugin_store.h"
#include "plugmsg.h"
#include "plugrun.h"
#include "statefile.h"
#include "audio.h"
#include "btout.h"
#include "call.h"
#include "duck.h"
#include "musiclight.h"
#include "resample.h"
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/filter.h>
#include <net/if.h>
#include <netinet/in.h>
#include "../third_party/monocypher.h"
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define NPLUG         PS_MAX            /* one entry per plugin installed; a launcher slot only while it has a process */
/* Places (4.1, 2.6): plugins that run at once.  As many as the launcher has slots (a user 6900 + slot each, the
 * firewall's DROP rule for each: lockdown.sh PLUGIN_SLOTS), and within RUN_MEM_MB of their memory limits together.
 * Memory with real plugins is unmeasured (4.1), so the bound is what the launcher holds each to: its RLIMIT_AS, the
 * manifest's memory_mb (8 to 96).  The address space a process may map is more than what it keeps resident, never
 * less: the sum of the limits is a bound on what plugins can take that needs no measurement.  128 MB is a quarter of
 * the smallest Echo's memory (the Dot 2's 512 MB), less where /proc/meminfo says the Echo has less (run_mem).  A
 * plugin that finds no place waits for one, says so on its card, and starts when another stops (oldest first, each
 * as it fits); the wake word engine's plugin always has one (start).  Phase 1 ran two, "until memory with real
 * plugins is measured" */
#define RUN_MAX       PLUGRUN_SLOTS
#define RUN_MEM_MB    128
static int run_mem = RUN_MEM_MB;
/* RUN_MEM_MB, or a quarter of the Echo's memory where that is less (32 MB at least), from /proc/meminfo's MemTotal
 * (KB; 0 or less: not known, RUN_MEM_MB).  MemTotal is not what the Echo has: the kernel's own image and what is
 * reserved at boot are left out of it, and a Dot 2's 512 MB read as some 470: a quarter of that was 117 MB, and the
 * eighth plugin of 16 MB waited for a place that 4.1 promises it (P5 of a review of 7be9343).  So it is rounded up to
 * the next 128 MB first, the step every Echo's memory comes in (512 MB, 1 GB, 2 GB) */
static int run_mem_of(long kb)
{
    long mb = (kb + 1023) / 1024, phys = (mb + 127) / 128 * 128, q = phys / 4;
    if (kb <= 0 || q >= RUN_MEM_MB) return RUN_MEM_MB;
    return q < 32 ? 32 : (int)q;
}
#define RATE          16000
#define HELLO_MS      5000              /* from the start to HELLO, and from WELCOME to READY */
#define PING_MS       10000
#define PINGS_MAX     3
#define STOP_MS       2000
#define WAKE_OPEN_MS  5000
#define CHECK_MS      5000
#define MSGS_SEC      200
#define LINES_MIN     50                /* of a plugin's stderr into the log: /data is flash */
#define RETRY_MS      300000            /* a plugin that keeps crashing, and the wake engine's for ever */
#define CRASH_WINDOW  600000
#define CRASHES_MAX   5
/* What a detection may claim (4.13).  Arbitration waits 150 to 1000 ms for the other Echos' claims: one reported
 * later than this claims after they decided, and two Echos answer.  The ring holds 4 s and has to cover the word, the
 * aligner's 1.5 s back from its end and the time the report took */
#define HEARD_LATE    (RATE * 3 / 10)
#define HEARD_MIN     (RATE * 3 / 10)
#define HEARD_MAX     (RATE * 3 / 2)
#define HEARD_GAP_MS  1000
#define WORDS_CHANGES 30                /* another list of wake words, in 10 minutes: more is no owner editing files */
#define HEARD_BURST   20                /* in 10 minutes: then the engine is taken as broken (HASSMIC_PLUGIN_HEARD: fewer, for the tests) */
/* The broker's loop comes round every 250 ms (its poll) plus what it had to do, which is milliseconds.  A round that
 * took longer than this was the thread waiting (core_lock, the store's lock over a write to the flash), and counts as
 * this much on the broker's clock */
#define STALL_MS      1000
#define QUEUE_MS      5000              /* the control socket's queue takes not even the smallest message: for this long, then killed */
/* More than a full core's worth of CPU over a minute, for an ordinary plugin (2.6).  One thread that spins stays
 * under it by a hair and is left to the CPU group's weight; what goes over runs several.  HASSMIC_PLUGIN_CPU
 * "<percent of a core>:<ms>": less, for the tests */
#define CPU_WINDOW_MS 60000
#define CPU_MAX_PCT   100
#define CPU_ANSWER_MS 5000              /* the launcher's answer to PLUGRUN_USAGE: asked again after this (its sends do not wait) */
#define FALLS_MAX     3                 /* hassmic's own crashes in a plugin's message: then none but the wake engine's */
#define FALLS_FORGET_MS (24 * 3600 * 1000LL)
/* hassmic's own crashes in what the core did with one plugin's reports (a detection, a list of words): this many
 * within this time, and that plugin's start waits RETRY_MS, each time anew.  HASSMIC_PLUGIN_CORE "<count>:<ms>":
 * fewer and sooner, for the tests */
#define CORE_MARKS    3
#define CORE_WINDOW   CRASH_WINDOW
#define CORE_KEEP     8                 /* marks kept on file: what the count is made of */
/* storage (4.4): a bucket of writes per plugin, 10 that refill by one a minute.  statefile_write syncs the file and
 * its folder: one a second would be 86,400 synced writes a day on the Echo's flash, and hassmic holds its own files
 * to once a minute (arb.c SAVE_MS).  HASSMIC_PLUGIN_STORE "<bucket>:<ms a write>": others, for the tests */
#define STORE_BUCKET  10
#define STORE_REFILL  60000
/* the log (4.5): looked at once a second for a plugin that follows it, at most LOG_LINES lines and LOG_BUDGET bytes a
 * message (a packet holds 32 kB) */
#define LOG_EVERY_MS  1000
#define LOG_LINES     64
#define LOG_BUDGET    24000
/* A LOG with no OK for this long (the broker's own clock): said once in the log and on its card.  The log's flow
 * control (R9) came after the first clients were written, without a minor of its own (0.2 was never released): a
 * client of before it gets one LOG and no more, silently (a review of 7144548, 2) */
#define LOG_ANSWER_MS 10000
/* the network (4.6): CONNECTs under way at once, and in any minute, per plugin (the last NET_MINUTE asked: a window
 * that moves with each, not one that starts anew every minute and let twice as many through around its turn).  A
 * connect is a name looked up and a server asked: thirty a minute is a plugin that reconnects every two seconds,
 * which no forwarder needs */
/* a net capability: NET tells the plugin the Echo's address (SIP and SlimProto put it into their messages) */
#define NET_CAPS      (PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_INTERNET) | PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER))
#define NET_PENDING   2
#define NET_MINUTE    30
/* A request refused for what a plugin may not do (a CONNECT elsewhere): one line in the log at most this often, the
 * others counted into the next.  A line each, a plugin's 200 messages a second wrote 200 lines a second, past its own
 * 50 a minute, rotated the 2 MB log in under two minutes (followers lost lines in the gap) and wore the flash (R5 of a
 * review, 2026-11-21) */
#define REFUSAL_MS    10000
/* Phase 4, each a rate per plugin.  Its states and events in Home Assistant: 10 a second (4.10: what HA's
 * recorder writes to its database, an entity at a time; a sensor that changes faster is the plugin's to average).
 * A music source's state: 5 a second, each may pause Sendspin and a phone.  The volume: 5 a second, as a person turns
 * a knob.  UDP: 50 datagrams a second through SENDTO, a new peer a second (UDP_PEER: what goes to it then sends past
 * the broker, RTP's 50 a second never pass it), one search a second.  A stream opened: ten a minute.  A proposal: one a minute a row */
#define ENT_PER_SEC   10
#define MSTATE_PER_SEC 5
#define VOL_PER_SEC   5
#define VOL_PER_MIN   20                /* ... and twenty a minute (one every three seconds once a burst is spent) */
#define VOL_APPLY_MS  500               /* the Echo's volume set from a plugin's word at most this often: its latest */
#define VOL_SAY_MS    10000             /* "sets the volume" in the log: once in this, the rest counted */
#define SEND_PER_SEC  50
#define PEER_PER_SEC  1                 /* UDP_PEER: a new peer a second (two at once) */
/* button: the buttons' presses and holds a plugin hears, 5 a second (ten at once): a person presses a key about as
 * fast as that, a volume key held does not repeat (buttons.c: one press, one hold), and what is more is no person's
 * (a key that bounces, an event device gone wrong), which must not keep a plugin's queue full */
#define BUTTON_PER_SEC 5
#define MOPEN_MINUTE  10
#define PROPOSE_MS    60000
/* music plugins with a place in the core's table at once: every one that runs, and two more.  A lane is free again
 * only once its plugin's process and its stream's thread are gone (music_open_message), so with RUN_MAX lanes a
 * plugin that started in the place of one still on its way out found none (P7 of a review of 7be9343) */
#define LANES         (RUN_MAX + 2)
#define BUTTON_RESUME_MS (30LL * 60 * 1000)     /* the button resumes what it or a stop paused, as Sendspin's */
#define POS_MS        200               /* MUSIC_POS, five a second */
#define READ_MIN_MS   5                 /* a plugin's stream read after a small read: not before this (mstream_thread) */
static int read_min_ms = READ_MIN_MS;   /* (the unit test sets it 0: the pace alone) */
#define NET_LOOK_MS   5000              /* the Echo's address looked at for NET */
#define RELIST_MS     5000              /* what Home Assistant lists of the plugins, looked at again (ha_relist) */
/* Phase 5, calls (docs/plugins.md 3.6, 4.8; "calls" below).  A call rings as a Drop In does, RING_MS; the far
 * side's answer after a person answered here comes within LIVE_WAIT_MS (SIP's 200 OK and ACK: round trips, not
 * seconds), a call going out is taken within CALLING_MS (two minutes: SIP servers give up at 60 to 180 s), or ends.
 * RINGS_MAX unanswered rings of one plugin in RINGS_WINDOW, then it rings no more until the window has moved on (3.6:
 * picked numbers, 9.14).  A token is good for TOKEN_MS and once; TOKENS kept per plugin.  Its call messages: five a
 * second.  The audio from the plugin: twice the call's pace at most, the rest left in its socket (its writes then
 * say EAGAIN), towards it ~0.5 s of queue, then frames are dropped (what is late in a call is not wanted).
 * HASSMIC_PLUGIN_CALL_TIMES "<ring ms>:<live wait ms>:<calling ms>:<rings window ms>": shorter, for the tests */
#define RING_MS       30000
#define LIVE_WAIT_MS  10000
#define CALLING_MS    120000
#define RINGS_MAX     3
#define RINGS_WINDOW  600000
#define TOKEN_MS      10000
#define TOKENS        4
#define CALL_PER_SEC  5
/* A CALL_INCOMING refused busy (another call has the line): one a second, two at once; past that it is refused busy
 * without a look at the line.  Busy cost a plugin nothing, and one that asked five times a second would find the moment
 * the line comes free, and ring in it (W1 of a review of fc43c40: the end of a Drop In was such a moment) */
#define BUSY_PER_SEC  1
#define MEDIA_PER_SEC 100
#define MEDIA_PKT     (4 + CALL_PCM_BYTES)
#define MEDIA_SNDBUF  (MEDIA_PKT * 25)
/* a plugin's music stream (mstream_thread below) */
struct mstream { int fd, entry, src; unsigned rate, ch, gen; atomic_int end, closing; };     /* src: its lane's place in the core's table */
/* Why hassmic does not play what a music plugin writes, whatever it says it does (struct plug's myield): another
 * source started (MY_OTHER: heard again once every other source is off, as a phone without remote control), or
 * hassmic paused it (MY_HELD: the button, the stop word, Home Assistant's pause or stop; until hassmic resumes it or
 * the plugin starts anew, saying it paused and then that it plays).  A plugin was only told to pause before: one that
 * went on saying "playing" went on playing over the pause, and took every press of the button as its own (M6 of a
 * review of efde36e) */
enum { MY_NONE, MY_OTHER, MY_HELD };
struct bucket { double tokens; long long at; };
enum { P_FREE, P_SPAWNING, P_HELLO, P_CONFIG, P_RUN, P_STOPPING, P_DEAD };
enum { W_CLOSED, W_OPENING, W_OPEN, W_FAILED };

struct plug {
    int state, ctl, err, want;          /* want: it should run (enabled, accepted, the feature on) */
    int withdrawn;                      /* not started, or stopped, for the revocation list or because community plugins are
                                         * switched off (plugin_withdrawn_changed): started when neither holds */
    int slot;                           /* the launcher's, and with it the user 6900 + slot: -1 without a process */
    int mem;                            /* its memory limit (the manifest's memory_mb), what its place counts (RUN_MEM_MB) */
    int waits;                          /* with want: no place for it (RUN_MAX, RUN_MEM_MB), started when one is free */
    int room;                           /* with want: the wake word engine's plugin, its place made by stopping another */
    int failn;                          /* starts in a row that failed before the launcher had it (start(): the program
                                         * not opened, no socket or pipe): the next one's back-off, as a crash's */
    long long wait_since;               /* since when it waits: the oldest first */
    char id[PS_ID_MAX], why[200];       /* why: what the page shows of one that does not run */
    pid_t pid;
    pm_caps_t caps;
    /* the version of each scope spoken with it (sdk/PROTOCOL.md "Versions"): its manifest's requires, set at its
     * spawn.  Nothing of a scope it does not speak, or of a later version, goes to it (psend) or is taken from it */
    uint16_t ver[PM_SCOPES];
    long long since, next_start, stop_at, ping_at, crash[CRASHES_MAX], msg_sec, line_min;
    int pings, ncrash, starts, msgs, dropped, lines, lines_lost, off;     /* off: switched off for crashing */
    int unsent;                         /* packets not for it (a value its versions have no word for, messages.def `>-`) */
    uint32_t ping_id, next_id;
    char line[400]; size_t linen;
    /* wake */
    int stream, stream_sent, have_words, nwords, wake_ready;
    char words[16][41], words_list[33];
    struct { int state; char word[41]; long long asked, heard; } ws[WAKE_SLOTS];
    uint64_t pos, sent_pos;             /* the stream's clock after the last block; the block sent last (once per round) */
    long long heard_at[HEARD_BURST]; int nheard; unsigned blocks_lost;
    /* a file check under way: one at a time */
    struct { int active, staged, done, ok; uint32_t id; long long at; char list[33], item[41], title[65], why[201];
             struct { char name[33], value[257]; } fields[PS_FIELDS]; int nfields; } check;
    int look;                           /* look for items not checked yet */
    int lost, killed;                   /* its end of the control socket is closed; the launcher was told to kill it */
    int gone;                           /* removed while it ran: its entry goes with its process */
    /* Which process this is (gen_next: new at every spawn, never repeated in a run, whichever entry; `starts` began
     * at 1 again when an entry was taken anew, so an answer for a removed plugin's process could go to the next one's;
     * R6 of a review, 2026-11-21) and which package it runs: the digest of the manifest it was started under, which
     * the seal's wipe waits on (plugin_old_running: a wake word plugin's process, stopped for an update without
     * `wake`, wrote into the folder the update had just wiped; R1 of a review, 2026-11-21).  Its storage and files
     * are its plugin's until it is gone, never once the plugin was removed (revoked, until another process starts: a
     * removed one wrote into the storage of the same id installed again a moment later, R1; store_mine) */
    unsigned gen; uint8_t mdigest[32]; int revoked;
    int words_dirty, words_changes; long long words_told, words_since;      /* the core is told of a new list, 2 s apart at most */
    int unheard; long long unheard_log;                                      /* detections dropped since the log last said so */
    long long full_since;               /* on the broker's clock: since when the control socket's queue takes nothing (0: it does) */
    /* its CPU time when the launcher last read it and when that was, both as the launcher said them (ms, modulo
     * 2^32); when it was asked and has not answered, when to ask next (hassmic's clock; 0: not asked, at once) */
    uint32_t cpu_ms, cpu_at; int cpu_have; long long cpu_asked, cpu_next;
    char bad[8][41]; int nbad;          /* items it refused in this run: not asked again */
    /* storage: the write bucket (over its restarts: a plugin that crashes and comes back does not get a full one) */
    int tokens; long long token_at; int store_said;
    /* the log it follows (log.read): where the next LOG begins; when to look; a send that did not fit is sent again
     * from the same place.  Only the broker's thread reads or writes these */
    int log_on; struct logpos log_at; long long log_next, log_lines;
    uint32_t log_wait;                  /* the LOG sent last and not answered yet (its id): the next waits for its OK */
    long long log_sent;                 /* when that was, on the broker's clock (btime) */
    int log_deaf;                       /* said in the log that it does not answer LOG (once a process) */
    long long refused_at; int refused_n;        /* refusals said in the log: once in REFUSAL_MS, the rest counted */
    /* the network: CONNECTs under way, when the last NET_MINUTE were asked (a ring, over its restarts), the last
     * outcome for the card */
    int net_pending, net_next; long long net_asked[NET_MINUTE], net_ok, net_fail;
    int develop;                        /* started with the development flag (SIGSYS instead of EPERM) */
    /* phase 4.  audio.music (4.7, 3.5): its lane in the core's table of music sources (-1 none), what its
     * player says it does, whether another source took over since (what it writes is not played until it plays
     * again), when the button or a stop paused it (the button resumes it), its stream (the stream thread's, NULL
     * none), the opens of the last minute */
    int lane, mstate, myield; long long mby;
    int mdue;                           /* its place in the core's table to be told again (music_tell, by the broker) */
    int vol_want; long long vol_applied; int vol_untold; long long vol_said_at;     /* VOLUME_SET coalesced (M5): the latest + 1, 0 none */
    int vol_own;                        /* the value + 1 vol_flush is setting now (its own word coming back: not another's) */
    char mtitle[129], martist[129];
    struct mstream *ms;
    long long mopens[MOPEN_MINUTE]; int mopen_next;
    struct bucket r_ent, r_mstate, r_vol, r_vol_min, r_send, r_peer;
    long long bcast_at; int bcast_said;
    /* net.udp, net.discover: the broker's copy of each socket by handle (udp_open: there is one), its port and kind */
    int udp[PS_UDP_MAX + 1], udp_open[PS_UDP_MAX + 1], udp_kind[PS_UDP_MAX + 1]; unsigned short udp_port[PS_UDP_MAX + 1];
    /* proposals (4.3): one per row at a time, shown on the card until the owner takes or drops it */
    struct { char setting[33], value[16]; long long at; int live; } prop[PS_NET];
    /* ha.entities: the states it said, by the manifest's order (ent_have: a bit each), and which the core has not
     * been told of yet (the broker's loop tells it, without bl) */
    char ent[PS_ENTITIES][256]; uint32_t ent_have, ent_dirty;
    /* what it was told of the Echo (state: STATE; audio.volume: VOLUME; a net capability: NET) */
    int told_state, told_muted, told_vol, told_volume; uint32_t told_addr, told_mask; int told_net;
    /* phase 5, call: its name as the screen shows it beside the caller's; the call tokens its actions marked
     * "calls" brought (one use each, TOKEN_MS); when its rings went unanswered (RINGS_MAX in RINGS_WINDOW, over its
     * restarts: one that crashes to ring again does not start counting anew); its call messages' rate */
    char name[49];
    struct { char t[33]; long long at; } tok[TOKENS];
    long long rings[RINGS_MAX];
    struct bucket r_call, r_busy;
    /* phase 6, request.audio: the run it gets a copy of (rq_open), hassmic's end of that run's socket (-1
     * none), where the run is (samples Home Assistant got of it) and how many of them did not fit the socket; for the
     * card, the runs it had and what it lost, over its restarts, and when the last began */
    int rq_open, rq_fd; uint32_t rq_run, rq_off, rq_dropped;
    long long rq_runs, rq_lost, rq_last;
    int partner;                        /* not started for the partner rule (ps_partner): looked at again when the engine changes */
    struct bucket r_button;             /* button: the presses passed on (BUTTON_PER_SEC) */
    long long button_at; int button_n;  /* presses not passed on: said once in REFUSAL_MS, the rest counted (a person's
                                         * pace, never the plugin's doing: not refusal()'s, not in `dropped`) */
};

static pthread_mutex_t bl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv = PTHREAD_COND_INITIALIZER;       /* a slot's wake state, a plugin's state, a check's answer */
static struct plug plugs[NPLUG];
static int launcher = -1, launcher_ver, sandbox_ok, domain, feature, started, go, selftest_expected;
static int ending;                      /* hassmic ends (plugin_shutdown): nothing starts, nothing is told the core */
static int conf_off;                    /* PLUGINS=0 in hassmic.conf: the owner's word, not the build's failure (selftest) */
static long long start_ms;
static int tell_core;                   /* the broker thread: say core_wake_plugin_changed (asked by one who holds core_lock) */
static int relist_due;                  /* the broker thread: ha_relist (asked by one who holds core_lock: plugin_feature); 2: the digest only */
static int community_due;               /* the broker thread: community plugins were switched on or off (plugin_community) */
static char why_none[160] = "hassmic was started without the plugins' launcher";
static char wake_id[PS_ID_MAX];        /* the plugin the core wants as its wake word engine ("" none): started first,
                                         * never switched off for crashing */
static wake_cb wake_user_cb;
static int heard_burst = HEARD_BURST, retry_ms = RETRY_MS;     /* HASSMIC_PLUGIN_HEARD, HASSMIC_PLUGIN_RETRY: less, for the tests */
static int store_bucket = STORE_BUCKET, store_refill = STORE_REFILL;   /* HASSMIC_PLUGIN_STORE */
static int lines_min = LINES_MIN;       /* HASSMIC_PLUGIN_LINES: more, for a test that starts one plugin many times a minute */
static int cpu_max = CPU_MAX_PCT, cpu_window = CPU_WINDOW_MS, cpu_blind;       /* HASSMIC_PLUGIN_CPU; blind: said once that nothing watches */
static long long btime = 1, blast;      /* the broker's own clock (ms; never 0), and when it last ticked (bl) */
/* hassmic's own crashes (2.6) */
static int fall_fd = -1;                /* the mark's file, opened when the first plugin starts: the handler only writes */
/* handling: the entry whose message the broker's thread is reading: from its packet to where message() hands what it
 * says to the core.  What the core does with a detection or a new list of words is the core's own code on the core's
 * own state: a fault there is hassmic's, whichever plugin spoke last (core_begin) */
static volatile sig_atomic_t fall_armed, handling = -1;
/* in_core: the entry on whose behalf the broker's thread is in the core's code, from core_begin on: what the plugin
 * said was in range and of the right form, and the core's own code fell over it.  A mark of its own kind (fall_begin) */
static volatile sig_atomic_t in_core = -1;
static pid_t own_pid, broker_tid;
/* ... and the same for a thread that is not the broker's: main() at start lists and loads the words of the plugin
 * that is the engine, which is the core at work on what that plugin reported as much as core_wake_plugin_changed on
 * the broker's thread is.  A fault there named nobody ("fall -"), so nothing counted it and nothing slowed the loop
 * core_note is there to slow (a review, 2026-11-19).  A thread says so itself (plugin_core_enter): a place with its
 * thread id and the plugin's id.  For the handler's sake nothing but atomics: a place is taken with one
 * compare-and-swap (as -1, nobody's thread id, while the id is written), and the handler only loads the ids and
 * reads the text under its own, which no other thread writes while that id stands */
#define CORE_FOR 4
static struct { atomic_int tid; char id[PS_ID_MAX]; } core_for[CORE_FOR];
static int falls, falls_off, falls_dirty;                       /* the count, "none but the wake engine's", to be written (bl) */
static char fall_held[PS_ID_MAX];       /* the plugin the last crash's mark names: not started in this run */
static long long fall_until;            /* ... but for the wake engine's: until then (0: not asked for yet) */
static char core_held[PS_ID_MAX];       /* the plugin whose reports the core fell over CORE_MARKS times: its start waits */
static long long core_until;            /* ... until then (0: not asked for yet) */
static int core_marks = CORE_MARKS, core_window = CORE_WINDOW, core_count;      /* HASSMIC_PLUGIN_CORE; how many the last start counted */
static unsigned gen_next;               /* struct plug's gen (bl) */
/* What the core said of itself last (plugin_core_state; bl): what a plugin with `state` or `audio.volume` is told at
 * its READY and on every change */
static int cs_state, cs_muted, cs_vol = -1;
static uint32_t net_addr, net_mask; static long long net_looked;     /* the Echo's address, the broker's last look */
/* audio.music's lanes (plugin's music sources in the core's table, 4.7): the entry each plays for (-1 none), the
 * lane's number in the table (registered once, at its first use: a source never leaves the table), and the entry
 * whose stream went on last (bl): Home Assistant's media player's */
static atomic_int lane_of[LANES] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int lane_src[LANES] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
_Static_assert(LANES == 10, "lane_of[] and lane_src[] begin as -1 each");
static int music_newest = -1;

/* phase 5: the plugin's call there is, if any (bl; "calls" below) */
enum { C_NONE, C_RINGING, C_ANSWERED, C_CALLING, C_LIVE };
static struct {
    int state, entry, outgoing;         /* entry: plugs[]'s, -1 none */
    unsigned gen;                       /* the process it is with */
    long long since, answered, live;    /* when it rang or went out, a person answered here, it went live */
    char peer[120];                     /* as the screen and the log show it: "<plugin's name>: <who>" */
    int media;                          /* call.c's generation, -1 */
} pc = { .entry = -1, .media = -1 };
static int ring_ms = RING_MS, live_wait_ms = LIVE_WAIT_MS, calling_ms = CALLING_MS, rings_window = RINGS_WINDOW;   /* HASSMIC_PLUGIN_CALL_TIMES */

/* phase 6: what the core said of requests, queued for the broker thread (rq_*, "a request's copy" below) */
#define RQ_MAX        (RUN_MAX * 8)   /* a run's REQ_START, two texts and REQ_END for every listener, twice over */
struct rq { int entry; unsigned gen; uint16_t type; uint8_t a, b; uint32_t run, c, d; int fd; char text[1025]; };
static struct rq rq[RQ_MAX];
static unsigned rq_in, rq_out, rq_lost_n;       /* bl */
static uint32_t req_run;                        /* bl: the request now or last (REQ_START's number) */
static int req_live;                            /* bl: the core said start and not yet end */
static int req_texts;                           /* bl: texts are req_run's: it is open, or the last and no run began since
                                                 * that is none (plugin_request_none) */
static int bwake[2] = { -1, -1 };               /* a byte into [1] wakes the broker's poll: what is queued goes at once */
static int partner_due;                         /* bl: the engine changed: the partner rule looked at again (partner_check) */
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
/* phase 4 (below, "phase 4: the helpers" on) */
static void music_tell(int i);
static void mstream_end(struct plug *p);
static void tell_echo(struct plug *p);
static int bucket_take(struct bucket *b, int per_sec, long long now);
static int bucket_rate(struct bucket *b, double per_sec, double cap, long long now);
static void vol_flush(int i);
static void music_due(void);
static int shown_text(char *t);
static int lane_mine(const struct plug *p);
static void answer(struct plug *p, uint32_t id, int err);
static void ha_relist(void);
static void ha_flush(void);
static void call_over(int why_hangup, const char *why);
static void call_timers(long long now);
static void rq_close(struct plug *p, int why);
static void rq_drain(void);
static void partner_check(void);
static void token_new(struct plug *p, char out[33]);

/* The wake word plugins of this run, as they were started (start(): the manifest's `wake` of the package that runs).
 * What such a plugin says (its stderr), and the broker's lines about it (the reasons it gave, its detections, its
 * exit status and CPU time: values it chooses), are written sealed (logread.h): a forwarder with log.read must not be
 * its way out, and the mark is in the line, whatever is installed when the line is read (the seal decided at reading
 * time let them pass once the plugin was removed or updated without `wake`; a review, 2026-11-21).  Its own lock, the
 * last of all: say() is called with any of the others held */
/* The same, private, for a plugin that hears requests (request.audio, request.text): what it was told may be in what it
 * says and in the values the broker's lines about it carry (the blocks it did not take, the volume it set, a file
 * check's reason, ...), all of them its choice, and a log.read plugin with a way out must not be its way out when it
 * has none (a review of b294fa0, C3).  What stays is named in docs/SECURITY.md: its CPU time and when it acts.  And
 * for a plugin with `call` (4.11): it hears the room for as long as a call is live, and a person on the other end, so
 * what it says and the values the broker's lines about it carry are as much what was said as a listener's (a review
 * of 7be9343: they were public) */
#define PRIVATE_CAPS (PM_CAPBIT(PM_CAP_REQUEST_AUDIO) | PM_CAPBIT(PM_CAP_REQUEST_TEXT) | PM_CAPBIT(PM_CAP_CALL))
/* One place per entry of plugs[], written by the start that spawns a process in that entry: what its own package's
 * capabilities make it, so the table cannot fill up.  It held one place per id that had ever been sealed or private,
 * never given back (a removal freed nothing): sixteen such plugins installed and removed in one run, and the
 * seventeenth listener's broker lines were public again (a review of c4ace24, F2).  An entry is taken by another
 * plugin only once it is free, i.e. its process is gone and nothing of it is read any more (exited: its pipe read to
 * the end, then closed), so nothing the place's last plugin chose is said after that.  The same id in another entry
 * (removed, installed again, and given a fresh entry while the old one's stream thread held it) is that id's past:
 * its place is cleared, and the newest start decides */
enum { SEAL_NONE, SEAL_WAKE, SEAL_PRIVATE };
static pthread_mutex_t seal_mu = PTHREAD_MUTEX_INITIALIZER;
static char seal_ids[NPLUG][PS_ID_MAX];
static int seal_kind[NPLUG];
static void seal_note(int entry, const char *id, int kind)
{
    if (entry < 0 || entry >= NPLUG) return;
    pthread_mutex_lock(&seal_mu);
    for (int i = 0; i < NPLUG; i++) if (i != entry && !strcmp(seal_ids[i], id)) { seal_ids[i][0] = 0; seal_kind[i] = SEAL_NONE; }
    snprintf(seal_ids[entry], PS_ID_MAX, "%s", id); seal_kind[entry] = kind;
    pthread_mutex_unlock(&seal_mu);
}
/* T begins with a sealed or private plugin's id, then ':' or ' ': which of the two, else SEAL_NONE */
static int seal_of(const char *t)
{
    int r = SEAL_NONE;
    pthread_mutex_lock(&seal_mu);
    for (int i = 0; i < NPLUG && !r; i++) { size_t n = strlen(seal_ids[i]); if (n && !strncmp(t, seal_ids[i], n) && (t[n] == ':' || t[n] == ' ')) r = seal_kind[i]; }
    pthread_mutex_unlock(&seal_mu);
    return r;
}
/* "plugins: ..." into the log, one line in one write; sealed when it is about a wake word plugin, private when about
 * one that hears requests (it begins with its id, as every line of the broker's about one plugin does) */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char b[LOG_MARKED_MAX]; va_list ap; int k;
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    k = seal_of(b);
    log_marked_f(k == SEAL_WAKE ? LOG_SEALED_MARK : k == SEAL_PRIVATE ? LOG_PRIVATE_MARK : NULL, "plugins: %s", b);
}

static struct plug *plug_of(const char *id)        /* bl held */
{
    for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && !strcmp(plugs[i].id, id)) return &plugs[i];
    return NULL;
}
static int running(const struct plug *p) { return p && (p->state == P_SPAWNING || p->state == P_HELLO || p->state == P_CONFIG || p->state == P_RUN); }

/* ---------------------------------------------------------------- hassmic's own crashes (2.6) */

/* Counted only when there is evidence of one.  init's stop is SIGKILL (main.sh svc_stop, svc_restart), so an engine
 * switch, an update and a power cut all end hassmic without a word, and none of them is a crash: what counts is a
 * mark that only a fatal signal's handler writes.  And only in a run that started a plugin: before that a crash is
 * none of theirs.  The files' names begin with a dot, as the staging file's: "fall" would pass for a plugin's id */
static void fall_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", ps_dir(), name); }

static const int fall_sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
#define NFALLSIG ((int)(sizeof fall_sigs / sizeof fall_sigs[0]))
static struct sigaction fall_was[NFALLSIG];

/* A fatal signal, on the thread it came to.  Only what a handler may do: one write to the descriptor opened before
 * (no statefile here: nothing of it may be called from a handler, and the kernel keeps what a dead process wrote),
 * then the signal goes on to whoever had it before (bionic's debuggerd and its tombstone, or the default: the end).
 * Not in a child that has not reached its exec: that is not hassmic ending */
static void fall_handler(int sig, siginfo_t *si, void *uc)
{
    char b[8 + PS_ID_MAX]; size_t n = 5; int i = handling, k = in_core, core = 0, e = errno; const char *id = NULL;
    (void)uc;
    if (fall_armed && getpid() == own_pid) {
        int me = (int)syscall(SYS_gettid);
        fall_armed = 0;
        /* the plugin whose message was being handled ("fall"), or on whose behalf the core was at work ("core"):
         * only if the thread that fell is the one that was: the broker's, or one that said so (core_for) */
        if (i < 0 || i >= NPLUG) { i = k; core = 1; }
        if (me == (int)broker_tid) { if (i >= 0 && i < NPLUG) id = plugs[i].id; }
        else for (int f = 0; f < CORE_FOR && !id; f++) if (atomic_load(&core_for[f].tid) == me) { id = core_for[f].id; core = 1; }
        memcpy(b, core ? "core " : "fall ", 5);
        for (const char *c = id; c && *c && n < 5 + PS_ID_MAX; c++) b[n++] = *c;
        if (n == 5) { memcpy(b, "fall ", 5); b[n++] = '-'; }
        b[n++] = '\n';
        if (write(fall_fd, b, n) < 0) { }
    }
    for (int k = 0; k < NFALLSIG; k++) sigaction(fall_sigs[k], &fall_was[k], NULL);
    /* a fault happens again when this returns; a signal that was sent (abort(), a kill) has to be sent again */
    if (si->si_code <= 0) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
    errno = e;
}

/* an orderly end: what a library's destructor does after it (Amazon's abort there now and then) is no crash */
static void fall_disarm(void) { fall_armed = 0; }

/* Any thread, no lock: before the first plugin of this run is started.  The mark's file is made empty here, which is
 * no mark; the handler fills it */
static void fall_arm(void)
{
    static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER; char path[400]; struct sigaction sa;
    pthread_mutex_lock(&one);
    if (fall_fd < 0) {
        fall_path(path, sizeof path, ".fall");
        /* (never waited for: a FIFO of that name without a reader is no file to write a mark into) */
        if ((fall_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)) >= 0) {
            memset(&sa, 0, sizeof sa); sa.sa_sigaction = fall_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
            own_pid = getpid();
            for (int k = 0; k < NFALLSIG; k++) sigaction(fall_sigs[k], &sa, &fall_was[k]);
            atexit(fall_disarm);
            fall_armed = 1;
        }
    }
    pthread_mutex_unlock(&one);
}

/* At start, before any thread of ours: the last run's mark, taken away, and counted if it names a plugin.  One that
 * names nobody is a crash somewhere in hassmic while plugins ran (a decoder, one of Amazon's libraries, an abort in
 * ws.c), with nothing to say a plugin had a part in it: said in the log and forgotten.  Counted, three of those in a
 * day switched every plugin off for faults that were not theirs (a review, 2026-11-19) */
/* A mark of the second kind: hassmic fell in the core's own code, at work on what plugin ID had reported (a
 * detection whose positions were in range, a list of words of the right form).  That is a fault of hassmic's, and
 * the first fix for marks rightly stopped holding it against the plugin: a bug in the wake path must not silence
 * the wake word for good.  But left at that, a plugin whose report hassmic falls over every time made a loop with
 * nothing to slow it: the wake engine's plugin is started in main() at every start, reports again, hassmic falls
 * again, init starts it again.  So these marks are kept with their time (state/plugins/.cores, "<id> <ms>" on the
 * monotonic clock, which hassmic's restarts share; after a reboot the old ones are in the future or long ago, and
 * go), never counted towards "all plugins off" and never a reason to refuse the plugin: CORE_MARKS of one plugin's
 * within CORE_WINDOW hold its start back for RETRY_MS, then it is tried; if hassmic falls again, the next start
 * counts again and waits again.  An Echo in that state is down for seconds every five minutes instead of all the
 * time, and its log and the plugin's card say what over.
 * That "never refused" is the wake engine's alone (start()).  Left to every plugin, an ordinary one whose report
 * the core falls over took hassmic down every five minutes for good, and nothing but the owner finding its card
 * ended that: what an Echo without its wake word is worth does not hold for a plugin the Echo listens without.  So
 * for those the marks count as its crashes do (exited(): the third waits 30 s, the fourth RETRY_MS, the fifth
 * within the window switches it off until the owner switches it on again), and hassmic's fault is still said */
static void core_note(const char *id)
{
    char path[400], line[80], out[CORE_KEEP * 48]; long long t[CORE_KEEP], now = now_ms(); int n = 0; size_t on = 0; FILE *f;
    fall_path(path, sizeof path, ".cores");
    if ((f = ps_fopen(path))) {
        while (fgets(line, sizeof line, f)) {
            char was[PS_ID_MAX]; long long at;
            if (sscanf(line, "%24s %lld", was, &at) != 2 || strcmp(was, id) || at > now || now - at >= core_window) continue;
            if (n == CORE_KEEP - 1) memmove(t, t + 1, sizeof t[0] * (size_t)--n);
            t[n++] = at;
        }
        fclose(f);
    }
    t[n++] = now;
    for (int i = 0; i < n; i++) on += (size_t)snprintf(out + on, sizeof out - on, "%s %lld\n", id, t[i]);
    statefile_write(path, 0600, out, on);
    core_count = n;
    if (n >= core_marks) {
        snprintf(core_held, sizeof core_held, "%.24s", id);
        say("hassmic's last run ended in a crash in what it does with what %s reports, the %d. time in %d min: a fault of hassmic's, not counted against the plugins, but %s is started only after a wait this time (the wake word engine's: every time; another plugin: switched off at the %d.)", id, n, core_window / 60000, id, CRASHES_MAX);
    } else say("hassmic's last run ended in a crash in what it does with what %s reports (%d in %d min; at %d its start waits): a fault of hassmic's, not counted against the plugins", id, n, core_window / 60000, core_marks);
}

static void fall_begin(void)
{
    char path[400], b[16 + PS_ID_MAX] = ""; FILE *f; int marked = 0;
    fall_path(path, sizeof path, ".falls");
    if ((f = ps_fopen(path))) { if (fscanf(f, "%d", &falls) != 1 || falls < 0) falls = 0; fclose(f); }
    if (falls > FALLS_MAX) falls = FALLS_MAX;
    fall_path(path, sizeof path, ".fall");
    if ((f = ps_fopen(path))) { marked = fgets(b, sizeof b, f) && (!strncmp(b, "fall ", 5) || !strncmp(b, "core ", 5)); fclose(f); }
    if (marked) {
        char count[400], n[16];
        b[strcspn(b, "\n")] = 0;
        if (b[0] == 'c') { if (ps_id_ok(b + 5)) core_note(b + 5); }
        else if (ps_id_ok(b + 5)) {
            snprintf(fall_held, sizeof fall_held, "%.24s", b + 5);
            if (falls < FALLS_MAX) falls++;
            /* the count first, then the mark away: a power cut between the two counts one crash twice, never none */
            fall_path(count, sizeof count, ".falls"); snprintf(n, sizeof n, "%d\n", falls);
            statefile_write(count, 0600, n, strlen(n));
            say("hassmic's last run ended in a crash while it read a message of %s: %d of %d; that plugin is held back", fall_held, falls, FALLS_MAX);
        } else say("hassmic's last run ended in a crash, not in a plugin's message: not counted against the plugins");
        unlink(path); statefile_sync(ps_dir());
    }
    if ((falls_off = falls >= FALLS_MAX)) say("hassmic crashed %d times in a plugin's message: none but the wake word engine's starts until they are switched on again on the page", FALLS_MAX);
}

/* The broker's thread, from a plugin's message into the core: what follows is not the plugin's any more (handling),
 * and is the core at work on what it reported (in_core: core_note).  core_end puts it back for what the broker
 * still does of that message */
static int core_begin(void) { int h = handling; in_core = h; handling = -1; return h; }
static void core_end(int h) { handling = h; in_core = -1; }

/* Any other thread, no lock: from here to plugin_core_leave it is in the core's code for what plugin ID reported
 * (core_for).  No place free: not marked, as before */
void plugin_core_enter(const char *id)
{
    int me = (int)syscall(SYS_gettid);
    for (int f = 0; f < CORE_FOR; f++) {
        int none = 0;
        if (!atomic_compare_exchange_strong(&core_for[f].tid, &none, -1)) continue;
        snprintf(core_for[f].id, sizeof core_for[f].id, "%s", id);
        atomic_store(&core_for[f].tid, me);
        return;
    }
}

void plugin_core_leave(void)
{
    int me = (int)syscall(SYS_gettid);
    for (int f = 0; f < CORE_FOR; f++) if (atomic_load(&core_for[f].tid) == me) atomic_store(&core_for[f].tid, 0);
}

/* The broker's thread, no lock: the count to the flash when it changed; forgotten after a day up (not once it stands
 * at three: that is the owner's to lift) */
static void falls_keep(void)
{
    char path[400], b[16]; int n, w;
    pthread_mutex_lock(&bl);
    if (falls && falls < FALLS_MAX && now_ms() - start_ms > FALLS_FORGET_MS) { falls = 0; falls_dirty = 1; say("a day up: hassmic's earlier crashes are forgotten"); }
    w = falls_dirty; falls_dirty = 0; n = falls;
    pthread_mutex_unlock(&bl);
    if (!w) return;
    fall_path(path, sizeof path, ".falls"); snprintf(b, sizeof b, "%d\n", n);
    if (n) statefile_write(path, 0600, b, strlen(b)); else unlink(path);
}

/* ---------------------------------------------------------------- sending (bl held) */

/* W is rewritten in place for P (pm_fit below) and marked so: one encoded packet, one plugin.  Every caller encodes
 * its packet for the plugin it sends it to (pm_winit, pm_put_*, then here), never one buffer for two plugins: the
 * second would get what the first speaks (a value fitted to an older peer's fallback, or nothing at all) */
static int psend(struct plug *p, struct pm_w *w, int fd)
{
    long n = pm_wlen(w); struct iovec iov = { w->b, n < 0 ? 0 : (size_t)n };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } c;
    struct msghdr h = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (n < 0 || p->ctl < 0) { if (n < 0) say("%s: a message that does not fit was not sent", p->id); return -1; }
    if (w->fitted) { say("%s: a message encoded for another plugin was not sent (hassmic's error)", p->id); return -1; }
    /* Only what it speaks (its manifest's requires): a plugin never gets a message it did not ask for, a field of a
     * later version than it speaks, or a value of an enum it does not have.  A value a later version brought goes as
     * its fallback, the value of the version the plugin speaks that sdk/messages.def names for it (pm_fit, in place),
     * so no message is lost for it: a version-1 plugin hears that its call ended whatever new reason a later hassmic
     * has (finding 4 of a review of 60a8589: the packet was dropped with a line in the log).  Where messages.def names
     * none (`>-`: no value the plugin has is right, finding 3 of a review of 4c55b54) the packet is not for it, and
     * goes nowhere: what a plugin of that version cannot be told (a key of the buttons it does not know, never heard
     * as the action button), not hassmic's error: said once a start (p->unsent counts them), and 0, done with.
     * Only a message marked `drop` is ever such a one (pm_droppable: tools/mkplugmsg.py refuses `>-` on an enum any
     * other message holds, finding 1 of a review of 61e3d8e), BUTTON alone so far, whose one sender reads nothing
     * of the answer.  So a caller that tells its own caller "it went" from a 0 here (action_send and entity_set to
     * Home Assistant and the page, the told_* of STATE, VOLUME and NET, LOG's wait, FILE_CHECK's, WAKE_OPEN's) never
     * gets that 0 for a packet that did not go: what would be one (a definition that broke the rule) is hassmic's
     * error, said and -1.
     * Then the packet as it is about to go, held to what the plugin speaks as the plugin will read it (pm_check).
     * Every path here sends by its capability, which requires covers (ps_manifest_parse); one that does not is
     * hassmic's error, said and not sent */
    { struct pm_hdr ph = { 0, 0, 0 }; const uint8_t *body; size_t bl; int f = 0, r = pm_head(w->b, (size_t)n, &ph, &body, &bl) ? 1 : ((f = pm_fit(ph.type, w->b + PM_HEADER, bl, p->ver)) == 1 ? 0 : pm_check(ph.type, body, bl, p->ver));
      w->fitted = 1;
      if (f == 1 && !pm_droppable(ph.type)) { say("%s: %s not sent: a value its version has no word for, on a message that must arrive (hassmic's error)", p->id, pm_name(ph.type)); return -1; }
      if (f == 1) { if (!p->unsent++) say("%s: %s not sent: a value of a later version than it speaks, which its version has no word for (the first this start)", p->id, pm_name(ph.type)); return 0; }
      if (r) { say("%s: %s not sent: %s (hassmic's error)", p->id, pm_name(ph.type), r < 0 ? "not of what it speaks" : "not as what it speaks has it"); return -1; } }
    if (fd >= 0) {
        memset(&c, 0, sizeof c);
        h.msg_control = &c; h.msg_controllen = sizeof c.b;
        c.h.cmsg_level = SOL_SOCKET; c.h.cmsg_type = SCM_RIGHTS; c.h.cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(&c.h), &fd, sizeof(int));
    }
    if (sendmsg(p->ctl, &h, MSG_DONTWAIT | MSG_NOSIGNAL) != n) {                               /* it does not read: its loss */
        if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) && !p->full_since) p->full_since = btime;
        p->dropped++; return -1;
    }
    p->full_since = 0;
    return 0;
}

/* bl held: 1 when W, encoded for P, is not for P at all (pm_fit's 1: a value its versions have no word for, on a
 * message marked drop), asked of a copy so that W goes to psend as it is.  A packet over the copy's room is said to
 * be P's (0): psend decides it then, as for any other */
static int pnot_for(const struct plug *p, const struct pm_w *w)
{
    uint8_t c[256]; long n = pm_wlen(w); struct pm_hdr h; const uint8_t *body; size_t bl;
    if (n <= 0 || n > (long)sizeof c || pm_head(w->b, (size_t)n, &h, &body, &bl)) return 0;
    memcpy(c, w->b, (size_t)n);
    return pm_fit(h.type, c + PM_HEADER, bl, p->ver) == 1;
}

static void send_err(struct plug *p, uint32_t id, int code)
{
    uint8_t b[16]; struct pm_w w; struct pm_err m = { (uint16_t)code };
    pm_winit(&w, b, sizeof b); pm_put_err(&w, id, &m); psend(p, &w, -1);
}
static void send_ok(struct plug *p, uint32_t id, int fd)
{
    uint8_t b[16]; struct pm_w w;
    pm_winit(&w, b, sizeof b); pm_put_ok(&w, id); psend(p, &w, fd);
}

/* ---------------------------------------------------------------- starting and stopping */

static void launcher_say(uint32_t type, uint32_t slot, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const int *fds, int nfd)    /* bl held */
{
    struct plugrun_msg m = { type, slot, a, b, c, d }; struct iovec iov = { &m, sizeof m };
    union { struct cmsghdr h; char b[CMSG_SPACE(3 * sizeof(int))]; } ctl;
    struct msghdr h = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (launcher < 0) return;
    if (nfd) {
        memset(&ctl, 0, sizeof ctl);
        h.msg_control = &ctl; h.msg_controllen = CMSG_SPACE((size_t)nfd * sizeof(int));
        ctl.h.cmsg_level = SOL_SOCKET; ctl.h.cmsg_type = SCM_RIGHTS; ctl.h.cmsg_len = CMSG_LEN((size_t)nfd * sizeof(int));
        memcpy(CMSG_DATA(&ctl.h), fds, (size_t)nfd * sizeof(int));
    }
    if (sendmsg(launcher, &h, MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof m) say("the launcher does not take a message");
}

static void wake_released(struct plug *p)      /* bl held: nothing of its wake side is left */
{
    for (int s = 0; s < WAKE_SLOTS; s++) p->ws[s].state = p->ws[s].state == W_CLOSED ? W_CLOSED : W_FAILED;
    p->have_words = p->nwords = p->stream_sent = 0;
    if (p->stream >= 0) { close(p->stream); p->stream = -1; }
}

static void stop(struct plug *p, const char *why);

/* Is the program open as FD the one whose BLAKE2b-256 is WANT (the manifest's `digest`)?  Read through the
 * descriptor the launcher will exec, so what is checked is what runs.  The store checks this when it installs; here
 * it is checked again at every start, because the manifest decides what the plugin may do and the two are separate
 * files: a folder in which they did not belong together (an install that broke off between them, before the store
 * put packages in place whole; a file written there by hand) ran a program nobody had accepted under a manifest
 * that named another (a review, 2026-11-20).  A few milliseconds for a program of megabytes, once a start */
static int program_is(int fd, const uint8_t want[32])
{
    crypto_blake2b_ctx ctx; uint8_t have[32], *b; struct stat st; off_t o = 0; ssize_t k; int ok = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > PS_PACKAGE_MAX || !(b = malloc(65536))) return 0;
    crypto_blake2b_init(&ctx, 32);
    while (o < st.st_size && (k = pread(fd, b, 65536, o)) > 0) { crypto_blake2b_update(&ctx, b, (size_t)k); o += k; }
    crypto_blake2b_final(&ctx, have);
    ok = o == st.st_size && !memcmp(have, want, 32);
    free(b);
    return ok;
}

/* The plugins' SELinux domain executes one type of file and no other (scripts/system/sepolicy-hassmic.rules:
 * hassmic_plugin_file), a type /data gives nothing: so hassmic labels the program, through the very descriptor it hands
 * the launcher, before every start where the launcher found that domain (a new install, an update, a package from
 * before the policy had the type: all alike here).  Only from system_data_file, which /data gives every file of
 * hassmic's own (the policy's one relabel for hassmic); a program of any other type is not started, with that said:
 * its exec in the domain would fail at the launcher's last step (EACCES) and say nothing of why.  Not where the
 * kernel has no SELinux (the PC: a security.* attribute is then the superuser's alone).  0: labelled */
#define PLUGIN_FILE_CON "u:object_r:hassmic_plugin_file:s0"
static int program_label(int fd, char *why, size_t cap)
{
    char have[128]; ssize_t n;
    if (access("/sys/fs/selinux", F_OK)) return 0;
    if ((n = fgetxattr(fd, "security.selinux", have, sizeof have - 1)) > 0) { have[n] = 0; if (!strcmp(have, PLUGIN_FILE_CON)) return 0; }
    if (!fsetxattr(fd, "security.selinux", PLUGIN_FILE_CON, sizeof PLUGIN_FILE_CON, 0)) return 0;
    snprintf(why, cap, "its program could not be given the plugins' SELinux type (%s; it is %s): not started", strerror(errno), n > 0 ? have : "of no type that can be read");
    return -1;
}

/* ID's program opened and what its manifest says of it (struct run: the digest, the capabilities the broker holds
 * its messages to, the limits the launcher gives it), all under one hold of the store's lock: the lock an install
 * writes the package under (ps_stage_install's renames), so they are of one package as far as the list of plugins
 * is.  start() takes all of it from here and nothing from its own earlier look, which only decides whether to
 * start at all: after the reread below the digest was the new manifest's and the capabilities and limits still
 * the old one's, so an update that dropped a capability ran with it, and one that added `wake` ran without (a
 * review, 2026-11-20).  AGAIN: the list read anew first (an install ends, lets go of the lock, and its caller scans only
 * then: in between the list has the old manifest and the folder the new program).  It was the digest copied under
 * the lock and the program opened after it: an update in between, and a healthy plugin was told "not the one its
 * manifest names ... Remove it", or its start failed on a folder being renamed (a review, 2026-11-20).  The
 * descriptor, or -1 (errno); -2: it may not run as the list has it now (start() is called again by whoever changed
 * that).  The hash is the caller's, on the descriptor, without the lock */
struct run { uint8_t digest[32], manifest[32]; pm_caps_t caps; uint16_t ver[PM_SCOPES]; int mem, threads, develop; char name[49]; };
static int program_open(const char *id, struct run *r, int again)
{
    const struct ps_plugin *sp, *list; char path[500]; int fd = -2;
    ps_lock();
    if (again) ps_scan(&list);
    /* an update without `wake` wipes what the plugin was given under the seal (plugin_store.c seal_upkeep) once no
     * process of the old package runs: here, its process gone, before the new one starts and reads its storage */
    ps_seal_settle(id);
    if ((sp = ps_find(id)) && sp->enabled && !sp->refused[0] && sp->accepted) {
        memcpy(r->digest, sp->m.digest, 32); memcpy(r->manifest, sp->m.manifest_digest, 32);
        r->caps = sp->m.caps; r->mem = sp->m.memory_mb; r->threads = sp->m.threads; r->develop = sp->develop;
        memcpy(r->ver, sp->m.requires, sizeof r->ver);
        snprintf(r->name, sizeof r->name, "%s", sp->m.name);
        fd = ps_program(id, path, sizeof path) ? (errno = ENAMETOOLONG, -1) : open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    ps_unlock();
    return fd;
}

/* bl held: the places taken, but for ME's (NULL: none): every process (running or on its way out) and the wake word
 * engine's plugin whenever it is wanted and not off, running or not: while another is stopped for it (room), between
 * a crash and its start again, held back after hassmic fell over it.  Counted only while it had a process, its place
 * went to whoever asked in between: a plugin that waited took it while the engine was down for a second, and was
 * stopped again at the engine's start (P3 of a review of 7be9343); or another start took the last launcher slot while
 * the engine's own had bl let go for its program, and the engine was never started again (P2).  *USED of them, *MEM
 * their memory limits in MB */
static int engine_waits(const struct plug *q) { return q->state == P_DEAD && q->want && !q->off && (q->room || (wake_id[0] && !strcmp(q->id, wake_id))); }
static void places(const struct plug *me, int *used, int *mem)
{
    *used = *mem = 0;
    for (int i = 0; i < NPLUG; i++) {
        const struct plug *q = &plugs[i];
        if (q == me) continue;
        if (running(q) || q->state == P_STOPPING || engine_waits(q)) { (*used)++; *mem += q->mem; }
    }
}

/* Any thread, no lock held.  Starts it if it may run and does not: the program opened here, and handed to the
 * launcher with the plugin's ends of a control socket and a stderr pipe */
static void start(const char *id)
{
    int prog = -1, sv[2] = { -1, -1 }, pp[2] = { -1, -1 }, ok = 0, slot = -1, used = 0, mem = 16, mem_used = 0, fits, wait = 0;
    const struct ps_plugin *sp; struct plug *p; char why[300] = ""; int held = 0, room = 0, off = 0, wd = 0, partner = 0; struct run run;
    ps_lock();
    if ((sp = ps_find(id))) {
        if (!sp->enabled) snprintf(why, sizeof why, "switched off");
        /* 3.3: not started, whoever signed it and whatever was accepted (a package that came past the store's check);
         * withdrawn: plugin_withdrawn_changed starts it when a list no longer names it */
        else if (sp->refused[0]) { snprintf(why, sizeof why, "%s", sp->refused); wd = (sp->withdrawn[0] && !sp->anyway) || sp->community_off; }
        else if (!sp->accepted) snprintf(why, sizeof why, "waits for you to accept what it may do");
        /* 3.3's partner: no request.audio plugin beside a wake word engine nobody vouches for.  Where both are here all
         * the same (the install and the engine's choice refuse the second of the two), this one is not started: the
         * engine is what keeps the Echo working.  partner_check starts it once the engine is another */
        else if (ps_partner(&sp->m, sp->tier, 0, why, sizeof why)) partner = 1;
        else ok = 1;                    /* (whether to start; what is started, and as what, is program_open's) */
        mem = sp->m.memory_mb;          /* (what its place counts: program_open's run.mem is the same manifest's) */
    }
    ps_unlock();
    pthread_mutex_lock(&bl);
    p = plug_of(id);
    if (ending) { pthread_mutex_unlock(&bl); return; }
    if (!sp) { if (p && p->state == P_DEAD) p->state = P_FREE; pthread_mutex_unlock(&bl); return; }
    if (running(p)) { pthread_mutex_unlock(&bl); return; }
    if (ok && (launcher < 0 || !sandbox_ok || !feature)) { ok = 0; snprintf(why, sizeof why, "%s", why_none); }
    /* hassmic's own crashes (2.6): the plugin the mark names is not started in this run; at three, none but the
     * engine's.  The wake engine's is not refused for good: an Echo with it down hears no wake word at all (Amazon's
     * stands in only for a plugin that is missing), and "for this run" is until someone finds the page.  It is held
     * back as one that keeps crashing is, RETRY_MS, and then tried: if hassmic falls over it again, the next start
     * holds it back again, so the Echo is down for seconds every five minutes and listens in between */
    if (ok && !strcmp(id, fall_held)) {
        if (strcmp(id, wake_id)) { ok = 0; snprintf(why, sizeof why, "hassmic crashed while it read this plugin's message: held back for this run (off and on again starts it)"); }
        else {
            if (!fall_until) { fall_until = now_ms() + retry_ms; say("%s is the wake word engine's plugin: tried again in %d s", id, retry_ms / 1000); }
            if (now_ms() < fall_until) held = 1; else fall_held[0] = 0;
        }
    }
    else if (ok && falls_off && strcmp(id, wake_id)) { ok = 0; snprintf(why, sizeof why, "hassmic crashed %d times in a plugin's message: they stay off until you switch them on again", FALLS_MAX); }
    /* ... and the one whose reports the core fell over, time and again (core_note): the wake engine's is never
     * refused, its start waits; any other is switched off at the fifth, as one that crashes five times is */
    if (ok && !held && !strcmp(id, core_held)) {
        int wake = !strcmp(id, wake_id);
        if (!wake && core_count >= CRASHES_MAX) {
            ok = 0; off = 1;
            snprintf(why, sizeof why, "hassmic itself crashed %d times in %d minutes over what this plugin reported (a fault of hassmic's): switched off. Off and on again starts it", core_count, core_window / 60000);
        } else {
            if (!core_until) core_until = now_ms() + (wake || core_count > core_marks || retry_ms < 30000 ? retry_ms : 30000);
            if (now_ms() < core_until) held = 2; else { core_held[0] = 0; core_until = 0; }
        }
    }
    /* on its way out (an update, a removal and an install right after): started when its process is gone */
    if (p && p->state == P_STOPPING) { p->gone = 0; p->want = ok; pthread_mutex_unlock(&bl); return; }
    /* (a free entry whose stream's thread has not ended is not free yet: the thread writes into it, and its lane is held by it: L3) */
    for (int i = 0; i < NPLUG; i++) if (plugs[i].state == P_FREE && !plugs[i].ms && slot < 0) slot = i;
    places(p, &used, &mem_used);
    fits = used < RUN_MAX && mem_used + mem <= run_mem;
    /* The places (RUN_MAX, run_mem), and the wake word engine's plugin always has one: refused a place, it was not
     * tried again, and the Echo heard no wake word because other plugins had started first (the engine chosen after
     * them, or the engine's plugin back from a crash).  The newest of the others is stopped for it, one at a time
     * until it fits, and waits for a place again.  Not a place kept free for it instead: the engine can be chosen
     * while every place is taken, so something has to be stopped in any case, and places kept empty on every Echo
     * whose engine might one day be a plugin would be lost to all the others */
    if (ok && !fits && !strcmp(id, wake_id) && !held) {
        struct plug *v = NULL; int leaving = 0;
        for (int i = 0; i < NPLUG; i++) {
            leaving |= plugs[i].state == P_STOPPING;
            if (running(&plugs[i]) && strcmp(plugs[i].id, wake_id) && (!v || plugs[i].since > v->since)) v = &plugs[i];
        }
        if (!leaving && v) {
            v->want = 1; v->waits = 1; v->wait_since = now_ms();
            stop(v, "stopped for the wake word engine's plugin, which always has a place: it starts again when a place is free");
            say("%s: %s", v->id, v->why);
        }
        room = 1;
    }
    /* any other waits (timers() starts the oldest that fits when a place is free); it was refused for good in phase
     * 1, and started only when its owner switched it off and on */
    else if (ok && !fits && strcmp(id, wake_id) && !held) {
        wait = 1;
        if (used >= RUN_MAX) snprintf(why, sizeof why, "waits for a place: %d plugins run at once on this Echo, and %d do. It starts when one of them stops", RUN_MAX, used);
        else snprintf(why, sizeof why, "waits for a place: the plugins that run at once may use %d MB of memory between them, %d MB are taken and this one may use %d MB. It starts when one of them stops", run_mem, mem_used, mem);
    }
    if (!p) {
        if (slot < 0) { pthread_mutex_unlock(&bl); return; }
        p = &plugs[slot]; memset(p, 0, sizeof *p); snprintf(p->id, sizeof p->id, "%s", id); p->ctl = p->err = p->stream = p->slot = p->rq_fd = -1; p->state = P_DEAD; p->lane = -1;
        p->tokens = store_bucket; p->token_at = now_ms();
    }
    p->want = ok; p->withdrawn = wd; p->partner = partner; p->mem = mem; p->room = room;
    if (!wait) p->waits = 0;
    else {
        if (!p->waits) { p->waits = 1; p->wait_since = now_ms(); say("%s: %s", id, why); }
        snprintf(p->why, sizeof p->why, "%s", why); p->next_start = 0;
        pthread_mutex_unlock(&bl); return;
    }
    if (!ok) {
        snprintf(p->why, sizeof p->why, "%s", why); p->next_start = 0;
        if (off && !p->off) { p->off = 1; say("%s: %s", id, why); }
        pthread_mutex_unlock(&bl); return;
    }
    if (held == 1) {                    /* timers() starts it then, as it does one that crashed */
        snprintf(p->why, sizeof p->why, "hassmic crashed while it read this plugin's message: held back, and tried again %d min after the start", (retry_ms + 59999) / 60000);
        p->next_start = fall_until; pthread_mutex_unlock(&bl); return;
    }
    if (held) {
        snprintf(p->why, sizeof p->why, "hassmic itself crashed %d times in %d minutes over what this plugin reported: a fault of hassmic's. Held back, and tried again %d s after the start", core_count, core_window / 60000, (int)((core_until - now_ms() + 999) / 1000));
        p->next_start = core_until; pthread_mutex_unlock(&bl); return;
    }
    if (room) {                         /* when the one stopped for it is gone: timers() asks again */
        snprintf(p->why, sizeof p->why, "starts as soon as another plugin has made room for it");
        p->next_start = now_ms() + 250; pthread_mutex_unlock(&bl); return;
    }
    pthread_mutex_unlock(&bl);

    fall_arm();
    if ((prog = program_open(id, &run, 0)) == -2) return;       /* (no longer one that runs: whoever changed it calls again) */
    if (prog >= 0 && !program_is(prog, run.digest)) {           /* once more against the folder as it is now */
        close(prog);
        if ((prog = program_open(id, &run, 1)) == -2) return;
    }
    if (prog < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) || pipe2(pp, O_CLOEXEC)) {
        /* Wanted all the same: asked again after 1, 5, 30 s, then every RETRY_MS, as a crash is (never counted as
         * one: nothing of the plugin's ran).  These returns left it P_DEAD, wanted, with nothing to start it again but
         * its owner; the engine's entry, counted by places() whenever it is wanted, held its place and its memory
         * from every plugin that waits for good (F1 of a review of 5aa32ee).  Descriptors run out (EMFILE, ENFILE)
         * and a program being renamed in by an install are passing; one gone for good costs a line every 5 min */
        int e = errno, n;
        say("%s: cannot start (%s)", id, strerror(e));
        if (prog >= 0) close(prog);
        if (sv[0] >= 0) { close(sv[0]); close(sv[1]); }
        pthread_mutex_lock(&bl);
        if (!ending && (p = plug_of(id)) && p->state == P_DEAD && p->want && !p->waits && !p->next_start) {
            n = p->failn < 4 ? ++p->failn : 4;
            p->next_start = now_ms() + (n <= 1 ? 1000 : n == 2 ? 5000 : n == 3 ? 30000 : retry_ms);
            snprintf(p->why, sizeof p->why, "could not be started (%s): tried again in %lld s", strerror(e), (p->next_start - now_ms() + 999) / 1000);
        }
        pthread_mutex_unlock(&bl);
        return;
    }
    if (!program_is(prog, run.digest)) {
        static const char not_it[] = "the program in its folder is not the one its manifest names: it is not started. Remove it and install it again";
        pthread_mutex_lock(&bl);
        if ((p = plug_of(id)) && p->state == P_DEAD) { p->want = 0; p->next_start = 0; snprintf(p->why, sizeof p->why, "%s", not_it); }
        pthread_mutex_unlock(&bl);
        say("%s: %s", id, not_it);
        close(prog); close(sv[0]); close(sv[1]); close(pp[0]); close(pp[1]);
        return;
    }
    if (domain == PLUGRUN_DOMAIN_PLUGIN && program_label(prog, why, sizeof why)) {
        pthread_mutex_lock(&bl);
        if ((p = plug_of(id)) && p->state == P_DEAD) { p->want = 0; p->next_start = 0; snprintf(p->why, sizeof p->why, "%s", why); }
        pthread_mutex_unlock(&bl);
        say("%s: %s", id, why);
        close(prog); close(sv[0]); close(sv[1]); close(pp[0]); close(pp[1]);
        return;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK); fcntl(pp[0], F_SETFL, O_NONBLOCK);
#ifdef PLUGIN_TEST_BETWEEN
    PLUGIN_TEST_BETWEEN(id);            /* (tests/unit/plugin_harness.h: what another thread does while bl is let go) */
#endif
    pthread_mutex_lock(&bl);
    slot = -1;
    for (int k = 0; k < PLUGRUN_SLOTS && slot < 0; k++) { int taken = 0; for (int i = 0; i < NPLUG; i++) taken |= plugs[i].state != P_FREE && plugs[i].slot == k; if (!taken) slot = k; }
    /* `ending` again: plugin_shutdown may have come while bl was let go for program_open; a plugin spawned after it is
     * never told STOP and holds the shutdown's wait for all of its 2 s (a review of 655efd7) */
    /* the places again, as they are now: bl was let go for program_open, and another start may have taken the last
     * one meanwhile (two at once both saw it free), or the package is a newer one than the list had (program_open
     * read it again) that may use more memory.  Not the wake engine's: its place was made for it */
    if (!ending && (p = plug_of(id)) && p->state == P_DEAD && p->want && strcmp(id, wake_id)) {
        int u, mu;
        places(p, &u, &mu);
        if (u >= RUN_MAX || mu + run.mem > run_mem) {
            if (!p->waits) { p->waits = 1; p->wait_since = now_ms(); }
            p->mem = run.mem; snprintf(p->why, sizeof p->why, "waits for a place: none was left as it started (another plugin took it, or this version may use more memory, %d MB). It starts when one stops", run.mem);
            say("%s: %s", id, p->why);
            slot = -1;
        }
    }
    if (!ending && (p = plug_of(id)) && p->state == P_DEAD && p->want && slot >= 0) {
        int fds[3] = { prog, sv[1], pp[1] };
        p->slot = slot;
        p->ctl = sv[0]; p->err = pp[0]; p->caps = run.caps; memcpy(p->ver, run.ver, sizeof p->ver); p->state = P_SPAWNING; p->since = now_ms(); p->starts++;
        p->mem = run.mem; p->waits = p->room = 0; p->failn = 0;
        if (!++gen_next) gen_next = 1;
        p->gen = gen_next; memcpy(p->mdigest, run.manifest, 32); p->revoked = 0;
        seal_note((int)(p - plugs), id, run.caps & PM_CAPBIT(PM_CAP_WAKE) ? SEAL_WAKE : run.caps & PRIVATE_CAPS ? SEAL_PRIVATE : SEAL_NONE);
        p->pings = 0; p->msgs = 0; p->unsent = 0; p->linen = 0; p->look = 1; p->nbad = 0; p->why[0] = 0; p->check.active = 0; p->wake_ready = 0; p->lost = p->killed = p->gone = 0;
        p->pos = 0; p->sent_pos = UINT64_MAX; p->nheard = 0;
        p->words_dirty = 0; p->unheard = 0;
        p->full_since = 0; p->cpu_at = p->cpu_ms = 0; p->cpu_have = 0; p->cpu_asked = p->cpu_next = 0; p->pid = 0;
        p->log_on = 0; p->log_lines = 0; p->log_wait = 0; p->log_deaf = 0; p->net_pending = 0; p->store_said = 0; p->refused_n = 0; p->refused_at = 0;
        p->button_n = 0; p->button_at = 0;
        /* phase 4: nothing of the last process's (its stream and sockets went with it in exited) */
        p->mstate = 0; p->myield = 0; p->mby = 0; p->mdue = 0; p->vol_want = 0; p->mtitle[0] = p->martist[0] = 0; p->ent_have = 0; p->bcast_at = 0;
        p->told_state = p->told_muted = p->told_vol = p->told_volume = -1; p->told_net = 0;
        /* phase 5: no token of the last process's; its unanswered rings stay counted */
        memset(p->tok, 0, sizeof p->tok); snprintf(p->name, sizeof p->name, "%s", run.name);
        /* the development flag only where the launcher reads it (version 3); an older one starts it without */
        p->develop = run.develop && launcher_ver >= 3;
        if (run.develop && launcher_ver < 3) say("%s: the launcher is an older one (version %d): started without the development flag", id, launcher_ver);
        for (int s = 0; s < WAKE_SLOTS; s++) p->ws[s].state = W_CLOSED;
        launcher_say(PLUGRUN_SPAWN, (uint32_t)p->slot, (uint32_t)run.mem, (uint32_t)run.threads,
                     !strcmp(id, wake_id) ? PLUGRUN_CPU_WAKE : PLUGRUN_CPU_ORDINARY, p->develop ? PLUGRUN_SPAWN_DEVELOP : 0, fds, 3);
        sv[0] = pp[0] = -1;
    }
    /* Wanted and left without a slot (the launcher's were all taken as bl was let go, or the places' look above): asked
     * again in a moment, the engine's as one that waits for room.  It was left P_DEAD with nothing to start it again
     * but its owner (P2 of a review of 7be9343); one that waits for a place is started by timers() as any */
    else if (!ending && (p = plug_of(id)) && p->state == P_DEAD && p->want && !p->waits) {
        p->next_start = now_ms() + 250;
        if (!strcmp(id, wake_id)) { p->room = 1; snprintf(p->why, sizeof p->why, "starts as soon as another plugin has made room for it"); }
    }
    pthread_mutex_unlock(&bl);
    close(prog); close(sv[1]); close(pp[1]);
    if (sv[0] >= 0) { close(sv[0]); close(pp[0]); }
}

/* bl held: it ends.  The launcher's SIGTERM comes STOP_MS after our STOP */
static void stop(struct plug *p, const char *why)
{
    uint8_t b[16]; struct pm_w w;
    if (!running(p)) return;
    if (pc.entry == p - plugs && pc.gen == p->gen) call_over(PM_HANGUP_STOPPED, "the plugin is stopped");     /* (told before its STOP) */
    rq_close(p, 0);                     /* (a request's copy ends with it: STOP says why) */
    pm_winit(&w, b, sizeof b); pm_put_stop(&w, 0);
    if (p->state != P_SPAWNING) psend(p, &w, -1);
    p->state = P_STOPPING; p->stop_at = now_ms() + (p->ctl >= 0 ? STOP_MS : 0);
    if (why != p->why) snprintf(p->why, sizeof p->why, "%s", why);
}

/* bl held: the launcher's child is gone.  What it held is released; one that should run is started again after 1, 5,
 * 30 s, then every 5 minutes, and switched off after five crashes in ten minutes, unless it is the wake word engine:
 * an Echo that stops listening for its wake word until someone finds the page is the worse fault */
static void log_lines(struct plug *p, const char *d, size_t n);
static void exited(struct plug *p, const char *how, int crashed)
{
    long long now = now_ms(); int recent = 0;
    if (p->ctl >= 0) close(p->ctl);
    /* what it wrote last is still in the pipe when the launcher's word comes first (the poll's order): read, then
     * closed.  It says why, often (a crash's message, the development flag's refused call) */
    if (p->err >= 0) { char d[512]; ssize_t r; int k = 0; while (k++ < 16 && (r = read(p->err, d, sizeof d)) > 0) log_lines(p, d, (size_t)r); close(p->err); }
    /* its call ends with it: the media at once, the core told, the claim let go (it hears nothing more of the room) */
    if (pc.entry == p - plugs && pc.gen == p->gen) call_over(0, "the plugin ended");
    rq_close(p, 0);                     /* (and a request's copy: nobody reads it) */
    p->ctl = p->err = p->slot = -1;
    wake_released(p);
    /* phase 4: its stream ends (the thread tells the core), its UDP sockets close, its entities are unavailable */
    mstream_end(p);
    if (!p->ms && lane_mine(p)) atomic_store(&lane_of[p->lane], -1);
    for (int h = 0; h <= PS_UDP_MAX; h++) if (p->udp_open[h]) { close(p->udp[h]); p->udp_open[h] = 0; }
    p->ent_dirty |= p->ent_have; p->ent_have = 0;
    if (p->check.active && !p->check.done) { p->check.done = 1; p->check.ok = -1; }
    p->state = P_DEAD; p->next_start = 0;
    pthread_cond_broadcast(&bcv);
    if (p->gone) { p->state = P_FREE; return; }             /* removed: its entry is another plugin's to have */
    if (!crashed) return;
    if (how != p->why) snprintf(p->why, sizeof p->why, "%s", how);
    memmove(p->crash + 1, p->crash, sizeof p->crash[0] * (CRASHES_MAX - 1)); p->crash[0] = now;
    if (p->ncrash < CRASHES_MAX) p->ncrash++;
    for (int i = 0; i < p->ncrash; i++) recent += now - p->crash[i] < CRASH_WINDOW;
    if (recent >= CRASHES_MAX && strcmp(p->id, wake_id)) {
        p->want = 0; p->off = 1;
        say("%s: %s; five times in ten minutes: switched off", p->id, how);
        return;
    }
    p->next_start = now + (recent <= 1 ? 1000 : recent == 2 ? 5000 : recent == 3 ? 30000 : retry_ms);
    say("%s: %s; started again in %lld s", p->id, how, (p->next_start - now) / 1000);
}

/* ---------------------------------------------------------------- what a plugin is told */

/* no lock held: every file list (FILES), then CONFIG.  The first CONFIG closes a plugin's start: it answers READY */
static void send_config(const char *id)
{
    static uint8_t buf[PM_MAX_PACKET]; static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
    struct ps_item *items = malloc(sizeof *items * PS_ITEMS); const struct ps_plugin *sp; struct pm_w w; struct plug *p;
    if (!items) return;
    pthread_mutex_lock(&one);
    ps_lock();
    if ((sp = ps_find(id))) {
        for (int i = 0; i < sp->m.nsettings; i++) {
            const struct ps_setting *s = &sp->m.settings[i]; struct pm_files fm; int n;
            if (s->type != PS_FILES) continue;
            n = ps_items(sp, s->name, items, PS_ITEMS);
            fm.list = pm_s(s->name); fm.items.n = (uint16_t)n;
            pm_winit(&w, buf, sizeof buf); pm_put_files(&w, 0, &fm);
            for (int k = 0; k < n; k++) {
                struct pm_files_items it = { pm_s(items[k].id), (uint8_t)items[k].checked, pm_s(items[k].title), { { 0 }, (uint16_t)items[k].nfields }, { { 0 }, (uint16_t)items[k].nfiles } };
                pm_add_files_items(&w, &it);
                for (int f = 0; f < items[k].nfields; f++) { struct pm_files_items_fields e = { pm_s(items[k].fields[f].name), pm_s(items[k].fields[f].value) }; pm_add_files_items_fields(&w, &e); }
                for (int f = 0; f < items[k].nfiles; f++) { struct pm_files_items_files e = { pm_s(items[k].files[f].suffix), (uint32_t)items[k].files[f].size }; pm_add_files_items_files(&w, &e); }
            }
            pthread_mutex_lock(&bl);
            if ((p = plug_of(id)) && (p->state == P_CONFIG || p->state == P_RUN)) psend(p, &w, -1);
            pthread_mutex_unlock(&bl);
        }
        { struct pm_config m = { { { 0 }, 0 } }; static char val[PS_SETTINGS][PS_VALUE_MAX];
        for (int i = 0; i < sp->m.nsettings; i++) if (sp->m.settings[i].type != PS_FILES) m.settings.n++;
        pm_winit(&w, buf, sizeof buf); pm_put_config(&w, 0, &m);
        for (int i = 0, k = 0; i < sp->m.nsettings; i++) {
            struct pm_config_settings e;
            if (sp->m.settings[i].type == PS_FILES) continue;
            ps_config_get(sp, sp->m.settings[i].name, val[k], sizeof val[k]);
            e.name = pm_s(sp->m.settings[i].name); e.value = pm_s(val[k]); pm_add_config_settings(&w, &e); k++;
        }
        pthread_mutex_lock(&bl);
        if ((p = plug_of(id)) && (p->state == P_CONFIG || p->state == P_RUN)) psend(p, &w, -1);
        pthread_mutex_unlock(&bl);
        }
    }
    ps_unlock();
    pthread_mutex_unlock(&one);
    free(items);
}

/* ---------------------------------------------------------------- a plugin's messages (the broker thread) */

/* bl held.  What the detection's positions feed in the core: the replay's start, the aligner, the level the gain
 * starts from and arbitration's own score.  So they are held to the stream's clock and to what a word can be */
static int heard_ok(struct plug *p, const struct pm_wake_heard *m, long long now)
{
    if (m->slot >= WAKE_SLOTS || p->ws[m->slot].state != W_OPEN) return 0;
    if (m->begin >= m->end || m->end > p->pos || p->pos - m->end > HEARD_LATE) return 0;
    if (m->end - m->begin < HEARD_MIN || m->end - m->begin > HEARD_MAX) return 0;
    if (p->ws[m->slot].heard && now - p->ws[m->slot].heard < HEARD_GAP_MS) return 0;
    p->ws[m->slot].heard = now;
    return 1;
}

/* rmu: the mark's write and its look, each in one; no other lock is held under it */
static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;

/* No lock: under which version the wake plugin was last ready (2.6), and whether it ran as listed: an official
 * listing held under that build ("listed 1"), or it ran by the owner's switch for community plugins ("listed 0": a
 * file's, a listing that build did not read, or a community listing).  plugin_selftest_ok holds an update back for
 * a listing it does not read only where the plugin ran as listed: one that ran as a file's under the build before
 * was no listing that build read either, and community plugins switched off since is the owner's choice for it (a
 * review of 3fcd319, S8: every update was held, and none became the fallback).  A community listing is no more:
 * with community plugins off it does not run under any build, so it never ran but by that switch, and "listed 1"
 * for it held every update after the owner switched them off (a review of 9cd7a77, T5) */
static void ready_note(const char *id)
{
    char path[400], t[160]; const struct ps_plugin *sp; int listed, n;
    ps_lock(); listed = (sp = ps_find(id)) && sp->tier == PS_OFFICIAL; ps_unlock();
    n = snprintf(t, sizeof t, "%s\nlisted %d\n", VERSION, listed);
    snprintf(path, sizeof path, "%s/%s/ready", ps_dir(), id);
    pthread_mutex_lock(&rmu);
    statefile_write(path, 0644, t, (size_t)n);
    pthread_mutex_unlock(&rmu);
}

/* What a plugin may ask between WELCOME and its READY: what it needs to load (its files), to say its wake words, and
 * its storage and the log's place (the SDK's ready callback runs before READY goes out).  Each of those
 * is answered at once, on this thread.  Not CONNECT: it waits for a server (10 s), and READY is due within HELLO_MS of
 * WELCOME; a plugin that connected in its ready callback to a server slow to answer was stopped for not saying READY,
 * counted as a crash, and switched off after five (a review, 2026-11-21).  It connects once it runs (the SDK's tick) */
static int before_ready(uint16_t type)
{
    return type == PM_READY || type == PM_FILE_OPEN || type == PM_WAKE_WORDS || type == PM_STORE_GET || type == PM_STORE_PUT || type == PM_STORE_DEL
           || type == PM_LOG_FOLLOW;
}

static void net_done(const struct pnet_req *r, int fd, int err);
static int net_live(const struct pnet_req *r);

/* bl held: a refusal of what P may not do, said once in REFUSAL_MS (REFUSAL_MS), the others counted into the next line
 * or timers()'s */
static void refusal(struct plug *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void refusal(struct plug *p, const char *fmt, ...)
{
    char b[300]; va_list ap; long long now = now_ms();
    p->dropped++;
    if (p->refused_at && now - p->refused_at < REFUSAL_MS) { p->refused_n++; return; }
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (p->refused_n) say("%s: %s (and %d more refused since the last such line)", p->id, b, p->refused_n);
    else say("%s: %s", p->id, b);
    p->refused_at = now; p->refused_n = 0;
}

/* A request that hassmic answers (OK, ERR or its own answer) */
static int is_request(uint16_t type)
{
    return type == PM_FILE_OPEN || type == PM_STORE_GET || type == PM_STORE_PUT || type == PM_STORE_DEL || type == PM_LOG_FOLLOW || type == PM_CONNECT
           || type == PM_UDP_OPEN || type == PM_UDP_PEER || type == PM_SENDTO || type == PM_BROADCAST || type == PM_PROPOSE || type == PM_MUSIC_OPEN
           || type == PM_MUSIC_STATE || type == PM_MUSIC_CLOSE || type == PM_VOLUME_SET || type == PM_ENTITY_STATE || type == PM_EVENT
           || type == PM_CALL_INCOMING || type == PM_CALL_OUT || type == PM_CALL_RINGING_THERE || type == PM_CALL_LIVE || type == PM_CALL_END;
}

/* ps_lock held by the caller, bl not (the order: ps_lock, then bl): whether process GEN of entry P may touch the
 * storage and files of ID now.  Not once its plugin was removed (R1 of a review, 2026-11-21: the same id installed
 * again a moment later had its storage written by the removed one's process); the mark read here, under ps_lock:
 * plugin_remove sets it before its ps_remove, which waits for this lock, so a write seen allowed lands before the
 * folder goes, never in a new install's.  A process of an older package may, until it is gone: an ordinary update
 * stops it, and the cursor a forwarder writes in its stop callback is what the next version goes on from (F1).  It
 * was refused for its package's digest, and every update lost that cursor (a review of 7144548, 1); the seal needs
 * no such rule: its wipe waits until no process of the old package runs (plugin_old_running, seal_upkeep) and is done
 * before the new one starts (program_open's ps_seal_settle).  The digest against the store's list was a race
 * besides: plugin_installed stops the old process, then scans */
static int store_mine(struct plug *p, unsigned gen, const char *id)
{
    int r;
    pthread_mutex_lock(&bl); r = p->gen == gen && !p->revoked; pthread_mutex_unlock(&bl);
    return r && ps_find(id);
}
static void store_message(struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen);
static void connect_message(int i, struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen);
static void udp_message(struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen);
static void music_open_message(int i, struct plug *p, const struct pm_hdr *h, const uint8_t *body, size_t blen);
static void entity_message(struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen);
static void call_message(int i, struct plug *p, const struct pm_hdr *h, const uint8_t *body, size_t blen);

/* The store's question before the seal's wipe (plugin_store.h ps_busy), under its lock: does a process of ID run a
 * package other than MANIFEST's (or one that was removed)?  Set before anything scans: hassmic's start and the tests'
 * harness alike */
static int plugin_old_running(const char *id, const uint8_t manifest[32])
{
    struct plug *p; int r;
    pthread_mutex_lock(&bl);
    p = plug_of(id);
    r = p && (running(p) || p->state == P_STOPPING) && (p->revoked || memcmp(p->mdigest, manifest, 32));
    pthread_mutex_unlock(&bl);
    return r;
}
__attribute__((constructor)) static void busy_hook(void) { ps_busy = plugin_old_running; }

/* One message of plugin P (its slot's index I).  Called without locks; takes what it needs */
static void message(int i, const uint8_t *pkt, size_t len)
{
    struct plug *p = &plugs[i]; struct pm_hdr h; const uint8_t *body; size_t blen; long long now = now_ms(); int state, r, spoken;
    char id[PS_ID_MAX]; pm_caps_t caps, need;
    pthread_mutex_lock(&bl);
    snprintf(id, sizeof id, "%s", p->id); state = p->state; caps = p->caps;
    if (now - p->msg_sec >= 1000) { p->msg_sec = now; p->msgs = 0; }
    if (++p->msgs > MSGS_SEC) { p->dropped++; if (p->msgs > MSGS_SEC * 5) stop(p, "it floods hassmic with messages"); pthread_mutex_unlock(&bl); return; }
    /* After STOP (P_STOPPING, STOP_MS until the launcher's SIGTERM, KILL_MS more until it is gone) its storage writes
     * are done: a forwarder writes its cursor as it ends, and a STORE_PUT left unanswered held the client until it was
     * killed, its cursor not kept, so the next start sent up to a minute of lines again (F1, found by a forwarder's tests,
     * 2026-11-21).  Any other request is answered FAILED: left unanswered, a request that waited when STOP came held the
     * client until the kill, and its stop callback never ran (R2 of a review, 2026-11-21) */
    if (pm_head(pkt, len, &h, &body, &blen) || state == P_DEAD || state == P_FREE) { pthread_mutex_unlock(&bl); return; }
    if (state == P_STOPPING && h.type != PM_STORE_PUT && h.type != PM_STORE_DEL) {
        if (is_request(h.type) && h.id) send_err(p, h.id, PM_ERR_FAILED);
        pthread_mutex_unlock(&bl); return;
    }
    /* What it speaks is what its manifest requires (sdk/PROTOCOL.md "Versions"): a type this build has not is unknown
     * (never a reason to end); one of a capability not granted is denied, as ever; one of a granted capability's
     * later version than it requires is unknown to it, as to a hassmic of that version.  Its form is held to the
     * version it speaks: bytes after its last field are as malformed as a field cut short */
    if (!pm_dir(h.type)) { send_err(p, h.id, PM_ERR_UNKNOWN); pthread_mutex_unlock(&bl); return; }
    spoken = pm_spoken(h.type, p->ver); need = pm_caps(h.type);
    r = spoken ? pm_check(h.type, body, blen, p->ver) : 0;
    if (r > 0 || pm_dir(h.type) == '<' || (state == P_HELLO) != (h.type == PM_HELLO) || (state == P_CONFIG && !before_ready(h.type))
        || (h.type == PM_READY && state != P_CONFIG)) {
        send_err(p, h.id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return;
    }
    if (need && !(caps & need)) { send_err(p, h.id, PM_ERR_DENIED); p->dropped++; pthread_mutex_unlock(&bl); return; }
    if (!spoken) { send_err(p, h.id, PM_ERR_UNKNOWN); pthread_mutex_unlock(&bl); return; }

    switch (h.type) {
    case PM_HELLO: {
        /* (b: WELCOME at its bound, every scope there will ever be: PM_SCOPES_MAX of 2 + 24 + 2 bytes) */
        struct pm_hello m; struct pm_hello_scopes e; uint8_t b[PM_HEADER + 300 + PM_SCOPES_MAX * 28]; struct pm_w w; char zone[64] = "", lacks[89] = ""; uint16_t has[PM_SCOPES] = { 0 }, ver[PM_SCOPES]; int n = 0, twice = 0;
        _Static_assert(sizeof b >= PM_HEADER + 4 + 2 + 48 + 3 * (2 + 64) + 2 + PM_SCOPES_MAX * (2 + 24 + 2), "WELCOME at its bound: every scope there will ever be");
        pm_get_hello(body, blen, &m);
        pm_winit(&w, b, sizeof b);
        /* what its client speaks, against what its manifest requires: the client offers every scope it has at the
         * version of its SDK, and must have each one required at that version or later (an SDK older than the
         * manifest would misread what hassmic sends).  A name it offers that this build has not is a later SDK's:
         * none of it is spoken.  (lacks: what fits the card's why, 200 bytes with its words around it; the scopes in
         * their order as long as each fits whole with room left for " and more", which says that others were left
         * out: never a name cut, never a list that looks whole and is not, finding 7 of a review of 4c55b54) */
        while (pm_next_hello_scopes(&m.scopes, &e)) { int sc = pm_scope_of(e.name.s, e.name.n); if (sc >= 0) { twice |= has[sc] != 0; has[sc] = e.version; } }
        { static const char more[] = " and more"; int cut = 0;
          for (int sc = 0; sc < PM_SCOPES; sc++)
              if (p->ver[sc] && has[sc] < p->ver[sc]) {
                  size_t ln = strlen(lacks); int k;
                  if (cut) continue;
                  k = snprintf(lacks + ln, sizeof lacks - sizeof more + 1 - ln, "%s%s %u", n++ ? ", " : "", pm_scope_name(sc), p->ver[sc]);
                  if (k < 0 || ln + (size_t)k >= sizeof lacks - sizeof more + 1) { lacks[ln] = 0; cut = 1; }
              }
          if (cut) strcat(lacks, more); }
        if (n || twice) {
            struct pm_refused rf = { PM_REFUSED_SCOPES };
            pm_put_refused(&w, h.id, &rf); psend(p, &w, -1);
            if (twice) snprintf(p->why, sizeof p->why, "its client names a scope twice in its HELLO");
            else snprintf(p->why, sizeof p->why, "its client does not speak %s, which its manifest requires: it was built with an older SDK than its manifest says", lacks);
            say("%s: %s", id, p->why);
            p->want = 0; stop(p, p->why);
            pthread_mutex_unlock(&bl); return;
        }
        memcpy(ver, p->ver, sizeof ver);
        { unsigned gen = p->gen; int sent = 0;
        pthread_mutex_unlock(&bl);
#ifdef PLUGIN_TEST_HELLO
        PLUGIN_TEST_HELLO(id);              /* (tests/unit/plugin_harness.h: what another thread does while bl is let go) */
#endif
        { int was = core_begin(); uint16_t k = 0;
          clock_zone(zone, sizeof zone);
          for (int sc = 0; sc < PM_SCOPES; sc++) k += ver[sc] != 0;
          /* (what was granted is what is spoken: core and the scope of each capability, which ps_manifest_parse holds
           * to be the manifest's requires exactly; no set of bits that would bound how many there may be) */
          { struct pm_welcome wm = { (uint32_t)clock_synced(), pm_s(board.model), pm_s(core_node_name()), pm_s(core_name), pm_s(zone), { { 0 }, k } };
            pm_put_welcome(&w, h.id, &wm); }
          /* (what is spoken from here on: exactly its requires) */
          for (int sc = 0; sc < PM_SCOPES; sc++) if (ver[sc]) { struct pm_welcome_scopes x = { pm_s(pm_scope_name(sc)), ver[sc] }; pm_add_welcome_scopes(&w, &x); }
          core_end(was); }
        /* the same process, still waiting for it: while bl was let go it may have ended and the entry been started
         * again, a new process in P_HELLO whose own HELLO is still to come, and the old one's WELCOME (its id, its
         * versions) went to it (an observation of a review of 6631974) */
        pthread_mutex_lock(&bl);
        if (p->gen == gen && p->state == P_HELLO) { psend(p, &w, -1); p->state = P_CONFIG; p->since = now; sent = 1; }
        pthread_mutex_unlock(&bl);
        if (sent) send_config(id);
        return; } }
    case PM_READY:
        p->state = P_RUN; p->ping_at = btime + PING_MS; p->pings = 0;
        say("%s: running", id);
        tell_echo(p);                                       /* state, volume, its address: what it may know of the Echo */
        if (p->caps & NET_CAPS) net_looked = 0;      /* (looked at again now) */
        pthread_cond_broadcast(&bcv);
        if (p->have_words) p->words_dirty = 1;              /* listed while it started: up now (timers tells the core) */
        break;
    case PM_PONG: if (h.id == p->ping_id) p->pings = 0; break;
    /* the client handled the LOG sent last: the next one now (log_pump) */
    case PM_OK: if (h.id && h.id == p->log_wait) { p->log_wait = 0; p->log_next = 0; } break;
    case PM_ERR: break;
    case PM_FILE_OPEN: {
        struct pm_file_open m; char list[33], item[41], file[65]; const struct ps_plugin *sp; int fd = -1, staged, mine; unsigned gen = p->gen;
        pm_get_file_open(body, blen, &m);
        if (pm_cstr(list, sizeof list, m.list) || pm_cstr(item, sizeof item, m.item) || pm_cstr(file, sizeof file, m.file)) { send_err(p, h.id, PM_ERR_BAD); break; }
        staged = p->check.active && p->check.staged && !strcmp(p->check.list, list) && !strcmp(p->check.item, item);
        pthread_mutex_unlock(&bl);
        ps_lock(); if ((mine = store_mine(p, gen, id)) && (sp = ps_find(id))) fd = ps_item_open(sp, list, item, file, staged); ps_unlock();
        pthread_mutex_lock(&bl);
        if (p->gen != gen) ;                                    /* (another process of its since: not its answer) */
        else if (!running(p) || !mine) send_err(p, h.id, PM_ERR_FAILED);
        else if (fd < 0) send_err(p, h.id, PM_ERR_NOT_FOUND); else send_ok(p, h.id, fd);
        pthread_mutex_unlock(&bl);
        if (fd >= 0) close(fd);
        return; }
    case PM_FILE_OK: case PM_FILE_BAD:
        if (!p->check.active || p->check.done || h.id != p->check.id) { send_err(p, h.id, PM_ERR_BAD); break; }
        if (h.type == PM_FILE_OK) {
            struct pm_file_ok m; struct pm_file_ok_fields e;
            pm_get_file_ok(body, blen, &m);
            pm_cstr(p->check.title, sizeof p->check.title, m.title); p->check.nfields = 0; p->check.ok = 1;
            /* the title: ps_item_checked keeps its whole characters only; a field's value only as well-formed text
             * (the page shows it, the store refuses any other: item_field) */
            while (pm_next_file_ok_fields(&m.fields, &e) && p->check.nfields < PS_FIELDS)
                if (!pm_cstr(p->check.fields[p->check.nfields].name, sizeof p->check.fields[0].name, e.name) && !pm_cstr(p->check.fields[p->check.nfields].value, sizeof p->check.fields[0].value, e.value)
                    && ps_utf8_ok(p->check.fields[p->check.nfields].value, strlen(p->check.fields[p->check.nfields].value))) p->check.nfields++;
        } else {
            struct pm_file_bad m; pm_get_file_bad(body, blen, &m);
            pm_cstr(p->check.why, sizeof p->check.why, m.why); p->check.ok = 0;
            ps_utf8_mend(p->check.why);                     /* one line of text, whatever it holds: the page and the log show it */
            if (!p->check.why[0]) snprintf(p->check.why, sizeof p->check.why, "the plugin cannot use it (reason %u)", m.code);
        }
        p->check.done = 1;
        pthread_cond_broadcast(&bcv);
        break;
    case PM_WAKE_WORDS: {
        /* What the core does with a new list is work under its lock, a listing in the log, and Home Assistant sent
         * away to read it: so only for a list that is another one, 2 s apart at most (timers), and a plugin whose
         * list changes more often than an owner edits files is taken as broken */
        struct pm_wake_words m; struct pm_wake_words_words e; char list[33], ids[16][41]; int n = 0;
        pm_get_wake_words(body, blen, &m);
        memset(ids, 0, sizeof ids);
        if (pm_cstr(list, sizeof list, m.list)) { send_err(p, h.id, PM_ERR_BAD); break; }
        while (pm_next_wake_words_words(&m.words, &e) && n < 16) if (!pm_cstr(ids[n], sizeof ids[0], e.id) && ps_item_id_ok(ids[n])) n++;
        if (n < 16) memset(ids[n], 0, sizeof ids[0] * (size_t)(16 - n));
        if (p->have_words && n == p->nwords && !strcmp(list, p->words_list) && !memcmp(ids, p->words, sizeof ids)) break;
        memcpy(p->words, ids, sizeof ids); snprintf(p->words_list, sizeof p->words_list, "%s", list);
        if (now - p->words_since > CRASH_WINDOW) { p->words_since = now; p->words_changes = 0; }
        /* counted over its restarts (a process's first list is none of them: it has to say one).  Over the count it
         * is stopped and the count starts anew: what bounds it then is how seldom it is started again */
        if (p->have_words) p->words_changes++;
        p->nwords = n; p->have_words = 1; p->words_dirty = 1;
        if (p->words_changes > WORDS_CHANGES) {
            p->words_changes = 0; stop(p, "its list of wake words keeps changing: taken as broken"); say("%s: %s", id, p->why); }
        pthread_cond_broadcast(&bcv);
        break; }
    case PM_WAKE_READY: case PM_WAKE_FAILED: {
        struct pm_wake_failed m = { 0, 0 }; int first = 0;
        if (h.type == PM_WAKE_READY) { struct pm_wake_ready rm; pm_get_wake_ready(body, blen, &rm); m.slot = rm.slot; } else pm_get_wake_failed(body, blen, &m);
        if (m.slot >= WAKE_SLOTS || p->ws[m.slot].state != W_OPENING) { send_err(p, h.id, PM_ERR_BAD); break; }
        p->ws[m.slot].state = h.type == PM_WAKE_READY ? W_OPEN : W_FAILED;
        if (h.type == PM_WAKE_READY) { first = !p->wake_ready; p->wake_ready = 1; say("%s: \"%s\" listens%s", id, p->ws[m.slot].word, m.slot ? " (second wake word)" : ""); }
        else {
            static const char *const whys[] = { "?", "it has no such model", "the model cannot be read", "out of memory", "the model needs something this version lacks", "it failed" };
            char word[41]; snprintf(word, sizeof word, "%s", p->ws[m.slot].word);
            snprintf(p->why, sizeof p->why, "cannot listen for %s: %s", word, whys[m.why >= 1 && m.why <= 5 ? m.why : 0]);
            say("%s: %s", id, p->why);
        }
        pthread_cond_broadcast(&bcv);
        pthread_mutex_unlock(&bl);
        if (first) ready_note(id);
        core_begin();
        core_wake_plugin_changed(id);
        return; }
    case PM_WAKE_HEARD: {
        struct pm_wake_heard m; wake_cb cb = wake_user_cb; char word[41]; int n = 0;
        pm_get_wake_heard(body, blen, &m);
        if (!heard_ok(p, &m, now)) {                /* said in the log once in 5 s, however many: /data is flash */
            p->dropped++; p->unheard++;
            if (now - p->unheard_log >= 5000) {
                say("%s: a detection that cannot be (slot %u, %llu to %llu, the stream at %llu): dropped%s", id, m.slot, (unsigned long long)m.begin, (unsigned long long)m.end,
                    (unsigned long long)p->pos, p->unheard > 1 ? ", as others since the last" : "");
                p->unheard_log = now; p->unheard = 0;
            }
            break;
        }
        memmove(p->heard_at + 1, p->heard_at, sizeof p->heard_at[0] * (HEARD_BURST - 1)); p->heard_at[0] = now;
        if (p->nheard < HEARD_BURST) p->nheard++;
        for (int k = 0; k < p->nheard; k++) n += now - p->heard_at[k] < CRASH_WINDOW;
        if (n >= heard_burst) {         /* a broken model, or a plugin that starts runs as it likes: not listened to */
            p->nheard = 0;
            stop(p, "it reported twenty wake words in ten minutes: taken as broken");
            say("%s: %s", id, p->why);
            break;
        }
        snprintf(word, sizeof word, "%s", p->ws[m.slot].word);
        /* how old the word's end is when hassmic hears of it, on the stream's own clock (what was sent to the plugin
         * since): the plugin's inference and the way back, against heard_ok's 300 ms */
        { unsigned age = (unsigned)((p->pos - m.end) / 16), len = (unsigned)((m.end - m.begin) / 16);
          pthread_mutex_unlock(&bl);
          /* sealed: the word's name, its score and its times are the plugin's to choose */
          log_sealed("wake: %s (plugin %s, score %u/255%s; %u ms long, its end %u ms old)", word, id, m.score, m.slot ? ", second wake word" : "", len, age); }
        /* The detection is read, checked and counted: from here it is the core's wake path (the ring, the aligner,
         * arbitration, the run), which is not this plugin's doing.  Left marked, any fault in there named the wake
         * plugin, and the next start held back the one thing that hears the wake word */
        core_begin();
        if (cb) cb(m.slot, word, m.begin, m.end);
        return; }
    case PM_STORE_GET: case PM_STORE_PUT: case PM_STORE_DEL: store_message(p, id, &h, body, blen); return;      /* (bl let go there) */
    case PM_LOG_FOLLOW: {
        /* from a cursor it kept (a restart, the network back: what was written meanwhile), "start" or "" (now) */
        struct pm_log_follow m; char from[41]; struct logpos at; unsigned gen = p->gen;
        pm_get_log_follow(body, blen, &m);
        if (pm_cstr(from, sizeof from, m.from)) { send_err(p, h.id, PM_ERR_BAD); break; }
        pthread_mutex_unlock(&bl);
        if (!from[0]) logread_now(&at);                         /* (two stat()s and a read of 64 bytes: no lock) */
        else if (!strcmp(from, "start")) logread_start(&at);
        else if (logread_cursor_parse(from, &at)) { pthread_mutex_lock(&bl); if (p->gen == gen) send_err(p, h.id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
        pthread_mutex_lock(&bl);
        /* the process that asked, still (bl was let go: another start's would follow from this one's place) */
        if (p->gen == gen && (p->state == P_CONFIG || p->state == P_RUN)) {
            if (!p->log_on) say("%s: follows the log%s", id, from[0] ? (strcmp(from, "start") ? " from where it left off" : " from its oldest line") : "");
            /* the new place from now: a LOG of the old one still unanswered is none to wait for (the client drops it) */
            p->log_on = 1; p->log_at = at; p->log_next = 0; p->log_wait = 0;
            send_ok(p, h.id, -1);
        } else if (p->gen == gen) send_err(p, h.id, PM_ERR_FAILED);
        break; }
    case PM_CONNECT: connect_message(i, p, id, &h, body, blen); return;
    case PM_UDP_OPEN: case PM_UDP_PEER: case PM_SENDTO: case PM_BROADCAST: case PM_PROPOSE: udp_message(p, id, &h, body, blen); return;
    case PM_ENTITY_STATE: case PM_EVENT: entity_message(p, id, &h, body, blen); return;
    case PM_MUSIC_OPEN: music_open_message(i, p, &h, body, blen); return;
    case PM_CALL_INCOMING: case PM_CALL_OUT: case PM_CALL_RINGING_THERE: case PM_CALL_LIVE: case PM_CALL_END: call_message(i, p, &h, body, blen); return;
    case PM_MUSIC_STATE: {
        /* what its player does: the button, the stop word and Home Assistant's media player go by it.  A transition to
         * playing makes it the newest source again (one that ignored the pause and kept saying "playing" does not
         * take over by repeating it: yielded stays until it says something else first) */
        struct pm_music_state m; int was = p->mstate;
        pm_get_music_state(body, blen, &m);
        if (p->state != P_RUN || m.state < PM_MUSIC_PLAYING || m.state > PM_MUSIC_STOPPED) { if (h.id) send_err(p, h.id, PM_ERR_BAD); break; }
        if (!bucket_take(&p->r_mstate, MSTATE_PER_SEC, now)) { if (h.id) send_err(p, h.id, PM_ERR_BUSY); p->dropped++; break; }
        p->mstate = m.state;
        if (m.state == PM_MUSIC_PLAYING && was != PM_MUSIC_PLAYING) p->myield = 0;
        if (m.state != PM_MUSIC_PAUSED) p->mby = m.state == PM_MUSIC_PLAYING ? 0 : p->mby;
        pm_cstr(p->mtitle, sizeof p->mtitle, m.title); pm_cstr(p->martist, sizeof p->martist, m.artist);
        shown_text(p->mtitle); shown_text(p->martist);
        answer(p, h.id, 0);
        pthread_mutex_unlock(&bl);
        if (was != m.state) music_tell(i);
        return; }
    case PM_MUSIC_CLOSE:
        if (p->ms) { atomic_store(&p->ms->closing, 1); answer(p, h.id, 0); } else answer(p, h.id, PM_ERR_BAD);
        break;
    case PM_VOLUME_SET: {
        /* the Echo's volume, as the keys set it (core_set_volume: the ring shows it, Home Assistant hears of it, and
         * every music source's remote) */
        /* Five a second and twenty a minute, and the Echo's volume set from the latest at most every VOL_APPLY_MS: each
         * set is a line in the log, the ring's volume pattern and a write of the volume to the flash (main.c, once a
         * second at most), and five a second for ever were all of that for as long as it ran (M5 of a review of
         * efde36e).  A knob turned is a burst, and its last value is what counts */
        struct pm_volume_set m; pm_get_volume_set(body, blen, &m);
        if (p->state != P_RUN || m.percent > 100) { send_err(p, h.id, PM_ERR_BAD); break; }
        if (!bucket_take(&p->r_vol, VOL_PER_SEC, now) || !bucket_rate(&p->r_vol_min, VOL_PER_MIN / 60.0, VOL_PER_MIN, now)) { send_err(p, h.id, PM_ERR_BUSY); p->dropped++; break; }
        answer(p, h.id, 0);
        p->vol_want = m.percent + 1;                     /* (0: none waiting) */
        if (now - p->vol_applied < VOL_APPLY_MS) break;         /* (the broker's loop sets the latest: vol_flush) */
        pthread_mutex_unlock(&bl);
        vol_flush(i);
        return; }
    default: send_err(p, h.id, PM_ERR_UNKNOWN);
    }
    pthread_mutex_unlock(&bl);
}

/* ---------------------------------------------------------------- storage (4.4) */

/* bl held on entry, let go here.  The value to the flash and back without bl: the capture thread takes it */
static void store_message(struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen)
{
    static uint8_t out[PS_VALUE_BYTES + 128], value[PS_VALUE_BYTES];      /* (the broker's thread alone) */
    char key[PS_KEY_MAX + 1]; struct pm_str k, v = { "", 0 }; int r, token = 0; unsigned gen = p->gen; long long now = now_ms(); size_t n = 0;
    if (h->type == PM_STORE_GET) { struct pm_store_get m; pm_get_store_get(body, blen, &m); k = m.key; }
    else if (h->type == PM_STORE_DEL) { struct pm_store_del m; pm_get_store_del(body, blen, &m); k = m.key; }
    else { struct pm_store_put m; pm_get_store_put(body, blen, &m); k = m.key; v = m.value; }
    if (pm_cstr(key, sizeof key, k) || !ps_store_key_ok(key)) { send_err(p, h->id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
    if (h->type != PM_STORE_GET) {
        /* a write: one of the bucket's, which refills by one every store_refill ms */
        long long d = now - p->token_at;
        if (d >= store_refill) { int add = (int)(d / store_refill); p->tokens = p->tokens + add > store_bucket ? store_bucket : p->tokens + add; p->token_at += (long long)add * store_refill; }
        if (p->tokens <= 0) {
            send_err(p, h->id, PM_ERR_BUSY); p->dropped++;
            if (!p->store_said) { p->store_said = 1; say("%s: writes its data more often than once a minute: the writes over that are refused, and the plugin keeps the latest itself", id); }
            pthread_mutex_unlock(&bl); return;
        }
        if (p->tokens == store_bucket) p->token_at = now;          /* (a full bucket starts its minute now) */
        p->tokens--; token = 1;
        memcpy(value, v.s, v.n);
    }
    pthread_mutex_unlock(&bl);
    /* (the store's lock over the look and the write: an install renames under it) */
    ps_lock();
    if (!store_mine(p, gen, id)) r = -1;
    else if (h->type == PM_STORE_GET) r = ps_store_get(id, key, value, sizeof value, &n);
    else if (h->type == PM_STORE_DEL) r = ps_store_del(id, key);
    else r = ps_store_put(id, key, value, v.n);
    ps_unlock();
    pthread_mutex_lock(&bl);
    if (p->gen != gen) ;                                    /* (another process of its since: not its answer) */
    else if (r == PS_STORE_FULL || r < 0 || r == PS_STORE_NONE) {
        if (token) p->tokens++;                             /* nothing was written: the write is not spent */
        send_err(p, h->id, r == PS_STORE_FULL ? PM_ERR_LIMIT : r == PS_STORE_NONE ? PM_ERR_NOT_FOUND : PM_ERR_DENIED);
    } else if (h->type == PM_STORE_GET) {
        struct pm_w w; struct pm_store_value m = { pm_b(value, n > sizeof value ? sizeof value : n) };
        pm_winit(&w, out, sizeof out); pm_put_store_value(&w, h->id, &m); psend(p, &w, -1);
    } else send_ok(p, h->id, -1);
    pthread_mutex_unlock(&bl);
}

/* ---------------------------------------------------------------- the network (4.6) */

/* bl held on entry, let go here.  Checked against the manifest's entry and, for net.server, what the owner entered in
 * its rows; for net.lan the plugin names a private or link-local address (as an address: no lookup), for net.internet
 * any name or address.  A worker of plugin_net.c resolves, checks the address again and connects, and net_done hands
 * the socket over */
static int ip_text(const char *s, uint32_t *a);
static void connect_message(int i, struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen)
{
    struct pm_connect m; char host[254], want[PS_VALUE_MAX], port_v[PS_VALUE_MAX]; const struct ps_plugin *sp; int err = 0, udp = 0; long port = 0; long long now = now_ms();
    struct pnet_req rq; unsigned gen = p->gen; const char *what = NULL;
    pm_get_connect(body, blen, &m);
    /* (the capability as the kind says: CONNECT is the same message for the three, 3.3 "each accepted on its own line") */
    if ((m.kind != PM_CAP_NET_SERVER && m.kind != PM_CAP_NET_LAN && m.kind != PM_CAP_NET_INTERNET) || !(p->caps & PM_CAPBIT(m.kind))) { send_err(p, h->id, PM_ERR_DENIED); p->dropped++; pthread_mutex_unlock(&bl); return; }
    if (pm_cstr(host, sizeof host, m.host)) { send_err(p, h->id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
    if (p->net_pending >= NET_PENDING) { send_err(p, h->id, PM_ERR_BUSY); pthread_mutex_unlock(&bl); return; }
    /* the oldest of the last NET_MINUTE within a minute: that many in the minute before this one */
    if (p->net_asked[p->net_next] && now - p->net_asked[p->net_next] < 60000) { send_err(p, h->id, PM_ERR_BUSY); p->dropped++; pthread_mutex_unlock(&bl); return; }
    pthread_mutex_unlock(&bl);
    want[0] = port_v[0] = 0;
    ps_lock();
    if (!(sp = ps_find(id)) || !(sp->m.caps & PM_CAPBIT(m.kind))) err = PM_ERR_DENIED;
    else if (m.kind == PM_CAP_NET_SERVER) {
        const struct ps_net *e;
        if (m.entry >= sp->m.nnet) { err = PM_ERR_DENIED; what = "entry or port not its manifest's"; goto checked; }
        e = &sp->m.net[m.entry]; udp = e->udp;
        /* only what the owner stored: never a row's default (a manifest from before net_parse refused one) */
        ps_config_owner(sp, e->setting, want, sizeof want);
        if (e->port_row[0]) {
            ps_config_owner(sp, e->port_row, port_v, sizeof port_v);
            port = atol(port_v);
            if (!port_v[0] || port < 1 || port > 65535) err = PM_ERR_UNSET;
            else if (m.port && m.port != port) err = PM_ERR_DENIED;
        } else {
            int ok = 0;
            for (int k = 0; k < e->nports; k++) ok |= e->ports[k] == m.port;
            if (!m.port && e->nports == 1) { port = e->ports[0]; ok = 1; }
            else port = m.port;
            if (!ok) err = m.port ? PM_ERR_DENIED : PM_ERR_BAD;
        }
        /* the host is the owner's, and nothing else: as written, or left out (no lookup of a plugin's own) */
        if (!err && !want[0]) err = PM_ERR_UNSET;
        if (!err && host[0] && strcmp(host, want)) { err = PM_ERR_DENIED; what = "another host than the one entered"; }
        if (err == PM_ERR_DENIED && !what) what = "entry or port not its manifest's";
    } else {
        /* net.lan, net.internet: the plugin's own address or name, at one of the entry's ports */
        const struct ps_reach *e = m.kind == PM_CAP_NET_LAN ? sp->m.lan : sp->m.inet; int ne = m.kind == PM_CAP_NET_LAN ? sp->m.nlan : sp->m.ninet, ok = 0; uint32_t a;
        if (m.entry >= ne) { err = PM_ERR_DENIED; what = "entry not its manifest's"; goto checked; }
        udp = e[m.entry].udp;
        if (!m.port) { if (!e[m.entry].any && e[m.entry].nports == 1) { port = e[m.entry].ports[0]; ok = 1; } }
        else { port = m.port; ok = e[m.entry].any && m.port != 5555; for (int k = 0; k < e[m.entry].nports; k++) ok |= e[m.entry].ports[k] == m.port; }
        /* net.lan never at DNS's ports (PS_PORT_RELAY: the router's forwarder asks the internet for it), whatever
         * the entry: "any" is any port but those, and a listed one is a manifest from before the rule */
        if (ok && m.kind == PM_CAP_NET_LAN && PS_PORT_RELAY(port)) { err = PM_ERR_DENIED; what = "DNS's port: a device on the network asks the internet there"; }
        else if (!ok) { err = m.port ? PM_ERR_DENIED : PM_ERR_BAD; what = "a port its entry does not list"; }
        /* net.lan: an address, a.b.c.d as written, private or link-local (lockdown.sh's LOCAL4 without multicast and
         * broadcast); never a name, so nothing a plugin chooses goes into a DNS query.  net.internet: a name or an
         * address as a host row takes one.  Loopback, the Echo's own, multicast, broadcast, 0/8: the worker
         * (pnet_address_ok), whatever the kind */
        else if (!host[0]) err = PM_ERR_BAD;
        else if (m.kind == PM_CAP_NET_LAN && (ip_text(host, &a) || !pnet_local(htonl(a)))) { err = PM_ERR_DENIED; what = "not an address on the local network (an address, not a name)"; }
        else if (m.kind == PM_CAP_NET_INTERNET && !ps_host_name_ok(host)) err = PM_ERR_BAD;
        /* net.internet: the internet, not the network (that is net.lan's, asked and shown on a line of its own; a
         * plugin that wants both asks for both).  Here for an address as written; a name the worker resolves is held
         * to it there, so that a name its author points at a device of the home is refused too */
        else if (m.kind == PM_CAP_NET_INTERNET && !ip_text(host, &a) && pnet_local(htonl(a))) { err = PM_ERR_DENIED; what = "an address on the local network: net.internet is the internet's"; }
        snprintf(want, sizeof want, "%s", host);
    }
checked:
    ps_unlock();
    pthread_mutex_lock(&bl);
    if (err || p->state != P_RUN || p->gen != gen) {
        if (p->gen != gen) ;
        else if (err) { send_err(p, h->id, err); if (err == PM_ERR_DENIED) refusal(p, "asked to connect where it may not (%s%s%s, port %u): refused", m.kind == PM_CAP_NET_SERVER ? "" : ps_cap_name(m.kind),
                                                         m.kind == PM_CAP_NET_SERVER ? "" : ": ", what ? what : "not its capability", m.port); }
        else send_err(p, h->id, PM_ERR_FAILED);             /* (stopped meanwhile) */
        pthread_mutex_unlock(&bl); return;
    }
    memset(&rq, 0, sizeof rq);
    rq.who = i; rq.gen = gen; rq.id = h->id; rq.port = (uint16_t)port; rq.udp = udp; rq.kind = m.kind;
    /* (a plugin that hears requests or calls: the worker's lines private, as the broker's own lines about it are) */
    rq.mark = p->caps & PRIVATE_CAPS ? LOG_PRIVATE_MARK : NULL;
    snprintf(rq.plugin, sizeof rq.plugin, "%s", id); snprintf(rq.host, sizeof rq.host, "%s", want);
    if (pnet_ask(&rq, net_done, net_live)) send_err(p, h->id, PM_ERR_BUSY);
    else { p->net_pending++; p->net_asked[p->net_next] = now; p->net_next = (p->net_next + 1) % NET_MINUTE; }
    pthread_mutex_unlock(&bl);
}

/* a worker's thread, no lock held: whether the process that asked still runs.  A request of one that ended is not
 * worked on (its queue place went to the next start's, which waited behind it) */
static int net_live(const struct pnet_req *r)
{
    struct plug *p; int live;
    pthread_mutex_lock(&bl);
    p = &plugs[r->who];
    live = p->gen == r->gen && !strcmp(p->id, r->plugin) && p->state == P_RUN;
    pthread_mutex_unlock(&bl);
    return live;
}

/* a worker's thread, no lock held: the socket to the plugin that asked, if it is still the process that asked */
static void net_done(const struct pnet_req *r, int fd, int err)
{
    struct plug *p;
    pthread_mutex_lock(&bl);
    p = &plugs[r->who];
    if (p->gen == r->gen && !strcmp(p->id, r->plugin)) {
        if (p->net_pending) p->net_pending--;
        if (p->state == P_RUN) {
            if (fd >= 0) send_ok(p, r->id, fd); else send_err(p, r->id, err);
            if (fd >= 0) p->net_ok = now_ms(); else p->net_fail = now_ms();
        } else send_err(p, r->id, PM_ERR_FAILED);           /* stopping: no socket, but an answer (R2) */
    }
    pthread_mutex_unlock(&bl);
    if (fd >= 0) close(fd);                                 /* sent or not: hassmic keeps no copy */
}

/* ---------------------------------------------------------------- phase 4: the helpers */

/* a bucket of PER_SEC a second, holding up to two seconds' worth (bl held): 1 when one more may go now */
static int bucket_take(struct bucket *b, int per_sec, long long now)
{
    if (!b->at) { b->at = now; b->tokens = 2.0 * per_sec; }
    b->tokens += (double)(now - b->at) * per_sec / 1000.0; b->at = now;
    if (b->tokens > 2.0 * per_sec) b->tokens = 2.0 * per_sec;
    if (b->tokens < 1) return 0;
    b->tokens -= 1;
    return 1;
}

/* a bucket of PER_SEC a second (a fraction: a minute's rate) holding CAP (bl held): 1 when one more may go now */
static int bucket_rate(struct bucket *b, double per_sec, double cap, long long now)
{
    if (!b->at) { b->at = now; b->tokens = cap; }
    b->tokens += (double)(now - b->at) * per_sec / 1000.0; b->at = now;
    if (b->tokens > cap) b->tokens = cap;
    if (b->tokens < 1) return 0;
    b->tokens -= 1;
    return 1;
}

/* No lock held, the broker's thread: entry I's latest VOLUME_SET is the Echo's volume now, if VOL_APPLY_MS passed since
 * the one before.  The log says so once in VOL_SAY_MS, the rest counted into the next line */
static void vol_flush(int i)
{
    struct plug *p = &plugs[i]; int v = -1, said = 0, untold = 0; char id[PS_ID_MAX]; long long now = now_ms();
    pthread_mutex_lock(&bl);
    if (p->vol_want && p->state == P_RUN && now - p->vol_applied >= VOL_APPLY_MS) {
        v = p->vol_want - 1; p->vol_want = 0; p->vol_applied = now; p->vol_own = v + 1;
        if (now - p->vol_said_at >= VOL_SAY_MS) { said = 1; untold = p->vol_untold; p->vol_untold = 0; p->vol_said_at = now; } else p->vol_untold++;
    }
    snprintf(id, sizeof id, "%s", p->id);
    pthread_mutex_unlock(&bl);
    if (v < 0) return;
    if (said && untold) say("%s: sets the volume to %d (and %d times more since the last such line)", id, v, untold);
    else if (said) say("%s: sets the volume to %d", id, v);
    { int was = core_begin(); pthread_mutex_lock(&core_lock); core_set_volume(v); pthread_mutex_unlock(&core_lock); core_end(was); }
    pthread_mutex_lock(&bl); if (p->vol_own == v + 1) p->vol_own = 0; pthread_mutex_unlock(&bl);
}

/* No lock held, the broker's thread: what the lanes' functions changed (a pause, a resume, a source heard again), told
 * to the core's table and to Home Assistant's media player: they run inside the core's core_music or under its lock,
 * where neither may be called (M6 of a review of efde36e) */
static void music_due(void)
{
    for (int i = 0; i < NPLUG; i++) {
        int due;
        pthread_mutex_lock(&bl); due = plugs[i].mdue; plugs[i].mdue = 0; pthread_mutex_unlock(&bl);
        if (due) music_tell(i);
    }
}

/* an id-0 note refused: said in the log as any refusal; a request (an id) is answered */
static void answer(struct plug *p, uint32_t id, int err)
{
    if (err && id) send_err(p, id, err);
    else if (!err && id) send_ok(p, id, -1);
}

/* An address a plugin's UDP may send to (net.udp: UDP_PEER, SENDTO): a private or link-local unicast one (the
 * firewall holds a plain socket to local addresses in any case; the broker refuses first, which is also all there is
 * on a PC).  Never 0/8, loopback, multicast, a network's broadcast or the Echo's own (as plugin_net.c refuses them for
 * CONNECT: behind the Echo's own address and loopback are hassmic's ports and Amazon's daemons), never port 5555,
 * never DNS's 53 or 853 */
static int private4(uint32_t a)
{
    return (a & 0xff000000u) == 0x0a000000u || (a & 0xfff00000u) == 0xac100000u || (a & 0xffff0000u) == 0xc0a80000u || (a & 0xffff0000u) == 0xa9fe0000u;
}
static int udp_dest_ok(uint32_t a, unsigned port)
{
    /* (nor DNS's ports, PS_PORT_RELAY: the router's forwarder asks the internet for what a datagram there names) */
    struct ifaddrs *ifa, *i; int ok = private4(a) && port && port != 5555 && !PS_PORT_RELAY(port);
    if (!ok || getifaddrs(&ifa)) return 0;
    for (i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        uint32_t own = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr), mask = i->ifa_netmask ? ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0;
        if (a == own || (mask && mask != 0xffffffffu && (a | mask) == 0xffffffffu)) ok = 0;    /* its own, a network's broadcast */
    }
    freeifaddrs(ifa);
    /* and only where the kernel sends it out of the Echo (plugin_net.c's rule for CONNECT, route_out): an address no
     * interface has can still be the Echo's own by a "local" route, and then a datagram went over lo to every
     * listener of the Echo's own on that port, past the firewall's `-o lo` (A3 of a review of efde36e) */
    return ok && pnet_address_ok(htonl(a), (uint16_t)port);
}

/* The second wall of net.udp's "reachable from your network" (the firewall's rule in lockdown.sh is the first, and
 * the only one that holds before the packet reaches the Echo's stack): a socket filter that drops every datagram
 * whose source is not a private or link-local address, locked (SO_LOCK_FILTER) so that nobody takes it off; the
 * plugin could not anyway (its setsockopt is held to three options, sdk/filter.def).  On a PC, where the tests run
 * without the firewall, it is all there is */
static int private_only(int fd)
{
    static struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)SKF_NET_OFF + 12),         /* the IPv4 header's source */
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xff000000u), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0a000000u, 7, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)SKF_NET_OFF + 12),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xfff00000u), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xac100000u, 4, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)SKF_NET_OFF + 12),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xffff0000u), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xc0a80000u, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xa9fe0000u, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0xffffffffu),
        BPF_STMT(BPF_RET | BPF_K, 0) };
    struct sock_fprog prog = { sizeof f / sizeof f[0], f }; int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog)) return -1;
#ifdef SO_LOCK_FILTER
    setsockopt(fd, SOL_SOCKET, SO_LOCK_FILTER, &one, sizeof one);           /* Linux 3.9: every Echo's kernel has it */
#endif
    (void)one;
    return 0;
}

/* the Echo's address on its network (the first interface that is up, not loopback, with IPv4) */
static void net_look(uint32_t *addr, uint32_t *mask)
{
    struct ifaddrs *ifa, *i;
    *addr = *mask = 0;
    if (getifaddrs(&ifa)) return;
    for (i = ifa; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && !(i->ifa_flags & IFF_LOOPBACK)) {
            *addr = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
            *mask = i->ifa_netmask ? ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0;
            break;
        }
    freeifaddrs(ifa);
}

/* bl held: what P is told of the Echo, where it changed: STATE (state), VOLUME (audio.volume), NET (a net capability) */
static void tell_echo(struct plug *p)
{
    uint8_t b[64]; struct pm_w w;
    if (p->state != P_RUN) return;
    if ((p->caps & PM_CAPBIT(PM_CAP_STATE)) && cs_vol >= 0 && (p->told_state != cs_state || p->told_muted != cs_muted || p->told_vol != cs_vol)) {
        struct pm_state m = { (uint8_t)cs_state, (uint8_t)cs_muted, (uint8_t)cs_vol };
        pm_winit(&w, b, sizeof b); pm_put_state(&w, 0, &m);
        if (!psend(p, &w, -1)) { p->told_state = cs_state; p->told_muted = cs_muted; p->told_vol = cs_vol; }
    }
    if ((p->caps & PM_CAPBIT(PM_CAP_AUDIO_VOLUME)) && cs_vol >= 0 && p->told_volume != cs_vol) {
        struct pm_volume m = { (uint8_t)cs_vol };
        pm_winit(&w, b, sizeof b); pm_put_volume(&w, 0, &m);
        if (!psend(p, &w, -1)) p->told_volume = cs_vol;
    }
    if ((p->caps & NET_CAPS) && net_looked && (!p->told_net || p->told_addr != net_addr || p->told_mask != net_mask)) {
        struct pm_net m = { net_addr, net_mask, (uint8_t)(net_addr != 0) };
        pm_winit(&w, b, sizeof b); pm_put_net(&w, 0, &m);
        if (!psend(p, &w, -1)) { p->told_net = 1; p->told_addr = net_addr; p->told_mask = net_mask; }
    }
}

/* The volume set from elsewhere (Home Assistant, the keys, another plugin) while a plugin's VOLUME_SET waits for
 * vol_flush: the newer word stands, and the one that waited is dropped.  It was applied after it, up to VOL_APPLY_MS
 * later, and undid what the owner had just set (L4 of a review of 92ac2e1).  A plugin's own value coming back from
 * core_set_volume (vol_own) is no word from elsewhere.  As built that never happens (vol_flush clears vol_want before
 * it calls core_set_volume, and only the broker's thread runs vol_flush): vol_own is a guard for a later caller, not
 * a path of today's (a review of c06980e) */
void plugin_core_state(int st, int muted, int volume)
{
    pthread_mutex_lock(&bl);
    if (volume != cs_vol && cs_vol >= 0)
        for (int i = 0; i < NPLUG; i++) if (plugs[i].vol_want && plugs[i].vol_own != volume + 1) { say("%s: its volume of %d not set: set to %d elsewhere meanwhile", plugs[i].id, plugs[i].vol_want - 1, volume); plugs[i].vol_want = 0; }
    if (st != cs_state || muted != cs_muted || volume != cs_vol) {
        cs_state = st; cs_muted = muted; cs_vol = volume;
        for (int i = 0; i < NPLUG; i++) tell_echo(&plugs[i]);
    }
    pthread_mutex_unlock(&bl);
}

/* ---------------------------------------------------------------- the buttons (button; docs/plugins.md 3.3)
 *
 * What Home Assistant's "Button" event entity hears, as main.c button_event tells it: the press that answered a login
 * or a phone's setup, and the hold that dismissed a note, never got there and never get here (the one place both are
 * told).  The plugin hears and nothing more: no answer, and what the button does on the Echo is done all the same */
void plugin_button(int key, int hold)
{
    uint8_t b[32]; struct pm_w w; struct pm_button m = { (uint8_t)(key + PM_BUTTON_KEY_ACTION), (uint8_t)(hold ? PM_BUTTON_HOW_HOLD : PM_BUTTON_HOW_PRESS) };
    long long now = now_ms();
    if (key < 0 || !pm_button_key_ok((uint64_t)key + PM_BUTTON_KEY_ACTION, NULL)) return;   /* (a key this build has) */
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i];
        if (p->state != P_RUN || !(p->caps & PM_CAPBIT(PM_CAP_BUTTON))) continue;
        pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m);
        /* A press its version has no word for (a key of a later button version whose fallback is none, `>-`) is not
         * the plugin's at all: left out before its budget is asked, so a key it never hears spends none of the five a
         * second the keys it does hear have (finding 1 of a review of 61e3d8e).  Asked of a copy: W goes to psend as
         * encoded, which fits it and counts what it leaves unsent */
        if (pnot_for(p, &w)) { psend(p, &w, -1); continue; }
        /* Past BUTTON_PER_SEC: not passed on, said once in REFUSAL_MS and the rest counted into the next line or
         * timers()'s (a key gone wrong writes no log full).  Its own count, not refusal(): the pace is a person's (or a
         * bouncing key's), not the plugin's doing, so it is not in the plugin's `dropped`, and it shares no window with
         * the plugin's refusals, whose lines it would fold into "and N more" for 10 s (a review of 541f0a8) */
        if (!bucket_take(&p->r_button, BUTTON_PER_SEC, now)) {
            p->button_n++;
            if (!p->button_at || now - p->button_at >= REFUSAL_MS) {
                say("%s: %d %s of the buttons not passed on (%d a second at most; said once in %d s)", p->id, p->button_n, p->button_n == 1 ? "press" : "presses", BUTTON_PER_SEC, REFUSAL_MS / 1000);
                p->button_at = now; p->button_n = 0;
            }
            continue;
        }
        psend(p, &w, -1);
    }
    pthread_mutex_unlock(&bl);
}

/* ---------------------------------------------------------------- music (audio.music, audio.volume; 4.7, 3.5) */

/* A plugin's stream: a SOCK_STREAM pair, the plugin's end handed over with MUSIC_OPEN's OK, this end read by a thread
 * of its own (one at a time: plug_mu, the backend's plugin stream) that writes it at playback speed.  The thread owns
 * the descriptor: the broker ends it by `end` and a shutdown() (never a close: the thread may be in its poll), the
 * thread takes itself out of the entry under bl, closes, and tells the core */
static pthread_mutex_t plug_mu = PTHREAD_MUTEX_INITIALIZER;     /* the backend's one plugin stream (audio.h plug_*) */
static atomic_int mstreams_live;                                /* stream threads that have not ended (the tests wait for 0) */
static pthread_mutex_t mtell = PTHREAD_MUTEX_INITIALIZER;       /* music_tell's, before bl; never under it */

static int lane_mine(const struct plug *p) { return p->lane >= 0 && p->lane < LANES && atomic_load(&lane_of[p->lane]) == (int)(p - plugs); }

/* bl held: the music command to P, said in the log once (the plugin's to do) */
static void mcmd(struct plug *p, int cmd, int why)
{
    uint8_t b[32]; struct pm_w w; struct pm_music_cmd m = { (uint8_t)cmd, (uint8_t)why };
    static const char *const cmds[] = { "?", "pause", "resume", "stop", "next", "previous" }, *const whys[] = { "?", "the button", "the stop word", "another source", "Home Assistant" };
    pm_winit(&w, b, sizeof b); pm_put_music_cmd(&w, 0, &m); psend(p, &w, -1);
    say("%s: music: %s (%s)", p->id, cmds[cmd < 6 ? cmd : 0], whys[why < 5 ? why : 0]);
}

/* No lock held: entry I's place in "the newest source wins" as it stands now: on while its stream is open, its player
 * says it plays and no other source took over since.  Told again until it holds: a lane's pause (another source
 * started, on any thread) may come between the look and the core's word, and the look after it sees that */
static void music_tell(int i)
{
    pthread_mutex_lock(&mtell);
    for (int k = 0; k < 4; k++) {
        struct plug *p; int on, src;
        pthread_mutex_lock(&bl);
        p = &plugs[i];
        src = lane_mine(p) ? lane_src[p->lane] : -1;
        on = src >= 0 && p->ms && !atomic_load(&p->ms->end) && p->mstate == PM_MUSIC_PLAYING && !p->myield && (running(p) || p->state == P_STOPPING);
        if (on) music_newest = i;
        pthread_mutex_unlock(&bl);
        if (src < 0) break;
        core_music(src, on);
        pthread_mutex_lock(&bl);
        int again = lane_mine(p) && (p->ms && !atomic_load(&p->ms->end) && p->mstate == PM_MUSIC_PLAYING && !p->myield && (running(p) || p->state == P_STOPPING)) != on;
        pthread_mutex_unlock(&bl);
        if (!again) break;
    }
    pthread_mutex_unlock(&mtell);
    core_plugin_music_changed();
}

/* The lanes' functions (core.h struct music_source), any thread, no bl held by the caller (core_lock may be: stop) */
static struct plug *lane_plug(int l)    /* bl held */
{
    int e = atomic_load(&lane_of[l]);
    return e >= 0 && e < NPLUG && plugs[e].lane == l && (running(&plugs[e]) || plugs[e].state == P_STOPPING) ? &plugs[e] : NULL;
}
/* Each says whether it changed what plays (the core's table and Home Assistant's media player are told by the broker:
 * music_due), and none waits for the plugin: what it writes is dropped by the stream's thread from here on, whether
 * it obeys or not (M6 of a review of efde36e) */
static void lane_pause(int l)
{
    struct plug *p; int src = lane_src[l], off = 0;
    pthread_mutex_lock(&bl);
    if ((p = lane_plug(l)) && p->ms && p->mstate == PM_MUSIC_PLAYING && !p->myield) { mcmd(p, PM_MCMD_PAUSE, PM_MWHY_OTHER_SOURCE); p->mby = 0; p->myield = MY_OTHER; p->mdue = 1; off = 1; }
    pthread_mutex_unlock(&bl);
    if (off && src >= 0) core_music(src, 0);       /* (not music_tell: this may run inside its core_music) */
}
static int lane_stop(int l)
{
    struct plug *p; int r = 0;
    pthread_mutex_lock(&bl);
    if ((p = lane_plug(l)) && p->ms && p->mstate == PM_MUSIC_PLAYING && !p->myield) { mcmd(p, PM_MCMD_PAUSE, PM_MWHY_STOP_WORD); p->mby = now_ms(); p->myield = MY_HELD; p->mdue = 1; r = 1; }
    pthread_mutex_unlock(&bl);
    return r;
}
static int lane_button(int l, int resume)
{
    struct plug *p; int r = 0;
    pthread_mutex_lock(&bl);
    if ((p = lane_plug(l))) {
        if (!resume && p->ms && p->mstate == PM_MUSIC_PLAYING && !p->myield) { mcmd(p, PM_MCMD_PAUSE, PM_MWHY_BUTTON); p->mby = now_ms(); p->myield = MY_HELD; p->mdue = 1; r = 1; }
        /* what the button or a stop paused, whether the plugin said "paused" or went on saying "playing" */
        else if (resume && (p->mstate == PM_MUSIC_PAUSED || p->myield == MY_HELD) && p->mby && now_ms() - p->mby < BUTTON_RESUME_MS) {
            mcmd(p, PM_MCMD_RESUME, PM_MWHY_BUTTON); p->mby = 0; if (p->myield == MY_HELD) p->myield = MY_NONE; p->mdue = 1; r = 1;
        }
    }
    pthread_mutex_unlock(&bl);
    return r;
}
static int lane_skip(int l, int dir)
{
    struct plug *p; int r = 0;
    pthread_mutex_lock(&bl);
    if ((p = lane_plug(l)) && p->ms && p->mstate == PM_MUSIC_PLAYING && !p->myield) { mcmd(p, dir > 0 ? PM_MCMD_NEXT : PM_MCMD_PREVIOUS, PM_MWHY_BUTTON); r = 1; }
    pthread_mutex_unlock(&bl);
    return r;
}
/* every other source is off: one made silent for another is heard again (one hassmic paused stays paused).  Inside the
 * core's core_music: the core is told by the broker (music_due), never from here (music_tell's lock is held there) */
static void lane_unyield(int l)
{
    struct plug *p;
    pthread_mutex_lock(&bl);
    if ((p = lane_plug(l)) && p->myield == MY_OTHER) { p->myield = MY_NONE; p->mdue = 1; say("%s: music: heard again (no other source plays)", p->id); }
    pthread_mutex_unlock(&bl);
}
#define LANE(L) static void lp##L(void) { lane_pause(L); } static int ls##L(void) { return lane_stop(L); } \
    static int lb##L(int r) { return lane_button(L, r); } static int lk##L(int d) { return lane_skip(L, d); } static void lu##L(void) { lane_unyield(L); }
LANE(0) LANE(1) LANE(2) LANE(3) LANE(4) LANE(5) LANE(6) LANE(7) LANE(8) LANE(9)
static const struct music_source lane_srcs[LANES] = { { "plugin", lp0, ls0, lb0, lk0, NULL, lu0 }, { "plugin", lp1, ls1, lb1, lk1, NULL, lu1 },
                                                      { "plugin", lp2, ls2, lb2, lk2, NULL, lu2 }, { "plugin", lp3, ls3, lb3, lk3, NULL, lu3 },
                                                      { "plugin", lp4, ls4, lb4, lk4, NULL, lu4 }, { "plugin", lp5, ls5, lb5, lk5, NULL, lu5 },
                                                      { "plugin", lp6, ls6, lb6, lk6, NULL, lu6 }, { "plugin", lp7, ls7, lb7, lk7, NULL, lu7 },
                                                      { "plugin", lp8, ls8, lb8, lk8, NULL, lu8 }, { "plugin", lp9, ls9, lb9, lk9, NULL, lu9 } };
_Static_assert(LANES == 10 && LANES == RUN_MAX + 2, "a LANE() each, two more than the places");
_Static_assert(MUSIC_SOURCES >= LANES + 3, "the core's table holds every lane, Sendspin, Bluetooth and the PC's stand-in");

/* The stream's thread: what the plugin writes, as stereo s16 at its rate, ducked under the voice assistant as
 * Sendspin's is (duck.c, the setting music_duck), into the ring's music effects (musiclight.c) and the backend's
 * plugin stream, which blocks at playback speed: the socket's buffer is the plugin's back-pressure.  MUSIC_POS five
 * times a second.  Nothing of the plugin's is waited for: a poll of 100 ms, and the end when the broker says so.
 * While it is not heard (it says it paused, another source took over, hassmic paused it) what it writes is read and
 * dropped at the stream's own rate, no faster: read as fast as it came, a plugin that said "paused" and wrote in a loop
 * had this thread take two thirds of a core and bl some 340 000 times a second, bl the capture thread's own (M1 of a
 * review of efde36e).  Paced, its socket fills and its writes come back EAGAIN, as while it plays.  The pace is a
 * running total of the frames dropped since it began (drop_base, drop_frames), each read's frames added exactly: a
 * read's own share rounded to whole milliseconds was 0 for one under a millisecond, and a plugin that wrote 8 bytes
 * at a time into a socket made as small as it goes was read unpaced again, 57 % of a core (L1 of a review of 92ac2e1).
 * And whether heard or not, a read of less than READ_MIN_MS of audio has the next wait that long: the socket holds
 * 170 ms for writes of 5 ms or more (PROTOCOL.md asks for that), so nothing is lost; a socket buffer is charged
 * ~768 bytes a write, so a plugin writing under ~3 frames at a time at 48 kHz stereo starves its own stream, and only
 * its own (a review of c06980e), and a plugin's writes of a few bytes cost hassmic READ_MIN_MS a wakeup at most, not one
 * each (while it played, 8 bytes at a time were 24 000 rounds of this loop a second, bl twice in each) */
static void *mstream_thread(void *arg)
{
    struct mstream *m = arg; int opened = 0, closing_seen = 0; unsigned frame = 2 * m->ch; float duck_g = 1;
    /* what is read and written at a time: 40 ms of the plugin's audio.  A pause, a duck or another source's start
     * takes effect from the next one: what is under way plays (the backend's own buffer, 200 ms on the PC, comes on top) */
    size_t chunk = (size_t)m->rate / 25 * frame; uint8_t *in = malloc(chunk + 8); int16_t *st = malloc(sizeof(int16_t) * 2 * (m->rate / 25 + 1)); size_t have = 0;
    unsigned long long written = 0, drop_frames = 0; long long pos_at = 0, lat = 70000, drop_at = 0, drop_base = 0, next_read = 0;
    const size_t small = (size_t)m->rate / 200 * frame;     /* a read of less than READ_MIN_MS (5 ms) of it */
    const char *e = getenv("HASSMIC_OUTPUT_LATENCY_MS"), *gap = getenv("HASSMIC_PLUGIN_MSTREAM_GAP_MS");
    if (e) lat = atoll(e) * 1000;
    pthread_mutex_lock(&plug_mu);           /* the stream of the plugin before (newest wins) ends first */
    if (in && st && !atomic_load(&m->end)) opened = !plug_open(m->rate, 2);
    while (opened && !atomic_load(&m->end)) {
        struct pollfd pf = { m->fd, POLLIN, 0 }; ssize_t r; int yield, wait = 100;
        long long now = now_ms();
        pthread_mutex_lock(&bl);
        { struct plug *p = &plugs[m->entry]; yield = p->ms != m || p->myield || p->mstate != PM_MUSIC_PLAYING; }
        pthread_mutex_unlock(&bl);
        /* not heard: the next 40 ms of it are read when they are due on the stream's clock, not before */
        if (!yield) drop_at = 0;
        else if (!drop_at || now - drop_at > 200) { drop_at = drop_base = now; drop_frames = 0; }       /* (behind by more: from now) */
        { long long due = yield && drop_at > now ? drop_at : 0;
          if (next_read > now && next_read > due) due = next_read;
          if (due > now) {
              wait = (int)(due - now < 100 ? due - now : 100);
              /* heard: what it wrote is all played, so its end closed is read after the wait, never here */
              if (!yield) { poll(NULL, 0, wait); continue; }
              pf.events = 0;
          } }
        if (poll(&pf, 1, wait) < 0 && errno != EINTR) break;
        if (atomic_load(&m->closing) && !closing_seen) closing_seen = 1;
        if (pf.revents & (POLLHUP | POLLERR) && !(pf.events & POLLIN)) break;                    /* its end closed, and nothing of it is to be heard */
        if (pf.revents & (POLLIN | POLLHUP | POLLERR)) {
            r = recv(m->fd, in + have, chunk - have, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;      /* its end closed */
            if (r > 0 && (size_t)r < small) next_read = now_ms() + read_min_ms;
        } else r = -1;                                          /* (nothing came: the poll's 100 ms) */
        if (r < 0 && closing_seen) break;                       /* closed, and nothing more waiting: what it wrote has played */
        if (r > 0) {
            size_t n, frames;
            have += (size_t)r; frames = have / frame; n = frames * frame;
            pthread_mutex_lock(&bl);
            { struct plug *p = &plugs[m->entry]; yield = p->ms != m || p->myield || p->mstate != PM_MUSIC_PLAYING; }
            pthread_mutex_unlock(&bl);
            /* another source took over, or hassmic paused it, and the plugin went on writing: not heard until it plays
             * again (as a phone without remote control: a2dp.c yielded); dropped at the stream's rate */
            if (!yield && frames) {
                const int16_t *s = (const int16_t *)in;
                for (size_t f = 0; f < frames; f++) { st[2 * f] = s[f * m->ch]; st[2 * f + 1] = s[f * m->ch + (m->ch > 1)]; }
                music_duck(st, frames, m->rate, &duck_g, duck_active() ? settings_duck_gain() : 1);     /* under the voice assistant, ours or another Echo's */
                musiclight_feed(st, frames, 2, m->rate, plug_queued_us());
                plug_write(st, frames * 4);
                written += frames;
            } else if (yield && frames) {
                if (!drop_at) { drop_base = now_ms(); drop_frames = 0; }
                drop_frames += frames;
                drop_at = drop_base + (long long)(drop_frames * 1000 / m->rate);       /* (exact: no read's share lost to rounding) */
            }
            memmove(in, in + n, have - n); have -= n;
        }
        now = now_ms();
        if (now - pos_at >= POS_MS) {
            /* what reached the speaker: written, less what is queued and the output's own latency after the write (as
             * Sendspin reckons it: 70 ms on the Echos, a Bluetooth speaker's on top) */
            long long q = plug_queued_us(), out = q + lat + btout_latency_us(), played = (long long)written - out * (long long)m->rate / 1000000;
            uint8_t b[48]; struct pm_w w; struct timespec t; struct pm_music_pos mp;
            clock_gettime(CLOCK_MONOTONIC, &t);
            mp.frames = played > 0 ? (uint64_t)played : 0; mp.at = (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; mp.queued = (uint32_t)(q > 0 ? q : 0);
            pm_winit(&w, b, sizeof b); pm_put_music_pos(&w, 0, &mp);
            pthread_mutex_lock(&bl);
            if (plugs[m->entry].ms == m && plugs[m->entry].gen == m->gen && plugs[m->entry].state == P_RUN) psend(&plugs[m->entry], &w, -1);
            pthread_mutex_unlock(&bl);
            pos_at = now;
        }
    }
    if (opened) plug_close();
    pthread_mutex_unlock(&plug_mu);
    free(in); free(st);
    /* The core is told it is off while the entry still holds the stream, and with it the lane: exited() and another
     * plugin's MUSIC_OPEN leave a lane alone while its entry has a stream.  The stream was taken out of the entry first
     * and the core told after, without bl: a plugin's process that ended in that gap had exited() free the lane,
     * music_tell found none, and the core's bit for that source stayed on for good (Amazon's engine heard music, the
     * sound detection dropped every window, no phone was ever heard again; M2 of a review of efde36e).  And told by
     * the lane's place the stream was opened in (m->src), not by what the entry says now: a plugin removed while its
     * stream ended had exited() free its entry, another plugin's start took it (memset, no lane), and music_tell
     * found no lane there (L3 of a review of 92ac2e1; start() takes no entry that still has a stream, either).  Under
     * mtell, as music_tell: its look and its word to the core are one, so a word "on" from a look before the end was
     * set comes before this one, never after it.  HASSMIC_PLUGIN_MSTREAM_GAP_MS: that gap held open, for the test
     * that would find it */
    atomic_store(&m->end, 1);
    if (gap && atoi(gap) > 0) { struct timespec d = { atoi(gap) / 1000, atoi(gap) % 1000 * 1000000L }; nanosleep(&d, NULL); }
    pthread_mutex_lock(&mtell); core_music(m->src, 0); pthread_mutex_unlock(&mtell);     /* off in the core's table first: then the lane may go */
    pthread_mutex_lock(&bl);
    { struct plug *p = &plugs[m->entry];
      if (p->ms == m) p->ms = NULL;
      if (!p->ms && !running(p) && p->state != P_STOPPING && lane_mine(p)) atomic_store(&lane_of[p->lane], -1);   /* (its process is gone) */
      if (!p->ms && music_newest == m->entry) music_newest = -1; }
    pthread_mutex_unlock(&bl);
    close(m->fd);
    core_plugin_music_changed();        /* (Home Assistant's media player: nothing of it plays) */
    free(m);
    atomic_fetch_sub(&mstreams_live, 1);
    return NULL;
}

/* bl held: P's stream ends (its thread sees it within its poll) */
static void mstream_end(struct plug *p)
{
    if (!p->ms) return;
    atomic_store(&p->ms->end, 1);
    shutdown(p->ms->fd, SHUT_RDWR);
}

/* MUSIC_OPEN.  bl held on entry, let go here */
static void music_open_message(int i, struct plug *p, const struct pm_hdr *h, const uint8_t *body, size_t blen)
{
    static const unsigned rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };
    struct pm_music_open m; int ok = 0, sv[2] = { -1, -1 }, l = -1, reg = -1, buf = 32768; struct mstream *ms; pthread_t t; long long now = now_ms();
    pm_get_music_open(body, blen, &m);
    for (size_t k = 0; k < sizeof rates / sizeof rates[0]; k++) ok |= rates[k] == m.rate;
    if (p->state != P_RUN || !ok || (m.channels != 1 && m.channels != 2)) { send_err(p, h->id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
    if (p->ms) { send_err(p, h->id, PM_ERR_BUSY); pthread_mutex_unlock(&bl); return; }
    if (p->mopens[p->mopen_next] && now - p->mopens[p->mopen_next] < 60000) { send_err(p, h->id, PM_ERR_LIMIT); p->dropped++; pthread_mutex_unlock(&bl); return; }
    /* a lane: its own from before, or a free one (a lane is free again once its plugin's process and stream are gone) */
    if (lane_mine(p)) l = p->lane;
    else for (int k = 0; k < LANES && l < 0; k++) { int e = atomic_load(&lane_of[k]); if (e < 0 || (!running(&plugs[e]) && plugs[e].state != P_STOPPING && !plugs[e].ms) || plugs[e].lane != k) l = k; }
    if (l < 0) { send_err(p, h->id, PM_ERR_LIMIT); pthread_mutex_unlock(&bl); return; }
    atomic_store(&lane_of[l], i); p->lane = l;
    pthread_mutex_unlock(&bl);
    /* (the core's table, once a lane: a source never leaves it, and the lane's functions say what its plugin does) */
    pthread_mutex_lock(&mtell);
    if (lane_src[l] < 0) lane_src[l] = music_register(&lane_srcs[l]);
    reg = lane_src[l];
    pthread_mutex_unlock(&mtell);
    pthread_mutex_lock(&bl);
    if (reg < 0 || p->state != P_RUN || p->ms || !(ms = calloc(1, sizeof *ms)) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) {
        if (sv[0] >= 0) { close(sv[0]); close(sv[1]); }
        send_err(p, h->id, reg < 0 ? PM_ERR_LIMIT : PM_ERR_FAILED); pthread_mutex_unlock(&bl); return;
    }
    /* both ends non-blocking: the plugin cannot set it (no F_SETFL), and a full buffer must be EAGAIN to it, its
     * poll's business.  32 kB: 170 ms of 48 kHz stereo, what the backend's own buffer adds to */
    fcntl(sv[0], F_SETFL, O_NONBLOCK); fcntl(sv[1], F_SETFL, O_NONBLOCK);
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf); setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    ms->fd = sv[0]; ms->entry = i; ms->src = reg; ms->rate = m.rate; ms->ch = m.channels; ms->gen = p->gen;
    /* newest wins between plugins too: the backend has one plugin stream, the one before ends */
    for (int k = 0; k < NPLUG; k++) if (k != i && plugs[k].ms) { mstream_end(&plugs[k]); say("%s: music: its stream ends, %s opened one", plugs[k].id, p->id); }
    atomic_fetch_add(&mstreams_live, 1);
    if (pthread_create(&t, NULL, mstream_thread, ms)) { atomic_fetch_sub(&mstreams_live, 1); close(sv[0]); close(sv[1]); free(ms); send_err(p, h->id, PM_ERR_FAILED); pthread_mutex_unlock(&bl); return; }
    pthread_detach(t);
    p->ms = ms; p->mstate = PM_MUSIC_PLAYING; p->myield = 0; p->mby = 0;
    p->mopens[p->mopen_next] = now; p->mopen_next = (p->mopen_next + 1) % MOPEN_MINUTE;
    send_ok(p, h->id, sv[1]);
    close(sv[1]);
    say("%s: music: a stream at %u Hz, %u channel%s", p->id, m.rate, m.channels, m.channels > 1 ? "s" : "");
    pthread_mutex_unlock(&bl);
    music_tell(i);
}

/* Text a plugin says that is shown (a title, an entity's state, an action's text argument on its way to it): one line
 * of well-formed UTF-8 (plugin_store.c ps_utf8_char: no overlong form, no surrogate, nothing past U+10FFFF), the
 * control characters spaces.  1 when it was well-formed as it came; else it is cut before the first byte that is no
 * character and 0.  A check by the lead byte alone took C0 80, E0 80 80, ED A0 80 and F4 90 80 80 for characters:
 * Home Assistant's link dropped on the text sensor's state and on every link after it (H1 of a review of efde36e) */
static int shown_text(char *t)
{
    size_t n = strlen(t), i = 0, k;
    for (; i < n; i += k) {
        if (!(k = ps_utf8_char(t + i, n - i))) { t[i] = 0; return 0; }
        if (k == 1 && ((unsigned char)t[i] < 0x20 || t[i] == 0x7f)) t[i] = ' ';
    }
    return 1;
}

int plugin_music_state(void)
{
    int r = 0;
    pthread_mutex_lock(&bl);
    if (music_newest >= 0) {
        struct plug *p = &plugs[music_newest];
        if (running(p) && p->ms) r = p->mstate == PM_MUSIC_PLAYING && !p->myield ? PM_MUSIC_PLAYING : PM_MUSIC_PAUSED;
    }
    pthread_mutex_unlock(&bl);
    return r;
}

int plugin_music_ha(int cmd)
{
    int r = 0;
    pthread_mutex_lock(&bl);
    if (music_newest >= 0) {
        struct plug *p = &plugs[music_newest];
        /* a pause or a stop holds it silent until it is resumed (MY_HELD), whether the plugin obeys or not; a play
         * resumes it from whatever silenced it, and it is the newest source then */
        if (running(p) && p->ms) {
            if (cmd == PM_MCMD_PAUSE && p->mstate == PM_MUSIC_PLAYING && !p->myield) { mcmd(p, PM_MCMD_PAUSE, PM_MWHY_HOME_ASSISTANT); p->mby = 0; p->myield = MY_HELD; p->mdue = 1; r = 1; }
            else if (cmd == PM_MCMD_RESUME && (p->mstate == PM_MUSIC_PAUSED || p->myield)) { mcmd(p, PM_MCMD_RESUME, PM_MWHY_HOME_ASSISTANT); p->myield = MY_NONE; p->mby = 0; p->mdue = 1; r = 1; }
            else if (cmd == PM_MCMD_STOP) { mcmd(p, PM_MCMD_STOP, PM_MWHY_HOME_ASSISTANT); p->myield = MY_HELD; p->mby = 0; p->mdue = 1; r = 1; }
        }
    }
    pthread_mutex_unlock(&bl);
    return r;
}

/* ---------------------------------------------------------------- UDP of its own, the search, proposals (4.6, 4.3) */

static int ip_text(const char *s, uint32_t *a)     /* a.b.c.d as written, no zeros in front */
{
    unsigned x[4]; char tail, again[16];
    if (sscanf(s, "%3u.%3u.%3u.%3u%c", &x[0], &x[1], &x[2], &x[3], &tail) != 4 || x[0] > 255 || x[1] > 255 || x[2] > 255 || x[3] > 255) return -1;
    snprintf(again, sizeof again, "%u.%u.%u.%u", x[0], x[1], x[2], x[3]);
    if (strcmp(again, s)) return -1;
    *a = x[0] << 24 | x[1] << 16 | x[2] << 8 | x[3];
    return 0;
}

/* UDP_OPEN, UDP_PEER, SENDTO, BROADCAST, PROPOSE.  bl held on entry, let go here */
static void udp_message(struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen)
{
    long long now = now_ms(); unsigned gen = p->gen;
    if (p->state != P_RUN) { send_err(p, h->id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
    switch (h->type) {
    case PM_UDP_OPEN: {
        struct pm_udp_open m; int handle, need, n, fd = -1, copy = -1, one = 1, ttl = 1; unsigned short ports[PS_UDP_MAX + 1]; const struct ps_plugin *sp;
        struct sockaddr_in a; char why[80] = "";
        pm_get_udp_open(body, blen, &m);
        if ((m.kind != PM_CAP_NET_UDP && m.kind != PM_CAP_NET_DISCOVER) || !(p->caps & PM_CAPBIT(m.kind))) { send_err(p, h->id, PM_ERR_DENIED); p->dropped++; break; }
        handle = m.kind == PM_CAP_NET_UDP ? m.entry : PS_UDP_MAX;
        if (handle > PS_UDP_MAX || (m.kind == PM_CAP_NET_DISCOVER && m.entry)) { send_err(p, h->id, PM_ERR_DENIED); break; }
        if (p->udp_open[handle]) { send_err(p, h->id, PM_ERR_BUSY); break; }
        pthread_mutex_unlock(&bl);
        /* the port from the store (its file: the same at every start), under its lock */
        ps_lock();
        sp = ps_find(id);
        if (!sp || !store_mine(p, gen, id) || (m.kind == PM_CAP_NET_UDP && m.entry >= sp->m.udp_ports)) n = -2;
        else { need = sp->m.udp_ports + ((sp->m.caps & PM_CAPBIT(PM_CAP_NET_DISCOVER)) != 0); n = ps_udp_ports(id, need, ports); }
        ps_unlock();
        memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (n >= 0) {
            a.sin_port = htons(m.kind == PM_CAP_NET_UDP ? ports[m.entry] : ports[n - 1]);
            /* a plain socket (local only, by the firewall's chain), never net_socket's group */
            if ((fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one)
                || private_only(fd) || bind(fd, (struct sockaddr *)&a, sizeof a) || (copy = fcntl(fd, F_DUPFD_CLOEXEC, 3)) < 0)
                snprintf(why, sizeof why, "%s", strerror(errno));
            if (!why[0] && m.kind == PM_CAP_NET_DISCOVER) {
                unsigned char off = 0;
                setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
                setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);      /* the search stays on the network */
                /* and does not come back to the Echo: a multicast is handed to the sender's own host as well unless
                 * this is off, as if it came in on wlan0 from the Echo's own address (avahi's 5353, a group a daemon
                 * of the Echo's has joined; A1 of a review of efde36e).  A broadcast comes back whatever a socket
                 * says: the manifest names none of hassmic's ports (plugin_store.c discover_parse), and lockdown.sh
                 * drops what comes in from the Echo's own address and a plugin's port before stock's rules see it */
                if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &off, sizeof off)) snprintf(why, sizeof why, "IP_MULTICAST_LOOP: %s", strerror(errno));
            }
        }
        pthread_mutex_lock(&bl);
        if (p->gen != gen || p->state != P_RUN || n < 0 || why[0] || p->udp_open[handle]) {
            if (p->gen == gen) send_err(p, h->id, n == -2 ? PM_ERR_DENIED : n < 0 ? PM_ERR_LIMIT : why[0] ? PM_ERR_FAILED : PM_ERR_BUSY);
            if (why[0] && p->gen == gen) say("%s: no UDP port: %s", id, why);
            if (fd >= 0) close(fd);
            if (copy >= 0) close(copy);
            break;
        }
        { uint8_t b[32]; struct pm_w w; struct pm_udp_opened o = { (uint8_t)handle, ntohs(a.sin_port) };
          p->udp[handle] = fd; p->udp_open[handle] = 1; p->udp_kind[handle] = m.kind; p->udp_port[handle] = o.port;
          pm_winit(&w, b, sizeof b); pm_put_udp_opened(&w, h->id, &o); psend(p, &w, copy);
          say("%s: %s on UDP port %u", id, m.kind == PM_CAP_NET_UDP ? "reachable from the network" : "searches the network", o.port); }
        close(copy);
        break; }
    case PM_UDP_PEER: case PM_SENDTO: {
        struct pm_udp_peer pr; struct pm_sendto st; struct sockaddr_in a; int peer = h->type == PM_UDP_PEER;
        if (peer) { pm_get_udp_peer(body, blen, &pr); st.handle = pr.handle; st.address = pr.address; st.port = pr.port; st.data = pm_b("", 0); }
        else pm_get_sendto(body, blen, &st);
        if (st.handle > PS_UDP_MAX || !p->udp_open[st.handle] || p->udp_kind[st.handle] != PM_CAP_NET_UDP) { if (h->id) send_err(p, h->id, PM_ERR_BAD); else p->dropped++; break; }
        /* SENDTO fifty a second; a new peer one a second (two at once): what goes to a peer once it is fixed passes no
         * broker (RTP's fifty a second), so the peer itself is what is counted.  Unbounded, UDP_PEER and send() asked
         * some two hundred addresses a second whether a port was open (ECONNREFUSED on the connected socket): a scan
         * of the network from the Echo, and a getifaddrs and a route's look on the broker's thread for each (A2 of a
         * review of efde36e) */
        if (!bucket_take(peer ? &p->r_peer : &p->r_send, peer ? PEER_PER_SEC : SEND_PER_SEC, now)) { if (h->id) send_err(p, h->id, PM_ERR_BUSY); p->dropped++; break; }
        { int fd = p->udp[st.handle], ok;
          pthread_mutex_unlock(&bl);
          ok = udp_dest_ok(st.address, st.port);          /* (getifaddrs: not under bl) */
          pthread_mutex_lock(&bl);
          if (p->gen != gen || !p->udp_open[st.handle] || p->udp[st.handle] != fd) break;
          if (!ok) { if (h->id) send_err(p, h->id, PM_ERR_DENIED); refusal(p, "asked to send UDP where it may not (%s): refused", private4(st.address) ? "the Echo's own address, a broadcast, port 5555 or DNS's port" : "not an address on the network"); break; }
          memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(st.address); a.sin_port = htons(st.port);
          /* UDP's connect and a non-blocking sendto do not wait, and the plugin cannot change the socket's mode */
          if (peer) { if (connect(fd, (struct sockaddr *)&a, sizeof a)) send_err(p, h->id, PM_ERR_FAILED); else answer(p, h->id, 0); }
          else if (sendto(fd, st.data.s, st.data.n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a) < 0) { if (h->id) send_err(p, h->id, PM_ERR_BUSY); }
          else answer(p, h->id, 0); }
        break; }
    case PM_BROADCAST: {
        struct pm_broadcast m; int fd, sent = 0; uint32_t group = 0; unsigned port = 0; const struct ps_plugin *sp;
        pm_get_broadcast(body, blen, &m);
        if (m.handle > PS_UDP_MAX || !p->udp_open[m.handle] || p->udp_kind[m.handle] != PM_CAP_NET_DISCOVER) { if (h->id) send_err(p, h->id, PM_ERR_BAD); else p->dropped++; break; }
        if (p->bcast_at && now - p->bcast_at < 1000) { if (h->id) send_err(p, h->id, PM_ERR_BUSY); p->dropped++; break; }
        p->bcast_at = now; fd = p->udp[m.handle];
        { uint8_t data[1472]; size_t n = m.data.n; memcpy(data, m.data.s, n);
          pthread_mutex_unlock(&bl);
          ps_lock(); if ((sp = ps_find(id))) { group = sp->m.discover_group; port = (unsigned)sp->m.discover_port; } ps_unlock();
          pthread_mutex_lock(&bl);
          if (p->gen != gen || !p->udp_open[m.handle] || p->udp[m.handle] != fd || !port) break;
          /* every network's broadcast address (a directed broadcast: 255.255.255.255 goes out on one interface only,
           * the default route's, and with none at all nowhere), or the manifest's group */
          { struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
            if (group) { a.sin_addr.s_addr = htonl(group); sent = sendto(fd, data, n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a) >= 0; }
            else {
                struct ifaddrs *ifa, *i;
                pthread_mutex_unlock(&bl);
                if (!getifaddrs(&ifa)) {
                    /* the network's broadcast from its address and mask, not the interface's own broadcast address:
                     * one that was given none (`ip addr add` without "brd") reports 0.0.0.0 there */
                    for (i = ifa; i; i = i->ifa_next)
                        if (i->ifa_addr && i->ifa_netmask && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_BROADCAST) && !(i->ifa_flags & IFF_LOOPBACK)) {
                            uint32_t ad = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr), mk = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
                            if (!private4(ad) || mk == 0xffffffffu) continue;      /* (a private network's, and one with room for others) */
                            a.sin_addr.s_addr = htonl(ad | ~mk);
                            sent += sendto(fd, data, n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a) >= 0;
                        }
                    freeifaddrs(ifa);
                }
                pthread_mutex_lock(&bl);
                if (p->gen != gen) break;
            } }
          if (!sent && !p->bcast_said) { p->bcast_said = 1; say("%s: its search went nowhere (no network to send it on)", id); }
          answer(p, h->id, sent ? 0 : PM_ERR_FAILED); }
        break; }
    case PM_PROPOSE: {
        struct pm_propose m; char setting[33], value[16], have[PS_VALUE_MAX]; uint32_t a; int k = -1, err = 0; const struct ps_plugin *sp;
        pm_get_propose(body, blen, &m);
        if (pm_cstr(setting, sizeof setting, m.setting) || pm_cstr(value, sizeof value, m.value)) { send_err(p, h->id, PM_ERR_BAD); break; }
        pthread_mutex_unlock(&bl);
        have[0] = 0;
        /* a host row its net.server names, and nothing else: what the owner then confirms is "the server you enter" */
        ps_lock();
        if (!(sp = ps_find(id))) err = PM_ERR_DENIED;
        else { for (int e = 0; e < sp->m.nnet && k < 0; e++) if (!strcmp(sp->m.net[e].setting, setting)) k = e; ps_config_owner(sp, setting, have, sizeof have); }
        ps_unlock();
        /* only a private address: a public host is the owner's to type (4.3) */
        if (!err && (k < 0 || ip_text(value, &a) || !private4(a))) err = PM_ERR_DENIED;
        if (!err && !udp_dest_ok(a, 1)) err = PM_ERR_DENIED;
        pthread_mutex_lock(&bl);
        if (p->gen != gen) break;
        if (err) { send_err(p, h->id, err); refusal(p, "proposed what it may not (a row its net.server does not name, or not a private address): refused"); break; }
        /* the slot of that row: one at a time, a minute apart (the same one again changes nothing) */
        { int slot = -1;
          for (int x = 0; x < PS_NET; x++) if (p->prop[x].at && !strcmp(p->prop[x].setting, setting)) slot = x;
          if (slot >= 0 && now - p->prop[slot].at < PROPOSE_MS) { send_err(p, h->id, PM_ERR_BUSY); break; }
          if (!strcmp(have, value)) { if (slot >= 0) p->prop[slot].live = 0; send_ok(p, h->id, -1); break; }       /* (what is there already) */
          if (slot < 0) for (int x = 0; x < PS_NET && slot < 0; x++) if (!p->prop[x].live) slot = x;
          if (slot < 0) slot = 0;
          snprintf(p->prop[slot].setting, sizeof p->prop[0].setting, "%s", setting); snprintf(p->prop[slot].value, sizeof p->prop[0].value, "%s", value);
          p->prop[slot].at = now; p->prop[slot].live = 1;
          send_ok(p, h->id, -1); }
        pthread_mutex_unlock(&bl);
        log_private("plugin %s: proposes %s for its row %s (shown on its card)", id, value, setting);
        return; }
    }
    pthread_mutex_unlock(&bl);
}

/* The owner's click on a proposal (web.c): written as the owner's own value, and the plugin started again as for any
 * change of a server's row.  0, -1 (none such: it was replaced or dropped meanwhile).  DROP: the proposal goes */
int plugin_propose_take(const char *id, const char *setting, const char *value, int drop, char *err, size_t errsz)
{
    struct plug *p; int found = 0, rc = 0; const struct ps_plugin *sp;
    pthread_mutex_lock(&bl);
    if ((p = plug_of(id))) for (int x = 0; x < PS_NET; x++)
        if (p->prop[x].live && !strcmp(p->prop[x].setting, setting) && !strcmp(p->prop[x].value, value)) { p->prop[x].live = 0; found = 1; }
    pthread_mutex_unlock(&bl);
    if (!found) { snprintf(err, errsz, "that proposal is not there any more"); return -1; }
    if (drop) return 0;
    /* asked again as PROPOSE asked it: an update since may have taken the row out of net.server (then it is no
     * server's row the owner confirms), and the address may be the Echo's own by now (a review of efde36e) */
    { uint32_t a; int named = 0;
      ps_lock(); if ((sp = ps_find(id))) for (int e = 0; e < sp->m.nnet; e++) named |= !strcmp(sp->m.net[e].setting, setting); ps_unlock();
      if (!named || ip_text(value, &a) || !udp_dest_ok(a, 1)) { snprintf(err, errsz, "that proposal is no longer one this plugin may make"); return -1; } }
    ps_lock(); sp = ps_find(id); rc = sp ? ps_config_set(sp, setting, value, err, errsz) : -1; ps_unlock();
    if (!rc) plugin_restart(id, "the server you entered changed");
    return rc;
}

/* ---------------------------------------------------------------- Home Assistant's entities and actions (4.10) */

/* Listed: plugins on and able to run (a launcher, plugin_feature_on's word, and a sandbox that holds; listed with
 * none, every entity of every plugin stood there for good as unavailable), the plugin enabled, accepted and runnable
 * (4.10: "while the plugin is enabled, running or not").  ON: runnable() under bl, asked by the caller */
static int runnable(void) { return feature && launcher >= 0 && sandbox_ok; }        /* bl held */
static int ha_listed(const struct ps_plugin *sp, int on) { return on && sp->here && !sp->unread && sp->enabled && sp->accepted && !sp->refused[0]; }

/* An entity's NAME: what Home Assistant keeps it by (aioesphomeapi build_unique_id, version 3: "<mac>/0/<type>/<name>";
 * the key only within a listing, plugin_store.c's .hakeys) and what makes its entity id, as the entity is first
 * made: <domain>.slugify("<area> <device's name> <entity's name>") (HA 2026.10.1 entity_registry: unidecode, lower case,
 * every run of anything but a-z0-9 one "_").  Built as "Plugin <length of the id> <id>: <label>".
 *  - Its slug is never one of hassmic's own.  "<plugin's name> · <label>" was: the "·" is slugified away, and the
 *    plugin "Last" with the text sensor "heard" made sensor.<node>_last_heard, the feature's "Last heard".  Home
 *    Assistant removes the registry entry of an entity that is no longer listed (a feature switched off, a plugin
 *    removed: entry_data.async_remove_entities), so whichever comes next takes its entity id, and the one that comes
 *    back gets "_2": automations and history went with the wrong one (K1 of a review of 92ac2e1).  This one's slug
 *    begins "plugin_<digits>_", and no name of hassmic's own slugifies to that (tests/unit/ha_names_test.py holds
 *    every string of proto_esphome.c to it, with HA's own slugify where build/ha-venv has it, run-time names too).
 *  - Two plugins' are never one: the id's length before it says where the id ends in the slug ("plugin_9_weather_x_
 *    temp"), so "weather" with "x temp" and "weather-x" with "temp" are two entity ids.  The id alone was not enough
 *    for that, and neither is anything after the id (a label may begin with what a length would look like).  The
 *    names themselves, and with them the unique ids, are two as well: ids are one plugin's each (K2: a dedupe by the
 *    name between listed plugins was the fix before, and missed a plugin switched off).
 *  - Within one plugin labels are its own one by one (plugin_store.c entities_parse); two that slugify alike ("Temp",
 *    "temp!") are that plugin's own, as with any device: Home Assistant gives the second "_2" */
static void ha_name(const struct ps_plugin *sp, int k, char *out, size_t cap) { snprintf(out, cap, "Plugin %zu %s: %s", strlen(sp->m.id), sp->m.id, sp->m.entities[k].label); }
_Static_assert(sizeof "Plugin 24 : " - 1 + PS_ID_MAX - 1 + sizeof ((struct ps_entity *)0)->label <= sizeof ((struct plugin_entity *)0)->name, "a plugin entity's name is never cut");

/* the store's lock held: entity K (or action K) of SP goes to Home Assistant */
static int ha_entity_out(const struct ps_plugin *sp, int k, int on) { return ha_listed(sp, on) && (sp->m.caps & PM_CAPBIT(PM_CAP_HA_ENTITIES)) && sp->ent_key[k]; }
static int ha_action_out(const struct ps_plugin *sp, int k, int on) { return ha_listed(sp, on) && (sp->m.caps & PM_CAPBIT(PM_CAP_HA_ACTIONS)) && sp->act_key[k]; }

/* The object id, which names an action's service in Home Assistant (esphome.<node>_<object id>) and an entity's id
 * as it is first made: "p<length of the id>_<id, - as _>_<name>".  "p_<id, - as _>_<name>" was one for two: the id
 * "a-b" with the action "c" and the id "a" with "b_c" were both p_a_b_c, and Home Assistant had one service for both
 * (a review of efde36e).  The length makes it one: none of hassmic's own starts with "p" and a digit
 * (tests/unit/ha_names_test.py) */
static void ha_object_id(const char *id, const char *name, char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "p%zu_", strlen(id));
    for (const char *c = id; *c && n + 1 < cap; c++) out[n++] = *c == '-' ? '_' : *c;
    snprintf(out + n, cap > n ? cap - n : 0, "_%s", name);
}
_Static_assert(4 + PS_ID_MAX + 1 + 33 <= sizeof ((struct plugin_entity *)0)->object_id, "an object id is never cut");

void plugin_entities_each(void (*fn)(const struct plugin_entity *e, void *arg), void *arg)
{
    const struct ps_plugin *list; int n; static struct plugin_entity e;        /* (one at a time: the store's lock) */
    int on;
    pthread_mutex_lock(&bl); on = runnable(); pthread_mutex_unlock(&bl);
    if (!on) return;
    ps_lock();
    n = ps_list(&list);
    for (int i = 0; i < n; i++) {
        const struct ps_plugin *sp = &list[i];
        if (!ha_listed(sp, on)) continue;
        for (int k = 0; k < sp->m.nentities; k++) {
            const struct ps_entity *x = &sp->m.entities[k];
            if (!ha_entity_out(sp, k, on)) continue;
            memset(&e, 0, sizeof e);
            e.key = sp->ent_key[k]; e.kind = x->kind;
            ha_object_id(sp->m.id, x->name, e.object_id, sizeof e.object_id);
            ha_name(sp, k, e.name, sizeof e.name);
            snprintf(e.icon, sizeof e.icon, "%s", x->icon); snprintf(e.unit, sizeof e.unit, "%s", x->unit);
            e.decimals = x->decimals; e.min = x->min; e.max = x->max; e.step = x->step;
            e.n = x->kind == PE_SELECT ? x->noptions : x->kind == PE_EVENT ? x->ntypes : 0;
            for (int o = 0; o < e.n; o++) snprintf(e.words[o], sizeof e.words[0], "%s", x->kind == PE_SELECT ? x->options[o] : x->types[o]);
            fn(&e, arg);
        }
        for (int k = 0; k < sp->m.nactions; k++) {
            const struct ps_action *x = &sp->m.actions[k];
            if (!ha_action_out(sp, k, on)) continue;
            memset(&e, 0, sizeof e);
            e.key = sp->act_key[k]; e.kind = -1;
            ha_object_id(sp->m.id, x->name, e.object_id, sizeof e.object_id);
            snprintf(e.name, sizeof e.name, "%s: %s", sp->m.name, x->label);
            e.n = x->nargs;
            for (int o = 0; o < e.n; o++) { snprintf(e.words[o], sizeof e.words[0], "%s", x->args[o].name); e.is_int[o] = x->args[o].is_int; }
            fn(&e, arg);
        }
    }
    ps_unlock();
}

int plugin_has_music(void)
{
    const struct ps_plugin *list; int n, r = 0, on;
    pthread_mutex_lock(&bl); on = runnable(); pthread_mutex_unlock(&bl);
    if (!on) return 0;
    ps_lock(); n = ps_list(&list); for (int i = 0; i < n; i++) r |= ha_listed(&list[i], on) && (list[i].m.caps & PM_CAPBIT(PM_CAP_AUDIO_MUSIC)); ps_unlock();
    return r;
}

/* which plugin and which of its entities (or actions) KEY is, of those listed now: the store's lock held by the
 * caller.  A key is one plugin's alone (plugin_store.c's table), so the first found is the only one */
static const struct ps_plugin *ha_find(unsigned key, int action, int *k, int on)
{
    const struct ps_plugin *list; int n = ps_list(&list);
    if (!key) return NULL;
    for (int i = 0; i < n; i++) {
        const struct ps_plugin *sp = &list[i];
        if (!ha_listed(sp, on)) continue;
        if (!action) for (int x = 0; x < sp->m.nentities; x++) if (sp->ent_key[x] == key) { if (!ha_entity_out(sp, x, on)) return NULL; *k = x; return sp; }
        if (action) for (int x = 0; x < sp->m.nactions; x++) if (sp->act_key[x] == key) { if (!ha_action_out(sp, x, on)) return NULL; *k = x; return sp; }
    }
    return NULL;
}

/* What Home Assistant lists of the plugins (every entity and action with its key, name and kind, and whether the
 * media player offers pause and play), as a digest: a change of it closes the links, and Home Assistant reads the
 * list again (core_plugin_entities_changed, as for a feature's switch).  Asked wherever the list may have changed
 * (an install, an update, a removal, a plugin switched on or off, a revocation list, plugins on or off, the launcher
 * gone, hassmic's crashes forgiven) and every few seconds by the broker besides: an update that dropped ha.entities
 * was asked about by what the new manifest has, so the links stayed and Home Assistant kept the entities it had,
 * frozen; a revocation and the plugins' switch were not asked about at all (H4 of a review of efde36e).  No lock held:
 * it takes the store's and core_lock */
static void ha_digest_one(const struct plugin_entity *e, void *arg)
{
    crypto_blake2b_update(arg, (const uint8_t *)e, sizeof *e);     /* (memset before it is filled: no stray bytes) */
}
static void ha_relist_as(int tell)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static uint8_t told[16]; static int have;
    crypto_blake2b_ctx c; uint8_t d[16]; int music = plugin_has_music(), changed;
    pthread_mutex_lock(&mu);
    crypto_blake2b_init(&c, sizeof d);
    crypto_blake2b_update(&c, (const uint8_t *)&music, sizeof music);
    plugin_entities_each(ha_digest_one, &c);
    crypto_blake2b_final(&c, d);
    changed = have && memcmp(d, told, sizeof d);
    memcpy(told, d, sizeof d); have = 1;
    pthread_mutex_unlock(&mu);
    if (changed && tell) core_plugin_entities_changed();
}
static void ha_relist(void) { ha_relist_as(1); }

int plugin_entity_value(unsigned key, int *kind, char *out, size_t cap)
{
    const struct ps_plugin *sp; int k, r = -1; char id[PS_ID_MAX] = "";
    out[0] = 0;
    pthread_mutex_lock(&bl); int on = runnable(); pthread_mutex_unlock(&bl);
    if (!on) return -1;
    ps_lock(); if ((sp = ha_find(key, 0, &k, on))) { snprintf(id, sizeof id, "%s", sp->m.id); *kind = sp->m.entities[k].kind; r = 0; } ps_unlock();
    if (r < 0) return -1;
    pthread_mutex_lock(&bl);
    { struct plug *p = plug_of(id);
      if (p && p->state == P_RUN && p->ent_have & 1u << k) { snprintf(out, cap, "%s", p->ent[k]); r = 1; } }
    pthread_mutex_unlock(&bl);
    return r;
}

/* is V one the entity takes (ENTITY_STATE from the plugin, a command from Home Assistant alike).  A number within its
 * limits as Home Assistant has them: they go to it as float32 (proto_esphome.c list_plugin), and its slider at the top
 * sends that float back, which for a limit float32 does not hold (0.1, 99.99) lies past the manifest's double: refused,
 * so the top and bottom of the slider did nothing (H5 of a review of efde36e).  Taken where it is the limit's float,
 * and written as the limit (V has CAP bytes) */
static int ent_value_ok(const struct ps_entity *x, char *v, size_t cap)
{
    char *end; double d;
    switch (x->kind) {
    case PE_BINARY_SENSOR: case PE_SWITCH: return !strcmp(v, "on") || !strcmp(v, "off");
    case PE_SENSOR: d = strtod(v, &end); return *v && !*end && end != v && d == d && d > -1e15 && d < 1e15;
    case PE_NUMBER:
        d = strtod(v, &end);
        if (!*v || *end || end == v || d != d) return 0;
        if (d < x->min && (float)d == (float)x->min) snprintf(v, cap, "%.17g", x->min);
        else if (d > x->max && (float)d == (float)x->max) snprintf(v, cap, "%.17g", x->max);
        else if (d < x->min || d > x->max) return 0;
        return 1;
    case PE_SELECT: for (int o = 0; o < x->noptions; o++) if (!strcmp(x->options[o], v)) return 1; return 0;
    case PE_TEXT_SENSOR: return 1;
    default: return 0;
    }
}

int plugin_entity_command(unsigned key, const char *value)
{
    const struct ps_plugin *sp; int k, ok = 0, kind = -1, on; char id[PS_ID_MAX] = "", name[33] = "", v[256];
    snprintf(v, sizeof v, "%s", value);
    pthread_mutex_lock(&bl); on = runnable(); pthread_mutex_unlock(&bl);
    ps_lock();
    if ((sp = ha_find(key, 0, &k, on))) {
        const struct ps_entity *x = &sp->m.entities[k]; kind = x->kind;
        ok = x->kind == PE_BUTTON ? 1 : (x->kind == PE_SWITCH || x->kind == PE_NUMBER || x->kind == PE_SELECT) && ent_value_ok(x, v, sizeof v);
        snprintf(id, sizeof id, "%s", sp->m.id); snprintf(name, sizeof name, "%s", x->name);
    }
    ps_unlock();
    if (!ok) return -1;
    pthread_mutex_lock(&bl);
    { struct plug *p = plug_of(id); uint8_t b[400]; struct pm_w w; struct pm_entity_set m = { pm_s(name), pm_s(kind == PE_BUTTON ? "" : v) };
      if (p && p->state == P_RUN) { pm_winit(&w, b, sizeof b); pm_put_entity_set(&w, 0, &m); ok = !psend(p, &w, -1); } else ok = 0; }
    pthread_mutex_unlock(&bl);
    return ok ? 0 : -1;
}

/* An action to its plugin.  Each argument as the manifest types it: an int from INTS, a string from ARGS.  Home
 * Assistant sends each in a field of its type, and proto3 leaves out a field at its default: an empty string came as
 * no field at all, was taken for an int, and reached the plugin as "0" (H6 of a review of efde36e).  The card's button
 * (ID_OR_NULL) has none */
static int action_send(unsigned key, const char *id_or_null, const char *name_or_null, int from, char (*args)[256], const int *ints, int nargs)
{
    const struct ps_plugin *sp; int k = -1, ok = 0, on, calls = 0; char id[PS_ID_MAX] = "", name[33] = "", vals[PS_ARGS][256], names[PS_ARGS][33], tok[33] = ""; int n = 0;
    pthread_mutex_lock(&bl); on = runnable(); pthread_mutex_unlock(&bl);
    ps_lock();
    if (id_or_null) {
        /* the card's button: only an action marked "page", and one without arguments (actions_parse holds that) */
        if ((sp = ps_find(id_or_null)) && ha_listed(sp, on) && (sp->m.caps & PM_CAPBIT(PM_CAP_HA_ACTIONS)))
            for (int x = 0; x < sp->m.nactions; x++) if (!strcmp(sp->m.actions[x].name, name_or_null) && sp->m.actions[x].page) k = x;
        if (k < 0) sp = NULL;
    } else sp = ha_find(key, 1, &k, on);
    if (sp) {
        const struct ps_action *a = &sp->m.actions[k];
        ok = nargs >= a->nargs; n = a->nargs;
        for (int i = 0; i < n && ok; i++) {
            snprintf(names[i], sizeof names[0], "%s", a->args[i].name);
            if (a->args[i].is_int) snprintf(vals[i], sizeof vals[0], "%d", ints ? ints[i] : 0);
            else { snprintf(vals[i], sizeof vals[0], "%s", args[i]); ok = shown_text(vals[i]); }
        }
        snprintf(id, sizeof id, "%s", sp->m.id); snprintf(name, sizeof name, "%s", a->name);
        calls = a->calls && (sp->m.caps & PM_CAPBIT(PM_CAP_CALL));
    }
    ps_unlock();
    if (!ok) return -1;
    pthread_mutex_lock(&bl);
    { struct plug *p = plug_of(id); static uint8_t b[2048]; struct pm_w w; struct pm_action m;
      if (p && p->state == P_RUN) {
          /* An action marked "calls" brings a call token (3.6): both ways here are a person's, Home Assistant's keyed
           * link (proto_esphome.c takes no other) and an approved browser's signed request (web.c).  An automation
           * that calls the plugin's other actions opens no microphone */
          if (calls) token_new(p, tok);
          m = (struct pm_action){ pm_s(name), (uint8_t)from, pm_s(tok), { { 0 }, (uint16_t)n } };
          pm_winit(&w, b, sizeof b); pm_put_action(&w, 0, &m);
          for (int i = 0; i < n; i++) { struct pm_action_args e = { pm_s(names[i]), pm_s(vals[i]) }; pm_add_action_args(&w, &e); }
          ok = !psend(p, &w, -1);
      } else ok = 0; }
    pthread_mutex_unlock(&bl);
    if (ok) say("%s: action %s (%s%s)", id, name, from == PM_FROM_PAGE ? "its card's button" : "Home Assistant", calls ? ", with a call token" : "");
    return ok ? 0 : -1;
}

int plugin_action(unsigned key, char (*args)[256], const int *ints, int nargs) { return action_send(key, NULL, NULL, PM_FROM_HOME_ASSISTANT, args, ints, nargs); }
int plugin_action_page(const char *id, const char *name) { return action_send(0, id, name, PM_FROM_PAGE, NULL, NULL, 0); }

/* ENTITY_STATE, EVENT.  bl held on entry, let go here */
static void entity_message(struct plug *p, const char *id, const struct pm_hdr *h, const uint8_t *body, size_t blen)
{
    char name[33], value[256], type[33] = ""; int k = -1, kind = -1, ok = 0, ev = h->type == PM_EVENT; unsigned gen = p->gen, key = 0; const struct ps_plugin *sp;
    if (ev) { struct pm_event m; pm_get_event(body, blen, &m); if (pm_cstr(name, sizeof name, m.name) || pm_cstr(type, sizeof type, m.type)) name[0] = 0; value[0] = 0; }
    else { struct pm_entity_state m; pm_get_entity_state(body, blen, &m); if (pm_cstr(name, sizeof name, m.name) || pm_cstr(value, sizeof value, m.value)) name[0] = 0; }
    if (p->state != P_RUN || !name[0]) { if (h->id) send_err(p, h->id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
    if (!bucket_take(&p->r_ent, ENT_PER_SEC, now_ms())) { if (h->id) send_err(p, h->id, PM_ERR_BUSY); p->dropped++; pthread_mutex_unlock(&bl); return; }
    pthread_mutex_unlock(&bl);
    ps_lock();
    if ((sp = ps_find(id))) for (int x = 0; x < sp->m.nentities; x++) if (!strcmp(sp->m.entities[x].name, name)) {
        const struct ps_entity *e = &sp->m.entities[x]; k = x; kind = e->kind; key = sp->ent_key[x];
        if (ev) { if (e->kind == PE_EVENT) for (int t = 0; t < e->ntypes; t++) ok |= !strcmp(e->types[t], type); }
        else ok = e->kind != PE_EVENT && e->kind != PE_BUTTON && ent_value_ok(e, value, sizeof value) && shown_text(value);
    }
    ps_unlock();
    pthread_mutex_lock(&bl);
    if (p->gen != gen) { pthread_mutex_unlock(&bl); return; }
    if (!ok) {
        if (h->id) send_err(p, h->id, PM_ERR_BAD);
        /* (the name into the log only where it is one of its manifest's: anything else is the plugin's free text) */
        refusal(p, "said what its entity cannot take (%s %s): refused", k < 0 ? "no entity" : ps_entity_kind(kind), k < 0 ? "?" : name);
        pthread_mutex_unlock(&bl); return;
    }
    if (!ev) {
        if (!(p->ent_have & 1u << k) || strcmp(p->ent[k], value)) { snprintf(p->ent[k], sizeof p->ent[0], "%s", value); p->ent_have |= 1u << k; p->ent_dirty |= 1u << k; }
    }
    answer(p, h->id, 0);
    pthread_mutex_unlock(&bl);
    if (ev && key) core_plugin_entity(key, type);
}

/* The broker's loop, no lock held: the states the core has not been told of (ENTITY_STATE, a plugin that ended:
 * unavailable), and the Echo's address every NET_LOOK_MS for NET */
static void ha_flush(void)
{
    unsigned keys[NPLUG * PS_ENTITIES]; int nk = 0; long long now = now_ms(), look = 0;
    char ids[NPLUG][PS_ID_MAX]; uint32_t dirty[NPLUG]; int nd = 0;
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && plugs[i].ent_dirty) { snprintf(ids[nd], PS_ID_MAX, "%s", plugs[i].id); dirty[nd++] = plugs[i].ent_dirty; plugs[i].ent_dirty = 0; }
    for (int i = 0; i < NPLUG && !look; i++) look = running(&plugs[i]) && (plugs[i].caps & NET_CAPS);
    if (look && now - net_looked < NET_LOOK_MS) look = 0;
    pthread_mutex_unlock(&bl);
    if (nd) {
        const struct ps_plugin *sp;
        ps_lock();
        for (int d = 0; d < nd; d++) if ((sp = ps_find(ids[d]))) for (int k = 0; k < sp->m.nentities; k++) if (dirty[d] & 1u << k && sp->ent_key[k]) keys[nk++] = sp->ent_key[k];
        ps_unlock();
        for (int i = 0; i < nk; i++) core_plugin_entity(keys[i], NULL);
    }
    if (look) {
        uint32_t a, m; net_look(&a, &m);
        pthread_mutex_lock(&bl);
        net_addr = a; net_mask = m; net_looked = now;
        for (int i = 0; i < NPLUG; i++) tell_echo(&plugs[i]);
        pthread_mutex_unlock(&bl);
    }
}

/* ---------------------------------------------------------------- calls (call; phase 5, docs/plugins.md 3.6, 4.8)
 *
 * One call on the Echo at a time (call.c's claim), Drop In's or one plugin's.  A plugin's:
 *   in:   CALL_INCOMING: it rings (C_RINGING), hassmic's ringtone and light, on a Show the plugin's name beside the
 *         caller's.  A person answers here, at the Echo (the action button, the screen): CALL_ANSWER (C_ANSWERED,
 *         the core told CALL_ANSWERING: the ring stops), and once the far side has it the plugin says CALL_LIVE.
 *         Nothing else answers: not Home Assistant, not the page, no token, never by itself (Drop In's automatic answer
 *         does not apply; 3.6)
 *   out:  only with a token: an ACTION marked "calls", from Home Assistant's keyed link or an approved browser (both a
 *         person's), brings one; CALL_OUT spends it (C_CALLING), CALL_LIVE when the far side took it.  Tokens are
 *         for this way only: CALL_OUT is the one message that takes one
 *   live: the media is call.c's without a codec (CALL_CODEC_PCM): gate, echo suppressor, gain, the call microphone and
 *         listening mode as Drop In's.  One SOCK_SEQPACKET pair, the plugin's end handed over with CALL_LIVE's OK: the
 *         room's frames to it (call_send, under call.c's tx_mu: one send that never waits), the far side's from it (a
 *         thread of its own, media_reader, into call.c's jitter buffer)
 *   end:  CALL_END from the plugin; CALL_HANGUP to it (the button, "stop" or "hang up", the screen, a ring nobody
 *         answered, no CALL_LIVE in time, the plugin stopped); its process gone.  The media ends at once
 * So the room reaches a plugin only between CALL_LIVE and the end, and an incoming call is live only after a person
 * answered it.  Home Assistant sees none of it but through the plugin's own entities (4.8: no new members of proto).
 *
 * Locks: all of it under bl, as everything of a plugin's.  The core hears of each state from a thread of its own
 * (teller), in order: core_call takes core_lock, which is never taken under bl, and the button, the screen and a "stop"
 * call in here from under core_lock or not (core_transcript holds it).  The claim goes when the core has heard "idle":
 * a call that began meanwhile would otherwise be shown idle by the end of the one before.  The media socket under cml,
 * a leaf (call_send holds call.c's tx_mu, then it); call_begin and call_end are taken under bl (bl before call.c's ml). */
static pthread_mutex_t cml = PTHREAD_MUTEX_INITIALIZER;
static int media_fd = -1;               /* cml: hassmic's end while live, -1 */
static unsigned media_tag;              /* cml: which media media_fd is of (call_send's arg) */
static atomic_uint media_dropped;       /* frames to the plugin that did not fit its socket, this call */
static atomic_int media_readers;        /* media_reader threads that have not ended (the tests wait for 0) */
/* what the core is told, in order (teller) */
#define TELLQ 16
static struct tell { int who, state, outgoing; char peer[120]; } tq[TELLQ];
static unsigned tq_in, tq_out;
static pthread_cond_t tcv = PTHREAD_COND_INITIALIZER;
static int teller_up;                   /* bl: the teller runs */

static void *teller(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&bl);
    for (;;) {
        while (tq_in == tq_out) pthread_cond_wait(&tcv, &bl);
        struct tell t = tq[tq_out++ % TELLQ];
        pthread_mutex_unlock(&bl);
        core_call(t.who, t.state, t.peer, t.outgoing);
        if (t.state == CALL_IDLE) call_release(t.who);
        pthread_mutex_lock(&bl);
    }
    return NULL;
}
/* bl held: from plugin_start, and again before every state and in every round of the broker while one waits: a teller
 * that could not be made (pthread_create failed once, under pthread_once never again) left every state untold and the
 * claim the plugin's for good: no call, Drop In's neither, until a restart (W6 of a review of fc43c40) */
static void teller_start(void)
{
    pthread_t t;
    if (teller_up) return;
    if (pthread_create(&t, NULL, teller, NULL)) { static long long said; if (now_ms() - said > 10000) { said = now_ms(); say("calls: no thread to tell the core (tried again)"); } return; }
    pthread_detach(t); teller_up = 1;
}

static void tell(int entry, int state)  /* bl held */
{
    struct tell *t = &tq[tq_in++ % TELLQ];
    teller_start();
    if (tq_in - tq_out > TELLQ) tq_out = tq_in - TELLQ;     /* (a call makes four: never) */
    t->who = CALL_PLUGIN + entry; t->state = state; t->outgoing = pc.outgoing;
    snprintf(t->peer, sizeof t->peer, "%s", state == CALL_IDLE ? "" : pc.peer);
    pthread_cond_signal(&tcv);
}

/* call.c's side, from the capture thread or its call microphone's, with its tx_mu held: one frame to the plugin, a
 * u32 sequence number in front, sent without waiting (3.5: dropped when its socket is full) */
static void call_send(const uint8_t *frame, size_t len, uint32_t seq, void *arg)
{
    uint8_t pkt[MEDIA_PKT];
    if (len != CALL_PCM_BYTES) return;
    for (int i = 0; i < 4; i++) pkt[i] = (uint8_t)(seq >> 8 * i);
    memcpy(pkt + 4, frame, len);
    pthread_mutex_lock(&cml);
    if (media_fd >= 0 && media_tag == (unsigned)(uintptr_t)arg && send(media_fd, pkt, sizeof pkt, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)sizeof pkt) atomic_fetch_add(&media_dropped, 1);
    pthread_mutex_unlock(&cml);
}

/* The far side's audio from the plugin: packets of MEDIA_PKT bytes (anything else is dropped), MEDIA_PER_SEC at most,
 * every packet counted whatever its size, and after one of another size the next read READ_MIN_MS later: what is more
 * stays in its socket, and its writes say EAGAIN.  Counting only whole frames, a plugin that sent packets of one byte
 * had this thread read ~690 000 a second, a core's worth of hassmic's time (W2 of a review of fc43c40).  The thread
 * owns its descriptor: it closes it when the media ended (media_fd no longer its own: nobody sends on it any more) or
 * the plugin closed its end */
struct media_rd { int fd, gen; unsigned tag; };
static void *media_reader(void *arg)
{
    struct media_rd r = *(struct media_rd *)arg; uint8_t pkt[MEDIA_PKT + 1]; long long win = now_ms(), next_read = 0; int n = 0;
    free(arg);
    for (;;) {
        struct pollfd pf = { r.fd, POLLIN, 0 }; ssize_t k; uint32_t seq = 0; int on; long long now = now_ms(), wait = 0;
        pthread_mutex_lock(&cml); on = media_fd == r.fd && media_tag == r.tag; pthread_mutex_unlock(&cml);
        if (!on) break;
        if (now - win >= 1000) { win = now; n = 0; }
        if (n >= MEDIA_PER_SEC) wait = win + 1000 - now;                    /* the second's share spent: until the next */
        else if (next_read > now) wait = next_read - now;
        if (wait > 0) { usleep((useconds_t)(wait < 100 ? wait : 100) * 1000); continue; }     /* (looks at the end in between) */
        if (poll(&pf, 1, 100) <= 0) continue;
        k = recv(r.fd, pkt, sizeof pkt, MSG_DONTWAIT);
        if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {      /* its end closed: nothing more to send it either */
            pthread_mutex_lock(&cml); if (media_fd == r.fd && media_tag == r.tag) media_fd = -1; pthread_mutex_unlock(&cml);
            break;
        }
        if (k < 0) continue;
        n++;
        if (k != MEDIA_PKT) { next_read = now_ms() + read_min_ms; continue; }
        for (int i = 0; i < 4; i++) seq |= (uint32_t)pkt[i] << 8 * i;
        call_rx(r.gen, seq, pkt + 4, CALL_PCM_BYTES);
    }
    close(r.fd);
    atomic_fetch_sub(&media_readers, 1);
    return NULL;
}

static void media_stop(void)            /* bl held: the media, if any, ends now */
{
    if (pc.media >= 0) { call_end(pc.media); pc.media = -1; }
    pthread_mutex_lock(&cml);
    if (media_fd >= 0) { shutdown(media_fd, SHUT_RDWR); media_fd = -1; }
    media_tag++;
    pthread_mutex_unlock(&cml);
}

/* bl held: the call of plugs[pc.entry] is over.  WHY_HANGUP: told to the plugin (0: it said so itself, or it is gone) */
static void call_over(int why_hangup, const char *why)
{
    struct plug *p; struct call_stats st; int was = pc.state;
    if (pc.state == C_NONE || pc.entry < 0) return;
    p = &plugs[pc.entry];
    /* a ring that ended unanswered counts (declined, not answered in time, or the caller gave up: three in ten
     * minutes and the plugin rings no more, M4 of the first review) */
    if (was == C_RINGING && p->gen == pc.gen) { memmove(p->rings + 1, p->rings, sizeof p->rings[0] * (RINGS_MAX - 1)); p->rings[0] = now_ms(); }
    if (why_hangup && p->gen == pc.gen && running(p) && p->ctl >= 0) {
        uint8_t b[32]; struct pm_w w; struct pm_call_hangup m = { (uint8_t)why_hangup };
        pm_winit(&w, b, sizeof b); pm_put_call_hangup(&w, 0, &m); psend(p, &w, -1);
    }
    call_stats(&st);
    media_stop();
    if (was == C_LIVE)
        say("%s: call ended after %lld s (%s); %u frames in, %u played, %u of them missing, %u to it dropped", p->id, (now_ms() - pc.live) / 1000, why,
            st.rx, st.played, st.lost, atomic_load(&media_dropped));
    else say("%s: call %s (%s)", p->id, was == C_RINGING || was == C_ANSWERED ? "that rang ended" : "going out ended", why);
    tell(pc.entry, CALL_IDLE);
    pc.state = C_NONE; pc.entry = -1;
}

/* bl held: a token for an action of P's marked "calls" (the caller knows it came from a person: Home Assistant's
 * keyed link, an approved browser).  "" when none can be made */
static void token_new(struct plug *p, char out[33])
{
    uint8_t r[16]; int u = -1, k = 0; long long now = now_ms();
    out[0] = 0;
    if ((u = open("/dev/urandom", O_RDONLY | O_CLOEXEC)) < 0 || read(u, r, sizeof r) != (ssize_t)sizeof r) { if (u >= 0) close(u); return; }
    close(u);
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
    for (int i = 1; i < TOKENS; i++) if (!p->tok[i].at || p->tok[i].at < p->tok[k].at) k = i;      /* the oldest goes */
    snprintf(p->tok[k].t, sizeof p->tok[k].t, "%s", out); p->tok[k].at = now;
}

/* bl held: T one of P's tokens, fresh?  Spent here, whatever then becomes of what it was for: one use */
static int token_take(struct plug *p, const char *t)
{
    long long now = now_ms();
    if (strlen(t) != 32) return 0;
    for (int i = 0; i < TOKENS; i++) if (p->tok[i].at && !strcmp(p->tok[i].t, t)) {
        int ok = now - p->tok[i].at <= TOKEN_MS;
        p->tok[i].at = 0; memset(p->tok[i].t, 0, sizeof p->tok[i].t);
        return ok;
    }
    return 0;
}

static int rings_unanswered(const struct plug *p)
{
    int n = 0; long long now = now_ms();
    for (int i = 0; i < RINGS_MAX; i++) n += p->rings[i] && now - p->rings[i] < rings_window;
    return n;
}

/* bl held: a person answered the ringing call here */
static void answer_here(const char *how)
{
    struct plug *p = &plugs[pc.entry]; uint8_t b[16]; struct pm_w w;
    pc.state = C_ANSWERED; pc.answered = now_ms();
    tell(pc.entry, CALL_ANSWERING);     /* the ringtone stops at the press (C4 of a review of fc43c40: it rang on until
                                         * live, up to LIVE_WAIT_MS, and a second press, the person's "did it take?", hung up) */
    pm_winit(&w, b, sizeof b); pm_put_call_answer(&w, 0); psend(p, &w, -1);
    say("%s: the call answered here (%s)", p->id, how);
}

/* The call messages (CALL_*).  bl held on entry, let go here */
static void call_message(int i, struct plug *p, const struct pm_hdr *h, const uint8_t *body, size_t blen)
{
    long long now = now_ms(); int mine = pc.entry == i && pc.gen == p->gen;
    if (p->state != P_RUN) { if (h->id) send_err(p, h->id, PM_ERR_BAD); pthread_mutex_unlock(&bl); return; }
    if (!bucket_take(&p->r_call, CALL_PER_SEC, now)) { if (h->id) send_err(p, h->id, PM_ERR_BUSY); p->dropped++; pthread_mutex_unlock(&bl); return; }
    switch (h->type) {
    case PM_CALL_INCOMING: case PM_CALL_OUT: {
        char who[65] = "", tok[33] = ""; const char *no; int out = h->type == PM_CALL_OUT, err = 0;
        if (out) { struct pm_call_out m; pm_get_call_out(body, blen, &m); if (pm_cstr(tok, sizeof tok, m.token) || pm_cstr(who, sizeof who, m.who)) err = PM_ERR_BAD; }
        else { struct pm_call_incoming m; pm_get_call_incoming(body, blen, &m); if (pm_cstr(who, sizeof who, m.who)) err = PM_ERR_BAD; }
        if (!err && !shown_text(who)) err = PM_ERR_BAD;            /* (strict UTF-8: the screen and the log show it) */
        /* out: the token first and spent whatever follows (one use: a person asked for one call); in: the rings */
        if (!err && out && !token_take(p, tok)) { err = PM_ERR_DENIED; refusal(p, "a call going out without a token of a person's (an action marked \"calls\", within %d s, once): refused", TOKEN_MS / 1000); }
        no = core_call_refusal(!out);                               /* (plain reads: any lock) */
        int took = 0;
        if (err) ;
        /* a ring's look at the line costs one of BUSY_PER_SEC, given back below unless it was busy: asked faster, busy
         * without a look (a call going out spends its token first: one look per person's ask) */
        else if (!out && !(took = bucket_rate(&p->r_busy, BUSY_PER_SEC, 2 * BUSY_PER_SEC, now))) err = PM_ERR_BUSY;
        /* another call (Drop In's, another plugin's, this one's), or the claim not let go yet by the end of one */
        else if (pc.state != C_NONE || call_claimed() != CALL_NOBODY) err = PM_ERR_BUSY;
        else if (no) { err = PM_ERR_DENIED; refusal(p, "a call %s refused: %s", out ? "going out" : "coming in", no); }
        else if (!out && rings_unanswered(p) >= RINGS_MAX) { err = PM_ERR_LIMIT; refusal(p, "a call coming in refused: %d rings unanswered in the last %d s", RINGS_MAX, rings_window / 1000); }
        else if (call_claim(CALL_PLUGIN + i)) err = PM_ERR_BUSY;
        if (took && err != PM_ERR_BUSY) p->r_busy.tokens += 1;     /* (the line was free: that look cost nothing) */
        if (err) { send_err(p, h->id, err); if (err != PM_ERR_DENIED && err != PM_ERR_LIMIT) p->dropped++; break; }
        pc.state = out ? C_CALLING : C_RINGING; pc.entry = i; pc.gen = p->gen; pc.outgoing = out; pc.since = now; pc.media = -1;
        snprintf(pc.peer, sizeof pc.peer, "%s: %s", p->name[0] ? p->name : p->id, who[0] ? who : out ? "a call" : "a caller");
        tell(i, out ? CALL_CALLING : CALL_RINGING);
        say("%s: a call %s", p->id, out ? "goes out (its token: a person asked for it)" : "comes in: it rings");
        send_ok(p, h->id, -1);
        break; }
    case PM_CALL_RINGING_THERE:
        if (!mine || pc.state != C_CALLING) { if (h->id) send_err(p, h->id, PM_ERR_BAD); break; }
        say("%s: the call rings on the far side", p->id);
        answer(p, h->id, 0);
        break;
    case PM_CALL_LIVE: {
        struct pm_call_live m; char tok[33] = ""; int sv[2] = { -1, -1 }, gen; struct media_rd *rd; pthread_t t; int buf = MEDIA_SNDBUF;
        pm_get_call_live(body, blen, &m);
        if (pm_cstr(tok, sizeof tok, m.token) || !mine || (pc.state != C_RINGING && pc.state != C_ANSWERED && pc.state != C_CALLING)) { send_err(p, h->id, PM_ERR_BAD); break; }
        /* 3.6: a call coming in is live only once a person answered it here, at the Echo (the action button, the
         * screen: C_ANSWERED).  No token answers it, whatever brought one: a token is for a call going out (CALL_OUT),
         * and an automation in Home Assistant that answered the plugin's ring would be an automatic answer (C3 of a
         * review of fc43c40).  CALL_LIVE's token field is not looked at: the wire keeps it (call 1's layout, as
         * released), and it is read as every text field is (pm_cstr above: one that is no string is BAD, the same rule
         * for every message), its value never used (sdk/PROTOCOL.md says both) */
        if (pc.state == C_RINGING) {
            send_err(p, h->id, PM_ERR_DENIED);
            refusal(p, "a call that rings may not go live before a person answered it at the Echo: refused");
            break;
        }
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { send_err(p, h->id, PM_ERR_FAILED); call_over(PM_HANGUP_NOT_LIVE, "no socket for its audio"); break; }
        setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf); setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
        fcntl(sv[0], F_SETFL, O_NONBLOCK);
        pthread_mutex_lock(&cml); media_tag++; media_fd = sv[0]; atomic_store(&media_dropped, 0); unsigned tag = media_tag; pthread_mutex_unlock(&cml);
        gen = call_begin(CALL_PLUGIN + i, CALL_CODEC_PCM, call_send, (void *)(uintptr_t)tag);
        if (gen < 0 || !(rd = malloc(sizeof *rd))) {
            pthread_mutex_lock(&cml); media_fd = -1; pthread_mutex_unlock(&cml);
            if (gen >= 0) call_end(gen);
            close(sv[0]); close(sv[1]); send_err(p, h->id, PM_ERR_FAILED); call_over(PM_HANGUP_NOT_LIVE, "its audio could not begin"); break;
        }
        rd->fd = sv[0]; rd->gen = gen; rd->tag = tag;
        atomic_fetch_add(&media_readers, 1);
        if (pthread_create(&t, NULL, media_reader, rd)) {
            atomic_fetch_sub(&media_readers, 1); free(rd); pthread_mutex_lock(&cml); media_fd = -1; pthread_mutex_unlock(&cml);
            call_end(gen); close(sv[0]); close(sv[1]); send_err(p, h->id, PM_ERR_FAILED); call_over(PM_HANGUP_NOT_LIVE, "its audio could not begin"); break;
        }
        pthread_detach(t);
        say("%s: the call is live (%s)", p->id, pc.state == C_CALLING ? "the far side took it" : "answered here");
        pc.media = gen; pc.state = C_LIVE; pc.live = now;
        tell(i, CALL_LIVE);
        send_ok(p, h->id, sv[1]);
        close(sv[1]);
        break; }
    case PM_CALL_END: {
        struct pm_call_end m; static const char *const res[] = { "?", "hung up there", "busy there", "declined there", "not answered there", "failed" };
        pm_get_call_end(body, blen, &m);
        if (!mine) { send_err(p, h->id, PM_ERR_BAD); break; }
        call_over(0, res[m.result >= 1 && m.result <= 5 ? m.result : 0]);
        send_ok(p, h->id, -1);
        break; }
    }
    pthread_mutex_unlock(&bl);
}

/* bl held, the broker's round: what rings or waits too long */
static void call_timers(long long now)
{
    if (tq_in != tq_out) teller_start();    /* (states waiting and no teller: made again) */
    if (pc.state == C_RINGING && now - pc.since > ring_ms) call_over(PM_HANGUP_NOT_ANSWERED, "not answered");
    else if (pc.state == C_ANSWERED && now - pc.answered > live_wait_ms) call_over(PM_HANGUP_NOT_LIVE, "answered here, and the far side never came");
    else if (pc.state == C_CALLING && now - pc.since > calling_ms) call_over(PM_HANGUP_NOT_ANSWERED, "not answered there");
}

/* A person here (main.c, any lock but bl; core_lock may be held): the action button, the screen's Answer, a hang-up */
int plugin_call_button(void)
{
    int r = 1;
    pthread_mutex_lock(&bl);
    if (pc.state == C_RINGING) answer_here("the action button");
    else if (pc.state != C_NONE) call_over(PM_HANGUP_BUTTON, "the action button");
    else r = 0;
    pthread_mutex_unlock(&bl);
    return r;
}

int plugin_call_answer_by(const char *how)
{
    int r = 0;
    pthread_mutex_lock(&bl);
    if (pc.state == C_RINGING) { answer_here(how); r = 1; }
    pthread_mutex_unlock(&bl);
    return r;
}

int plugin_call_answer(void) { return plugin_call_answer_by("the screen"); }

int plugin_call_hangup(int why)
{
    int r = 0;
    pthread_mutex_lock(&bl);
    if (pc.state != C_NONE) { call_over(why, why == PM_HANGUP_SCREEN ? "the screen" : why == PM_HANGUP_SPOKEN ? "\"stop\" or \"hang up\"" : "hassmic"); r = 1; }
    pthread_mutex_unlock(&bl);
    return r;
}

/* ---------------------------------------------------------------- a request's copy (request.audio, request.text; 3.5, 3.6, 4.12)
 *
 * The core says where a request begins and ends and hands over each block of its audio as Home Assistant got it, all
 * with core_lock held (main.c mic_out: lead_in and send_out, after proto->audio).  A request is Home Assistant's; a
 * plugin with request.audio gets a read-only copy, one way: nothing it does can delay, change, end or start one.
 *
 * The audio: per run and plugin a SOCK_SEQPACKET pair, made as the run begins.  Each packet a u32 run, a u32 offset
 * (samples since the run's first), then the block; sent with MSG_DONTWAIT by whoever holds core_lock (the capture
 * thread for the most of it), with bl, which is held over nothing that waits: a full socket (64 kB, 2 s) drops the
 * block and counts it, and the next packet's offset shows the plugin the gap (issue #77: the capture thread waits for
 * nobody).  hassmic's end is closed as the run ends: the plugin reads what is in its socket and then its end.  A socket
 * per run rather than one for good (the design's first word: "the first time"): REQ_START goes through the broker's
 * queue, and the audio does not wait for it, so with one socket a run's first blocks were readable before the message
 * that says they begin; with a socket of its own the plugin cannot read them before it has REQ_START, and needs no rule
 * about which comes first.  These sockets are the core's: made, written and closed under bl by the holder of
 * core_lock, closed by the broker (under bl) only when the plugin's process is gone or stopped; never polled.
 *
 * REQ_START (with the plugin's end of the pair), REQ_END and REQ_TEXT go into a queue for the broker thread, woken by
 * a byte into a pipe (bwake): the core's lock is held over no send of a message (4.12: "none sent from under the lock
 * but the audio"), and the order of one plugin's is kept.  A run is given only to a plugin that runs (READY) when it
 * begins: nothing of a run arrives before its REQ_START, nothing after its REQ_END, nothing outside a run.
 *
 * With the wake word in Home Assistant (-w remote) hassmic streams all the time; the core begins a run only at Home
 * Assistant's wake word (core_remote_wake) and ends it at the microphone off (3.6): the core's flag, set and cleared
 * where listening mode is */
_Static_assert((int)PLUGIN_REQ_WAKE_WORD == (int)PM_REQSRC_WAKE_WORD && (int)PLUGIN_REQ_BUTTON == (int)PM_REQSRC_BUTTON && (int)PLUGIN_REQ_FOLLOW_UP == (int)PM_REQSRC_FOLLOW_UP
               && (int)PLUGIN_REQ_HA_WAKE == (int)PM_REQSRC_HA_WAKE, "plugin.h's sources are the protocol's");
_Static_assert((int)PLUGIN_REQEND_HA == (int)PM_REQEND_HA && (int)PLUGIN_REQEND_HEARD_HERE == (int)PM_REQEND_HEARD_HERE && (int)PLUGIN_REQEND_CANCELLED == (int)PM_REQEND_CANCELLED
               && (int)PLUGIN_REQEND_TIMEOUT == (int)PM_REQEND_TIMEOUT && (int)PLUGIN_REQEND_ERROR == (int)PM_REQEND_ERROR && (int)PLUGIN_REQEND_LINK_LOST == (int)PM_REQEND_LINK_LOST
               && (int)PLUGIN_REQEND_MUTED == (int)PM_REQEND_MUTED, "plugin.h's ends are the protocol's");
_Static_assert((int)PLUGIN_TEXT_HEARD == (int)PM_REQTEXT_HEARD && (int)PLUGIN_TEXT_REPLY == (int)PM_REQTEXT_REPLY, "plugin.h's texts are the protocol's");
/* The request socket's send buffer.  A request's start comes at once, under core_lock, before the plugin can read any
 * of it (its REQ_START goes through the broker's queue first): the lead-in's 0.8 s (main.c LEAD_IN_MS) and the ring's
 * replay from the wake word on (stream_ring), which is at most the ring's 4 s (RING_SAMPLES: older is gone); in
 * practice the word (with wake_word_in_request from 100 ms ahead of it; Pryon's up to ~1.5 s), Pryon's lag behind its
 * end (116 to 128 ms), the arbitration round (up to 1000 ms) and the aligner's wait (up to 1.5 s past Pryon's end,
 * ALIGN_LATE).  So 4.8 s at the most, all of it before the first read.  The kernel takes SO_SNDBUF up to
 * net.core.wmem_max and doubles it for its bookkeeping, and a SOCK_SEQPACKET packet of 1024 samples costs ~4.3 kB of
 * that (the skb around its 2056 bytes; measured: 31 packets in the 131072 that 65536 gave, 98 in 425984).  212992 is
 * the default wmem_max of 64-bit kernels (163840 on 32-bit ones; athena's init.rc raises it to 8 MB): 98 packets, 6.3 s,
 * the burst and a second and a half for the plugin to begin reading.  Where wmem_max is lower the kernel gives less,
 * and plugin_request_start says so once.  Until a review of b294fa0 (M1) 65536, 2 s: a plugin that read at once still
 * lost the wake word and the command's start of a long replay (0.35 s of lead-in + 1.5 s, 0.85 s of lead-in + 2 s).
 * A plugin that falls behind loses blocks, never holds one up */
#define REQ_SNDBUF        212992
#define REQ_BURST_PACKETS 75            /* 4.8 s in packets of 1024 samples: with less room the burst does not fit */

static void bwake_poke(void)            /* any thread: the broker looks at its queue now (a full pipe: it will anyway) */
{
    if (bwake[1] >= 0 && write(bwake[1], "", 1) < 0) { }
}

/* bl held: one message for entry I's process, in order behind the others.  FD: a descriptor that goes with it (the
 * plugin's end of a run's socket), the queue's to close.  Full (the broker stuck for RQ_MAX messages: not in a run of
 * the core's pace) it is dropped and counted, its descriptor closed: never shared, nobody polls it */
static int rq_push(int i, uint16_t type, uint32_t run, uint8_t a, uint8_t b, uint32_t c, uint32_t d, int fd, const char *text)
{
    struct rq *e;
    if (rq_in - rq_out >= RQ_MAX) { if (fd >= 0) close(fd); rq_lost_n++; return -1; }
    e = &rq[rq_in++ % RQ_MAX];
    e->entry = i; e->gen = plugs[i].gen; e->type = type; e->run = run; e->a = a; e->b = b; e->c = c; e->d = d; e->fd = fd;
    /* (cut on a whole character to what the message holds: REQ_START's phrase 64 bytes, REQ_TEXT's text 1024) */
    if (text) { size_t n = strlen(text), k = (size_t)ps_cut(text, n, type == PM_REQ_START ? 64 : sizeof e->text - 1); memcpy(e->text, text, k); e->text[k] = 0; } else e->text[0] = 0;
    return 0;
}

/* bl held: P's copy of the run ends here (the run's end, the plugin stopped or gone): hassmic's end closed, what is
 * in the socket stays readable for the plugin.  WHY > 0: REQ_END told, with what the run had */
static void rq_close(struct plug *p, int why)
{
    if (!p->rq_open) return;
    close(p->rq_fd); p->rq_fd = -1; p->rq_open = 0;
    p->rq_lost += p->rq_dropped;
    if (why > 0) rq_push((int)(p - plugs), PM_REQ_END, p->rq_run, (uint8_t)why, 0, p->rq_off, p->rq_dropped, -1, NULL);
    if (p->rq_dropped) say("%s: %u of the request's %u samples not taken (it did not read in time)", p->id, p->rq_dropped, p->rq_off);
}

void plugin_request_start(int source, int slot, const char *phrase)
{
    int any = 0;
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) rq_close(&plugs[i], PM_REQEND_CANCELLED);     /* (a run the core did not end: never, but none overlaps) */
    if (!++req_run) req_run = 1;
    req_live = 1; req_texts = 1;
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i]; int sv[2], size = REQ_SNDBUF;
        if (p->state != P_RUN || !(p->caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO))) continue;
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { say("%s: no socket for the request's copy (%s)", p->id, strerror(errno)); continue; }
        setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size); fcntl(sv[0], F_SETFL, O_NONBLOCK);
        { static int told; int got = 0; socklen_t gl = sizeof got;
          if (!told && !getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &got, &gl) && got < REQ_BURST_PACKETS * 4400) {
              told = 1; say("a request's copy has %d kB here (net.core.wmem_max): the start of a long replay may not fit before the plugin reads", got / 1024); } }
        if (rq_push(i, PM_REQ_START, req_run, (uint8_t)source, (uint8_t)slot, 0, 0, sv[1], phrase)) { close(sv[0]); continue; }
        p->rq_fd = sv[0]; p->rq_open = 1; p->rq_run = req_run; p->rq_off = p->rq_dropped = 0; p->rq_runs++; p->rq_last = now_ms();
        any = 1;
    }
    pthread_mutex_unlock(&bl);
    if (any) bwake_poke();
}

void plugin_request_audio(const void *pcm, size_t len)
{
    const uint8_t *d = pcm; size_t n = len / 2;
    pthread_mutex_lock(&bl);
    while (n) {
        size_t k = n < PM_REQ_SAMPLES_MAX ? n : PM_REQ_SAMPLES_MAX;
        for (int i = 0; i < NPLUG; i++) {
            struct plug *p = &plugs[i]; uint8_t head[8]; struct iovec iov[2] = { { head, 8 }, { (void *)d, k * 2 } }; struct msghdr h = { .msg_iov = iov, .msg_iovlen = 2 };
            if (!p->rq_open) continue;
            for (int b = 0; b < 4; b++) { head[b] = (uint8_t)(p->rq_run >> 8 * b); head[4 + b] = (uint8_t)(p->rq_off >> 8 * b); }
            if (sendmsg(p->rq_fd, &h, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)(8 + k * 2)) p->rq_dropped += (uint32_t)k;
            p->rq_off += (uint32_t)k;
        }
        d += k * 2; n -= k;
    }
    pthread_mutex_unlock(&bl);
}

void plugin_request_end(int why)
{
    int any = 0;
    pthread_mutex_lock(&bl);
    req_live = 0;
    for (int i = 0; i < NPLUG; i++) { any |= plugs[i].rq_open; rq_close(&plugs[i], why); }
    pthread_mutex_unlock(&bl);
    if (any) bwake_poke();
}

void plugin_request_none(void) { pthread_mutex_lock(&bl); req_texts = 0; pthread_mutex_unlock(&bl); }

/* request.text: to every plugin that runs and has it, with the number of the request now or just ended (a reply comes
 * after the microphone went off).  Only while that is the run Home Assistant talks of (req_texts): with -w remote a
 * run streams for its wake word before any request is open, and a text of it carried the last request's number (a
 * review of b294fa0, M3: Wyoming's transcript, before its detection opened a request).  Not as a log line of the
 * broker's: it is what was said (log_private's kind) */
void plugin_request_text(int kind, const char *text)
{
    int any = 0;
    if (!text || !text[0]) return;
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i];
        if (p->state != P_RUN || !(p->caps & PM_CAPBIT(PM_CAP_REQUEST_TEXT)) || !req_run || !req_texts) continue;
        if (!rq_push(i, PM_REQ_TEXT, req_run, (uint8_t)kind, 0, 0, 0, -1, text)) any = 1;
    }
    pthread_mutex_unlock(&bl);
    if (any) bwake_poke();
}

/* The broker's thread (and timers(): the tests' rounds), no lock held: what the core queued, sent in order to the
 * process it was meant for (another process of the entry since: dropped, its descriptor closed).  A send that does
 * not fit is lost for the plugin: a REQ_START so lost ends that run's copy for it (nothing reads the socket) */
static void rq_drain(void)
{
    static uint8_t b[PM_MAX_PACKET]; char drain[64];
    if (bwake[0] >= 0) while (read(bwake[0], drain, sizeof drain) > 0) { }
    pthread_mutex_lock(&bl);
    if (rq_lost_n) { say("plugins: %u messages about requests not sent (the broker fell behind)", rq_lost_n); rq_lost_n = 0; }
    while (rq_out != rq_in) {
        struct rq *e = &rq[rq_out++ % RQ_MAX]; struct plug *p = &plugs[e->entry]; struct pm_w w; int ok = p->gen == e->gen && p->state == P_RUN;
        pm_winit(&w, b, sizeof b);
        if (e->type == PM_REQ_START) { struct pm_req_start m = { e->run, e->a, e->b, pm_s(e->text) }; pm_put_req_start(&w, 0, &m); }
        else if (e->type == PM_REQ_END) { struct pm_req_end m = { e->run, e->a, e->c, e->d }; pm_put_req_end(&w, 0, &m); }
        else { struct pm_req_text m = { e->run, e->a, pm_s(e->text) }; pm_put_req_text(&w, 0, &m); }
        if (ok && psend(p, &w, e->fd)) {
            say("%s: %s not sent (its control socket is full)", p->id, e->type == PM_REQ_START ? "a request's start" : e->type == PM_REQ_END ? "a request's end" : "a request's text");
            if (e->type == PM_REQ_START && p->rq_open && p->rq_run == e->run) rq_close(p, 0);
        }
        if (e->fd >= 0) close(e->fd);
        memset(e->text, 0, sizeof e->text);
    }
    pthread_mutex_unlock(&bl);
}

/* No lock held (the broker's round): the partner rule (3.3; plugin_store.c ps_partner) after the engine changed.  A
 * plugin with request.audio that runs and may not any more is stopped, one that was held back for it and may now is
 * started (start() decides, as for any) */
static void partner_check(void)
{
    char ids[NPLUG][PS_ID_MAX]; int n = 0;
    pthread_mutex_lock(&bl);
    if (!partner_due) { pthread_mutex_unlock(&bl); return; }
    partner_due = 0;
    for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && (running(&plugs[i]) ? (plugs[i].caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO)) != 0 : plugs[i].partner)) snprintf(ids[n++], PS_ID_MAX, "%s", plugs[i].id);
    pthread_mutex_unlock(&bl);
    for (int k = 0; k < n; k++) {
        const struct ps_plugin *sp; char why[300] = ""; int bad = 0; struct plug *p;
        ps_lock(); if ((sp = ps_find(ids[k]))) bad = ps_partner(&sp->m, sp->tier, 0, why, sizeof why) != 0; ps_unlock();
        pthread_mutex_lock(&bl);
        if (bad && (p = plug_of(ids[k])) && running(p)) { p->want = 0; p->partner = 1; stop(p, why); say("%s: stopped: %s", p->id, why); }
        pthread_mutex_unlock(&bl);
        if (!bad) start(ids[k]);
    }
}

/* ---------------------------------------------------------------- the log (4.5) */

/* the wake word plugins' ids, whose lines go to no plugin (3.3): asked of the store once a round */
struct sealed_ids { char id[PS_MAX][PS_ID_MAX]; int n; };
static int sealed_is(const char *id, size_t len, void *arg)
{
    const struct sealed_ids *s = arg;
    for (int i = 0; i < s->n; i++) if (strlen(s->id[i]) == len && !memcmp(s->id[i], id, len)) return 1;
    return 0;
}

/* The broker's thread, no lock: each plugin that follows the log gets what was written since, once a second, as one
 * LOG of whole lines.  The file is read without bl (the capture thread takes it); what is sent is decided under it.
 * A send that does not fit (the plugin does not read) moves nothing: the next round sends the same from the same
 * place, so a plugin that falls behind loses no line the log still has */
static void log_pump(void)
{
    static char lines[LOG_LINES][1024]; static uint8_t out[PM_MAX_PACKET];
    struct sealed_ids sealed; const struct ps_plugin *list; long long now = now_ms(); int np, any = 0;
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) any |= plugs[i].state == P_RUN && plugs[i].log_on && !plugs[i].net_pending && !plugs[i].log_wait && now >= plugs[i].log_next;
    pthread_mutex_unlock(&bl);
    if (!any) return;
    sealed.n = 0;
    ps_lock(); np = ps_list(&list);
    for (int i = 0; i < np && sealed.n < PS_MAX; i++) if (list[i].m.caps & PM_CAPBIT(PM_CAP_WAKE)) snprintf(sealed.id[sealed.n++], PS_ID_MAX, "%s", list[i].m.id);
    ps_unlock();
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i]; struct logpos at, next; int n, gap, full; unsigned gen; char cur[41];
        pthread_mutex_lock(&bl);
        /* Not while a CONNECT of its waits for its answer: the client keeps what comes meanwhile (hmplugin.c answer()),
         * and a connect takes up to 10 s, at LOG's pace 200 messages; held back here, nothing is lost and the client's
         * keeping stays small (a review, 2026-11-21: it dropped all past 32, and the cursor moved past them) */
        /* And one LOG at a time: the next once the client answered OK to this one, after its log callback returned.
         * Sent as fast as they were read, a callback that waited on something (a round trip of its own each time)
         * had them pile up in the client, which keeps what comes while a request waits: past its 2 MB it ended,
         * counted as a crash, and five of those switched it off (R9 of a review, 2026-11-21) */
        if (p->state != P_RUN || !p->log_on || p->net_pending || p->log_wait || now < p->log_next) { pthread_mutex_unlock(&bl); continue; }
        /* (log.read.full: the page's view, official plugins only, which the store holds at install and at every start;
         * a plugin with both gets the wider) */
        at = p->log_at; gen = p->gen; full = (p->caps & PM_CAPBIT(PM_CAP_LOG_READ_FULL)) != 0;
        pthread_mutex_unlock(&bl);
        n = logread_lines(&at, lines, LOG_LINES, LOG_BUDGET, &next, &gap, full, sealed_is, &sealed);
        pthread_mutex_lock(&bl);
        if (p->state == P_RUN && p->log_on && p->gen == gen && !p->log_wait && !memcmp(&p->log_at, &at, sizeof at)) {
            int sent = 1;
            if (n || gap) {
                struct pm_w w; struct pm_log m; uint32_t lid = ++p->next_id ? p->next_id : ++p->next_id;
                logread_cursor(&next, cur, sizeof cur);
                m.cursor = pm_s(cur); m.gap = (uint8_t)gap; m.lines.n = (uint16_t)n;
                pm_winit(&w, out, sizeof out); pm_put_log(&w, lid, &m);
                for (int k = 0; k < n; k++) { struct pm_log_lines e = { pm_s(lines[k]) }; pm_add_log_lines(&w, &e); }
                sent = !psend(p, &w, -1);
                if (sent) { p->log_lines += n; p->log_wait = lid; p->log_sent = btime; }
            }
            if (sent) p->log_at = next;
            /* it moved on (lines sent, or read and left out): there may be more at once; else in a second */
            p->log_next = now + (sent && memcmp(&next, &at, sizeof at) ? 50 : LOG_EVERY_MS);
        }
        pthread_mutex_unlock(&bl);
    }
}

/* a plugin's stderr into the log, as "plugin <id>: ...", 50 lines a minute, the rest counted */
static void log_lines(struct plug *p, const char *d, size_t n)     /* bl held */
{
    long long now = now_ms();
    for (size_t i = 0; i < n; i++) {
        if (d[i] != '\n' && p->linen < sizeof p->line - 1) { p->line[p->linen++] = (unsigned char)d[i] < 0x20 ? ' ' : d[i]; continue; }
        if (d[i] != '\n') continue;
        p->line[p->linen] = 0; p->linen = 0;
        /* whatever it wrote, one line of text in the log: the page's log viewer and every log.read plugin read it,
         * and a line cut at the buffer's end may end inside a character (ps_utf8_mend: "?" for what is none) */
        ps_utf8_mend(p->line);
        /* a wake word plugin's lines sealed where they are written (seal_note) */
        /* ... and one that hears requests (request.audio, request.text) or a call's room (call) writes private lines:
         * what it was told may be in them, what was said and answered, and no plugin with log.read gets that (4.5;
         * log_private's mark) */
        const char *mark = p->caps & PM_CAPBIT(PM_CAP_WAKE) ? LOG_SEALED_MARK : p->caps & PRIVATE_CAPS ? LOG_PRIVATE_MARK : NULL;
        if (now - p->line_min >= 60000) {
            if (p->lines_lost) log_marked_f(mark, "plugin %s: (%d more lines in that minute, not kept)", p->id, p->lines_lost);
            p->line_min = now; p->lines = p->lines_lost = 0;
        }
        if (++p->lines > lines_min) p->lines_lost++;
        else log_marked_f(mark, "plugin %s: %s", p->id, p->line);
    }
}

/* ---------------------------------------------------------------- file checks */

/* bl held: ask the plugin about an item.  staged: the upload still aside */
static int check_ask(struct plug *p, const char *list, const char *item, int staged)
{
    uint8_t b[128]; struct pm_w w; struct pm_file_check m = { pm_s(list), pm_s(item) };
    if (p->state != P_RUN || p->check.active || !(p->caps & PM_CAPBIT(PM_CAP_FILES))) return -1;
    memset(&p->check, 0, sizeof p->check);
    p->check.active = 1; p->check.staged = staged; p->check.id = ++p->next_id ? p->next_id : ++p->next_id; p->check.at = now_ms();
    snprintf(p->check.list, sizeof p->check.list, "%s", list); snprintf(p->check.item, sizeof p->check.item, "%s", item);
    pm_winit(&w, b, sizeof b); pm_put_file_check(&w, p->check.id, &m);
    if (psend(p, &w, -1)) { p->check.active = 0; return -1; }
    return 0;
}

/* no lock: what a finished check found, into the store.  1 kept as good, 0 bad */
static int check_apply(const char *id, const char *list, const char *item, int ok, const char *title, const void *fields, int nfields)
{
    const struct { char name[33], value[257]; } *f = fields; const struct ps_plugin *sp; char err[120];
    if (!ok) return 0;
    ps_lock();
    if ((sp = ps_find(id))) {
        ps_item_checked(sp, list, item, title);
        for (int i = 0; i < nfields; i++) ps_item_field(sp, list, item, f[i].name, f[i].value, 1, err, sizeof err);    /* the owner's own values stay */
    }
    ps_unlock();
    return 1;
}

static int item_uploaded(const char *id, const char *list, const char *item, int must_check, char *err, size_t errsz)
{
    const struct ps_plugin *sp; struct plug *p; int r = 2, asked = 0, ok = 0, nfields = 0; char title[65] = ""; struct timespec until;
    static struct { char name[33], value[257]; } fields[PS_FIELDS]; static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&one);
    clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += CHECK_MS / 1000 + 1;
    pthread_mutex_lock(&bl);
    /* a check of the broker's own under way: its answer first */
    while ((p = plug_of(id)) && p->state == P_RUN && p->check.active && !pthread_cond_timedwait(&bcv, &bl, &until)) ;
    if ((p = plug_of(id)) && !check_ask(p, list, item, 1)) {
        asked = 1;
        clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += CHECK_MS / 1000;
        while ((p = plug_of(id)) && p->check.active && !p->check.done && !pthread_cond_timedwait(&bcv, &bl, &until)) ;
        if (p && p->check.active) {
            ok = p->check.done ? p->check.ok : -2;
            snprintf(title, sizeof title, "%s", p->check.title); nfields = p->check.nfields; memcpy(fields, p->check.fields, sizeof fields);
            if (ok == 0) snprintf(err, errsz, "%s", p->check.why);
            if (ok == -2) { snprintf(err, errsz, "the plugin did not answer in %d s", CHECK_MS / 1000); stop(p, "it did not answer a file check"); }
            p->check.active = 0;
        }
    }
    pthread_mutex_unlock(&bl);
    ps_lock();
    if (!(sp = ps_find(id))) { snprintf(err, errsz, "no such plugin"); r = 0; }
    else if (asked && ok == 0) { ps_item_drop(sp, list, item); r = 0; }
    else if (asked && ok == -2) { ps_item_drop(sp, list, item); r = 0; }
    else if (!asked && must_check) { ps_item_drop(sp, list, item); snprintf(err, errsz, "the plugin did not look at it: it does not run now"); r = 0; }
    else if (ps_item_keep(sp, list, item, err, errsz)) { ps_item_drop(sp, list, item); r = 0; }
    else if (asked && ok == 1) r = check_apply(id, list, item, 1, title, fields, nfields);
    ps_unlock();
    pthread_mutex_unlock(&one);
    if (r) plugin_files_changed(id, list);
    return r;
}
int plugin_item_uploaded(const char *id, const char *list, const char *item, char *err, size_t errsz) { return item_uploaded(id, list, item, 0, err, errsz); }
int plugin_item_checked_only(const char *id, const char *list, const char *item, char *err, size_t errsz) { return item_uploaded(id, list, item, 1, err, errsz); }

/* the broker thread, no lock: an item kept while the plugin was down gets its check now */
static void look_unchecked(int i)
{
    struct plug *p = &plugs[i]; char id[PS_ID_MAX], list[33] = "", item[41] = ""; const struct ps_plugin *sp; struct ps_item *items;
    pthread_mutex_lock(&bl);
    if (p->state != P_RUN || !p->look || p->check.active) { pthread_mutex_unlock(&bl); return; }
    p->look = 0; snprintf(id, sizeof id, "%s", p->id);
    pthread_mutex_unlock(&bl);
    if (!(items = malloc(sizeof *items * PS_ITEMS))) return;
    ps_lock();
    if ((sp = ps_find(id)))
        for (int s = 0; s < sp->m.nsettings && !item[0]; s++) {
            if (sp->m.settings[s].type != PS_FILES) continue;
            for (int k = 0, n = ps_items(sp, sp->m.settings[s].name, items, PS_ITEMS); k < n && !item[0]; k++) {
                int bad = items[k].checked;
                pthread_mutex_lock(&bl); for (int b = 0; b < p->nbad; b++) bad |= !strcmp(p->bad[b], items[k].id); pthread_mutex_unlock(&bl);
                if (!bad) { snprintf(list, sizeof list, "%s", sp->m.settings[s].name); snprintf(item, sizeof item, "%s", items[k].id); }
            }
        }
    ps_unlock();
    free(items);
    if (!item[0]) return;
    pthread_mutex_lock(&bl);
    if (p->state == P_RUN && !strcmp(p->id, id)) check_ask(p, list, item, 0);
    pthread_mutex_unlock(&bl);
}

/* the broker thread, no lock: a check of its own that ended */
static void check_done(int i)
{
    struct plug *p = &plugs[i]; char id[PS_ID_MAX], list[33], item[41], title[65]; int ok, nfields; static struct { char name[33], value[257]; } fields[PS_FIELDS];
    pthread_mutex_lock(&bl);
    if (!p->check.active || p->check.staged || (!p->check.done && now_ms() - p->check.at < CHECK_MS)) { pthread_mutex_unlock(&bl); return; }
    ok = p->check.done ? p->check.ok : 0;
    snprintf(id, sizeof id, "%s", p->id); snprintf(list, sizeof list, "%s", p->check.list); snprintf(item, sizeof item, "%s", p->check.item);
    snprintf(title, sizeof title, "%s", p->check.title); nfields = p->check.nfields; memcpy(fields, p->check.fields, sizeof fields);
    if (ok != 1 && p->nbad < 8) { snprintf(p->bad[p->nbad++], sizeof p->bad[0], "%s", item); say("%s: %s cannot be used: %s", id, item, p->check.done ? p->check.why : "no answer"); }
    p->check.active = 0; p->look = 1;
    pthread_cond_broadcast(&bcv);
    pthread_mutex_unlock(&bl);
    if (ok == 1 && check_apply(id, list, item, 1, title, fields, nfields)) plugin_files_changed(id, list);
}

/* ---------------------------------------------------------------- the broker thread */

static void cpu_used(struct plug *p, const struct plugrun_msg *m);

static void launcher_message(const struct plugrun_msg *m)
{
    struct plug *p; char how[100]; int wake = 0; char id[PS_ID_MAX];
    if (m->slot >= PLUGRUN_SLOTS) return;
    pthread_mutex_lock(&bl);
    p = NULL;
    for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && plugs[i].slot == (int)m->slot) p = &plugs[i];
    if (!p) { pthread_mutex_unlock(&bl); return; }
    snprintf(id, sizeof id, "%s", p->id);
    if (m->type == PLUGRUN_SPAWNED && p->state == P_SPAWNING) { p->pid = (pid_t)m->a; p->state = P_HELLO; p->since = now_ms(); }
    else if (m->type == PLUGRUN_USED) cpu_used(p, m);
    else if (m->type == PLUGRUN_FAILED && p->state != P_FREE) {
        snprintf(how, sizeof how, "the launcher could not start it (step %u: %s)", m->b, strerror((int)m->a));
        if (!m->b && m->a == EBUSY) { pthread_mutex_unlock(&bl); return; }                  /* the slot's old process is still going: its EXITED follows */
        if (m->b == 0) exited(p, how, 1);                                                     /* no child: no EXITED will come */
        else snprintf(p->why, sizeof p->why, "%s", how);
    } else if (m->type == PLUGRUN_EXITED && p->state != P_FREE && p->state != P_DEAD) {
        int asked = p->state == P_STOPPING, st = (int)m->a;
        if (WIFEXITED(st)) snprintf(how, sizeof how, "ended with status %d", WEXITSTATUS(st));
        /* the development flag (SIGSYS for a call the filter refuses): the SDK's client names the call in its last line */
        else if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS && p->develop) snprintf(how, sizeof how, "the sandbox stopped it at a call it may not make (development flag: its last line says which)");
        else snprintf(how, sizeof how, "killed by signal %d", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        if (p->why[0] && (asked || p->state == P_SPAWNING)) snprintf(how, sizeof how, "%.99s", p->why);     /* why it was stopped, or never started */
        else if (p->lost && p->killed) snprintf(how, sizeof how, "it closed its control socket");
        if (p->lost) asked = 0;                             /* its own doing, whatever state that left it in: counted */
        wake = !strcmp(p->id, wake_id) && !ending;          /* (as hassmic ends the core has nothing to do with it) */
        say("%s: gone (%s; %u ms of CPU, %u kB at most)", p->id, how, m->b + m->c, m->d);
        /* (stopped for the wake engine's place, p->waits: no crash.  Counted as one, a healthy plugin was switched off
         * after five while the engine crashed and took its place each time; P1 of a review of 7be9343) */
        exited(p, how, !asked || (p->want && p->why[0] && !p->waits));
        if (asked && !p->want && !p->why[0]) snprintf(p->why, sizeof p->why, "stopped");
        if (asked && p->want && !p->next_start) p->next_start = now_ms() + 100;             /* stopped to be started again */
    }
    pthread_mutex_unlock(&bl);
    if (wake) core_wake_plugin_changed(id);
}

static void timers(void)
{
    long long now = now_ms(); char begin[NPLUG][PS_ID_MAX], changed[NPLUG + 1][PS_ID_MAX]; int nbegin = 0, nchanged = 0, whose[NPLUG + 1];
    pthread_mutex_lock(&bl);
    /* the broker's clock: on by what passed since the last round, and by no more than a round can take */
    { long long d = blast ? now - blast : 0; btime += d > STALL_MS ? STALL_MS : d < 0 ? 0 : d; blast = now; }
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i];
        if ((p->state == P_HELLO || p->state == P_CONFIG) && now - p->since > HELLO_MS) stop(p, p->state == P_HELLO ? "it did not say HELLO" : "it did not say READY");
        if (p->state == P_SPAWNING && now - p->since > HELLO_MS * 2) exited(p, "the launcher did not answer", 1);
        /* a PING every 10 s of the broker's own time: three in a row that it had the time to send and to hear the
         * answer of.  On the wall clock a broker that waited 30 s for core_lock would find three unanswered at once */
        if (p->state == P_RUN && btime >= p->ping_at) {
            uint8_t b[16]; struct pm_w w;
            if (p->pings >= PINGS_MAX) stop(p, "it stopped answering");
            else { pm_winit(&w, b, sizeof b); p->ping_id = ++p->next_id ? p->next_id : ++p->next_id; pm_put_ping(&w, p->ping_id); psend(p, &w, -1); p->pings++; p->ping_at = btime + PING_MS; }
        }
        /* The control socket's queue full (a send came back EAGAIN): is it still?  Asked with the smallest message
         * there is, a PING nobody waits for (id 0 is no ping's: its PONG matches none).  One that fits ends it; none
         * in 5 s is a plugin that does not read, and what hassmic tells it (its settings, STOP) is lost on it */
        if (running(p) && p->full_since) {
            uint8_t b[16]; struct pm_w w; long n;
            pm_winit(&w, b, sizeof b); pm_put_ping(&w, 0); n = pm_wlen(&w);
            if (p->ctl < 0 || send(p->ctl, b, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL) == n) p->full_since = 0;
            else if (btime - p->full_since >= QUEUE_MS) { stop(p, "it does not read what hassmic sends it"); say("%s: %s (its queue full for %d s): ended", p->id, p->why, QUEUE_MS / 1000); }
        }
        /* A client waiting on its CONNECT (up to PNET_DEADLINE_MS, as long as LOG_ANSWER_MS) answers the LOG only after
         * it: that time is not the LOG's, or a correct forwarder whose server drops packets was called one from before
         * flow control (a review of 655efd7) */
        if (p->log_wait && p->net_pending) p->log_sent = btime;
        if (p->state == P_RUN && p->log_wait && !p->log_deaf && btime - p->log_sent >= LOG_ANSWER_MS) {
            p->log_deaf = 1;
            say("%s: does not answer LOG (no OK in %d s): built on an SDK before flow control? No more of the log goes to it until it answers (sdk/PROTOCOL.md \"The log\")", p->id, LOG_ANSWER_MS / 1000);
        }
        if (p->button_n && now - p->button_at >= REFUSAL_MS) { say("%s: %d more %s of the buttons not passed on in the last %d s (%d a second at most)", p->id, p->button_n, p->button_n == 1 ? "press" : "presses", REFUSAL_MS / 1000, BUTTON_PER_SEC); p->button_n = 0; p->button_at = now; }
        if (p->refused_n && now - p->refused_at >= REFUSAL_MS) { say("%s: %d more requests refused in the last %d s (said once in %d s)", p->id, p->refused_n, REFUSAL_MS / 1000, REFUSAL_MS / 1000); p->refused_n = 0; p->refused_at = now; }
        if (p->state == P_STOPPING && now >= p->stop_at && p->stop_at) { if (p->slot >= 0) launcher_say(PLUGRUN_KILL, (uint32_t)p->slot, 0, 0, 0, 0, NULL, 0); p->stop_at = 0; p->killed = 1; }
        for (int s = 0; s < WAKE_SLOTS; s++) if (p->state == P_RUN && p->ws[s].state == W_OPENING && now - p->ws[s].asked > WAKE_OPEN_MS) {
            char word[41]; snprintf(word, sizeof word, "%s", p->ws[s].word);
            p->ws[s].state = W_FAILED; snprintf(p->why, sizeof p->why, "it did not answer for %s", word);
            say("%s: %s", p->id, p->why); p->words_dirty = 1;
            pthread_cond_broadcast(&bcv);
        }
        if (p->state != P_FREE && p->words_dirty && now - p->words_told >= 2000) { p->words_dirty = 0; p->words_told = now; whose[nchanged] = i; snprintf(changed[nchanged++], PS_ID_MAX, "%s", p->id); }
        if (p->state == P_DEAD && p->want && p->next_start && now >= p->next_start && (go || !strcmp(p->id, wake_id))) { p->next_start = 0; snprintf(begin[nbegin++], PS_ID_MAX, "%s", p->id); }
    }
    /* a place free: those that wait for one, the oldest first, each as it fits (start() decides again).  One that
     * does not fit the memory left lets a smaller one that waited less go first: a place is not kept empty for it */
    if (go) {
        int used, mem; char taken[NPLUG] = { 0 };
        places(NULL, &used, &mem);
        while (used < RUN_MAX && nbegin < NPLUG) {
            struct plug *w = NULL;
            for (int i = 0; i < NPLUG; i++) {
                struct plug *q = &plugs[i];
                if (q->state == P_DEAD && q->want && q->waits && !taken[i] && mem + q->mem <= run_mem && (!w || q->wait_since < w->wait_since)) w = q;
            }
            if (!w) break;
            taken[w - plugs] = 1; used++; mem += w->mem;
            snprintf(begin[nbegin++], PS_ID_MAX, "%s", w->id);
        }
    }
    call_timers(now);
    pthread_mutex_unlock(&bl);
    rq_drain();
    for (int i = 0; i < nbegin; i++) start(begin[i]);
    partner_check();
    /* the core lists that plugin's words: its report, as a detection is (core_note) */
    for (int i = 0; i < nchanged; i++) { in_core = whose[i]; core_wake_plugin_changed(changed[i]); in_core = -1; }
}

/* The broker's thread: asks the launcher for the CPU time of each ordinary plugin, once a window.  Not the wake
 * engine's: it has a group of its own, works all the time, and is never ended for working.  hassmic cannot read the
 * time itself: a plugin is another user's process, and Android mounts /proc so that only root and group readproc
 * see those (hidepid=2; hassmic's groups have no readproc, and it is not a group to give a daemon that parses what
 * arrives from the network).  The launcher is root and the plugins' parent */
static void cpu_watch(void)
{
    long long now = now_ms();
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i];
        if (!running(p) || p->state == P_SPAWNING || p->pid <= 0 || p->slot < 0) continue;
        if (!strcmp(p->id, wake_id)) { p->cpu_have = 0; p->cpu_asked = p->cpu_next = 0; continue; }
        if (p->cpu_asked ? now - p->cpu_asked < CPU_ANSWER_MS : now < p->cpu_next) continue;
        if (launcher_ver < 2) {
            /* a launcher from before PLUGRUN_USAGE ends itself over a message it does not know, and every plugin
             * with it: never asked.  (They come in one bundle; this is a hassmic pushed by hand beside an older one) */
            if (!cpu_blind) { cpu_blind = 1; say("the launcher is an older one (version %d) and does not read the plugins' CPU time: no CPU watchdog", launcher_ver); }
            continue;
        }
        launcher_say(PLUGRUN_USAGE, (uint32_t)p->slot, 0, 0, 0, 0, NULL, 0); p->cpu_asked = now;
    }
    pthread_mutex_unlock(&bl);
}

/* bl held: the launcher's answer for P.  Both numbers are the launcher's, read in one go (its CPU time and the
 * launcher's clock at that moment), so the share is right whenever the broker gets round to the answer: on hassmic's
 * own clock an answer read late and the next read in time made a plugin look busier than it was.  More than its
 * share since the reading before, a window ago or more: ended, counted as a crash */
static void cpu_used(struct plug *p, const struct plugrun_msg *m)
{
    uint32_t used, over;
    if (!running(p) || p->pid <= 0 || (pid_t)m->a != p->pid) return;         /* the slot's process before this one, or none */
    p->cpu_asked = 0; p->cpu_next = now_ms() + cpu_window;
    if (!strcmp(p->id, wake_id)) { p->cpu_have = 0; return; }
    if (!m->d) {
        if (!cpu_blind) { cpu_blind = 1; say("%s: the launcher cannot read its CPU time (/proc/%d/stat): no CPU watchdog", p->id, (int)p->pid); }
        p->cpu_have = 0; return;
    }
    used = m->b - p->cpu_ms; over = m->c - p->cpu_at;                         /* (modulo 2^32, as they are sent) */
    if (p->cpu_have && over < (uint32_t)cpu_window / 2) return;               /* two answers at once (one was late): the next, against the same reading */
    if (p->cpu_have && over < 0x80000000u && (unsigned long long)used * 100 > (unsigned long long)cpu_max * over) {
        char why[120]; snprintf(why, sizeof why, "it used more CPU than a plugin may (%u s in %u s)", used / 1000, over / 1000);
        stop(p, why); say("%s: %s: ended", p->id, p->why);
    }
    p->cpu_ms = m->b; p->cpu_at = m->c; p->cpu_have = 1;
}

static void start_all(void)
{
    const struct ps_plugin *list; char ids[PS_MAX][PS_ID_MAX]; int n;
    ps_lock(); n = ps_scan(&list); for (int i = 0; i < n; i++) snprintf(ids[i], PS_ID_MAX, "%s", list[i].m.id); ps_unlock();
    for (int i = 0; i < n; i++) start(ids[i]);
}

static void *broker(void *arg)
{
    static uint8_t buf[PM_MAX_PACKET + 1];
    (void)arg;
    broker_tid = (pid_t)syscall(SYS_gettid);
    for (;;) {
        struct pollfd pf[2 + 2 * NPLUG]; int who[2 + 2 * NPLUG], n = 0;
        pthread_mutex_lock(&bl);
        if (launcher >= 0) { pf[n] = (struct pollfd){ launcher, POLLIN, 0 }; who[n++] = -1; }
        if (bwake[0] >= 0) { pf[n] = (struct pollfd){ bwake[0], POLLIN, 0 }; who[n++] = -2; }     /* (a request's messages: rq_drain) */
        for (int i = 0; i < NPLUG; i++) {
            if (plugs[i].ctl >= 0 && plugs[i].state != P_SPAWNING) { pf[n] = (struct pollfd){ plugs[i].ctl, POLLIN, 0 }; who[n++] = i; }
            if (plugs[i].err >= 0) { pf[n] = (struct pollfd){ plugs[i].err, POLLIN, 0 }; who[n++] = i + 100; }
        }
        pthread_mutex_unlock(&bl);
        if (poll(pf, (nfds_t)n, 250) < 0 && errno != EINTR) { usleep(250000); continue; }
        for (int k = 0; k < n; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (who[k] == -2) { rq_drain(); continue; }
            if (who[k] < 0) {
                struct plugrun_msg m; ssize_t r;
                while ((r = recv(launcher, &m, sizeof m, MSG_DONTWAIT)) == (ssize_t)sizeof m) launcher_message(&m);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                    /* the launcher is gone, and its children with it: nothing more starts in this run */
                    pthread_mutex_lock(&bl);
                    say("the launcher is gone: no plugins until hassmic starts again");
                    close(launcher); launcher = -1; snprintf(why_none, sizeof why_none, "the plugins' launcher ended");
                    for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && plugs[i].state != P_DEAD) { exited(&plugs[i], why_none, 0); plugs[i].want = 0; snprintf(plugs[i].why, sizeof plugs[i].why, "%s", why_none); }
                    pthread_mutex_unlock(&bl);
                    if (wake_id[0]) core_wake_plugin_changed(wake_id);
                    ha_relist();        /* nothing can run: nothing listed (plugin_feature_on) */
                }
            } else if (who[k] >= 100) {
                char d[512]; ssize_t r; struct plug *p = &plugs[who[k] - 100];
                pthread_mutex_lock(&bl);
                if (p->err == pf[k].fd) {
                    while ((r = read(p->err, d, sizeof d)) > 0) log_lines(p, d, (size_t)r);
                    if (r == 0) { close(p->err); p->err = -1; }
                }
                pthread_mutex_unlock(&bl);
            } else {
                for (int round = 0; round < 64; round++) {      /* a flood does not keep the others waiting */
                    ssize_t r;
                    pthread_mutex_lock(&bl);
                    r = plugs[who[k]].ctl == pf[k].fd ? recv(pf[k].fd, buf, sizeof buf, MSG_DONTWAIT | MSG_TRUNC) : -1;
                    if (plugs[who[k]].ctl == pf[k].fd && (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR))) {
                        /* its end is closed: polled again it would be readable for ever (a plugin that closes
                         * descriptor 3 and sleeps had the broker spin until the pings ran out) */
                        struct plug *q = &plugs[who[k]];
                        close(q->ctl); q->ctl = -1;
                        /* gone, or (a moment for the launcher's EXITED to say so) alive without it: then killed.
                         * One that was told to stop closes it too: that is no doing of its own */
                        if (running(q)) { q->lost = 1; q->why[0] = 0; q->state = P_STOPPING; q->stop_at = now_ms() + 500; }
                    }
                    pthread_mutex_unlock(&bl);
                    if (r <= 0) break;
                    if ((size_t)r <= PM_MAX_PACKET) { handling = who[k]; message(who[k], buf, (size_t)r); handling = -1; in_core = -1; }
                }
            }
        }
        for (int i = 0; i < NPLUG; i++) { check_done(i); look_unchecked(i); }
        cpu_watch(); falls_keep(); log_pump(); ha_flush(); music_due();
        for (int i = 0; i < NPLUG; i++) vol_flush(i);
        /* (with the push port, every start on an Echo: main.c's self test lets the others go once it passed, also
         * long after its 30 s where only the wake word plugin held it.  Without, -o 0: 10 s after the start) */
        if (!go && !selftest_expected && now_ms() - start_ms > 10000) plugin_go();
        { int tell, relist; char id[PS_ID_MAX]; long long now = now_ms(); static long long relisted;
          int community; pthread_mutex_lock(&bl); tell = tell_core; tell_core = 0; relist = relist_due; relist_due = 0; community = community_due; community_due = 0; snprintf(id, sizeof id, "%s", wake_id); pthread_mutex_unlock(&bl);
          if (community) plugin_withdrawn_changed();     /* (stops or starts them, tells the core and Home Assistant) */
          if (tell) core_wake_plugin_changed(id);
          /* asked where the list may have changed, and every RELIST_MS besides: what changes it without a word to
           * the broker (a scan of the page's) is not left standing in Home Assistant */
          if (relist || now - relisted >= RELIST_MS) { relisted = now; ha_relist_as(relist != 2); } }
        timers();
    }
    return NULL;
}

void plugin_start(int on)
{
    const char *e = getenv(PLUGRUN_FD_ENV), *off = getenv("HASSMIC_PLUGINS"); struct plugrun_msg m; struct pollfd pf; pthread_t t;
    const struct ps_plugin *list;
    start_ms = now_ms(); feature = on;
    { const char *hb = getenv("HASSMIC_PLUGIN_HEARD"), *rt = getenv("HASSMIC_PLUGIN_RETRY"), *cp = getenv("HASSMIC_PLUGIN_CPU"), *cm = getenv("HASSMIC_PLUGIN_CORE"); int pct, ms;
      if (hb && atoi(hb) > 0 && atoi(hb) < HEARD_BURST) heard_burst = atoi(hb);
      if (rt && atoi(rt) > 0 && atoi(rt) < RETRY_MS) retry_ms = atoi(rt);
      if (cm && sscanf(cm, "%d:%d", &pct, &ms) == 2 && pct > 0 && pct <= CORE_MARKS && ms >= 1000 && ms <= CORE_WINDOW) { core_marks = pct; core_window = ms; }
      if (cp && sscanf(cp, "%d:%d", &pct, &ms) == 2 && pct > 0 && pct <= CPU_MAX_PCT && ms >= 500 && ms <= CPU_WINDOW_MS) { cpu_max = pct; cpu_window = ms; }
      if ((cp = getenv("HASSMIC_PLUGIN_LINES")) && atoi(cp) > LINES_MIN && atoi(cp) <= 1000) lines_min = atoi(cp);
      if ((cp = getenv("HASSMIC_PLUGIN_STORE")) && sscanf(cp, "%d:%d", &pct, &ms) == 2 && pct > 0 && pct <= STORE_BUCKET && ms >= 100 && ms <= STORE_REFILL) { store_bucket = pct; store_refill = ms; }
      { int a, b, c, d;
        if ((cp = getenv("HASSMIC_PLUGIN_CALL_TIMES")) && sscanf(cp, "%d:%d:%d:%d", &a, &b, &c, &d) == 4 && a >= 500 && a <= RING_MS && b >= 500 && b <= LIVE_WAIT_MS
            && c >= 500 && c <= CALLING_MS && d >= 1000 && d <= RINGS_WINDOW) { ring_ms = a; live_wait_ms = b; calling_ms = c; rings_window = d; } } }
    fall_begin();
    /* the memory the places take, a quarter of the Echo's where that is less than RUN_MEM_MB (run_mem) */
    { FILE *f = fopen("/proc/meminfo", "re"); char l[128]; long kb = 0;
      while (f && fgets(l, sizeof l, f)) if (sscanf(l, "MemTotal: %ld kB", &kb) == 1) break;
      if (f) fclose(f);
      run_mem = run_mem_of(kb); }
    for (int i = 0; i < NPLUG; i++) { plugs[i].ctl = plugs[i].err = plugs[i].stream = plugs[i].slot = plugs[i].rq_fd = -1; plugs[i].lane = -1; }
    /* the broker's poll wakes for a request's messages (rq_drain); without the pipe they go at its next round (250 ms) */
    if (pipe2(bwake, O_CLOEXEC | O_NONBLOCK)) bwake[0] = bwake[1] = -1;
    if (off && !strcmp(off, "0")) { conf_off = 1; snprintf(why_none, sizeof why_none, "PLUGINS=0 in hassmic.conf switches plugins off"); }
    else if (e && *e) {
        launcher = atoi(e);
        /* nothing hassmic runs may inherit it (ledctrl, pryon_test, the screen's UI) */
        if (fcntl(launcher, F_SETFD, FD_CLOEXEC) || fcntl(launcher, F_SETFL, O_NONBLOCK)) launcher = -1;
        unsetenv(PLUGRUN_FD_ENV);
        pf.fd = launcher; pf.events = POLLIN;
        /* Any version: a later launcher only adds messages (plugrun.h), this one sends an older launcher none it
         * does not know, and what it gets and does not know it leaves (launcher_message) */
        if (launcher >= 0 && poll(&pf, 1, 3000) == 1 && recv(launcher, &m, sizeof m, MSG_DONTWAIT) == (ssize_t)sizeof m && m.type == PLUGRUN_READY && m.a >= 1) {
            launcher_ver = m.a > PLUGRUN_VERSION ? PLUGRUN_VERSION : (int)m.a;
            sandbox_ok = (int)m.b; domain = (int)m.c;
            if (!sandbox_ok) snprintf(why_none, sizeof why_none, "this Echo's kernel cannot hold a plugin in its sandbox");
            else why_none[0] = 0;
        } else if (launcher >= 0) { close(launcher); launcher = -1; snprintf(why_none, sizeof why_none, "the plugins' launcher did not answer"); }
    }
    if (!why_none[0] && !feature) snprintf(why_none, sizeof why_none, "plugins are switched off");
    ps_lock(); int n = ps_scan(&list); ps_unlock();
    ps_mww_own();                       /* (a list that was the built-in engine's folder under a build before: its own now) */
    say("%s%s%s, %d installed", why_none[0] ? "none can run: " : "ready", why_none,
        why_none[0] ? "" : domain == PLUGRUN_DOMAIN_PLUGIN ? " (sandbox complete)" : domain == PLUGRUN_DOMAIN_HASSMIC ? " (sandbox without its SELinux wall)" : " (sandbox without SELinux)", n);
    pthread_mutex_lock(&bl); teller_start(); pthread_mutex_unlock(&bl);     /* (calls: before any can begin) */
    started = 1;
    ha_relist();                        /* (what Home Assistant will be told at its first link: the list changes from here) */
    if (!pthread_create(&t, NULL, broker, NULL)) pthread_detach(t);
}

void plugin_selftest(int expected) { selftest_expected = expected; }

void plugin_go(void)
{
    pthread_mutex_lock(&bl); int was = go; go = 1; pthread_mutex_unlock(&bl);
    if (!was && started) start_all();
}

void plugin_feature(int on)
{
    pthread_mutex_lock(&bl);
    /* switched on by the owner: on again after hassmic's crashes too (the count goes to the flash on the broker's
     * thread: the caller holds core_lock) */
    if (on && !feature && (falls || falls_off || fall_held[0])) { falls = falls_off = 0; fall_held[0] = 0; falls_dirty = 1; }
    if (on && !feature) { core_held[0] = 0; core_until = 0; }
    feature = on;
    if (!strcmp(why_none, "plugins are switched off") && on) why_none[0] = 0;
    if (!on && !why_none[0]) snprintf(why_none, sizeof why_none, "plugins are switched off");
    for (int i = 0; i < NPLUG; i++) if (running(&plugs[i]) && !on) { plugs[i].want = 0; stop(&plugs[i], "plugins are switched off"); }
    tell_core = 1;                      /* the caller holds core_lock (settings.c) */
    /* the list's digest, too, by the broker (which holds no lock: ha_relist takes core_lock), without closing the
     * links: plugins is a feature, and its switch closes them itself (settings.c), after this, under the same
     * core_lock, so Home Assistant reads the list as it is now.  Closed again by the broker, it reconnected twice */
    relist_due = 2;
    pthread_mutex_unlock(&bl);
    if (on && started && go) start_all();
}

const char *plugin_why(void) { return why_none; }

/* hassmic ends (main.c: SIGTERM, which root's main.sh sends before init's restart; main()'s own end): every plugin
 * that runs is told STOP, as for an update, and hassmic waits for them to be gone, MS at most.  Without it the
 * launcher saw hassmic's end of the socket close and killed them all at once: no stop callback ran, and a forwarder's
 * cursor was what it had written last by itself (the template's build, 2026-11-21).  Not on a path hassmic does not
 * see: a crash, SIGKILL (init's own stop and restart: SIGKILL to the process group, Android 7.1 and 9), a power cut.  Any
 * thread but the broker's, no lock held: the broker reads their last writes and the launcher's EXITED meanwhile */
int plugin_shutdown(int ms)
{
    struct timespec until; int n = 0, left = 0; long long t0 = now_ms();
    pthread_mutex_lock(&bl);
    ending = 1;
    for (int i = 0; i < NPLUG; i++) if (running(&plugs[i])) { plugs[i].want = 0; stop(&plugs[i], "hassmic ended"); n++; }
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000; until.tv_nsec += (long)(ms % 1000) * 1000000L; if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    for (;;) {
        left = 0;
        for (int i = 0; i < NPLUG; i++) left += running(&plugs[i]) || plugs[i].state == P_STOPPING;
        if (!left || launcher < 0 || pthread_cond_timedwait(&bcv, &bl, &until)) break;
    }
    pthread_mutex_unlock(&bl);
    if (n || left) say("hassmic ends: %d told to stop, %d gone within %lld ms%s", n, n - left > 0 ? n - left : 0, now_ms() - t0, left ? " (the launcher kills the others)" : "");
    return left;
}

void plugin_resume(void)
{
    char wid[PS_ID_MAX]; int g;
    pthread_mutex_lock(&bl);
    if (falls || falls_off || fall_held[0]) say("switched on again after hassmic's crashes");
    falls = falls_off = 0; fall_held[0] = 0; falls_dirty = 1; core_held[0] = 0; core_until = 0;
    g = go; snprintf(wid, sizeof wid, "%s", wake_id);
    pthread_mutex_unlock(&bl);
    if (!started) return;
    if (g) start_all(); else if (wid[0]) start(wid);
    if (wid[0]) core_wake_plugin_changed(wid);
    ha_relist();                        /* (what is listed does not hang on the crashes: asked all the same, H4) */
}

/* ---------------------------------------------------------------- the page's side */

void plugin_installed(const char *id)
{
    const struct ps_plugin *list; struct plug *p;
    /* The old version stopped first, then the list read: the scan's seal_upkeep sees a process of another package
     * running (plugin_old_running) and leaves the seal's wipe to the new version's start (program_open), when the old
     * one is gone.  Wiped here, the old process wrote into the folder until it was killed, and its mark was gone (R1) */
    pthread_mutex_lock(&bl);
    if (!strcmp(id, fall_held)) fall_held[0] = 0;       /* another version of what hassmic fell over: tried */
    if (!strcmp(id, core_held)) { core_held[0] = 0; core_until = 0; }
    if ((p = plug_of(id))) { p->off = 0; p->ncrash = 0; if (running(p)) { p->want = 1; stop(p, ""); } }       /* the new version: started when the old one is gone */
    pthread_mutex_unlock(&bl);
    ps_lock(); ps_scan(&list); ps_unlock();
    /* tests: the core told later than the page sees it accepted (HASSMIC_PLUGIN_INSTALLED_DELAY=<ms>:<file>, while the
     * file is there), so that the engine's choice meets the list from before the install every time */
    { const char *e = getenv("HASSMIC_PLUGIN_INSTALLED_DELAY"); char f[200]; int ms; struct stat st;
      if (e && sscanf(e, "%d:%199s", &ms, f) == 2 && ms > 0 && ms <= 10000 && !stat(f, &st)) { struct timespec d = { ms / 1000, ms % 1000 * 1000000L }; nanosleep(&d, NULL); } }
    /* the engine setting's choices before the start, which may take milliseconds (its program read and hashed): the
     * page shows it accepted as ps_stage_install wrote it and chooses it at once (P4 of a review of 7be9343) */
    core_wake_plugin_changed(id);
    start(id);
    core_wake_plugin_changed(id);       /* (whichever it is: the engine setting's choices are the wake plugins installed) */
    ha_relist();                        /* its entities and actions as this version names them, or none (H4) */
}

int plugin_enable(const char *id, int on)
{
    const struct ps_plugin *list; struct plug *p; int r;
    ps_lock(); r = ps_find(id) ? ps_enable(id, on) : -1; ps_scan(&list); ps_unlock();
    if (r) return -1;
    pthread_mutex_lock(&bl);
    if (on && !strcmp(id, fall_held)) fall_held[0] = 0;         /* the owner's own switch: tried again */
    if (on && !strcmp(id, core_held)) { core_held[0] = 0; core_until = 0; }
    if ((p = plug_of(id))) { p->off = 0; p->ncrash = 0; if (!on) { p->want = 0; stop(p, "switched off"); if (!running(p) && p->state != P_STOPPING) snprintf(p->why, sizeof p->why, "switched off"); } }
    pthread_mutex_unlock(&bl);
    if (on) start(id);
    if (!strcmp(id, wake_id)) core_wake_plugin_changed(id);
    ha_relist();                        /* listed while it is switched on (4.10) */
    return 0;
}

/* Another revocation list (plugin_fetch.c), or "run it anyway" pressed (web.c): the store's scan says per plugin
 * whether it may run (plugin_index.c pi_withdrawn, every tier; plugin_store.c's anyway mark), and each that may not
 * any more is stopped now, with that reason on its card.  Kept: removing it is the owner's.  And each that may run
 * again (a newer list no longer names it, or names a version below it; the owner's "run it anyway") is started, as
 * start_all would: it was stopped for the list, and nothing else would start it before the next start of hassmic
 * (a review, 2026-11-21: a plugin stayed stopped by a list that no longer withdrew it) */
void plugin_withdrawn_changed(void)
{
    const struct ps_plugin *list; char ids[PS_MAX][PS_ID_MAX], why[PS_MAX][200], again[PS_MAX][PS_ID_MAX]; int n, k = 0, na = 0, g, wd[PS_MAX];
    ps_lock(); n = ps_scan(&list);
    for (int i = 0; i < n && k < PS_MAX; i++) {
        if (list[i].refused[0]) { snprintf(ids[k], sizeof ids[0], "%s", list[i].m.id); snprintf(why[k], sizeof why[0], "%s", list[i].refused); wd[k] = list[i].withdrawn[0] != 0 || list[i].community_off; k++; }
        else if (na < PS_MAX) snprintf(again[na++], sizeof again[0], "%s", list[i].m.id);
    }
    ps_unlock();
    pthread_mutex_lock(&bl);
    for (int i = 0; i < k; i++) {
        struct plug *p = plug_of(ids[i]);
        if (p && running(p)) { p->want = 0; p->withdrawn = wd[i]; say("%s: stopped: %s", ids[i], why[i]); stop(p, why[i]); }
        else if (p && wd[i]) { p->withdrawn = 1; snprintf(p->why, sizeof p->why, "%.190s", why[i]); }
    }
    /* (only those a list stopped: one the owner switched off, or that crashed five times, stays as it is; start()
     * asks everything else again) */
    for (int i = 0; i < na; i++) { struct plug *p = plug_of(again[i]); if (!p || !p->withdrawn || running(p)) again[i][0] = 0; else p->withdrawn = 0; }
    /* the partner rule asked again: a withdrawn wake engine counts as one nobody vouches for (plugin_store.c vouched) */
    partner_due = 1;
    g = go;
    pthread_mutex_unlock(&bl);
    for (int i = 0; i < na; i++) if (again[i][0] && (g || !strcmp(again[i], wake_id))) { say("%s: no longer withdrawn: started again", again[i]); start(again[i]); }
    for (int i = 0; i < k; i++) core_wake_plugin_changed(ids[i]);
    for (int i = 0; i < na; i++) if (again[i][0]) core_wake_plugin_changed(again[i]);
    ha_relist();                        /* a withdrawn plugin's entities leave Home Assistant's list, and come back (H4) */
}

/* The setting plugins_community (settings.c put, core_lock held).  The store's flag at once: an install checked from
 * now on is checked by it (ps_stage_check, and again as it writes).  What it stops or starts the broker's thread does
 * (plugin_withdrawn_changed, which takes the store's lock, tells the core of the wake engine's plugin and has Home
 * Assistant's list made anew: the core's lock, which the caller holds).  Off: every plugin not listed as official is
 * stopped and kept, its card saying why (ps_scan's community_off); on: they start again, unless something else holds
 * them (switched off, withdrawn, crashed: start() asks as it always does) */
void plugin_community(int on)
{
    if (ps_community(-1) == !!on) return;
    ps_community(on);
    say("community plugins switched %s", on ? "on: those installed start" : "off: those installed are stopped and kept");
    pthread_mutex_lock(&bl); community_due = 1; pthread_mutex_unlock(&bl);
    if (bwake[1] >= 0 && write(bwake[1], "", 1) < 0) { }
}

/* (bl held) */
static const char *sandbox_name(void) { return !sandbox_ok ? "none" : domain == PLUGRUN_DOMAIN_PLUGIN ? "complete" : domain == PLUGRUN_DOMAIN_HASSMIC ? "hassmic" : "su"; }
const char *plugin_sandbox(void) { pthread_mutex_lock(&bl); const char *r = sandbox_name(); pthread_mutex_unlock(&bl); return r; }
int plugin_feature_on(void) { pthread_mutex_lock(&bl); int on = feature && launcher >= 0; pthread_mutex_unlock(&bl); return on; }

int plugin_remove(const char *id)
{
    const struct ps_plugin *list; struct plug *p; int r;
    pthread_mutex_lock(&bl);
    /* revoked: its process, until it is gone, touches nothing of the store's (the same id installed again a moment
     * later would have its storage written by the removed one's; R1) */
    if ((p = plug_of(id))) { p->want = 0; p->gone = 1; p->revoked = 1; if (running(p)) stop(p, "removed"); else if (p->state == P_DEAD) p->state = P_FREE; }
    pthread_mutex_unlock(&bl);
    ps_lock(); r = ps_remove(id); ps_scan(&list); ps_unlock();
    core_wake_plugin_changed(id);
    ha_relist();
    return r;
}

void plugin_config_changed(const char *id) { send_config(id); }

void plugin_restart(const char *id, const char *why)
{
    struct plug *p;
    pthread_mutex_lock(&bl);
    /* stop() first: from here no LOG goes to it (log_pump sends to P_RUN only), and a CONNECT of this process that a
     * worker answers later is dropped (net_done: another start); its sockets close with it */
    if ((p = plug_of(id)) && running(p)) { p->want = 1; stop(p, ""); say("%s: %s: started again", id, why); }
    pthread_mutex_unlock(&bl);
}

int plugin_develop(const char *id, int on)
{
    const struct ps_plugin *list; struct plug *p; int r;
    ps_lock(); r = ps_find(id) ? ps_develop(id, on) : -1; ps_scan(&list); ps_unlock();
    if (r) return -1;
    pthread_mutex_lock(&bl);
    /* started again at once with the flag as it is now; not where the launcher is an older one, which starts it
     * without the flag in any case (a restart for nothing; the card shows the flag as not available there) */
    if ((p = plug_of(id)) && running(p) && p->develop != (on && launcher_ver >= 3)) { p->want = 1; stop(p, ""); }
    pthread_mutex_unlock(&bl);
    say("%s: development flag %s%s", id, on ? "on (a system call the sandbox refuses stops it with SIGSYS)" : "off", launcher_ver && launcher_ver < 3 && on ? ", but the launcher is an older one: it has no effect" : "");
    return 0;
}

int plugin_storage_replace(const char *id, const void *tar, size_t n, char *err, size_t errsz)
{
    struct plug *p; struct timespec until; int r, was = 0;
    /* refused before anything stops: a key it cannot keep, more than its kB */
    if (ps_storage_replace(id, tar, n, 1, err, errsz)) return -1;
    /* stopped and waited for (its STOP_MS and the launcher's SIGTERM, a few seconds at most): a plugin writing while
     * its data is replaced would leave neither the one nor the other */
    pthread_mutex_lock(&bl);
    if ((p = plug_of(id)) && running(p)) { was = 1; p->want = 0; stop(p, "its data is replaced (copied from another Echo)"); }
    clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 6;
    while ((p = plug_of(id)) && (running(p) || p->state == P_STOPPING) && !pthread_cond_timedwait(&bcv, &bl, &until)) ;
    r = (p = plug_of(id)) && (running(p) || p->state == P_STOPPING);
    pthread_mutex_unlock(&bl);
    if (r) { snprintf(err, errsz, "the plugin did not stop in time"); r = -1; }
    else if ((r = ps_storage_replace(id, tar, n, 0, err, errsz))) ;
    else say("%s: its data was replaced by a copy from another Echo (%zu bytes of tar)", id, n);
    if (was) { pthread_mutex_lock(&bl); if ((p = plug_of(id))) p->why[0] = 0; pthread_mutex_unlock(&bl); start(id); }
    return r;
}

void plugin_files_changed(const char *id, const char *list)
{
    struct plug *p;
    (void)list;
    send_config(id);
    pthread_mutex_lock(&bl); if ((p = plug_of(id))) p->look = 1; pthread_mutex_unlock(&bl);
    if (!strcmp(id, wake_id)) core_wake_plugin_changed(id);        /* a title, a field, an item more or less */
}

static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...)
{
    va_list ap; int k;
    if (n >= cap) return n;
    va_start(ap, fmt); k = vsnprintf(o + n, cap - n, fmt, ap); va_end(ap);
    return k < 0 ? n : n + (size_t)k < cap ? n + (size_t)k : cap;
}
static size_t appj(char *o, size_t n, size_t cap, const char *s)       /* a JSON string (a capability's line at the longest) */
{
    char b[2 * PS_CAP_LINE + 8];
    if (json_esc(b, sizeof b, s) < 0) json_esc(b, sizeof b, "");
    return app(o, n, cap, "%s", b);
}

size_t plugin_json(char *o, size_t cap)
{
    const struct ps_plugin *list; struct ps_item *items = malloc(sizeof *items * PS_ITEMS); size_t n = 0; int np; char val[PS_VALUE_MAX];
    static const char *const types[] = { "bool", "int", "select", "text", "host", "secret", "files" };
    if (!items) return 0;
    pthread_mutex_lock(&bl);
    /* scopes: the version of each this build has (sdk/PROTOCOL.md "Versions"), which a plugin's requires are held to */
    n = app(o, n, cap, "{\"scopes\":{");
    for (int sc = 0; sc < PM_SCOPES; sc++) n = app(o, n, cap, "%s\"%s\":%u", sc ? "," : "", pm_scope_name(sc), ps_scope_have(sc));
    n = app(o, n, cap, "},\"available\":%s,\"why\":", why_none[0] ? "false" : "true");
    n = appj(o, n, cap, why_none);
    /* falls: hassmic's own crashes with plugins started, falls_off: at three, none but the wake engine's starts */
    /* develop_flag: the launcher takes the development flag (version 3; an older one starts every plugin without it) */
    /* hears: the plugins that run and get a copy of every request (request.audio): the page says so on its top (3.6) */
    n = app(o, n, cap, ",\"hears\":[");
    for (int i = 0, k = 0; i < NPLUG; i++) if (plugs[i].state == P_RUN && (plugs[i].caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO))) n = app(o, n, cap, "%s\"%s\"", k++ ? "," : "", plugs[i].id);
    n = app(o, n, cap, "]");
    /* run_max, run_mem: the places (RUN_MAX processes, run_mem MB of memory limits between them) */
    n = app(o, n, cap, ",\"launcher\":%s,\"sandbox\":\"%s\",\"run_max\":%d,\"run_mem\":%d,\"falls\":%d,\"falls_off\":%s,\"develop_flag\":%s,\"plugins\":[", launcher >= 0 ? "true" : "false",
            sandbox_name(), RUN_MAX, run_mem, falls, falls_off ? "true" : "false",
            launcher < 0 || launcher_ver >= 3 ? "true" : "false");
    pthread_mutex_unlock(&bl);
    ps_lock();
    np = ps_list(&list);
    for (int i = 0; i < np; i++) {
        const struct ps_plugin *sp = &list[i]; struct plug *p; const char *state = "stopped"; char why[200] = ""; int first = 1;
        char bad[8][41]; int nbad = 0, ready = 0, log_on = 0, log_deaf = 0; long long log_lines = 0, net_ok = 0, net_fail = 0;
        pthread_mutex_lock(&bl);
        if ((p = plug_of(sp->m.id))) {
            log_on = p->log_on && running(p); log_lines = p->log_lines; net_ok = p->net_ok; net_fail = p->net_fail;
            log_deaf = log_on && p->log_wait && !p->net_pending && btime - p->log_sent >= LOG_ANSWER_MS;
            state = p->state == P_RUN ? "running" : running(p) ? "starting" : p->state == P_STOPPING ? "stopping" : p->off ? "crashed"
                    : p->want && p->waits ? "waiting" : p->want && p->next_start ? "restarting" : "stopped";
            snprintf(why, sizeof why, "%s", p->why); nbad = p->nbad; memcpy(bad, p->bad, sizeof bad); ready = p->wake_ready;
        }
        pthread_mutex_unlock(&bl);
        if (sp->refused[0]) snprintf(why, sizeof why, "%s", sp->refused);      /* whatever else: it is why it never will */
        n = app(o, n, cap, "%s{\"id\":\"%s\",\"name\":", i ? "," : "", sp->m.id); n = appj(o, n, cap, sp->m.name);
        n = app(o, n, cap, ",\"author\":"); n = appj(o, n, cap, sp->m.author);
        /* (the author's name is the manifest's word; the key is who signed.  None for a manifest this build did not
         * read: its key is zeros, and a fingerprint of them named nobody's) */
        if (sp->here && !sp->unread) { char fp[20]; ps_fingerprint(sp->m.author_key, fp); n = app(o, n, cap, ",\"key\":\"%s\"", fp); }
        n = app(o, n, cap, ",\"description\":"); n = appj(o, n, cap, sp->m.description);
        n = app(o, n, cap, ",\"source\":"); n = appj(o, n, cap, sp->m.source);
        n = app(o, n, cap, ",\"license\":"); n = appj(o, n, cap, sp->m.license);
        n = app(o, n, cap, ",\"version_name\":"); n = appj(o, n, cap, sp->m.version_name);
        n = app(o, n, cap, ",\"version\":%ld,\"tier\":\"%s\",\"enabled\":%s,\"accepted\":%s,\"how\":", sp->m.version, sp->tier == PS_OFFICIAL ? "official" : sp->tier == PS_COMMUNITY ? "community" : "sideload",
                sp->enabled ? "true" : "false", sp->accepted ? "true" : "false");
        n = appj(o, n, cap, sp->how);
        n = app(o, n, cap, ",\"state\":\"%s\",\"detail\":", state); n = appj(o, n, cap, why);
        /* withdrawn: the revocation list's reason while it names this version ("" not), anyway: the owner said to run
         * it regardless, for that list (the card's "Run it anyway", web.c W_ANYWAY) */
        n = app(o, n, cap, ",\"withdrawn\":"); n = appj(o, n, cap, sp->withdrawn); n = app(o, n, cap, ",\"anyway\":%s", sp->anyway ? "true" : "false");
        /* not official, and community plugins are switched off here: stopped and kept (2.7) */
        n = app(o, n, cap, ",\"community_off\":%s", sp->community_off ? "true" : "false");
        /* here: it has a package this hassmic can start (false: a card that can only be removed); newer: one for a newer
         * hassmic (a fall back to this one: it requires what this one has not, `detail` says what), kept, and it runs
         * again under that one; requires: what it speaks */
        n = app(o, n, cap, ",\"here\":%s,\"newer\":%s,\"requires\":{", sp->here && !sp->unread ? "true" : "false", sp->m.newer ? "true" : "false");
        for (int sc = 0, k = 0; sc < PM_SCOPES; sc++) if (sp->m.requires[sc]) n = app(o, n, cap, "%s\"%s\":%u", k++ ? "," : "", pm_scope_name(sc), sp->m.requires[sc]);
        n = app(o, n, cap, "}");
        /* wake: what it keeps was given under the seal: `wake` in its manifest, or a wipe still due after an update
         * without it (ps_sealed).  The page's copy says "from-wake" by it: from the manifest alone, files went out
         * without it while the old process stopped (a review of 7144548, 3; the storage's first line had it already).
         * Whether it is a wake word engine is its capability list's `wake` */
        n = app(o, n, cap, ",\"wake\":%s,\"listening\":%s,\"memory_mb\":%d,\"files_mb\":%d,\"files_used\":%lld", ps_sealed(sp->m.id) ? "true" : "false", ready ? "true" : "false",
                sp->m.memory_mb, sp->m.files_mb, ps_files_used(sp));
        /* what the card shows of storage, the log and the network: how much it keeps, whether it follows the log
         * and how much of it it was sent, when it last connected and failed to (seconds ago, -1 never) */
        { int keys; long long used = ps_storage_used(sp->m.id, &keys), now = now_ms();
          n = app(o, n, cap, ",\"develop\":%s,\"storage_kb\":%d,\"storage_used\":%lld,\"storage_keys\":%d,\"log\":%s,\"log_lines\":%lld,\"log_unanswered\":%s,\"net_ok\":%lld,\"net_fail\":%lld,\"caps\":[",
                  sp->develop ? "true" : "false", sp->m.storage_kb, used, keys, log_on ? "true" : "false", log_lines, log_deaf ? "true" : "false", net_ok ? (now - net_ok) / 1000 : -1, net_fail ? (now - net_fail) / 1000 : -1); }
        for (int b = 0; ps_cap_name(b); b++) if (sp->m.caps & PM_CAPBIT(b)) {
            char line[PS_CAP_LINE]; ps_cap_line(&sp->m, b, line, sizeof line);
            n = app(o, n, cap, "%s{\"cap\":\"%s\",\"line\":", first ? "" : ",", ps_cap_name(b)); n = appj(o, n, cap, line); n = app(o, n, cap, "}"); first = 0;
        }
        /* (and what each version it requires above 1 lets it do beyond those lines: its install dialog's words) */
        { char wj[PS_WIDENS_JSON]; ps_widens_json(&sp->m, wj, sizeof wj); n = app(o, n, cap, "],\"widens\":%s", wj); }
        /* phase 4: what its player plays, the proposals waiting for the owner, the states its entities have now, its
         * actions with a button on the card, its UDP ports */
        { struct plug *q; char title[129] = "", artist[129] = "", props[PS_NET][2][33]; int ms = 0, np2 = 0; char ev[PS_ENTITIES][256]; uint32_t have = 0;
          unsigned short ports[PS_UDP_MAX + 1]; int nports = 0, cs = C_NONE, rq_now = 0, run = 0; long long rq_runs = 0, rq_lost = 0, rq_last = 0;
          pthread_mutex_lock(&bl);
          if ((q = plug_of(sp->m.id))) {
              rq_runs = q->rq_runs; rq_lost = q->rq_lost + (q->rq_open ? q->rq_dropped : 0); rq_last = q->rq_last; rq_now = q->rq_open;
              if (pc.entry == q - plugs && pc.gen == q->gen) cs = pc.state;          /* phase 5: its call, if it has the Echo's */
              ms = q->ms && running(q) ? (q->mstate == PM_MUSIC_PLAYING && !q->myield ? 1 : 2) : 0;
              snprintf(title, sizeof title, "%s", q->mtitle); snprintf(artist, sizeof artist, "%s", q->martist);
              for (int x = 0; x < PS_NET; x++) if (q->prop[x].live) { snprintf(props[np2][0], 33, "%s", q->prop[x].setting); snprintf(props[np2][1], 33, "%s", q->prop[x].value); np2++; }
              if (q->state == P_RUN) { run = 1; have = q->ent_have; memcpy(ev, q->ent, sizeof ev); }
              for (int x = 0; x <= PS_UDP_MAX; x++) if (q->udp_open[x]) ports[nports++] = q->udp_port[x];
          }
          pthread_mutex_unlock(&bl);
          n = app(o, n, cap, ",\"call\":\"%s\"", cs == C_RINGING ? "ringing" : cs == C_ANSWERED ? "answered" : cs == C_CALLING ? "calling" : cs == C_LIVE ? "live" : "");
          /* phase 6: what of the requests it had (request.audio): runs, samples it did not take, when the last began (s
           * ago, -1 never), whether it hears one now; "second": where it can send what it hears (3.3's second screen) */
          { char sl[160]; ps_second(&sp->m, sl, sizeof sl);
            n = app(o, n, cap, ",\"requests\":{\"runs\":%lld,\"lost\":%lld,\"last_s\":%lld,\"now\":%s},\"second\":", rq_runs, rq_lost, rq_last ? (now_ms() - rq_last) / 1000 : -1, rq_now ? "true" : "false");
            n = appj(o, n, cap, sl); }
          n = app(o, n, cap, ",\"music\":\"%s\",\"title\":", ms == 1 ? "playing" : ms == 2 ? "paused" : "");
          n = appj(o, n, cap, title); n = app(o, n, cap, ",\"artist\":"); n = appj(o, n, cap, artist);
          n = app(o, n, cap, ",\"ports\":[");
          for (int x = 0; x < nports; x++) n = app(o, n, cap, "%s%u", x ? "," : "", ports[x]);
          n = app(o, n, cap, "],\"proposals\":[");
          for (int x = 0; x < np2; x++) {
              const char *label = props[x][0];
              for (int r = 0; r < sp->m.nsettings; r++) if (!strcmp(sp->m.settings[r].name, props[x][0])) label = sp->m.settings[r].label;
              n = app(o, n, cap, "%s{\"setting\":\"%s\",\"value\":\"%s\",\"label\":", x ? "," : "", props[x][0], props[x][1]); n = appj(o, n, cap, label); n = app(o, n, cap, "}");
          }
          n = app(o, n, cap, "],\"entities\":[");
          { int on, k = 0; pthread_mutex_lock(&bl); on = runnable(); pthread_mutex_unlock(&bl);
          for (int x = 0; x < sp->m.nentities && (sp->m.caps & PM_CAPBIT(PM_CAP_HA_ENTITIES)); x++) {
              n = app(o, n, cap, "%s{\"name\":\"%s\",\"kind\":\"%s\",\"page\":%s,\"label\":", x ? "," : "", sp->m.entities[x].name, ps_entity_kind(sp->m.entities[x].kind),
                      sp->m.entities[x].page ? "true" : "false");
              n = appj(o, n, cap, sp->m.entities[x].label);
              /* not in Home Assistant's list though its plugin is: its key could not be kept (the card says so) */
              if (ha_listed(sp, on) && !sp->ent_key[x]) n = app(o, n, cap, ",\"unlisted\":\"nokey\"");
              /* (a row's value is in its row: never twice, 16 texts of 255 bytes a plugin are much of the answer) */
              if ((have & 1u << x) && !(sp->m.entities[x].page && run && ha_entity_out(sp, x, on))) { n = app(o, n, cap, ",\"value\":"); n = appj(o, n, cap, ev[x]); }
              n = app(o, n, cap, "}");
          }
          /* the rows of its own (an entity marked "page"), its states as Home Assistant has them, for the card
           * to show read-only under the manifest's labels.  Only while it runs and Home Assistant lists that entity
           * (ha_entity_out, what plugin_entities_each asks: its plugin listed and its key kept): a plugin that is
           * stopped, refused or switched off has no state to show (the entities above say "unavailable" for Home
           * Assistant's), and an entity whose key could not be written down is not in Home Assistant at all, so no
           * row: the entities above carry its "nokey" and the value, as for any entity (a row with it was a state
           * Home Assistant does not have, and hid the notice; a review of 541f0a8).  Every value is one ent_value_ok
           * took (a number, on/off, one of the options, a text sensor's strict UTF-8 text): the page shows it as
           * text, never as markup */
          n = app(o, n, cap, "],\"rows\":[");
          for (int x = 0; run && x < sp->m.nentities; x++) {
              const struct ps_entity *e = &sp->m.entities[x];
              if (!e->page || !ha_entity_out(sp, x, on)) continue;
              n = app(o, n, cap, "%s{\"name\":\"%s\",\"kind\":\"%s\",\"decimals\":%d,\"label\":", k++ ? "," : "", e->name, ps_entity_kind(e->kind), e->decimals);
              n = appj(o, n, cap, e->label); n = app(o, n, cap, ",\"unit\":"); n = appj(o, n, cap, e->unit);
              if (have & 1u << x) { n = app(o, n, cap, ",\"value\":"); n = appj(o, n, cap, ev[x]); }
              n = app(o, n, cap, "}");
          } }
          n = app(o, n, cap, "],\"actions\":[");
          for (int x = 0; x < sp->m.nactions && (sp->m.caps & PM_CAPBIT(PM_CAP_HA_ACTIONS)); x++) {
              n = app(o, n, cap, "%s{\"name\":\"%s\",\"page\":%s,\"label\":", x ? "," : "", sp->m.actions[x].name, sp->m.actions[x].page ? "true" : "false");
              n = appj(o, n, cap, sp->m.actions[x].label); n = app(o, n, cap, "}");
          }
          n = app(o, n, cap, "]"); }
        n = app(o, n, cap, ",\"settings\":[");
        for (int s = 0; s < sp->m.nsettings; s++) {
            const struct ps_setting *st = &sp->m.settings[s];
            n = app(o, n, cap, "%s{\"name\":\"%s\",\"type\":\"%s\",\"label\":", s ? "," : "", st->name, types[st->type]); n = appj(o, n, cap, st->label);
            n = app(o, n, cap, ",\"help\":"); n = appj(o, n, cap, st->help);
            n = app(o, n, cap, ",\"unit\":"); n = appj(o, n, cap, st->unit);
            if (st->type == PS_SECRET) n = app(o, n, cap, ",\"set\":%s", ps_config_get(sp, st->name, val, sizeof val) && val[0] ? "true" : "false");     /* never sent back */
            else if (st->type != PS_FILES) { ps_config_get(sp, st->name, val, sizeof val); n = app(o, n, cap, ",\"value\":"); n = appj(o, n, cap, val); }
            if (st->type == PS_INT) n = app(o, n, cap, ",\"min\":%ld,\"max\":%ld", st->min, st->max);
            if (st->type == PS_SELECT) { n = app(o, n, cap, ",\"options\":["); for (int k = 0; k < st->noptions; k++) { n = app(o, n, cap, "%s", k ? "," : ""); n = appj(o, n, cap, st->options[k]); } n = app(o, n, cap, "]"); }
            if (st->type == PS_FILES) {
                int ni = ps_items(sp, st->name, items, PS_ITEMS); char cat[4096];
                n = app(o, n, cap, ",\"max_items\":%d,\"item_files\":[", st->max_items);
                for (int k = 0; k < st->nfiles; k++) n = app(o, n, cap, "%s{\"suffix\":\"%s\",\"max_kb\":%d,\"optional\":%s}", k ? "," : "", st->files[k].suffix, st->files[k].max_kb, st->files[k].optional ? "true" : "false");
                n = app(o, n, cap, "],\"fields\":[");
                for (int k = 0; k < st->nfields; k++) {
                    n = app(o, n, cap, "%s{\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g,\"label\":", k ? "," : "", st->fields[k].name, st->fields[k].type, st->fields[k].min, st->fields[k].max, st->fields[k].step);
                    n = appj(o, n, cap, st->fields[k].label); n = app(o, n, cap, "}");
                }
                n = app(o, n, cap, "],\"items\":[");
                for (int k = 0; k < ni; k++) {
                    int isbad = 0; for (int b = 0; b < nbad; b++) isbad |= !strcmp(bad[b], items[k].id);
                    n = app(o, n, cap, "%s{\"id\":\"%s\",\"checked\":%s,\"bad\":%s,\"title\":", k ? "," : "", items[k].id, items[k].checked ? "true" : "false", isbad ? "true" : "false");
                    n = appj(o, n, cap, items[k].title); n = app(o, n, cap, ",\"fields\":{");
                    for (int f = 0; f < items[k].nfields; f++) { n = app(o, n, cap, "%s\"%s\":", f ? "," : "", items[k].fields[f].name); n = appj(o, n, cap, items[k].fields[f].value); }
                    n = app(o, n, cap, "},\"files\":[");
                    for (int f = 0; f < items[k].nfiles; f++) n = app(o, n, cap, "%s{\"suffix\":\"%s\",\"size\":%ld}", f ? "," : "", items[k].files[f].suffix, items[k].files[f].size);
                    n = app(o, n, cap, "]}");
                }
                n = app(o, n, cap, "]");
                if (ps_catalog_json(sp, st->name, cat, sizeof cat)) n = app(o, n, cap, ",\"catalog\":%s", cat);
            }
            n = app(o, n, cap, "}");
        }
        n = app(o, n, cap, "]}");
    }
    ps_unlock();
    n = app(o, n, cap, "]}");
    free(items);
    if (n >= cap) { n = (size_t)snprintf(o, cap, "{\"available\":false,\"why\":\"too much to show\",\"plugins\":[]}"); }
    return n;
}

/* ---------------------------------------------------------------- the wake word engine (wake.h, 4.13) */

void plugin_wake_why(const char *id, char *out, size_t cap)
{
    struct plug *p; const struct ps_plugin *sp; char cannot[200] = "";
    out[0] = 0;
    /* (listed and not to be run by this hassmic, one for a newer hassmic or a manifest it does not read: the card's words) */
    ps_lock(); if ((sp = ps_find(id)) && !sp->here) snprintf(cannot, sizeof cannot, "%s", sp->refused); ps_unlock();
    pthread_mutex_lock(&bl);
    if (!sp) snprintf(out, cap, "it is not installed on this Echo");
    else if (cannot[0]) snprintf(out, cap, "%s", cannot);
    else if (why_none[0]) snprintf(out, cap, "%s", why_none);
    else if (!(p = plug_of(id)) || !running(p)) snprintf(out, cap, "%s", p && p->why[0] ? p->why : "it does not run");
    else if (p->state != P_RUN || !p->have_words) snprintf(out, cap, "it is starting");
    else if (p->why[0] && !p->wake_ready) snprintf(out, cap, "%s", p->why);
    pthread_mutex_unlock(&bl);
}

int plugin_withdrawn_now(const char *id, char *why, size_t cap)
{
    const struct ps_plugin *sp; int r;
    ps_lock(); sp = ps_find(id); r = sp && sp->withdrawn[0] && !sp->anyway; if (r && why && cap) snprintf(why, cap, "%s", sp->withdrawn); ps_unlock();
    return r;
}

int plugin_community_off_now(const char *id)
{
    const struct ps_plugin *sp; int r;
    /* (the switch as it is now: the scan's mark lags it until the broker rescans, plugin_community) */
    ps_lock(); sp = ps_find(id); r = sp && ps_community_keeps_off(sp) ? (sp->tier == PS_COMMUNITY ? 1 : 2) : 0; ps_unlock();
    return r;
}

/* Missing: Amazon's wake word stands in.  That is the plugin not being here at all (or here for a newer hassmic only:
 * this one fell back, and the plugin requires what it has not, which no restart changes), or nothing being able to
 * run (no launcher, PLUGINS=0, the feature off): states that do not pass by themselves.  One that is installed and
 * down gets nothing in its place: an owner who chose a wake word so that nobody says "Alexa" does not get "Alexa"
 * back because of a crash (docs/plugins.md 4.13, the maintainer's rule) */
int plugin_wake_state(const char *id)
{
    const struct ps_plugin *sp; struct plug *p; int r, wake;
    ps_lock(); sp = ps_find(id); wake = sp && (sp->m.caps & PM_CAPBIT(PM_CAP_WAKE)); ps_unlock();
    if (!wake) return PLUGIN_WAKE_MISSING;
    pthread_mutex_lock(&bl);
    if (why_none[0]) r = PLUGIN_WAKE_MISSING;
    else r = (p = plug_of(id)) && p->state == P_RUN && p->have_words ? PLUGIN_WAKE_UP : PLUGIN_WAKE_DOWN;
    pthread_mutex_unlock(&bl);
    return r;
}

void plugin_wake_engine(const char *id)
{
    ps_engine_set(id);                  /* (the store's rules about the engine: request.audio's partner, 3.3) */
    pthread_mutex_lock(&bl);
    if (strcmp(wake_id, id)) partner_due = 1;
    snprintf(wake_id, sizeof wake_id, "%s", id);
    pthread_mutex_unlock(&bl);
}

int plugin_wake_listening(const char *id)
{
    struct plug *p; int r;
    pthread_mutex_lock(&bl); r = (p = plug_of(id)) && p->state == P_RUN && p->ws[0].state == W_OPEN; pthread_mutex_unlock(&bl);
    return r;
}

int plugin_wake_begin(const char *id, int ms)
{
    struct timespec until; struct plug *p;
    plugin_wake_engine(id);
    if (!started || plugin_wake_state(id) == PLUGIN_WAKE_MISSING) return plugin_wake_state(id);
    start(id);
    clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += ms / 1000; until.tv_nsec += ms % 1000 * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&bl);
    while ((p = plug_of(id)) && running(p) && !(p->state == P_RUN && p->have_words) && !pthread_cond_timedwait(&bcv, &bl, &until)) ;
    pthread_mutex_unlock(&bl);
    return plugin_wake_state(id);
}

int plugin_wake_wait(int slot, int ms)
{
    struct timespec until; int r = -1;
    clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += ms / 1000; until.tv_nsec += ms % 1000 * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&bl);
    for (;;) {
        int st = W_CLOSED;
        for (int i = 0; i < NPLUG; i++) if (plugs[i].state == P_RUN && plugs[i].ws[slot].state != W_CLOSED) st = plugs[i].ws[slot].state;
        if (st == W_OPEN) { r = 0; break; }
        if (st != W_OPENING || pthread_cond_timedwait(&bcv, &bl, &until)) break;
    }
    pthread_mutex_unlock(&bl);
    return r;
}

int plugin_wake_words(const char *id, struct plugin_word *out, int max)
{
    const struct ps_plugin *sp; struct ps_item *items; struct plug *p; char list[33] = "", ids[16][41]; int nids = -1, n = 0;
    pthread_mutex_lock(&bl);
    if ((p = plug_of(id)) && p->state == P_RUN && p->have_words) { nids = p->nwords; memcpy(ids, p->words, sizeof ids); snprintf(list, sizeof list, "%s", p->words_list); }
    pthread_mutex_unlock(&bl);
    if (!(items = malloc(sizeof *items * PS_ITEMS))) return 0;
    ps_lock();
    if ((sp = ps_find(id))) {
        for (int s = 0; s < sp->m.nsettings && !list[0]; s++) if (sp->m.settings[s].type == PS_FILES) snprintf(list, sizeof list, "%s", sp->m.settings[s].name);
        for (int k = 0, ni = ps_items(sp, list, items, PS_ITEMS); k < ni && n < max; k++) {
            int in = nids < 0;                              /* down: every item it once said it can use */
            for (int w = 0; w < nids; w++) in |= !strcmp(ids[w], items[k].id);
            if (!in || !items[k].checked) continue;
            snprintf(out[n].id, sizeof out[n].id, "%s", items[k].id);
            snprintf(out[n].title, sizeof out[n].title, "%s", items[k].title[0] ? items[k].title : items[k].id);
            n++;
        }
    }
    ps_unlock();
    free(items);
    return n;
}

int plugin_wake_engines(char (*ids)[25], int max, int fresh)
{
    const struct ps_plugin *list; int n = 0, np;
    /* as last scanned (an install, a removal), or scanned now: the core asks under its lock, the store's comes after
     * it */
    ps_lock(); np = fresh ? ps_scan(&list) : ps_list(&list);
    for (int i = 0; i < np && n < max; i++) if (list[i].m.caps & PM_CAPBIT(PM_CAP_WAKE)) snprintf(ids[n++], 25, "%s", list[i].m.id);
    ps_unlock();
    return n;
}

/* The self test waits for the wake word plugin that listened under the version before (its `ready` mark) unless the
 * owner chose that it does not run: plugins switched off (the setting, or PLUGINS=0 in hassmic.conf), the plugin
 * switched off on its card, or withdrawn by the plugin index (the list is the same for both builds; "run it anyway"
 * runs it, and then it is waited for).  None of that starts, so its WAKE_READY never comes, and a hassmic installed
 * while it was so never became the fallback (a review of 61028a8).  Everything else that keeps it down is the new
 * build's doing and holds the update back (2.6: an update that breaks a working wake word is a bad update): the
 * launcher missing, silent or ended, a kernel without the sandbox, a manifest this build does not read, a combination
 * it refuses, an acceptance it does not read.  913ec8d excused all of them with the owner's choices (why_none), and a
 * build whose launcher never answered passed in 1 s while Amazon's wake word listened (a review of 913ec8d).  An
 * update never makes a plugin one this build cannot speak with (a scope only adds, sdk/PROTOCOL.md "Versions"); a
 * fall back to an older hassmic can (the plugin requires a scope version this one has not: `newer`), and then this
 * build is the older one, whose self test does not wait for what it cannot run: excused, its card says why, and it
 * runs again under the build that has it.  Not installed: nothing to wait for.  Community plugins off is the owner's choice for a plugin that is not official, but
 * not for one whose listing this build does not read (5.2's
 * build of the new index key alone, the plugin still listed by the old one): that is the new build's doing, and an
 * update that turned the official wake word engine into a file's and stopped it was promoted with no wake word at
 * all (a review of 3c5d7ff, R2).  Failed, the self test only keeps this build from becoming the factory copy, the
 * one the Echo falls back to (no ota_healthy): nothing goes back, the build runs on (boot.sh steps back only after
 * failed starts, main.sh after a satellite that keeps ending).  Its own first look at the index, within a minute of
 * the start (plugin_fetch.c's relisting schedule), takes the new key's listing and the plugin runs again, and the
 * self test, which goes on asking (main.c selftest_thread), passes then.  Only where the plugin ran as listed (the
 * mark's "listed 1", ready_note): one that ran as a file's before, with community plugins on, is the owner's choice
 * again (a review of 3fcd319, S1 and S8).
 * LATE: asked after the self test's window, for the plugin it was held for: a mark that was there then and is gone
 * now is no answer (a removal takes the plugin with it: !sp; a review of 9cd7a77, T1) */
int plugin_selftest_ok(const char *id, int late)
{
    char path[400], line[64]; FILE *f; struct plug *p; const struct ps_plugin *sp; int was, ready, chosen, as_listed = 1;
    if (!id || !id[0]) return 1;
    snprintf(path, sizeof path, "%s/%s/ready", ps_dir(), id);
    /* (a mark without the line, a build's before 2026-11-22: as listed, the rule it was written under) */
    pthread_mutex_lock(&rmu);
    if ((was = (f = fopen(path, "r")) != NULL)) { while (fgets(line, sizeof line, f)) if (!strcmp(line, "listed 0\n")) as_listed = 0; fclose(f); }
    pthread_mutex_unlock(&rmu);
    pthread_mutex_lock(&bl); ready = (p = plug_of(id)) && p->wake_ready; chosen = conf_off || !feature; pthread_mutex_unlock(&bl);
    /* (a listing unread here that says "community" is the owner's switch whatever the mark says: a mark without the
     * line, or a "listed 1" of 9cd7a77's, which took any listing that held then for an official one, held every
     * update of a community wake word plugin once community plugins were off, and with them off it never runs to
     * write its mark anew: the other plugins stopped at every start (a review of 1d704e9, U2)) */
    ps_lock(); sp = ps_find(id);
    chosen = chosen || !sp || !sp->enabled || (sp->withdrawn[0] && !sp->anyway) || sp->m.newer
             || (sp->community_off && (!sp->listing_unread || !as_listed || sp->listing_said == PS_COMMUNITY));
    ps_unlock();
    return ready || (!was && !late) || chosen;     /* never ready on this Echo (not installed, broken already), or off by the owner's word: it cannot hold updates back */
}

/* "plugin:<id>/<word>" */
static int model_split(const char *model, char *id, size_t idcap, char *word, size_t wcap)
{
    const char *slash;
    if (strncmp(model, "plugin:", 7) || !(slash = strchr(model + 7, '/')) || (size_t)(slash - model - 7) >= idcap || strlen(slash + 1) >= wcap) return -1;
    snprintf(id, idcap, "%.*s", (int)(slash - model - 7), model + 7); snprintf(word, wcap, "%s", slash + 1);
    return 0;
}

/* The capture thread (main() before it runs).  Sends WAKE_OPEN and returns: the answer comes on the broker's
 * thread.  0 when that plugin runs and has listed the word */
static int wp_open(int slot, const char *model, wake_cb cb)
{
    char id[PS_ID_MAX], word[41]; struct plug *p; uint8_t b[96]; struct pm_w w; int r = -1, fd = -1, sv[2], listed = 0;
    if (slot < 0 || slot >= WAKE_SLOTS || model_split(model, id, sizeof id, word, sizeof word)) return -1;
    pthread_mutex_lock(&bl);
    wake_user_cb = cb;
    if ((p = plug_of(id)) && p->state == P_RUN && p->have_words) {
        for (int i = 0; i < p->nwords; i++) listed |= !strcmp(p->words[i], word);
        if (listed) {
            struct pm_wake_open m = { (uint8_t)slot, pm_s(word) };
            if (p->stream < 0 && !socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
                int size = 65536;                           /* 2 s of the stream: a plugin that falls behind loses blocks */
                setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size); fcntl(sv[0], F_SETFL, O_NONBLOCK);
                p->stream = sv[0]; fd = sv[1]; p->stream_sent = 0;
            }
            pm_winit(&w, b, sizeof b); pm_put_wake_open(&w, 0, &m);
            if (p->stream >= 0 && !psend(p, &w, p->stream_sent ? -1 : fd)) {
                p->stream_sent = 1; p->ws[slot].state = W_OPENING; p->ws[slot].asked = now_ms(); p->ws[slot].heard = 0;
                snprintf(p->ws[slot].word, sizeof p->ws[slot].word, "%s", word);
                p->why[0] = 0; r = 0;
            }
            if (fd >= 0) close(fd);
            if (r && !p->stream_sent && p->stream >= 0) { close(p->stream); p->stream = -1; }
        }
    }
    pthread_mutex_unlock(&bl);
    return r;
}

/* The capture thread: one block, once per round whatever the slots (one stream a plugin), never waited for.  Not
 * while muted, by the button or by Home Assistant: the positions show the gap.  hassmic's own engine is fed through a
 * soft mute (only the latch cuts the microphones); a plugin is not */
static void wp_feed(int slot, const int16_t *s, size_t n)
{
    uint64_t pos = wake_fed(); uint8_t head[8]; struct iovec iov[2] = { { head, 8 }, { NULL, 0 } }; struct msghdr h = { .msg_iov = iov, .msg_iovlen = 2 };
    int muted = core_muted();
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i];
        if (p->state != P_RUN || p->ws[slot].state == W_CLOSED || p->ws[slot].state == W_FAILED || p->stream < 0 || p->sent_pos == pos) continue;
        p->sent_pos = pos; p->pos = pos + n;
        if (muted) continue;
        /* PM_WAKE_SAMPLES_MAX a packet, each with its own position: the protocol's bound, which a reader sizes its
         * buffer by (the capture's block is the backend's, and nothing held it to the C client's 4096; a review of
         * 6631974, 9) */
        for (size_t off = 0; off < n; off += PM_WAKE_SAMPLES_MAX) {
            size_t len = n - off < PM_WAKE_SAMPLES_MAX ? n - off : PM_WAKE_SAMPLES_MAX;
            for (int k = 0; k < 8; k++) head[k] = (uint8_t)((pos + off) >> (8 * k));
            iov[1].iov_base = (void *)(s + off); iov[1].iov_len = len * 2;
            if (sendmsg(p->stream, &h, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) p->blocks_lost++;
        }
    }
    pthread_mutex_unlock(&bl);
}

static void wp_tell(int slot, int close_it)
{
    uint8_t b[16]; struct pm_w w;
    pthread_mutex_lock(&bl);
    for (int i = 0; i < NPLUG; i++) {
        struct plug *p = &plugs[i];
        if (p->ws[slot].state == W_CLOSED) continue;
        pm_winit(&w, b, sizeof b);
        if (close_it) { struct pm_wake_close m = { (uint8_t)slot }; pm_put_wake_close(&w, 0, &m); p->ws[slot].state = W_CLOSED; }
        else pm_put_wake_reset(&w, 0);
        if (p->state == P_RUN) psend(p, &w, -1);
    }
    pthread_mutex_unlock(&bl);
}
static void wp_reset(int slot) { wp_tell(slot, 0); }
static void wp_close(int slot) { wp_tell(slot, 1); }
static void wp_property(const char *name, int value) { (void)name; (void)value; }      /* Pryon's threshold hints: no plugin has them */
static int  wp_afe_times(int slot, long *start, long *end) { (void)slot; (void)start; (void)end; return 0; }    /* nor the front end's marks: the core's own SNR score */

const struct wake_engine wake_plugin = { wp_open, wp_feed, wp_reset, wp_property, wp_close, wp_afe_times };

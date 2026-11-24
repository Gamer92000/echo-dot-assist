/* The broker (src/hassmic/plugin.c) without hassmic around it, for tests/unit/plugin_broker_test.c and
 * tests/unit/plugfuzz.c: the file itself, so that its message(), timers() and the rest are in reach, the few things
 * it calls of the core as stand-ins, a clock the test can move, and a stand-in plugin: installed in a state folder
 * of the test's own (a manifest the store reads, an item with a file), with an entry whose control socket's other
 * end the test holds.  Nothing here runs a process: what a plugin would send is what the test hands to message(). */
#ifndef PLUGIN_HARNESS_H
#define PLUGIN_HARNESS_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

/* the broker's clock is CLOCK_MONOTONIC: the test's runs ahead of the real one by h_skew_ms */
static long long h_skew_ms;
static int h_clock_gettime(clockid_t c, struct timespec *t)
{
    int r = clock_gettime(c, t);
    if (!r && c == CLOCK_MONOTONIC) { t->tv_sec += h_skew_ms / 1000; t->tv_nsec += h_skew_ms % 1000 * 1000000L; if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; } }
    return r;
}
#define clock_gettime h_clock_gettime
/* the next h_thread_fail threads plugin.c makes are not made (EAGAIN): a broker that could not start one */
static atomic_int h_thread_fail;
static int h_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg)
{
    if (atomic_load(&h_thread_fail) > 0) { atomic_fetch_sub(&h_thread_fail, 1); return EAGAIN; }
    return pthread_create(t, a, f, arg);
}
#define pthread_create h_pthread_create
#ifndef VERSION
#define VERSION "test"
#endif
/* what another thread does while start() has bl let go for the program (its hash): another start, say (P2 of a
 * review of 7be9343) */
static void (*h_between)(const char *id);
#define PLUGIN_TEST_BETWEEN(id) do { if (h_between) h_between(id); } while (0)
/* ... and while HELLO's answer has bl let go for the core's reads (WELCOME's clock, zone and names) */
static void (*h_hello)(const char *id);
#define PLUGIN_TEST_HELLO(id) do { if (h_hello) h_hello(id); } while (0)
/* pm_droppable as the test says (h_nodrop: a message marked drop taken for one that must arrive): psend's answer to
 * a packet pm_fit leaves unsent on a message that must arrive, which no header tools/mkplugmsg.py makes can have
 * (it refuses `>-` there), so only this way can the test reach it */
#include "plugmsg.h"
static int h_nodrop;
#define pm_droppable(t) (!h_nodrop && pm_droppable(t))
#include "../../src/hassmic/plugin.c"
#undef clock_gettime
#undef pthread_create

/* ---- the core's side */
/* phase 4: what the broker asks of the core and the backend, as stand-ins that say what they were asked.
 * h_music: the core's word per music source (1 on), h_volume: the volume set, h_entity / h_event: what was said to
 * Home Assistant, h_listed: the links closed for a new list; the plugin stream's bytes in h_played */
pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
static int h_music[8], h_sources, h_volume = -1, h_listed, h_music_told;
static unsigned h_entity, h_entities; static char h_event[40];
static long long h_played;
static const struct music_source *h_src[8];
int music_register(const struct music_source *s) { if (h_sources == 8) return -1; h_src[h_sources] = s; return h_sources++; }
void core_music(int source, int on) { if (source >= 0 && source < 8) h_music[source] = on; }
static int h_volumes;            /* how often the core's volume was set */
static int h_volume_echo;        /* core_set_volume tells the plugins as main.c's does (tell_plugins) */
void core_set_volume(int v) { h_volume = v; h_volumes++; if (h_volume_echo) plugin_core_state(cs_state, cs_muted, v); }
void core_plugin_entity(unsigned key, const char *type) { h_entity = key; h_entities++; snprintf(h_event, sizeof h_event, "%s", type ? type : ""); }
void core_plugin_entities_changed(void) { h_listed++; }
void core_plugin_music_changed(void) { h_music_told++; }
int plug_open(unsigned rate, unsigned ch) { (void)rate; (void)ch; return 0; }
int plug_write(const void *d, size_t n) { (void)d; h_played += (long long)n; return 0; }
long long plug_queued_us(void) { return 0; }
void plug_close(void) { }
long long btout_latency_us(void) { return 0; }
int duck_active(void) { return 0; }
float settings_duck_gain(void) { return 1; }
void music_duck(int16_t *s, size_t frames, unsigned rate, float *g, float target) { (void)s; (void)frames; (void)rate; (void)g; (void)target; }
void musiclight_feed(const int16_t *pcm, size_t frames, int channels, unsigned rate, long long delay_us) { (void)pcm; (void)frames; (void)channels; (void)rate; (void)delay_us; }
/* phase 5: the core's side of calls (call.c itself is linked: its claim and media are the real ones), the Voip
 * stream nowhere.  h_call_*: what the core was told last (the broker's teller thread tells it); h_refuse_in/out: why a
 * call cannot run (core_call_refusal) */
static atomic_int h_call_state = -1, h_call_who, h_calls;
static char h_call_peer[120];
static const char *h_refuse_in, *h_refuse_out;
void core_call(int who, int st, const char *peer, int outgoing) { (void)outgoing; snprintf(h_call_peer, sizeof h_call_peer, "%s", peer); atomic_store(&h_call_who, who); atomic_store(&h_call_state, st); atomic_fetch_add(&h_calls, 1); }
const char *core_call_refusal(int incoming) { return incoming ? h_refuse_in : h_refuse_out; }
void core_call_listen(int on) { (void)on; }
int core_own_sound(void) { return 0; }
int voip_open(unsigned rate, unsigned ch) { (void)rate; (void)ch; return 0; }
int voip_write(const void *d, size_t n) { (void)d; (void)n; usleep(20000); return 0; }
long long voip_queued_us(void) { return 0; }
void voip_close(void) { }
int callmic_present(void) { return 0; }
int callmic_open(void) { return -1; }
int callmic_read(const void **d) { (void)d; return -1; }
void callmic_close(void) { }
const char *core_name = "Echo Test";
const char *core_node_name(void) { return "echo-test"; }
int core_muted(void) { return 0; }
int clock_zone(char *out, size_t cap) { snprintf(out, cap, "Europe/Zurich"); return 1; }
int clock_synced(void) { return 1; }
uint64_t wake_fed(void) { return 0; }
/* h_marked: whose message the crash mark would name (plugin.c `handling`) while the core is in its own code: -1 */
/* h_core: on whose behalf the core is at work then (plugin.c `in_core`), for the mark of the second kind */
static int h_changed, h_marked = -2, h_core = -2;
static int h_heard, h_fault;            /* h_fault: the core's wake path faults (a test of the crash mark) */
void core_wake_plugin_changed(const char *id) { (void)id; h_changed++; h_marked = handling; h_core = in_core; if (h_fault == 2) raise(SIGSEGV); }
__attribute__((unused)) static void h_wake_cb(int slot, const char *word, uint64_t begin, uint64_t end)
{
    /* what the core does with them: positions on the stream's clock, a word of the slot's */
    if (slot < 0 || slot >= WAKE_SLOTS || begin >= end || !word[0]) abort();
    h_heard++; h_marked = handling; h_core = in_core;
    if (h_fault == 1) raise(SIGSEGV);
}

/* ---- the stand-in plugin */
#define H_ID "standin"
static char h_dir[200];
static int h_peer = -1;                 /* the plugin's end of the control socket */

static int h_rm(const char *p, const struct stat *s, int t, struct FTW *f) { (void)s; (void)t; (void)f; return remove(p); }
static void h_cleanup(void) { if (h_dir[0]) nftw(h_dir, h_rm, 16, FTW_DEPTH | FTW_PHYS); }

static void h_file(const char *rel, const char *text)
{
    char p[400], *slash; FILE *f;
    snprintf(p, sizeof p, "%s/%s", h_dir, rel);
    for (slash = p + strlen(h_dir) + 1; (slash = strchr(slash, '/')); slash++) { *slash = 0; mkdir(p, 0700); *slash = '/'; }
    if ((f = fopen(p, "w"))) { fputs(text, f); fclose(f); }
}

/* a plugin installed and accepted in the test's state folder (the store reads it as any: no signature is asked for
 * what is installed already).  WAKE: a wake word plugin with a file list; else one with nothing but a text row */
static void h_add_full(const char *id, const char *caps, const char *rows, const char *program);
static void h_add_program(const char *id, int wake, const char *program)
{
    static const char models[] = "{ \"name\": \"models\", \"label\": \"Models\", \"type\": \"files\", \"item\": { \"by\": \"stem\", \"files\": [ { \"suffix\": \".model\", \"max_kb\": 16 } ] },"
        "                    \"fields\": [ { \"name\": \"level\", \"label\": \"Level\", \"type\": \"int\", \"min\": 0, \"max\": 9 } ] },";
    h_add_full(id, wake ? "\"wake\": true, \"files\": 1" : "\"files\": 1", wake ? models : "", program);
}
/* the "requires" a manifest of capabilities CAPS (their text, as in the manifest) has: core and each of their scopes,
 * at 1 (sdk/PROTOCOL.md "Versions"), without its braces */
static const char *h_reqs(const char *caps)
{
    static char out[700]; char w[4000]; struct json j = { 0 }; size_t o;
    o = (size_t)snprintf(out, sizeof out, "\"core\": 1");
    snprintf(w, sizeof w, "{ %s }", caps);
    if (!json_parse(&j, w, strlen(w)))
        for (int k = json_first(&j, 0); k >= 0; ) {
            int v = j.t[k].next; char name[40];
            if (v >= j.t[0].next) break;
            if (json_str(&j, k, name, sizeof name)) for (int b = 0; ps_cap_name(b); b++) if (!strcmp(name, ps_cap_name(b))) o += (size_t)snprintf(out + o, sizeof out - o, ", \"%s\": 1", name);
            k = json_next(&j, 0, v);
        }
    json_free(&j);
    return out;
}
/* any capabilities (CAPS, the manifest's text) and rows (ROWS, ending in a comma, before the stand-in's own text row) */
static int h_mem = 16;                  /* the manifests' memory_mb (what a place counts, plugin.c run_mem) */
static void h_add_full(const char *id, const char *caps, const char *rows, const char *program)
{
    static const char man[] =
        "{ \"id\": \"%s\", \"name\": \"Stand-in\", \"version\": 1, \"requires\": { %s }, \"author\": \"the tests\", \"memory_mb\": %d,"
        "  \"author_key\": \"0000000000000000000000000000000000000000000000000000000000000000\","
        "  \"digest\": \"%s\","
        "  \"capabilities\": { %s },"
        "  \"settings\": [ %s"
        "                  { \"name\": \"note\", \"label\": \"A note\", \"type\": \"text\", \"default\": \"hello\" } ] }";
    /* PROGRAM: what the launcher would run: nothing here does, but the broker holds it against the manifest's
     * digest at every start (plugin.c program_is) */
    char text[2000], rel[120], dg[65]; static struct ps_manifest m; uint8_t pd[32];
    crypto_blake2b(pd, 32, (const uint8_t *)program, strlen(program));
    for (int i = 0; i < 32; i++) sprintf(dg + 2 * i, "%02x", pd[i]);
    snprintf(text, sizeof text, man, id, h_reqs(caps), h_mem, dg, caps, rows);
    snprintf(rel, sizeof rel, "plugins/%s/pkg/manifest.json", id);
    h_file(rel, text);
    snprintf(rel, sizeof rel, "plugins/%s/pkg/plugin", id);
    h_file(rel, program);
    if (ps_manifest_parse(text, strlen(text), &m, NULL, 0)) abort();
    for (int i = 0; i < 32; i++) sprintf(dg + 2 * i, "%02x", m.accepted[i]);
    snprintf(text, sizeof text, "id %s\nkey %064d\ndigest %s\nhow the test\n", id, 0, dg);
    snprintf(rel, sizeof rel, "plugins/%s/accepted", id); h_file(rel, text);
    snprintf(rel, sizeof rel, "plugins/%s/enabled", id); h_file(rel, "1\n");
}

static void h_add(const char *id, int wake) { h_add_program(id, wake, "the stand-in's program\n"); }

/* a state folder with the stand-in plugin, one checked item with a file */
static void h_install(void)
{
    const struct ps_plugin *l;
    for (int i = 0; i < NPLUG; i++) plugs[i].ctl = plugs[i].err = plugs[i].stream = plugs[i].slot = -1;     /* as plugin_start leaves them */
    snprintf(h_dir, sizeof h_dir, "/tmp/plugin_harness.%d", (int)getpid()); mkdir(h_dir, 0700); setenv("HASSMIC_STATE", h_dir, 1);
    atexit(h_cleanup);
    h_add(H_ID, 1);
    h_file("plugins/" H_ID "/files/models/word/file.model", "MODEL Word\n");
    h_file("plugins/" H_ID "/files/models/word/title", "Word\n");
    h_file("plugins/" H_ID "/files/models/word/checked", "");
    ps_lock(); ps_scan(&l); ps_unlock();
}

/* entry 0 as a plugin in STATE with CAPS, its control socket a pair whose other end is h_peer */
static struct plug *h_plug(int state, pm_caps_t caps)
{
    struct plug *p = &plugs[0]; int sv[2];
    if (p->ctl >= 0) close(p->ctl);
    if (p->stream >= 0) close(p->stream);
    if (h_peer >= 0) close(h_peer);
    memset(p, 0, sizeof *p);
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) abort();
    fcntl(sv[0], F_SETFL, O_NONBLOCK); fcntl(sv[1], F_SETFL, O_NONBLOCK);
    snprintf(p->id, sizeof p->id, H_ID);
    p->ctl = sv[0]; h_peer = sv[1]; p->err = p->stream = -1; p->slot = 0; p->state = state; p->caps = caps; p->want = 1;
    /* (what it speaks: core and the scope of each capability, at 1, as its manifest's requires would say) */
    p->ver[PM_SCOPE_CORE] = 1;
    for (int s = 1; s < PM_SCOPES; s++) if (caps & PM_CAPBIT(pm_scope_cap(s))) p->ver[s] = 1;
    p->since = now_ms(); p->ping_at = btime + PING_MS; p->sent_pos = UINT64_MAX;
    if (!++gen_next) gen_next = 1;
    p->gen = gen_next;
    { const struct ps_plugin *sp; ps_lock(); if ((sp = ps_find(H_ID))) memcpy(p->mdigest, sp->m.manifest_digest, 32); ps_unlock(); }
    return p;
}

/* entry P as a process of plugin ID, started under the package the store has installed now (start(): its manifest's
 * digest, which storage and files are held to) */
static void h_as(struct plug *p, const char *id)
{
    const struct ps_plugin *sp;
    snprintf(p->id, sizeof p->id, "%s", id);
    memset(p->mdigest, 0, 32);
    ps_lock(); if ((sp = ps_find(id))) memcpy(p->mdigest, sp->m.manifest_digest, 32); ps_unlock();
}

/* what the broker sent the plugin since the last call: the count, and the last message's type (descriptors that came
 * along are closed by the kernel: no room is given for them) */
static int h_drain(int *last)
{
    uint8_t b[PM_MAX_PACKET]; ssize_t r; int n = 0;
    while ((r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) > 0) { n++; if (last && r >= 2) *last = b[0] | b[1] << 8; }
    return n;
}
#endif

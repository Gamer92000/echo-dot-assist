/* The Echo's 802.15.4 radio for Home Assistant: radio.h says what runs in each mode.  This side only forwards: root
 * owns every process that holds the radio (main.sh radio_watch, from state/config's radio= and thread_role=), and
 * hassmic forwards TCP from the addresses it lets in to the unix sockets those processes listen on, in root's radio
 * folder (RADIO_DIR, the radio's user's, group-readable for hassmic, which runs with that group):
 *   rest.sock   otbr-agent's REST API (--rest-listen-address): never on a TCP port of its own, since whoever reaches it
 *               reads the network's key (GET /node/dataset/active) and can replace the network.  Forwarded on
 *               RADIO_REST_PORT in the border router role only: that is what Home Assistant's OTBR integration is for
 *   rcp.sock    rcpbridge (src/tools/rcpbridge.c): Amazon's RCP for one host.  Forwarded on RADIO_RCP_PORT in the
 *               remote roles, one connection at a time (the radio is one thing for one host)
 * Who is let in: the addresses of Home Assistant's links (proto->peers: the keyed ones once Home Assistant set the
 * device key) and the one the owner names on the page (state/radio "allow <IPv4>": Zigbee2MQTT or a border router on
 * another machine).  Both ports are in 16384-32767, admitted from anywhere by the firewall (lockdown.sh keep): the
 * check is here, at accept, before a byte goes either way.
 * Why not a process of hassmic's own for the radio: a long-lived child of hassmic's makes Amazon's DHA module (davs.c)
 * wait for good, and two holders of the radio (ours and the border router's) must never meet: root starts one only
 * after the other ended. */
#define _GNU_SOURCE
#include "radio.h"
#include "board.h"
#include "core.h"
#include "json.h"
#include "statefile.h"
#include "netio.h"
#include "settings.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

const char *const radio_modes[RADIO_MODES] = { "off", "thread", "zigbee" };
const char *const radio_thread_roles[THREAD_ROLES] = { "border_router", "router", "remote" };
const char *const radio_zigbee_roles[ZIGBEE_ROLES] = { "coordinator", "router", "remote" };

static _Atomic int mode = RADIO_OFF, thread_role = THREAD_BR, zigbee_role = ZIGBEE_COORD;
static int wake_fd[2] = { -1, -1 };     /* a setting changed: the forwarding thread looks at its listeners again */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;     /* allow, the links; a leaf lock */
static uint32_t allow;                  /* state/radio's address (network order), 0: none */
#define REST_LINKS 4                    /* Home Assistant's integration makes a request at a time; the page's own beside it */
static int rest_links;
static _Atomic long long share_until;                  /* the ephemeral key's end (radio_share): 0 none */

static const char *dir(void) { const char *e = getenv("HASSMIC_RADIO_DIR"); return e ? e : "/data/local/hassmic/thread"; }
static void sock_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", dir(), name); }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static int port_of(int base)
{
    /* HASSMIC_RADIO_PORTS=<rest>:<rcp> for tests that run beside another Echo's ports */
    const char *e = getenv("HASSMIC_RADIO_PORTS"); int r, c;
    int z = 0;
    if (e && sscanf(e, "%d:%d:%d", &r, &c, &z) >= 2) return base == RADIO_REST_PORT ? r : base == RADIO_RCP_PORT ? c : z ? z : base;
    return base;
}

/* which radio program root runs (main.sh radio_program: "gprcp", "amazon", "fallback": ours failed this boot), from
 * root's file; "" not known (the radio off, or an older main.sh) */
static void program_now(char *o, size_t cap)
{
    const char *e = getenv("HASSMIC_RADIO_PROGRAM"); char b[16] = ""; int f = open(e ? e : "/data/local/hassmic/radio-program", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ssize_t n = f >= 0 ? read(f, b, sizeof b - 1) : 0;
    if (f >= 0) close(f);
    b[n > 0 ? n : 0] = 0; b[strcspn(b, "\r\n")] = 0;
    snprintf(o, cap, "%s", !strcmp(b, "gprcp") || !strcmp(b, "amazon") || !strcmp(b, "fallback") ? b : "");
}

/* CSL as root has it (main.sh radio_csl, its file): "on", or "unavailable <why>"; *o "" not on or not known.  The why
 * is root's words, printable ASCII only, never quotes or backslashes (it goes into JSON as is) */
static void csl_now(char *o, size_t cap, char *why, size_t wcap)
{
    const char *e = getenv("HASSMIC_RADIO_CSL"); char b[128] = ""; int f = open(e ? e : "/data/local/hassmic/radio-csl", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ssize_t n = f >= 0 ? read(f, b, sizeof b - 1) : 0;
    if (f >= 0) close(f);
    b[n > 0 ? n : 0] = 0; b[strcspn(b, "\r\n")] = 0;
    *o = *why = 0;
    if (!strcmp(b, "on")) snprintf(o, cap, "on");
    else if (!strncmp(b, "unavailable ", 12)) {
        snprintf(o, cap, "unavailable");
        size_t k = 0;
        for (const char *c = b + 12; *c && k + 1 < wcap; c++) why[k++] = *c >= 0x20 && *c < 0x7f && *c != '"' && *c != '\\' ? *c : ' ';
        why[k] = 0;
    }
}

int radio_present(void) { return board.radio_rcp || getenv("HASSMIC_RADIO_DIR"); }

static void poke(void) { if (wake_fd[1] >= 0) { char c = 1; if (write(wake_fd[1], &c, 1) < 0) {} } }
int radio_mode(int set) { if (set >= 0 && set < RADIO_MODES && set != mode) { mode = set; poke(); } return mode; }
int radio_thread_role(int set) { if (set >= 0 && set < THREAD_ROLES && set != thread_role) { thread_role = set; poke(); } return thread_role; }
int radio_zigbee_role(int set) { if (set >= 0 && set < ZIGBEE_ROLES && set != zigbee_role) { zigbee_role = set; poke(); } return zigbee_role; }

/* what the settings say runs now: the border router's REST forwarded, the radio forwarded */
static int want_rest(void) { return mode == RADIO_THREAD && thread_role == THREAD_BR; }
static int want_rcp(void) { return (mode == RADIO_THREAD && thread_role == THREAD_REMOTE) || (mode == RADIO_ZIGBEE && zigbee_role == ZIGBEE_REMOTE); }
static int want_zb(void) { return mode == RADIO_ZIGBEE && zigbee_role == ZIGBEE_COORD; }
static int zigbeed_role(void) { return mode == RADIO_ZIGBEE && zigbee_role != ZIGBEE_REMOTE; }
static int otbr_role(void) { return mode == RADIO_THREAD && thread_role != THREAD_REMOTE; }

/* ---------------------------------------------------------------- who may connect */

static void allow_load(void)
{
    char p[300], l[64] = ""; struct in_addr a;
    snprintf(p, sizeof p, "%s/radio", state_dir());
    int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);      /* not a FIFO's open that waits */
    if (fd < 0) return;
    ssize_t n = read(fd, l, sizeof l - 1); close(fd);
    if (n <= 0) return;
    l[n] = 0; l[strcspn(l, "\n")] = 0;
    if (!strncmp(l, "allow ", 6) && inet_pton(AF_INET, l + 6, &a) == 1) allow = a.s_addr;
}

static int allowed(uint32_t peer, char *why, size_t cap)
{
    uint32_t ha[8]; int n = core_ha_peers(ha, 8);
    for (int i = 0; i < n; i++) if (ha[i] == peer) { snprintf(why, cap, "Home Assistant"); return 1; }
    pthread_mutex_lock(&lk); int ok = allow && allow == peer; pthread_mutex_unlock(&lk);
    if (ok) { snprintf(why, cap, "the address named on the page"); return 1; }
    return 0;
}

/* ---------------------------------------------------------------- forwarding */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* a connection to one of root's sockets, within MS: a listener that is up but takes nobody (its backlog full: a
 * border router busy or hung) must hold neither the forwarding thread nor a page's request for longer.  Blocking
 * after it, as before.  -1: not there, or not within the time */
static int unix_connect(const char *name, int ms)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    sock_path(a.sun_path, sizeof a.sun_path, name);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), err = 0; socklen_t el = sizeof err;
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a)) {
        struct pollfd p = { s, POLLOUT, 0 };
        if ((errno != EAGAIN && errno != EINPROGRESS) || poll(&p, 1, ms) != 1
            || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(s); return -1; }
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    return s;
}

/* a line of the log at most every 10 s for what anyone on the network can cause, the rest counted */
static void refused(const char *fmt, const char *a, const char *b)
{
    static pthread_mutex_t rl = PTHREAD_MUTEX_INITIALIZER;    /* the forwarding thread and the links' */
    static long long at; static unsigned more;
    long long now = now_ms(); unsigned had;
    pthread_mutex_lock(&rl);
    if (at && now - at < 10000) { more++; pthread_mutex_unlock(&rl); return; }
    had = more; at = now; more = 0;
    pthread_mutex_unlock(&rl);
    char line[200]; snprintf(line, sizeof line, fmt, a, b);
    if (had) fprintf(stderr, "radio: %s (and %u more refused before it)\n", line, had);
    else fprintf(stderr, "radio: %s\n", line);
}

/* what is forwarded: the border router's REST (any number of requests, up to REST_LINKS at once), the radio itself and
 * the Zigbee coordinator (one host each: a second is refused, the same host again takes over from its old link) */
enum { K_REST, K_RCP, K_ZB, KINDS };
static const char *const kind_name[KINDS] = { "the border router's API", "the radio", "the Zigbee coordinator" };
static const char *const kind_sock[KINDS] = { "rest.sock", "rcp.sock", "zb.sock" };
static struct { int fd[2]; uint32_t peer; } excl[KINDS];    /* K_RCP, K_ZB: the link that holds it */

struct link { int a, b, kind; };

static int put_all(int fd, const char *p, size_t n)
{
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; p += w; n -= (size_t)w; }
    return 0;
}

/* A request a browser sent: every browser adds Origin to what a page sends elsewhere, and Sec-Fetch-* to everything
 * (Chromium, Firefox, Safari).  The border router's REST has no login: a web page in a browser on Home Assistant's
 * machine (or the address the owner named) must not reach it through the forwarding, whatever otbr's CORS says (its
 * build names no origin: otbr-build.sh).  Home Assistant's client (python-otbr-api, aiohttp) sends neither.  The
 * request's header lines are looked for across reads: TAIL keeps the end of the last one */
static int browser_said(char *tail, size_t *tn, const char *b, size_t n)
{
    static const char *const marks[] = { "\norigin:", "\nsec-fetch-" };
    char w[4096 + 32]; size_t k = *tn;
    memcpy(w, tail, k);
    size_t m = n < sizeof w - k ? n : sizeof w - k;
    for (size_t i = 0; i < m; i++) w[k + i] = (char)tolower((unsigned char)b[i]);
    k += m;
    for (size_t i = 0; i < sizeof marks / sizeof *marks; i++) {
        size_t ml = strlen(marks[i]);
        for (size_t j = 0; j + ml <= k; j++) if (!memcmp(w + j, marks[i], ml)) return 1;
    }
    *tn = k < 16 ? k : 16;
    memcpy(tail, w + k - *tn, *tn);
    return 0;
}

static void link_free(int kind)
{
    if (kind == K_REST) rest_links--;
    else { excl[kind].fd[0] = excl[kind].fd[1] = -1; excl[kind].peer = 0; }
}

/* copies both ways until either end goes, then ends both */
static void *link_thread(void *arg)
{
    struct link l = *(struct link *)arg; free(arg);
    char buf[4096], tail[16]; size_t tn = 0;
    for (;;) {
        struct pollfd p[2] = { { l.a, POLLIN, 0 }, { l.b, POLLIN, 0 } };
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        ssize_t r;
        if (p[0].revents) {
            if ((r = read(l.a, buf, sizeof buf)) <= 0) break;
            if (l.kind == K_REST && browser_said(tail, &tn, buf, (size_t)r)) { refused("the border router's API: %s%s a browser's request, dropped", "", ""); break; }
            if (put_all(l.b, buf, (size_t)r)) break;
        }
        if (p[1].revents) { if ((r = read(l.b, buf, sizeof buf)) <= 0 || put_all(l.a, buf, (size_t)r)) break; }
    }
    pthread_mutex_lock(&lk);
    link_free(l.kind);
    pthread_mutex_unlock(&lk);
    close(l.a); close(l.b);
    if (l.kind != K_REST) fprintf(stderr, "radio: %s: the host's connection ended\n", kind_name[l.kind]);
    return NULL;
}

/* a one-host link ended from here: the mode changed, or the same host came again (its reader cleans up) */
static void excl_drop(int kind)
{
    pthread_mutex_lock(&lk);
    if (excl[kind].fd[0] >= 0) { shutdown(excl[kind].fd[0], SHUT_RDWR); shutdown(excl[kind].fd[1], SHUT_RDWR); }
    pthread_mutex_unlock(&lk);
}

/* A host that went without a word (power cut, another network) is noticed within ~30 s: keepalive on an idle link,
 * a user timeout on one with data unacknowledged.  Else it held the radio (one host at a time) for good when the
 * radio was quiet, and the host's own reconnection after its reboot was refused */
static void keep_alive(int c)
{
    int one = 1, idle = 10, intvl = 5, cnt = 3; unsigned to = 20000;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#ifdef TCP_USER_TIMEOUT
    setsockopt(c, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof to);
#else
    (void)to;
#endif
}

static void take(int ls, int kind)
{
    struct sockaddr_in pa; socklen_t pl = sizeof pa; char ip[INET_ADDRSTRLEN], why[48];
    int c = accept4(ls, (struct sockaddr *)&pa, &pl, SOCK_CLOEXEC);
    if (c < 0) return;
    inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
    if (pa.sin_family != AF_INET || !allowed(pa.sin_addr.s_addr, why, sizeof why)) {
        refused("%s refused from %s: neither Home Assistant nor the address named on the page", kind_name[kind], ip);
        close(c); return;
    }
    pthread_mutex_lock(&lk);
    int busy = kind == K_REST ? rest_links >= REST_LINKS : excl[kind].fd[0] >= 0;
    int same = kind != K_REST && busy && excl[kind].peer == pa.sin_addr.s_addr;
    pthread_mutex_unlock(&lk);
    if (same) {
        /* the holder again: its old connection is one it gave up (Zigbee2MQTT or Home Assistant restarted, a
         * reboot): the new one takes over (rcpbridge gives it a fresh RCP; zigbeed a fresh host session) */
        fprintf(stderr, "radio: %s: %s connected again, its old connection ends\n", kind_name[kind], ip);
        excl_drop(kind);
        for (int i = 0; i < 40 && busy; i++) {
            usleep(50000);
            pthread_mutex_lock(&lk); busy = excl[kind].fd[0] >= 0; pthread_mutex_unlock(&lk);
        }
    }
    if (busy) {
        refused(kind == K_REST ? "%s refused to %s: busy" : "%s refused to %s: another host has it", kind_name[kind], ip);
        close(c); return;
    }
    int u = unix_connect(kind_sock[kind], 2000);
    if (u < 0) {
        refused("%s from %s: not up (yet)", kind_name[kind], ip);
        close(c); return;
    }
    keep_alive(c);
    struct link *l = malloc(sizeof *l); pthread_t t; pthread_attr_t at;
    if (!l) { close(c); close(u); return; }
    *l = (struct link){ c, u, kind };
    pthread_mutex_lock(&lk);
    if (kind == K_REST) rest_links++;
    else { excl[kind].fd[0] = c; excl[kind].fd[1] = u; excl[kind].peer = pa.sin_addr.s_addr; }
    pthread_mutex_unlock(&lk);
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, link_thread, l)) {
        pthread_mutex_lock(&lk); link_free(kind); pthread_mutex_unlock(&lk);
        close(c); close(u); free(l);
    } else if (kind != K_REST) fprintf(stderr, "radio: %s is %s's (%s)\n", kind_name[kind], ip, why);
    pthread_attr_destroy(&at);
}

static int listener(int port)
{
    int s = net_listen(port);
    if (s < 0) fprintf(stderr, "radio: port %d: %s\n", port, strerror(errno));
    else fcntl(s, F_SETFD, FD_CLOEXEC);
    return s;
}

static void *forward_thread(void *arg)
{
    (void)arg;
    int ls[KINDS] = { -1, -1, -1 }, announced = -1;
    static const int base[KINDS] = { RADIO_REST_PORT, RADIO_RCP_PORT, RADIO_ZB_PORT };
    for (;;) {
        int want[KINDS] = { want_rest(), want_rcp(), want_zb() }, missing = 0;
        if (want[K_ZB] != announced) { announced = want[K_ZB]; core_mdns_refresh(); }
        for (int k = 0; k < KINDS; k++) {
            if (want[k] && ls[k] < 0) ls[k] = listener(port_of(base[k]));
            if (!want[k] && ls[k] >= 0) { close(ls[k]); ls[k] = -1; }
            if (!want[k] && k != K_REST) excl_drop(k);
            missing |= want[k] && ls[k] < 0;
        }
        struct pollfd p[1 + KINDS] = { { wake_fd[0], POLLIN, 0 }, { ls[0], POLLIN, 0 }, { ls[1], POLLIN, 0 }, { ls[2], POLLIN, 0 } };
        /* a listener that could not be opened (the port in use) is asked again every 5 s */
        int r = poll(p, 1 + KINDS, missing ? 5000 : -1);
        if (r < 0 && errno != EINTR) { sleep(1); continue; }
        if (p[0].revents) { char b[16]; if (read(wake_fd[0], b, sizeof b) < 0) {} }
        for (int k = 0; k < KINDS; k++) if (ls[k] >= 0 && p[1 + k].revents) take(ls[k], k);
    }
    return NULL;
}

void radio_start(void)
{
    if (!radio_present()) return;
    for (int k = 0; k < KINDS; k++) excl[k].fd[0] = excl[k].fd[1] = -1;
    allow_load();
    if (pipe2(wake_fd, O_CLOEXEC | O_NONBLOCK)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, forward_thread, NULL)) return;
    pthread_detach(t);
    fprintf(stderr, "radio: %s%s%s\n", radio_modes[mode], mode == RADIO_THREAD ? ", " : mode == RADIO_ZIGBEE ? ", " : "",
            mode == RADIO_THREAD ? radio_thread_roles[thread_role] : mode == RADIO_ZIGBEE ? radio_zigbee_roles[zigbee_role] : "");
}

/* ---------------------------------------------------------------- the border router's REST API, for the page */

/* one request to rest.sock (HTTP/1.1, the connection closed after it), the body into out: its HTTP status, -1 when the
 * border router is not there or took longer than 5 s */
static int rest(const char *method, const char *path, const char *ctype, const char *body, char *out, size_t cap)
{
    int s = unix_connect("rest.sock", 2000); char req[1024]; size_t bl = body ? strlen(body) : 0, have = 0;
    if (s < 0) return -1;
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\nConnection: close\r\n"
                     "%s%s%sContent-Length: %zu\r\n\r\n", method, path, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "", bl);
    if (n < 0 || (size_t)n >= sizeof req || put_all(s, req, (size_t)n) || (bl && put_all(s, body, bl))) { close(s); return -1; }
    long long until = now_ms() + 5000;
    out[0] = 0;
    for (;;) {
        long long left = until - now_ms();
        struct pollfd p = { s, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) { close(s); return -1; }
        if (have + 1 >= cap) break;                 /* more than the caller wants: what came is enough */
        ssize_t r = read(s, out + have, cap - 1 - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += (size_t)r;
    }
    close(s);
    out[have] = 0;
    int status = 0;
    if (sscanf(out, "HTTP/1.%*d %d", &status) != 1) return -1;
    char *b = strstr(out, "\r\n\r\n");
    if (b) memmove(out, b + 4, strlen(b + 4) + 1); else out[0] = 0;
    return status;
}

/* the fields of the node and its dataset the page shows: never the network key, the PSKc or the mesh-local prefix */
static size_t thread_json(char *o, size_t cap)
{
    char *b = malloc(16384); struct json j = { 0 }; size_t n = 0;     /* json_parse grows j.t: from nothing */
    if (!b) return snprintf(o, cap, "null");
    int st = rest("GET", "/node", NULL, NULL, b, 16384);
    if (st != 200 || json_parse(&j, b, strlen(b))) { json_free(&j); free(b); return snprintf(o, cap, "{\"up\":false}"); }
    char state[24] = "", name[64] = "", xpan[24] = "", ename[140];
    json_str(&j, json_get(&j, 0, "state"), state, sizeof state);
    json_str(&j, json_get(&j, 0, "networkName"), name, sizeof name);
    json_str(&j, json_get(&j, 0, "extPanId"), xpan, sizeof xpan);
    long long routers = json_int(&j, json_get(&j, 0, "routerCount"), -1);
    json_free(&j);
    long long channel = -1, panid = -1;
    if (rest("GET", "/node/dataset/active", NULL, NULL, b, 16384) == 200 && !json_parse(&j, b, strlen(b))) {
        channel = json_int(&j, json_get(&j, 0, "channel"), -1);
        panid = json_int(&j, json_get(&j, 0, "panId"), -1);
    }
    json_free(&j);
    memset(b, 0, 16384);                /* the dataset's answer held the network key */
    free(b);
    for (char *c = state; *c; c++) if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = '?';
    for (char *c = xpan; *c; c++) if (!strchr("0123456789abcdefABCDEF", *c)) *c = '?';
    if (json_esc(ename, sizeof ename, name) < 0) snprintf(ename, sizeof ename, "\"\"");
    n = (size_t)snprintf(o, cap, "{\"up\":true,\"state\":\"%s\",\"network\":%s,\"extpanid\":\"%s\",\"channel\":%lld,\"panid\":%lld,\"routers\":%lld}",
                         state, *name ? ename : "null", xpan, channel, panid, routers);
    return n < cap ? n : cap - 1;
}

/* one line to zigbeed's control socket, its answer's first line into out: -1 not up */
static int zb_ctl(const char *line, char *out, size_t cap)
{
    int s = unix_connect("zb.ctl", 1000);
    if (s < 0) return -1;
    size_t have = 0;
    /* the line and its end in one write: zigbeed answers what it read and closes, so a "\n" written after the
     * line could find the socket closed (EPIPE), and the answer was thrown away: the page's "Starting…" now and then */
    char req[128];
    int rn = snprintf(req, sizeof req, "%s\n", line);
    if (rn <= 0 || (size_t)rn >= sizeof req || put_all(s, req, (size_t)rn)) { close(s); return -1; }
    for (long long until = now_ms() + 3000; have + 1 < cap; ) {
        struct pollfd p = { s, POLLIN, 0 };
        long long left = until - now_ms();
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
        ssize_t r = read(s, out + have, cap - 1 - have);
        if (r <= 0) break;
        have += (size_t)r;
        if (memchr(out, '\n', have)) break;
    }
    close(s);
    out[have] = 0;
    out[strcspn(out, "\n")] = 0;
    return have ? 0 : -1;
}

/* zigbeed's status as it says it (JSON of its own making: numbers, fixed words and hex), checked for its shape */
static void zigbee_json(char *o, size_t cap)
{
    char b[2600];
    if (zb_ctl("status", b, sizeof b) || b[0] != '{' || b[strlen(b) - 1] != '}') { snprintf(o, cap, "{\"up\":false}"); return; }
    for (char *c = b; *c; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) { snprintf(o, cap, "{\"up\":false}"); return; }
    snprintf(o, cap, "{\"up\":true,%s", b + 1);
}

size_t radio_status_json(char *out, size_t cap)
{
    char th[600] = "null", zg[2700] = "null", al[INET_ADDRSTRLEN] = "", holder[INET_ADDRSTRLEN] = "", zholder[INET_ADDRSTRLEN] = "", prog[16] = "";
    char csl[16] = "", csl_why[128] = "";
    if (!radio_present()) return (size_t)snprintf(out, cap, "{\"present\":false}");
    int m = mode, tr = thread_role, zr = zigbee_role;
    if (otbr_role()) thread_json(th, sizeof th);
    if (zigbeed_role()) zigbee_json(zg, sizeof zg);
    if (mode != RADIO_OFF) program_now(prog, sizeof prog);
    if (mode == RADIO_THREAD) csl_now(csl, sizeof csl, csl_why, sizeof csl_why);
    pthread_mutex_lock(&lk);
    if (allow) inet_ntop(AF_INET, &allow, al, sizeof al);
    if (excl[K_RCP].peer) inet_ntop(AF_INET, &excl[K_RCP].peer, holder, sizeof holder);
    if (excl[K_ZB].peer) inet_ntop(AF_INET, &excl[K_ZB].peer, zholder, sizeof zholder);
    pthread_mutex_unlock(&lk);
#define Q(x) *x ? "\"" : "", *x ? x : "null", *x ? "\"" : ""
    int n = snprintf(out, cap, "{\"present\":true,\"mode\":\"%s\",\"thread_role\":\"%s\",\"zigbee_role\":\"%s\",\"rest_port\":%d,"
                     "\"rcp_port\":%d,\"zb_port\":%d,\"rest\":%s,\"rcp\":%s,\"zb\":%s,\"allow\":%s%s%s,\"holder\":%s%s%s,"
                     "\"zb_holder\":%s%s%s,\"share_left\":%lld,\"program\":%s%s%s,\"csl\":%s%s%s,\"csl_why\":%s%s%s,"
                     "\"thread\":%s,\"zigbee\":%s}",
                     radio_modes[m], radio_thread_roles[tr], radio_zigbee_roles[zr], port_of(RADIO_REST_PORT), port_of(RADIO_RCP_PORT),
                     port_of(RADIO_ZB_PORT), want_rest() ? "true" : "false", want_rcp() ? "true" : "false", want_zb() ? "true" : "false",
                     Q(al), Q(holder), Q(zholder), share_until > now_ms() ? (share_until - now_ms() + 999) / 1000 : 0LL, Q(prog), Q(csl),
                     Q(csl_why), th, zg);
#undef Q
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

const char *radio_mdns_service(void)
{
    if (wake_fd[0] < 0) {
        /* -S, main.sh's service file before hassmic runs: the settings as state/config has them */
        char m[16] = "", z[16] = "";
        settings_peek("radio", m, sizeof m); settings_peek("zigbee_role", z, sizeof z);
        if (!strcmp(m, "zigbee")) { mode = RADIO_ZIGBEE; zigbee_role = !*z || !strcmp(z, "coordinator") ? ZIGBEE_COORD : !strcmp(z, "router") ? ZIGBEE_ROUTER : ZIGBEE_REMOTE; }
    }
    /* Home Assistant's ZHA discovers network coordinators by this type, and takes the radio type from the TXT record
     * (its config flow's zeroconf step: socket://<address>:<port>); Zigbee2MQTT's mdns:// finds it too */
    static char svc[400];
    if (!radio_present() || !want_zb()) return "";
    snprintf(svc, sizeof svc, "  <service>\n    <type>_zigbee-coordinator._tcp</type>\n    <port>%d</port>\n"
             "    <txt-record>radio_type=deconz</txt-record>\n    <txt-record>name=Echo 4</txt-record>\n"
             "    <txt-record>baud_rate=115200</txt-record>\n  </service>\n", port_of(RADIO_ZB_PORT));
    return svc;
}

/* ---------------------------------------------------------------- the page's actions */

/* a dataset as Thread writes it (TLVs: type, length, value); it must name the network (channel, PAN ID, extended PAN
 * ID, network key, mesh-local prefix) and be well formed: what the border router would refuse is refused here with
 * words a person reads */
static int dataset_ok(const uint8_t *t, size_t n, char *err, size_t errsz)
{
    unsigned have = 0;
    if (n < 2 || n > 254) { snprintf(err, errsz, "That is not a Thread dataset (its length)"); return -1; }
    for (size_t i = 0; i < n; ) {
        if (i + 2 > n || i + 2 + t[i + 1] > n) { snprintf(err, errsz, "That is not a Thread dataset (cut off)"); return -1; }
        uint8_t ty = t[i], l = t[i + 1];
        if ((ty == 0 && l != 3) || (ty == 1 && l != 2) || (ty == 2 && l != 8) || (ty == 5 && l != 16) || (ty == 7 && l != 8)) {
            snprintf(err, errsz, "That is not a Thread dataset (a field of the wrong size)"); return -1;
        }
        if (ty < 32) have |= 1u << ty;
        i += 2u + l;
    }
    unsigned need = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 7;
    if ((have & need) != need) { snprintf(err, errsz, "That dataset lacks the network's channel, PAN ID, extended PAN ID, key or mesh-local prefix"); return -1; }
    return 0;
}

static int rest_ok(int st, const char *what, char *err, size_t errsz)
{
    if (st >= 200 && st < 300) return 0;
    if (st < 0) snprintf(err, errsz, "The border router does not answer (it may still be starting): try again in a moment");
    else snprintf(err, errsz, "The border router refused to %s (HTTP %d)", what, st);
    return st < 0 ? -2 : -1;
}

/* The ephemeral key (Thread 1.4's ePSKc): a 9-digit code that lets a phone's Thread app (Google Home, Apple Home)
 * take the network's credentials from the border agent for RADIO_SHARE_S, over a DTLS port of its own (28938, in the
 * range lockdown.sh admits; the border agent's own: 28937, otbr-build.sh).  POST /node/ba-epskc/key on REST; the code
 * goes to the page sealed (web.c): whoever reads it in that time joins the network's commissioning.  One at a time;
 * share_until is when it ends (0: none), the page's countdown */
#define RADIO_SHARE_S 120
#define RADIO_SHARE_PORT 28938

int radio_share(int on, char code[16], char *err, size_t errsz)
{
    char b[1024];
    if (!want_rest()) { snprintf(err, errsz, "Only the border router shares the network"); return -1; }
    if (!on) {
        int st = rest("DELETE", "/node/ba-epskc/key", NULL, NULL, b, sizeof b);
        share_until = 0;
        return st >= 200 && st < 300 ? 0 : rest_ok(st, "end the code", err, errsz);
    }
    char body[64];
    snprintf(body, sizeof body, "{\"lifetime\":%d,\"port\":%d}", RADIO_SHARE_S * 1000, RADIO_SHARE_PORT);
    int st = rest("POST", "/node/ba-epskc/key", "application/json", body, b, sizeof b);
    if (st == 409 || st == 503) {          /* the feature off, or a key running: on, and again */
        rest("PUT", "/node/ba-epskc/state", "application/json", "\"enable\"", b, sizeof b);
        rest("DELETE", "/node/ba-epskc/key", NULL, NULL, b, sizeof b);
        st = rest("POST", "/node/ba-epskc/key", "application/json", body, b, sizeof b);
    }
    if (rest_ok(st, "share the network", err, errsz)) return -1;
    struct json j = { 0 }; char tap[16] = "";
    if (json_parse(&j, b, strlen(b)) || json_str(&j, json_get(&j, 0, "tap"), tap, sizeof tap) < 0) { json_free(&j); snprintf(err, errsz, "The border router's answer has no code"); return -1; }
    json_free(&j);
    for (char *c = tap; *c; c++) if (*c < '0' || *c > '9') { snprintf(err, errsz, "The border router's answer has no code"); return -1; }
    if (strlen(tap) != 9) { snprintf(err, errsz, "The border router's answer has no code"); return -1; }
    memcpy(code, tap, 10);
    share_until = now_ms() + RADIO_SHARE_S * 1000LL;
    fprintf(stderr, "radio: the network shared for %d s (an ephemeral key)\n", RADIO_SHARE_S);
    memset(tap, 0, sizeof tap); memset(b, 0, sizeof b);
    return 0;
}

/* the border router's network forgotten (DELETE /node: detach, erase what it keeps, the host reset in place), then
 * its answers again after the reset */
static int forget(char *b, size_t cap, char *err, size_t errsz)
{
    int st = rest("DELETE", "/node", NULL, NULL, b, cap), rc = rest_ok(st, "forget its network", err, errsz);
    if (rc) return rc;
    for (long long until = now_ms() + 15000; now_ms() < until; usleep(200000))
        if (rest("GET", "/node/state", NULL, NULL, b, cap) == 200) return 0;
    return rest_ok(-1, "", err, errsz);
}

int radio_action(const char *what, const char *arg, const uint8_t *tlv, size_t n, char *err, size_t errsz)
{
    char b[4096]; int rc;
    err[0] = 0;
    if (!radio_present()) { snprintf(err, errsz, "This Echo has no Thread or Zigbee radio"); return -1; }
    if (!strcmp(what, "allow")) {
        struct in_addr a = { 0 }; char p[300], line[40] = "";
        if (strcmp(arg, "-") && inet_pton(AF_INET, arg, &a) != 1) { snprintf(err, errsz, "Give an IPv4 address, as 192.168.1.20"); return -1; }
        if (a.s_addr) snprintf(line, sizeof line, "allow %s\n", arg);
        snprintf(p, sizeof p, "%s/radio", state_dir());
        if (a.s_addr ? statefile_write(p, 0600, line, strlen(line)) : (unlink(p) && errno != ENOENT ? -1 : 0)) {
            snprintf(err, errsz, "Could not save it"); return -1;
        }
        pthread_mutex_lock(&lk); allow = a.s_addr; pthread_mutex_unlock(&lk);
        fprintf(stderr, "radio: %s%s\n", a.s_addr ? "the radio and the border router's API also let in " : "only Home Assistant may connect to the radio", a.s_addr ? arg : "");
        return 0;
    }
    if (!strcmp(what, "permit") || !strcmp(what, "zb_leave") || !strcmp(what, "zb_rotate") || !strcmp(what, "zb_prefer")) {
        char line[40], b[64];
        if (!zigbeed_role()) { snprintf(err, errsz, "Switch the radio to Zigbee, as the coordinator or a router, first"); return -1; }
        if ((!strcmp(what, "zb_leave") || !strcmp(what, "zb_prefer")) && zigbee_role != ZIGBEE_ROUTER) { snprintf(err, errsz, "Only a router leaves or chooses its network"); return -1; }
        if (!strcmp(what, "zb_rotate") && zigbee_role != ZIGBEE_COORD) { snprintf(err, errsz, "Only the coordinator gives the network a new key"); return -1; }
        int sec = 0;
        if (!strcmp(what, "permit")) {
            const char *v = arg ? arg : "0";
            if (!*v || strspn(v, "0123456789") != strlen(v) || strlen(v) > 3 || (sec = atoi(v)) > 254) { snprintf(err, errsz, "0 to 254 seconds"); return -1; }
        }
        if (!strcmp(what, "zb_prefer")) {
            /* "any", or an extended PAN ID: 16 hex digits */
            size_t l = arg ? strlen(arg) : 0;
            int ok = arg && (!strcmp(arg, "any") || (l == 16 && strspn(arg, "0123456789abcdefABCDEF") == 16 && strspn(arg, "0") != 16));
            if (!ok) { snprintf(err, errsz, "An extended PAN ID (16 hex digits), or any"); return -1; }
            snprintf(line, sizeof line, "prefer %s", arg);
        } else if (!strcmp(what, "zb_rotate")) snprintf(line, sizeof line, "rotate");
        else if (!strcmp(what, "permit")) snprintf(line, sizeof line, "permit %d", sec); else snprintf(line, sizeof line, "leave");
        if (zb_ctl(line, b, sizeof b) || strcmp(b, "ok")) { snprintf(err, errsz, "The Zigbee stack does not answer (it may still be starting): try again in a moment"); return -2; }
        fprintf(stderr, "radio: zigbee %s\n", line);
        return 0;
    }
    if (!otbr_role()) { snprintf(err, errsz, "Switch the radio to Thread, as a border router or a router, first"); return -1; }
    if (!strcmp(what, "leave")) {
        if ((rc = forget(b, sizeof b, err, errsz))) return rc;
        fprintf(stderr, "radio: the Thread network forgotten\n");
        return 0;
    }
    if (!strcmp(what, "form")) {
        char name[17] = "", body[80], en[40];
        snprintf(name, sizeof name, "%s", arg && *arg ? arg : "hassmic");
        for (char *c = name; *c; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) { snprintf(err, errsz, "A network name of letters, digits and signs, 16 at most"); return -1; }
        if ((rc = forget(b, sizeof b, err, errsz))) return rc;
        json_esc(en, sizeof en, name);
        snprintf(body, sizeof body, "{\"networkName\":%s}", en);
        /* no dataset after the reset: the border router makes a new one with this name and random values for the rest */
        if ((rc = rest_ok(rest("PUT", "/node/dataset/active", "application/json", body, b, sizeof b), "make a new network", err, errsz))) return rc;
    } else if (!strcmp(what, "join")) {
        char hex[2 * 254 + 1];
        if (dataset_ok(tlv, n, err, errsz)) return -1;
        for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02X", tlv[i]);
        if ((rc = forget(b, sizeof b, err, errsz))) { memset(hex, 0, sizeof hex); return rc; }
        rc = rest_ok(rest("PUT", "/node/dataset/active", "text/plain", hex, b, sizeof b), "take that dataset", err, errsz);
        memset(hex, 0, sizeof hex);
        memset(b, 0, sizeof b);
        if (rc) return rc;
    } else { snprintf(err, errsz, "not found"); return -1; }
    if ((rc = rest_ok(rest("PUT", "/node/state", "application/json", "\"enable\"", b, sizeof b), "start Thread", err, errsz))) return rc;
    fprintf(stderr, "radio: Thread started on %s\n", !strcmp(what, "form") ? "a new network" : "the network of the dataset given");
    return 0;
}

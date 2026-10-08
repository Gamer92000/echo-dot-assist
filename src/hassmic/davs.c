/*
 * Downloading Amazon's DAVS artifacts (wake word sets, the newer sound detection model, the whisper model) from the
 * settings page, on the Echo itself: no scripts/artifacts.sh, no stock-online mode, no Alexa app.  What a download
 * needs is an access token DAVS takes; the Echo gets one by registering itself the way its MAP does (code pair login,
 * docs/re-davs-login.md):
 *
 *   1. POST api.amazon.<domain>/auth/create/codepair with the Echo's serial and device type -> a 6-character code.
 *      The page shows it with a link to amazon.<domain>/code; the user enters it into their Amazon account.
 *   2. The Echo polls /auth/register with the code pair and a device attestation token (dha.c: the piece whose absence
 *      made the PC probe fail with InvalidDevice), plus the MAC and the secure session token of the code pair answer.
 *      Once the code is entered Amazon answers with bearer tokens.
 *   3. GET api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=<quoted base64 of the request JSON> (the request
 *      assetmgrd makes; tools/davs-fetch.py) with the bearer token -> a signed CloudFront URL; the artifact is a
 *      gzipped tar of a model folder (two levels of folders inside at most: the en-US sets' nttfusionconfig/ntt_conv/).
 *   4. Unpacked into state/artifacts/<staging folder>, checked and readied like a copy from another Echo (artifacts.c);
 *      root installs it (the page's install), which restarts hassmic.
 *
 * The attestation works on biscuit and radar (dha.c); donut's MAP builds a drvV3 token around a certificate instead
 * (not reversed), so there the page says this Echo cannot download itself and points at copying models from another
 * Echo.  Tokens are the account's device credentials: state/davs, mode 600, never in an answer to the page (it is
 * plain HTTP).
 *
 * The registration stays until "log out" deregisters it, so the next download needs no code; it shows in the Alexa app
 * as a device under this satellite's name.  Deregistering a *stock* Echo factory-resets it; as a satellite nothing of
 * Amazon's listens for that, and the egress lock keeps the daemons from reaching Amazon anyway.
 */
#include "davs.h"
#include "artifacts.h"
#include "core.h"
#include "dha.h"
#include "hash.h"
#include "net.h"
#include "wake.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define ANS_MAX  12288             /* a register answer carries two ~1 kB tokens and more */
#define MAX_TARB (96L << 20)       /* as much as artifacts.c accepts (the biggest set so far is 24 MB) */
/* what a registered Echo's map.db says it registered with (docs/re-davs-login.md) */
#define APP_REG  "\"app_name\":\"MYAPP\",\"app_version\":\"0.1\",\"os_version\":\"1.1\",\"device_model\":\"EchoDevice\",\"software_version\":\"1\""

enum { C_NONE, C_LOGIN, C_LOGOUT, C_FETCH, C_CANCEL };        /* the one command the worker runs */
enum { D_NONE, D_WAITING, D_ON, D_BUSY };                     /* what the page sees */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int cmd;                                               /* lk: C_* */
static char cmd_dom[8], cmd_key[32], cmd_loc[8];              /* lk */
static unsigned gen;                                          /* lk: a newer command aborts a running login wait */

static int state = D_NONE;                                   /* lk */
static char domain[8] = "de";                                /* lk: which Amazon the registration is on */
static char login_domain[8];                                 /* lk: which one a waiting login is on (domain once it works) */
static char pub_code[16], priv_code[64], sst[1200];          /* lk: the login that waits (the private code never leaves) */
static long long code_until;                                 /* lk: unix s */
static char device_name[64], marketplace[32];                /* lk: what Amazon says this Echo is registered as */
static char busy_what[64], error_text[256], done_id[80];     /* lk */
static int progress_pct;                                     /* lk */
static char access_token[2048], refresh_token[2048];         /* lk: the account's device credentials */
static long long access_until;                               /* lk: unix s */

/* this Echo's own identity, read once */
static char idme_serial[64], idme_type[24], idme_mac[32], idme_secret[32];
static int dha_ok;

static const char *env_or(const char *e, const char *d) { const char *v = getenv(e); return v ? v : d; }
static const char *state_dir(void) { return env_or("HASSMIC_STATE", "/data/local/hassmic/state"); }

/* the engine compatibility ids DAVS is asked with; NS65741's when the engine says nothing (donut's list) */
static const char *const ECIDS_FALLBACK =
    "1,10,11,12,13,14,15,16,17,19,2,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,4,5,6,7,8,9";
static const char *const AED_ECIDS_FALLBACK = "1,2,3,5,6,7";

static void say_err(const char *fmt, ...)                     /* lk held */
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error_text, sizeof error_text, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- libcurl, from the firmware (update.c has the GETs) */

enum { OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018, OPT_HTTPHEADER = 10023,
       OPT_POSTFIELDSIZE = 60, OPT_TIMEOUT = 13, OPT_LOW_SPEED_LIMIT = 19, OPT_LOW_SPEED_TIME = 20, OPT_NOPROGRESS = 43,
       OPT_FAILONERROR = 45, OPT_FOLLOWLOCATION = 52, OPT_MAXREDIRS = 68, OPT_CONNECTTIMEOUT = 78, OPT_NOSIGNAL = 99,
       OPT_OPENSOCKETFUNCTION = 20163, OPT_OPENSOCKETDATA = 10164, OPT_PROTOCOLS = 181, OPT_REDIR_PROTOCOLS = 182,
       OPT_WRITEFUNCTION = 20011, OPT_XFERINFODATA = 10057, OPT_XFERINFOFUNCTION = 20219,
       INFO_RESPONSE_CODE = 0x200002, PROTO_HTTP_S = 1 | 2 };
struct curl_sockaddr { int family, socktype, protocol; unsigned addrlen; };
struct curl_slist;

static struct {
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void *(*init)(void);
    int (*setopt)(void *, int, ...);
    int (*perform)(void *);
    void (*cleanup)(void *);
    int (*getinfo)(void *, int, ...);
    const char *(*strerror)(int);
} curl;

static int curl_load(void)
{
    static int done, ok; void *h;
    if (done) return ok;
    done = 1;
    if (!(h = dlopen("libcurl.so", RTLD_NOW)) && !(h = dlopen("libcurl.so.4", RTLD_NOW)))
    {
        fprintf(stderr, "davs: no libcurl (%s)\n", dlerror());
        return 0;
    }
    curl.slist_append = (struct curl_slist *(*)(struct curl_slist *, const char *))dlsym(h, "curl_slist_append");
    curl.init = (void *(*)(void))dlsym(h, "curl_easy_init");
    curl.setopt = (int (*)(void *, int, ...))dlsym(h, "curl_easy_setopt");
    curl.perform = (int (*)(void *))dlsym(h, "curl_easy_perform");
    curl.cleanup = (void (*)(void *))dlsym(h, "curl_easy_cleanup");
    curl.getinfo = (int (*)(void *, int, ...))dlsym(h, "curl_easy_getinfo");
    curl.strerror = (const char *(*)(int))dlsym(h, "curl_easy_strerror");
    return ok = curl.slist_append && curl.init && curl.setopt && curl.perform && curl.cleanup && curl.getinfo
             && curl.strerror;
}

static int on_socket(void *arg, int purpose, struct curl_sockaddr *a)
{
    (void)arg; (void)purpose;
    return net_socket(a->family, a->socktype, a->protocol);   /* the egress lock lets this group out */
}

struct sink { char *p; size_t n, cap; };

static size_t on_data(char *d, size_t size, size_t n, void *arg)
{
    struct sink *o = arg; size_t len = size * n;
    if (o->n + len > o->cap) len = o->cap - o->n;
    memcpy(o->p + o->n, d, len); o->n += len;
    return size * n;
}

static int on_progress(void *arg, int64_t total, int64_t now, int64_t ut, int64_t un)
{
    (void)arg; (void)ut; (void)un;
    if (total <= 0) return 0;
    pthread_mutex_lock(&lk);
    progress_pct = (int)(100 * now / total);
    pthread_mutex_unlock(&lk);
    return 0;
}

/* the downloads land in a file, not in memory: 24 MB and more */
struct file_sink { FILE *f; long long n; int over; char full[160]; };

static size_t on_file(char *d, size_t size, size_t n, void *arg)
{
    struct file_sink *o = arg;
    size_t len = size * n;
    if (o->n + (long long)len > MAX_TARB) { o->over = 1; return 0; }   /* stops the transfer */
    if (art_room((long long)len, o->full, sizeof o->full)) return 0;   /* /data full would stop state/ writes too */
    size_t w = fwrite(d, 1, len, o->f);
    o->n += (long long)w;
    return w;
}

static void curl_common(void *h)
{
    curl.setopt(h, OPT_USERAGENT, "hassmic");
    curl.setopt(h, OPT_PROTOCOLS, (long)PROTO_HTTP_S); curl.setopt(h, OPT_REDIR_PROTOCOLS, (long)PROTO_HTTP_S);
    curl.setopt(h, OPT_FOLLOWLOCATION, 1L); curl.setopt(h, OPT_MAXREDIRS, 3L);
    curl.setopt(h, OPT_FAILONERROR, 0L);                       /* the answers' bodies say more than their codes */
    curl.setopt(h, OPT_NOSIGNAL, 1L);
    curl.setopt(h, OPT_CONNECTTIMEOUT, 20L);
    curl.setopt(h, OPT_OPENSOCKETFUNCTION, on_socket); curl.setopt(h, OPT_OPENSOCKETDATA, NULL);
    curl.setopt(h, OPT_XFERINFOFUNCTION, on_progress); curl.setopt(h, OPT_XFERINFODATA, NULL);
    curl.setopt(h, OPT_NOPROGRESS, 0L);
}

/* one request.  BODY ("" = GET) is JSON; TOKEN and SST_HD ("" if none) become headers.  The answer lands in OUT (a
 * NUL appended) and the HTTP status in CODE.  0, or -1 with the reason in ERR. */
static int request(const char *url, const char *body, const char *token, const char *sst_hd,
                   struct sink *out, long *code, char *err, size_t errsz)
{
    struct curl_slist *hdr = NULL;
    static char post[8192];                    /* POSTFIELDS is not copied: the body must outlive setopt until perform */
    void *h; size_t blen = strlen(body);
    *code = 0;
    out->n = 0;
    if (out->p) out->p[0] = 0;
    if (!curl_load() || !(h = curl.init())) { snprintf(err, errsz, "no libcurl on this device"); return -1; }
    curl.setopt(h, OPT_URL, url);
    curl_common(h);
    curl.setopt(h, OPT_TIMEOUT, 60L);
    curl.setopt(h, OPT_LOW_SPEED_LIMIT, 256L); curl.setopt(h, OPT_LOW_SPEED_TIME, 30L);
    hdr = curl.slist_append(hdr, "Content-Type: application/json");
    hdr = curl.slist_append(hdr, "Accept-Language: en-US");
    if (*token) { char a[2100]; snprintf(a, sizeof a, "Authorization: Bearer %s", token); hdr = curl.slist_append(hdr, a); }
    {   /* the host, as MAP's connectivity manager sends it (secure-stream policy map_1p_post) */
        char host[200], a[240];
        if (sscanf(url, "%*[^:]://%199[^/?]", host) != 1) host[0] = 0;
        snprintf(a, sizeof a, "x-amzn-identity-auth-domain: %s", host);
        hdr = curl.slist_append(hdr, a);
    }
    if (*sst_hd) { char a[1300]; snprintf(a, sizeof a, "x-amzn-identity-secure-session-token: %s", sst_hd);
                   hdr = curl.slist_append(hdr, a); }
    curl.setopt(h, OPT_HTTPHEADER, hdr);
    if (blen)
    {
        if (blen >= sizeof post) { curl.cleanup(h); snprintf(err, errsz, "request too long"); return -1; }
        memcpy(post, body, blen + 1);
        curl.setopt(h, OPT_POSTFIELDS, post);
        curl.setopt(h, OPT_POSTFIELDSIZE, (long)blen);
    }                                                          /* without POSTFIELDS the request is a GET */
    curl.setopt(h, OPT_WRITEFUNCTION, on_data);
    curl.setopt(h, OPT_WRITEDATA, out);
    int rc = curl.perform(h);
    curl.getinfo(h, INFO_RESPONSE_CODE, code);
    curl.cleanup(h);
    if (rc) { snprintf(err, errsz, "%s", curl.strerror(rc)); return -1; }
    if (out->p) out->p[out->n] = 0;                           /* n <= cap, and cap is one below the buffer's size */
    return 0;
}

/* GET URL into the file PATH (unlinked first; the caller unlinks it in the end).  0, or -1 with the reason in ERR. */
static int download(const char *url, const char *path, long *code, char *err, size_t errsz)
{
    struct file_sink o = { 0, 0, 0, "" };
    void *h; int rc; struct stat st;
    *code = 0;
    if (!curl_load() || !(h = curl.init())) { snprintf(err, errsz, "no libcurl on this device"); return -1; }
    if (!(o.f = fopen(path, "wb"))) { curl.cleanup(h); snprintf(err, errsz, "cannot write %.200s", path); return -1; }
    curl.setopt(h, OPT_URL, url);
    curl_common(h);
    curl.setopt(h, OPT_TIMEOUT, 900L);                        /* 15 min: 12 MB over a slow link */
    curl.setopt(h, OPT_LOW_SPEED_LIMIT, 512L); curl.setopt(h, OPT_LOW_SPEED_TIME, 60L);
    curl.setopt(h, OPT_WRITEFUNCTION, on_file);
    curl.setopt(h, OPT_WRITEDATA, &o);
    rc = curl.perform(h);
    curl.getinfo(h, INFO_RESPONSE_CODE, code);
    curl.cleanup(h);
    fclose(o.f);
    if (rc || o.over)
    {
        unlink(path);
        if (o.over) snprintf(err, errsz, "the download is bigger than %ld MB", MAX_TARB >> 20);
        else if (*o.full) snprintf(err, errsz, "%s", o.full);
        else snprintf(err, errsz, "download failed: %s", curl.strerror(rc));
        return -1;
    }
    if (*code != 200)
    {
        unlink(path);
        snprintf(err, errsz, "the download answered HTTP %ld", *code);
        return -1;
    }
    if (stat(path, &st) || st.st_size < 256)                  /* a page of HTML, not a model */
    {
        unlink(path);
        snprintf(err, errsz, "the download is no model archive");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the few fields of Amazon's answers */

/* KEY's value anywhere in JS (string contents skipped, first hit wins; each of our keys appears once outside the
 * tokens themselves): a string unescaped, or anything else up to its delimiter.  1 if there. */
static int json_find(const char *p, const char *end, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key), n = 0;
    while (p < end)
    {
        if (*p != '"') { p++; continue; }
        const char *s = ++p;
        while (p < end && *p != '"') p += (*p == '\\' && p + 1 < end) ? 2 : 1;
        if (p >= end) return 0;
        const char *q = p + 1;
        while (q < end && isspace((unsigned char)*q)) q++;
        if ((size_t)(p - s) == kl && !memcmp(s, key, kl) && q < end && *q == ':')
        {
            for (q++; q < end && isspace((unsigned char)*q); q++) ;
            if (q < end && *q == '"')
            {
                for (q++; q < end && *q != '"'; q++)
                {
                    char c[4]; int cl = 1;
                    c[0] = *q;
                    if (c[0] == '\\' && q + 1 < end)
                    {
                        c[0] = *++q;
                        if (c[0] == 'n') c[0] = '\n';
                        else if (c[0] == 't') c[0] = '\t';
                        else if (c[0] == 'u' && end - q > 4          /* Amazon escapes non-ASCII ("K\u00fcchen") */
                                 && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2])
                                 && isxdigit((unsigned char)q[3]) && isxdigit((unsigned char)q[4]))
                        {
                            unsigned u = 0;                             /* exactly four: no hex digit after them belongs to it */
                            for (int di = 1; di <= 4; di++) u = u << 4 | (isdigit((unsigned char)q[di]) ? q[di] - '0' : (tolower((unsigned char)q[di]) - 'a' + 10));
                            q += 4;
                            if (u >= 0xd800 && u < 0xe000) u = 0xfffd;     /* surrogates: not worth pairing up here */
                            if (u < 0x80) c[0] = (char)u;
                            else if (u < 0x800) { c[0] = (char)(0xc0 | u >> 6); c[1] = (char)(0x80 | (u & 63)); cl = 2; }
                            else { c[0] = (char)(0xe0 | u >> 12); c[1] = (char)(0x80 | (u >> 6 & 63)); c[2] = (char)(0x80 | (u & 63)); cl = 3; }
                        }
                    }
                    if (n + cl < cap) { memcpy(out + n, c, cl); n += cl; }
                }
                out[n] = 0;
                return 1;
            }
            while (q < end && !strchr(",}] \t\r\n\"", *q) && n + 1 < cap) out[n++] = *q++;
            out[n] = 0;
            return n > 0;
        }
        p = q;
    }
    return 0;
}

static size_t jesc(char *out, size_t cap, const char *s)     /* JSON string body (no quotes) */
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- this Echo's identity */

/* the idme files end in a NUL (device_type_id in a space before it): read them stripped, or nothing.  HASSMIC_IDME
 * points tests at a stand-in directory; on an Echo nobody sets it. */
static void idme_read(const char *name, char *out, size_t cap)
{
    char p[192]; ssize_t k; char *e; int fd;
    out[0] = 0;
    snprintf(p, sizeof p, "%s/%s", env_or("HASSMIC_IDME", "/proc/idme"), name);
    if ((fd = open(p, O_RDONLY)) < 0) return;
    k = read(fd, out, cap - 1);
    close(fd);
    if (k <= 0) { out[0] = 0; return; }
    for (e = out + k; e > out && (e[-1] == 0 || isspace((unsigned char)e[-1])); e--) ;
    *e = 0;                                                   /* the NUL the files end in, and any space before it */
    for (e = out; *e; e++) if (!isalnum((unsigned char)*e)) { out[0] = 0; return; }    /* an id, or nothing */
}

static void identity_read(void)
{
    char probe[2048], err[160];
    idme_read("serial", idme_serial, sizeof idme_serial);
    idme_read("device_type_id", idme_type, sizeof idme_type);
    idme_read("mac_addr", idme_mac, sizeof idme_mac);
    idme_read("mac_sec", idme_secret, sizeof idme_secret);    /* 0400 system: only a root-run hassmic sees it */
    if (!*idme_serial) dha_serial(idme_serial, sizeof idme_serial);   /* no idme (a PC): the key's own serial */
    if (!*idme_type) fprintf(stderr, "davs: no device type (no idme): cannot register with Amazon\n");
    else if ((dha_ok = !dha_jwt(idme_type, time(NULL), probe, sizeof probe, err, sizeof err)) == 0)
        fprintf(stderr, "davs: no device attestation (%s)\n", err);
}

/* ---------------------------------------------------------------- the tokens on disk */

static void tokens_save(void)                                 /* lk held */
{
    char p[300], tmp[320]; FILE *f;
    snprintf(p, sizeof p, "%s/davs", state_dir()); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!(f = fopen(tmp, "w"))) { fprintf(stderr, "davs: cannot write %s\n", tmp); return; }
    fprintf(f, "domain %s\nname %s\nmarketplace %s\nrefresh %s\naccess %s\nexpires %lld\n",
            domain, device_name, marketplace, refresh_token, access_token, access_until);
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, p)) unlink(tmp);
}

static void tokens_load(void)
{
    char p[300], line[2300], k[16], v[2200]; FILE *f;
    snprintf(p, sizeof p, "%s/davs", state_dir());
    if (!(f = fopen(p, "r"))) return;
    while (fgets(line, sizeof line, f))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %2199[^\n]", k, v) != 2) continue;
        if (!strcmp(k, "domain")) snprintf(domain, sizeof domain, "%.7s", v);
        else if (!strcmp(k, "name")) snprintf(device_name, sizeof device_name, "%.63s", v);
        else if (!strcmp(k, "marketplace")) snprintf(marketplace, sizeof marketplace, "%.31s", v);
        else if (!strcmp(k, "refresh")) snprintf(refresh_token, sizeof refresh_token, "%.2047s", v);
        else if (!strcmp(k, "access")) snprintf(access_token, sizeof access_token, "%.2047s", v);
        else if (!strcmp(k, "expires")) access_until = atoll(v);
    }
    fclose(f);
    if (*refresh_token)
    {
        state = D_ON;
        fprintf(stderr, "davs: a registration is here (%s)\n", device_name[0] ? device_name : "unnamed");
    }
}

static void tokens_clear(void)                                /* lk held */
{
    char p[300];
    snprintf(p, sizeof p, "%s/davs", state_dir());
    unlink(p);
    *refresh_token = *access_token = *device_name = *marketplace = 0;
    access_until = 0;
}

/* ---------------------------------------------------------------- talking to Amazon */

static const char *auth_base(const char *dom)                /* worker only: one buffer */
{
    static char b[80];
    const char *e = getenv("HASSMIC_DAVS_API");
    if (e) return e;                                          /* tests: one fake Amazon for every endpoint */
    snprintf(b, sizeof b, "https://api.amazon.%s", dom);
    return b;
}

static const char *davs_base(void)
{
    const char *e = getenv("HASSMIC_DAVS_API");
    return e ? e : "https://api.amazonalexa.com";
}

/* POST /auth/register the way MAP's map_registration_register_device does, the attestation token included */
static int register_call(const char *dom, const char *jwt, struct sink *out, long *code, char *err, size_t errsz)
{
    char body[7168], name[130], url[300], secret[48] = "";
    int n;
    jesc(name, sizeof name, core_name);                       /* this satellite's name: what the user sees in the app */
    if (strlen(idme_secret) == 20)                            /* idme mac_sec, the one only a root-run hassmic reads */
        snprintf(secret, sizeof secret, "\"device_secret\":\"%s\",", idme_secret);
    n = snprintf(body, sizeof body,
        "{\"requested_token_type\":[\"bearer\",\"mac_dms\"],\"requested_extensions\":[\"device_info\",\"customer_info\"],"
        "\"auth_data\":{\"code_pair\":{\"public_code\":\"%s\",\"private_code\":\"%s\"},"
        "\"use_global_authentication\":\"true\"},"
        "\"registration_data\":{\"domain\":\"Device\",\"device_type\":\"%s\",\"device_serial\":\"%s\","
        "\"device_name\":\"%s\"," APP_REG ",%s\"device_authentication_token\":\"%s\"},"
        "\"device_metadata\":{\"mac_address\":\"%s\"}}",
        pub_code, priv_code, idme_type, idme_serial, name, secret, jwt, idme_mac);
    if (n < 0 || (size_t)n >= sizeof body) { snprintf(err, errsz, "request too long"); return -1; }
    snprintf(url, sizeof url, "%s/auth/register", auth_base(dom));
    return request(url, body, "", sst, out, code, err, errsz);
}

/* the login on amazon.DOM: create the code pair, then poll /auth/register until the code is entered.  MINE: the
 * generation this login runs under; a newer command aborts it.  DOM becomes the registration's only once it works: an
 * older registration that survives a failed try keeps its own Amazon for refreshes and the logout. */
static void login_run(unsigned mine, const char *dom)
{
    char body[512], ans[ANS_MAX], url[300], err[300], v[2200];
    long code;
    int every = 5;
    long long expires = 600;
    time_t until;
    struct sink out = { ans, 0, sizeof ans - 1 };

    snprintf(body, sizeof body, "{\"code_data\":{\"domain\":\"Device\",\"device_serial\":\"%s\",\"device_type\":\"%s\"}}",
             idme_serial, idme_type);
    snprintf(url, sizeof url, "%s/auth/create/codepair", auth_base(dom));
    if (request(url, body, "", "", &out, &code, err, sizeof err) || code != 200
        || !json_find(ans, ans + strlen(ans), "public_code", pub_code, sizeof pub_code)
        || !json_find(ans, ans + strlen(ans), "private_code", priv_code, sizeof priv_code))
    {
        pthread_mutex_lock(&lk);
        state = *refresh_token ? D_ON : D_NONE;               /* an older registration survives a failed try */
        say_err("Amazon refused to start the login (HTTP %ld)", code);
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "davs: codepair: HTTP %ld %.120s %s\n", code, ans[0] ? ans : "", err);
        return;
    }
    if (json_find(ans, ans + strlen(ans), "secureSessionToken", sst, sizeof sst)) sst[sizeof sst - 1] = 0;
    else sst[0] = 0;                                            /* a login on another Amazon starts without the old one's */
    if (json_find(ans, ans + strlen(ans), "polling_interval_in_seconds", v, sizeof v) && atoi(v) >= 1 && atoi(v) <= 60)
        every = atoi(v);
    if (json_find(ans, ans + strlen(ans), "expires_in", v, sizeof v) && atoll(v) >= 15 && atoll(v) <= 3600)
        expires = atoll(v);

    until = time(NULL) + expires;
    pthread_mutex_lock(&lk);
    state = D_WAITING;
    error_text[0] = 0;
    code_until = until;
    snprintf(login_domain, sizeof login_domain, "%s", dom);
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "davs: login code %s shown, %lld s to enter it\n", pub_code, expires);

    while (time(NULL) < until)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += every;
        pthread_mutex_lock(&lk);
        while (gen == mine && time(NULL) < until)
        {
            if (pthread_cond_timedwait(&cond, &lk, &ts) != ETIMEDOUT) continue;
            break;
        }
        int abort = gen != mine;
        pthread_mutex_unlock(&lk);
        if (abort) return;                                    /* a newer command takes over */

        char jwt[2048], jerr[160];
        if (dha_jwt(idme_type, time(NULL), jwt, sizeof jwt, jerr, sizeof jerr))
        {
            pthread_mutex_lock(&lk);
            state = *refresh_token ? D_ON : D_NONE;
            say_err("the attestation key did not sign: %.150s", jerr);
            pthread_mutex_unlock(&lk);
            return;
        }
        if (register_call(dom, jwt, &out, &code, err, sizeof err))
        {
            pthread_mutex_lock(&lk);
            state = *refresh_token ? D_ON : D_NONE;
            say_err("%s", err);
            pthread_mutex_unlock(&lk);
            return;
        }
        char newer[1200];
        if (json_find(ans, ans + strlen(ans), "secureSessionToken", newer, sizeof newer))
        {
            pthread_mutex_lock(&lk);
            snprintf(sst, sizeof sst, "%s", newer);           /* each answer may carry a newer one */
            pthread_mutex_unlock(&lk);
        }
        if (code == 200)
        {
            char at[2048], rt[2048], ex[16], nm[64], mk[32];
            if (json_find(ans, ans + strlen(ans), "access_token", at, sizeof at)
                && json_find(ans, ans + strlen(ans), "refresh_token", rt, sizeof rt))
            {
                pthread_mutex_lock(&lk);
                snprintf(access_token, sizeof access_token, "%s", at);
                snprintf(refresh_token, sizeof refresh_token, "%s", rt);
                snprintf(domain, sizeof domain, "%s", dom);
                access_until = time(NULL) + 3600;
                if (json_find(ans, ans + strlen(ans), "expires_in", ex, sizeof ex))
                    access_until = time(NULL) + atoll(ex) - 60;
                device_name[0] = marketplace[0] = 0;
                if (json_find(ans, ans + strlen(ans), "device_name", nm, sizeof nm))
                    snprintf(device_name, sizeof device_name, "%s", nm);
                if (json_find(ans, ans + strlen(ans), "preferred_marketplace", mk, sizeof mk))
                    snprintf(marketplace, sizeof marketplace, "%s", mk);
                state = D_ON;
                error_text[0] = 0;
                tokens_save();
                pthread_mutex_unlock(&lk);
                fprintf(stderr, "davs: registered as \"%s\"\n", device_name[0] ? device_name : "unnamed");
                return;
            }
            fprintf(stderr, "davs: register answered 200 without tokens\n");
        }
        else
        {
            /* before the code is entered this is what Amazon answers; the same after it would mean a refusal */
            char ec[64] = "", em[200] = "";
            json_find(ans, ans + strlen(ans), "code", ec, sizeof ec);
            json_find(ans, ans + strlen(ans), "message", em, sizeof em);
            fprintf(stderr, "davs: register: HTTP %ld %.40s %.90s\n", code, ec, em);
        }
    }
    pthread_mutex_lock(&lk);
    state = *refresh_token ? D_ON : D_NONE;
    say_err("the code was not entered in time");
    pthread_mutex_unlock(&lk);
}

/* a new access token from the refresh token.  0; -1 with the reason in ERR when Amazon was not reached or failed
 * itself; -2 when it refused the refresh token (400/401: the registration is gone on the account's side).  lk not held;
 * the worker is the only writer of the tokens and the domain. */
static int token_refresh(char *err, size_t errsz)
{
    char body[2600], ans[ANS_MAX], url[300], at[2048], ex[16];
    long code;
    struct sink out = { ans, 0, sizeof ans - 1 };

    snprintf(body, sizeof body, "{\"requested_token_type\":\"access_token\",\"app_name\":\"MYAPP\",\"app_version\":\"0.1\","
                               "\"source_token_type\":\"refresh_token\",\"source_token\":\"%s\"}", refresh_token);
    snprintf(url, sizeof url, "%s/auth/token", auth_base(domain));
    if (request(url, body, "", "", &out, &code, err, errsz)) return -1;
    if (code == 400 || code == 401)
    {
        snprintf(err, errsz, "the registration is not usable any more (HTTP %ld): log in again", code);
        return -2;
    }
    if (code != 200 || !json_find(ans, ans + strlen(ans), "access_token", at, sizeof at))
    {
        snprintf(err, errsz, "Amazon did not renew the login (HTTP %ld): try again later", code);
        return -1;
    }
    pthread_mutex_lock(&lk);
    snprintf(access_token, sizeof access_token, "%s", at);
    access_until = time(NULL) + 3600;
    if (json_find(ans, ans + strlen(ans), "expires_in", ex, sizeof ex)) access_until = time(NULL) + atoll(ex) - 60;
    tokens_save();
    pthread_mutex_unlock(&lk);
    return 0;
}

/* deregister.  The access token lasts an hour, so a logout days after the login refreshes it first: a deregister
 * with the stale one answers 401, and forgetting the refresh token then would leave the Echo on the account with
 * nothing here able to remove it. */
static void logout_run(void)
{
    char body[128], ans[ANS_MAX], url[300], err[300];
    long code;
    int fresh = 0, rc;
    struct sink out = { ans, 0, sizeof ans - 1 };

    snprintf(body, sizeof body, "{\"request_metadata\":{\"app_version\":\"0.1\",\"app_name\":\"MYAPP\"}}");
    snprintf(url, sizeof url, "%s/auth/deregister", auth_base(domain));
    for (;;)
    {
        if (!fresh && time(NULL) > access_until)
        {
            if ((rc = token_refresh(err, sizeof err)) == -1)
            {
                pthread_mutex_lock(&lk); say_err("%s", err); pthread_mutex_unlock(&lk);
                return;
            }
            fresh = 1;
            if (rc == -2) { code = 401; break; }              /* the refresh token refused: already off the account */
        }
        if (request(url, body, access_token, "", &out, &code, err, sizeof err))
        {
            pthread_mutex_lock(&lk); say_err("%s", err); pthread_mutex_unlock(&lk);
            return;
        }
        if (code != 401 || fresh) break;
        pthread_mutex_lock(&lk);
        access_until = 0;                                     /* revoked early, or the clock was off: refresh, once */
        pthread_mutex_unlock(&lk);
    }
    pthread_mutex_lock(&lk);
    /* gone is gone, whatever Amazon meant by it; a 401 counts only for a token just refreshed */
    if (code == 200 || code == 400 || code == 401 || code == 404)
    {
        tokens_clear();
        state = D_NONE;
        error_text[0] = 0;
        fprintf(stderr, "davs: deregistered (HTTP %ld)\n", code);
    }
    else say_err("deregistering failed (HTTP %ld): remove the Echo in the Alexa app, then log out here again", code);
    pthread_mutex_unlock(&lk);
}

/* ---------------------------------------------------------------- DAVS: asking, downloading, unpacking */

/* "1,2,..." of KEY ("wakeword_ecids", "aed_ecids") from the engine's attributes, or the fallback */
static void ecids(const char *key, const char *fallback, char *out, size_t cap)
{
    const char *a = wake_attributes(), *s = NULL;
    size_t n = 0;
    *out = 0;
    if (a && (s = strstr(a, key)) && (s = strstr(s, "[")))
    {
        for (s++; *s && *s != ']' && n + 1 < cap; s++)
        {
            if (isdigit((unsigned char)*s)) out[n++] = *s;
            else if (*s == ',' && n && out[n - 1] != ',') out[n++] = ',';
        }
        while (n && out[n - 1] == ',') n--;
        out[n] = 0;
    }
    if (!*out) snprintf(out, cap, "%s", fallback);
}

/* DAVS keeps the sound detection model by region, not by language (tools/davs-fetch.py) */
static const char *region_of(const char *locale)
{
    if (!strcmp(locale, "en-US") || !strcmp(locale, "en-CA") || !strcmp(locale, "fr-CA")
        || !strcmp(locale, "es-MX") || !strcmp(locale, "pt-BR")) return "NA";
    if (!strcmp(locale, "ja-JP") || !strcmp(locale, "en-AU") || !strcmp(locale, "en-IN")) return "FE";
    return "EU";
}

/* KEY (a wake word, "aed", "whisper") + LOCALE -> the artifactFilter request JSON and the artifact id */
static int filter_build(const char *key, const char *locale, char *json, size_t jcap, char *id, size_t icap)
{
    char ec[180];
    if (!strcmp(key, "whisper"))
    {
        /* one model for every language; DAVS serves it under en-US only (docs/re-whisper.md) */
        snprintf(json, jcap, "{\"artifactType\":\"alexa-hybrid\",\"artifactKey\":\"whisper-static\",\"filters\":"
                             "{\"ecid\":[\"6\"],\"modelClass\":[\"odie-litespeed\"],\"locale\":[\"en-US\"]}}");
        snprintf(id, icap, "whisper");
        return 0;
    }
    if (!strcmp(key, "aed"))
    {
        ecids("aed_ecids", AED_ECIDS_FALLBACK, ec, sizeof ec);
        snprintf(json, jcap, "{\"artifactType\":\"AED\",\"artifactKey\":\"AED\",\"filters\":{\"filterVersion\":[\"2\"],"
                             "\"engineCompatibilityIdList\":[%s],\"modelClass\":[\"class-10\"],\"location\":[\"%s\"]}}",
                 ec, region_of(locale));
        snprintf(id, icap, "sound");
        return 0;
    }
    static const char *const ww[] = { "alexa", "echo", "computer", "amazon", "ziggy" };
    int ww_i;
    for (ww_i = 0; ww_i < (int)(sizeof ww / sizeof *ww) && strcmp(key, ww[ww_i]); ww_i++) ;
    if (ww_i == sizeof ww / sizeof *ww) return -1;
    for (const char *c = locale; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-') return -1;
    if (strlen(locale) < 4 || strlen(locale) > 5 || locale[2] != '-') return -1;
    ecids("wakeword_ecids", ECIDS_FALLBACK, ec, sizeof ec);
    snprintf(json, jcap, "{\"artifactType\":\"wakeword\",\"artifactKey\":\"%s\",\"filters\":"
                         "{\"engineCompatibilityIdList\":[%s],\"locale\":[\"%s\"],\"modelClass\":[\"B\"]}}",
             key, ec, locale);
    snprintf(id, icap, "wake:%s-%s", key, locale);
    return 0;
}

/* base64 with the few characters a query string cannot carry percent-encoded (Python's quote(safe="")) */
static void quote_b64(const char *b64, char *out, size_t cap)
{
    size_t n = 0;
    for (; *b64 && n + 4 < cap; b64++)
    {
        if (*b64 == '+') memcpy(out + n, "%2B", 3);
        else if (*b64 == '/') memcpy(out + n, "%2F", 3);
        else if (*b64 == '=') memcpy(out + n, "%3D", 3);
        else { out[n++] = *b64; continue; }
        n += 3;
    }
    out[n] = 0;
}

/* the model folder travels as a gzipped tar; unpack it into STAGE as artifacts.c keeps model folders: files, and
 * folders of files two levels down at most (whisper's whisper_components/, alexa-de-DE's BDPGeneratedFiles/, every
 * en-US set's nttfusionconfig/ntt_conv/; 2026-10-08, GitHub issue 14: refused while one level was the limit).  The sound
 * detection model is the one thing inside under "AED/".  PAX headers ('x': the tars carry Apple xattrs) and directory
 * entries are skipped (a file's folder is made when the file comes); anything deeper is refused. */
struct gunzip { z_stream z; FILE *f; unsigned char in[65536]; int eof; };

static size_t gunzip_fill(struct gunzip *g, unsigned char *out, size_t n)
{
    size_t got = 0;
    int ret = Z_OK;
    while (got < n)
    {
        if (!g->z.avail_in && !g->eof)
        {
            size_t k = fread(g->in, 1, sizeof g->in, g->f);
            if (!k) g->eof = 1;
            g->z.next_in = g->in;
            g->z.avail_in = (uInt)k;
        }
        g->z.next_out = out + got;
        g->z.avail_out = (uInt)(n - got);
        ret = inflate(&g->z, g->eof ? Z_FINISH : Z_NO_FLUSH);
        got = n - g->z.avail_out;
        if (ret < 0 || ret == Z_NEED_DICT) return 0;
        if (ret == Z_STREAM_END) break;
        if (!g->z.avail_out) break;
        if (g->z.avail_in || !g->eof) continue;
        break;                                                /* nothing left to inflate from */
    }
    return got;
}

static int untar(const char *tgz, const char *stage, int aed, char *err, size_t errsz)
{
    struct gunzip g;
    unsigned char h[512], scratch[8192];
    char name[300], sub[400];
    int files = 0;
    unsigned long long written = 0;                           /* MAX_TARB caps the archive, this what it unpacks to */

    memset(&g, 0, sizeof g);
    if (!(g.f = fopen(tgz, "rb")) || inflateInit2(&g.z, 16 + MAX_WBITS) != Z_OK)
    {
        if (g.f) fclose(g.f);
        snprintf(err, errsz, "cannot read the download");
        return -1;
    }
    for (;;)
    {
        unsigned long long size, left;                        /* left: the entry's data and padding not yet read */
        char type, sz[13];
        const char *nm;
        if (gunzip_fill(&g, h, sizeof h) != sizeof h) { snprintf(err, errsz, "the download is not a complete archive"); goto bad; }
        if (!h[0]) break;                                     /* the zero block that ends a tar */
        memcpy(sz, h + 124, 12);                              /* the size field runs into the next one: end it */
        sz[12] = 0;
        size = strtoull(sz, NULL, 8);
        left = (size + 511) & ~511ULL;                        /* the data is padded to a whole block before the next header */
        type = (char)h[156];
        if (!strncmp((const char *)h + 257, "ustar", 5) && h[345])
            snprintf(name, sizeof name, "%.*s/%.*s", 155, (const char *)h + 345, 100, (const char *)h);
        else
            snprintf(name, sizeof name, "%.*s", 100, (const char *)h);
        nm = name;
        while (*nm == '/' || !strncmp(nm, "./", 2)) nm++;
        if (type == '0' || type == 0)
        {
            char file[400];
            const char *fn = nm;
            if (aed)
            {
                if (strncmp(fn, "AED/", 4)) { snprintf(err, errsz, "the sound detection archive holds \"%.100s\"", fn); goto bad; }
                fn += 4;
            }
            const char *base = strrchr(fn, '/') ? strrchr(fn, '/') + 1 : fn;
            if (base[0] == '.' && (strncmp(base, "._", 2) == 0 || !strcmp(base, ".DS_Store")))
                fn = "";                                      /* AppleDouble crumbs of the Mac that packed it */
            if (*fn)
            {
                /* "file", "sub/file" or "sub/sub/file": each part a name artifacts.c keeps (alphanumeric first, at
                 * most 63), 127 in all */
                int ok = strlen(fn) <= 127, part = 0, slashes = 0;
                for (const char *c = fn; ok && *c; c++)
                {
                    if (*c == '/') { ok = part > 0 && ++slashes <= 2; part = 0; continue; }
                    if (part == 0 && !isalnum((unsigned char)*c)) ok = 0;
                    else if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') ok = 0;
                    else if (++part > 63) ok = 0;
                }
                if (!ok || !part) { snprintf(err, errsz, "the archive holds \"%.100s\", a file this Echo cannot keep", fn); goto bad; }
                for (const char *c = fn; (c = strchr(c, '/')); c++)   /* its folders first; there already for a second file */
                {
                    snprintf(sub, sizeof sub, "%.200s/%.*s", stage, (int)(c - fn), fn);
                    if (mkdir(sub, 0755) && errno != EEXIST) { snprintf(err, errsz, "cannot write %.200s", sub); goto bad; }
                }
                if ((unsigned long long)written + size > MAX_TARB) { snprintf(err, errsz, "the archive unpacks to more than %ld MB", MAX_TARB >> 20); goto bad; }
                /* this file, and root's copy of the whole set at install, on top of what is written already */
                if (art_room((long long)(written + 2 * size), err, errsz)) goto bad;
                written += size;
                snprintf(file, sizeof file, "%.200s/%.127s", stage, fn);  /* file: 400; the name at most 63 + 1 + 63 */
                FILE *o = fopen(file, "wb");
                if (!o) { snprintf(err, errsz, "cannot write %.200s", file); goto bad; }
                while (size)
                {
                    size_t want = size < sizeof scratch ? (size_t)size : sizeof scratch;
                    if (gunzip_fill(&g, scratch, want) != want) { fclose(o); snprintf(err, errsz, "the download is not a complete archive"); goto bad; }
                    if (fwrite(scratch, 1, want, o) != want) { fclose(o); snprintf(err, errsz, "cannot write %.200s", file); goto bad; }
                    size -= want;
                    left -= want;
                }
                fclose(o);
                files++;
            }
        }
        else if (type == 'x' || type == 'g' || type == 'L' || type == 'K' || type == '5')
        { /* extended headers, long names and directories: the payload is skipped below */ }
        else { snprintf(err, errsz, "the archive holds an entry of type '%c'", type ? type : '?'); goto bad; }
        while (left)                                          /* what is skipped, and the padding of what was written */
        {
            size_t want = left < sizeof scratch ? (size_t)left : sizeof scratch;
            if (gunzip_fill(&g, scratch, want) != want) { snprintf(err, errsz, "the download is not a complete archive"); goto bad; }
            left -= want;
        }
    }
    inflateEnd(&g.z);
    fclose(g.f);
    if (!files) { snprintf(err, errsz, "the archive holds no model files"); return -1; }
    return 0;
bad:
    inflateEnd(&g.z);
    fclose(g.f);
    return -1;
}

/* one artifact: ask DAVS, download, unpack, stage, check (artifacts.c).  lk is held only for short updates. */
static void fetch_run(const char *key, const char *locale)
{
    char filter[512], id[80], b64[800], url[1300], ans[ANS_MAX], err[300], path[400], stage[400], what[64], dl[2048];
    long code;
    struct sink out = { ans, 0, sizeof ans - 1 };

    if (filter_build(key, locale, filter, sizeof filter, id, sizeof id))
    {
        pthread_mutex_lock(&lk); say_err("%s %s is not something to download", key, locale); pthread_mutex_unlock(&lk);
        return;
    }
    snprintf(what, sizeof what, "%s%s%s", key, strcmp(key, "whisper") ? " " : "",
             strcmp(key, "whisper") ? locale : "");
    pthread_mutex_lock(&lk);
    state = D_BUSY;
    snprintf(busy_what, sizeof busy_what, "%s", what);
    error_text[0] = 0;
    done_id[0] = 0;
    progress_pct = 0;
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "davs: downloading %s\n", what);

    if (time(NULL) > access_until && token_refresh(err, sizeof err) < 0)
    {
        pthread_mutex_lock(&lk); state = D_ON; say_err("%s", err); pthread_mutex_unlock(&lk);
        return;
    }
    b64_encode(filter, strlen(filter), b64, 0, 1);
    snprintf(url, sizeof url, "%s/v2/deviceArtifacts/?artifactFilter=", davs_base());
    quote_b64(b64, url + strlen(url), sizeof url - strlen(url));
    if (request(url, "", access_token, "", &out, &code, err, sizeof err))
    {
        pthread_mutex_lock(&lk); state = D_ON; say_err("%s", err); pthread_mutex_unlock(&lk);
        return;
    }
    if (code != 200 || !json_find(ans, ans + strlen(ans), "downloadUrl", dl, sizeof dl))
    {
        pthread_mutex_lock(&lk);
        state = D_ON;
        if (code == 404) say_err("Amazon has no %s", what);
        else if (code == 401 || code == 403) say_err("the registration is not taken any more: log in again");
        else say_err("Amazon answered HTTP %ld to the %s request", code, what);
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "davs: deviceArtifacts: HTTP %ld %.120s\n", code, ans);
        return;
    }
    {   /* for the log: which version Amazon handed out */
        char aid[80];
        if (json_find(ans, ans + strlen(ans), "artifactIdentifier", aid, sizeof aid))
            fprintf(stderr, "davs: %s is artifact %.60s\n", what, aid);
    }

    snprintf(path, sizeof path, "%s/davs-download", state_dir());
    unlink(path);
    if (download(dl, path, &code, err, sizeof err))
    {
        unlink(path);
        pthread_mutex_lock(&lk); state = D_ON; say_err("%s", err); pthread_mutex_unlock(&lk);
        return;
    }
    if (art_stage_dir(id, stage, sizeof stage))
    {
        unlink(path);
        pthread_mutex_lock(&lk); state = D_ON; say_err("%s is not an artifact this Echo keeps", id); pthread_mutex_unlock(&lk);
        return;
    }
    art_unstage(id);                                          /* a download starts clean: no file of an older one stays */
    {   char arts[320];                                        /* art_begin makes these for a copy; a download needs its own */
        snprintf(arts, sizeof arts, "%s/artifacts", state_dir());
        mkdir(arts, 0700);
    }
    mkdir(stage, 0755);
    if (untar(path, stage, !strcmp(id, "sound"), err, sizeof err)
        || art_staged(id, err, sizeof err) || art_commit(id, err, sizeof err))
    {
        art_unstage(id);
        unlink(path);
        pthread_mutex_lock(&lk); state = D_ON; say_err("%s", err); pthread_mutex_unlock(&lk);
        fprintf(stderr, "davs: %s: %s\n", what, err);
        return;
    }
    unlink(path);
    pthread_mutex_lock(&lk);
    state = D_ON;
    progress_pct = 100;
    snprintf(done_id, sizeof done_id, "%s", id);
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "davs: %s (%s) arrived whole, waits to be installed\n", what, id);
}

/* ---------------------------------------------------------------- worker and interface */

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lk);
    for (;;)
    {
        int c;
        char dom[8], key[32], loc[8];
        unsigned mine;
        while (cmd == C_NONE)
            pthread_cond_wait(&cond, &lk);
        c = cmd;
        cmd = C_NONE;
        snprintf(dom, sizeof dom, "%s", cmd_dom);
        snprintf(key, sizeof key, "%s", cmd_key);
        snprintf(loc, sizeof loc, "%s", cmd_loc);
        mine = gen;                     /* command() counted it when it queued the work */
        pthread_mutex_unlock(&lk);

        if (c == C_LOGIN) login_run(mine, dom);
        else if (c == C_LOGOUT) logout_run();
        else if (c == C_CANCEL)
        {   /* the login it ends has returned already (gen); an older registration stays as it was */
            pthread_mutex_lock(&lk);
            if (state == D_WAITING) { state = *refresh_token ? D_ON : D_NONE; error_text[0] = 0; }
            pthread_mutex_unlock(&lk);
            fprintf(stderr, "davs: login cancelled\n");
        }
        else if (c == C_FETCH) fetch_run(key, loc);

        pthread_mutex_lock(&lk);
    }
    return NULL;
}

static int command(int c, const char *dom, const char *key, const char *loc)
{
    pthread_mutex_lock(&lk);
    if (cmd != C_NONE) { pthread_mutex_unlock(&lk); return -1; }
    cmd = c;
    snprintf(cmd_dom, sizeof cmd_dom, "%s", dom);
    snprintf(cmd_key, sizeof cmd_key, "%s", key);
    snprintf(cmd_loc, sizeof cmd_loc, "%s", loc);
    gen++;                              /* a login still waiting for its code: its time is up, the new command rules */
    if (c == C_LOGIN)
    {   /* waiting from the moment it is queued, still without a code: the page shows the step it is in, not the one before */
        state = D_WAITING;
        pub_code[0] = 0;
        code_until = 0;
        error_text[0] = 0;
        snprintf(login_domain, sizeof login_domain, "%s", dom);
    }
    if (c == C_FETCH)
    {   /* busy from the moment it is queued: a page polling before the worker picks it up would read the state from
         * before (registered, no error, nothing done) as this download's end */
        state = D_BUSY;
        snprintf(busy_what, sizeof busy_what, "%s %s", key, loc);
        error_text[0] = done_id[0] = 0;
        progress_pct = 0;
    }
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lk);
    return 0;
}

void davs_start(void)
{
    pthread_t t;
    identity_read();
    pthread_mutex_lock(&lk);
    tokens_load();
    pthread_mutex_unlock(&lk);
    if (pthread_create(&t, NULL, worker, NULL)) fprintf(stderr, "davs: no worker thread\n");
    else pthread_detach(t);
}

int davs_login(const char *dom, char *err, size_t errsz)
{
    if (!dha_ok)
    {
        snprintf(err, errsz, "this Echo cannot prove itself to Amazon: no attestation key that answers");
        return -1;
    }
    /* the Amazon sites with Alexa: an account lives in one region (North America, Europe, Far East), and any site of
     * its region takes the code.  Tried on the real Amazon: de (2026-10-06); the fake one in tests takes any. */
    static const char *const sites[] = { "com", "ca", "com.mx", "com.br", "co.uk", "de", "fr", "it", "es", "co.jp",
                                         "com.au", "in" };
    size_t i;
    for (i = 0; i < sizeof sites / sizeof *sites && strcmp(dom, sites[i]); i++) ;
    if (i == sizeof sites / sizeof *sites)
    {
        snprintf(err, errsz, "amazon.%.20s is not one of the Amazons to log in on", dom);
        return -1;
    }
    pthread_mutex_lock(&lk);
    if (state == D_BUSY) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "a download is running"); return -1; }
    pthread_mutex_unlock(&lk);
    if (command(C_LOGIN, dom, "", "")) { snprintf(err, errsz, "another command is starting"); return -1; }
    fprintf(stderr, "davs: logging in on amazon.%s\n", dom);
    return 0;
}

int davs_cancel(char *err, size_t errsz)
{
    pthread_mutex_lock(&lk);
    if (state != D_WAITING) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "no login waits for its code"); return -1; }
    pthread_mutex_unlock(&lk);
    if (command(C_CANCEL, "", "", "")) { snprintf(err, errsz, "another command is starting"); return -1; }
    return 0;
}

int davs_logout(char *err, size_t errsz)
{
    pthread_mutex_lock(&lk);
    if (state != D_ON) { pthread_mutex_unlock(&lk); snprintf(err, errsz, "not logged in"); return -1; }
    pthread_mutex_unlock(&lk);
    if (command(C_LOGOUT, "", "", "")) { snprintf(err, errsz, "another command is starting"); return -1; }
    return 0;
}

int davs_fetch(const char *key, const char *locale, char *err, size_t errsz)
{
    char probe[512], id[80];
    if (filter_build(key, locale, probe, sizeof probe, id, sizeof id))
    {
        snprintf(err, errsz, "%s %s is not something to download", key, locale);
        return -1;
    }
    pthread_mutex_lock(&lk);
    if (state != D_ON) { pthread_mutex_unlock(&lk); snprintf(err, errsz, state == D_BUSY ? "a download is running" : "not logged in"); return -1; }
    pthread_mutex_unlock(&lk);
    if (command(C_FETCH, "", key, locale)) { snprintf(err, errsz, "another command is starting"); return -1; }
    return 0;
}

size_t davs_status_json(char *out, size_t cap)
{
    static const char *const names[] = { "none", "waiting", "registered", "busy" };
    size_t n = 0;
    char e[280], dn[140], w[140], di[100];
    const char *dom;
    long long left;
    pthread_mutex_lock(&lk);
    dom = state == D_WAITING ? login_domain : domain;
    jesc(e, sizeof e, error_text);
    jesc(dn, sizeof dn, device_name);
    jesc(w, sizeof w, busy_what);
    jesc(di, sizeof di, done_id);
    left = state == D_WAITING ? code_until - time(NULL) : 0;
    if (left < 0) left = 0;
    n = (size_t)snprintf(out, cap, "{\"state\":\"%s\",\"dha\":%s,\"domain\":\"%s\",\"code\":\"%s\","
            "\"url\":\"https://www.amazon.%s/code\",\"left_s\":%lld,\"device\":\"%s\",\"busy\":\"%s\","
            "\"progress\":%d,\"error\":\"%s\",\"done\":\"%s\"}",
            names[state], dha_ok ? "true" : "false", dom, state == D_WAITING ? pub_code : "", dom,
            left, dn, w, progress_pct, e, di);
    pthread_mutex_unlock(&lk);
    return n < cap ? n : cap - 1;
}

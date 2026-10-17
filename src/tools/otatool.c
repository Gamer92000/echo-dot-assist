/*
 * otatool - update bundles for hassmic: pack and sign on the PC, verify and unpack on the Echo.
 *
 *   otatool keygen  SECRET PUBLIC                       new signing key pair (keep SECRET off the device)
 *   otatool pack    SECRET VERSION OUT FILE[:MODE]...   OUT = bundle, OUT.sig = detached signature
 *   otatool verify  PUBLIC BUNDLE SIG                   exit 0 if the signature is good; prints the version
 *   otatool install PUBLIC BUNDLE SIG DESTDIR           verify, then unpack into DESTDIR (must not exist yet)
 *   otatool push    HOST PORT BUNDLE SIG                send to a running hassmic, print its answer (exit 0 on "OK ...")
 *   otatool adb     HOST PORT SECRET                    open adb over Wi-Fi on it for 30 min (sign its challenge)
 *   otatool remount rw|ro PATH                          remount the file system PATH lives on (device, root)
 *
 * The device runs "install" as root from the read-only system partition, with the public key from there, so whatever
 * downloaded the bundle (an unprivileged, network-facing process) cannot get anything installed that the owner of
 * SECRET did not sign.  Signature: EdDSA as Monocypher defines it (Curve25519 + BLAKE2b), over the whole bundle.
 *
 * Bundle: "HMOTA1\n" "version <v>\n" then per file "file <name> <octal mode> <size>\n" + <size> bytes, then "end\n".
 * Names are plain file names ([A-Za-z0-9._-], no leading dot): nothing can be written outside DESTDIR.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../third_party/monocypher.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); uint8_t *buf; long n;
    if (!f) { fprintf(stderr, "otatool: %s: %s\n", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    buf = malloc(n > 0 ? n : 1);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "otatool: cannot read %s\n", path); fclose(f); free(buf); return NULL; }
    fclose(f); *len = n;
    return buf;
}

static int spit(const char *path, const void *data, size_t len, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) { fprintf(stderr, "otatool: %s: %s\n", path, strerror(errno)); return -1; }
    for (size_t off = 0; off < len; ) { ssize_t w = write(fd, (const char *)data + off, len - off); if (w <= 0) { close(fd); return -1; } off += w; }
    fchmod(fd, mode);                                   /* not subject to umask */
    return close(fd);
}

static int name_ok(const char *n)
{
    if (!*n || *n == '.' || strlen(n) > 64) return 0;
    for (; *n; n++) if (!((*n >= 'A' && *n <= 'Z') || (*n >= 'a' && *n <= 'z') || (*n >= '0' && *n <= '9') || *n == '.' || *n == '_' || *n == '-')) return 0;
    return 1;
}

static int keygen(const char *sec, const char *pub)
{
    uint8_t seed[32], sk[64], pk[32]; int f = open("/dev/urandom", O_RDONLY);
    if (f < 0 || read(f, seed, 32) != 32) { fprintf(stderr, "otatool: no randomness\n"); return 1; }
    close(f);
    crypto_eddsa_key_pair(sk, pk, seed);
    return spit(sec, sk, 64, 0600) || spit(pub, pk, 32, 0644);
}

static int pack(int argc, char **argv)     /* SECRET VERSION OUT FILE[:MODE]... */
{
    size_t skl, cap = 1 << 20, n = 0; uint8_t *sk = slurp(argv[0], &skl), *out = malloc(cap), sig[64]; char hdr[256], sigpath[512];
    if (!sk || skl != 64 || !out) { fprintf(stderr, "otatool: bad secret key\n"); return 1; }
    n += sprintf((char *)out, "HMOTA1\nversion %s\n", argv[1]);
    for (int i = 3; i < argc; i++) {
        char path[512]; int mode = 0755; size_t len; snprintf(path, sizeof path, "%s", argv[i]);
        char *colon = strrchr(path, ':'); if (colon) { *colon = 0; mode = (int)strtol(colon + 1, NULL, 8); }
        const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        uint8_t *data = slurp(path, &len);
        if (!data || !name_ok(base)) { fprintf(stderr, "otatool: cannot pack %s\n", path); return 1; }
        int hl = snprintf(hdr, sizeof hdr, "file %s %o %zu\n", base, mode, len);
        if (n + hl + len + 16 > cap) { cap = (n + hl + len + 16) * 2; out = realloc(out, cap); if (!out) return 1; }
        memcpy(out + n, hdr, hl); n += hl; memcpy(out + n, data, len); n += len; free(data);
    }
    memcpy(out + n, "end\n", 4); n += 4;
    crypto_eddsa_sign(sig, sk, out, n);
    snprintf(sigpath, sizeof sigpath, "%s.sig", argv[2]);
    unlink(argv[2]); unlink(sigpath);
    if (spit(argv[2], out, n, 0644) || spit(sigpath, sig, 64, 0644)) return 1;
    printf("%s: version %s, %d files, %zu bytes, signed\n", argv[2], argv[1], argc - 3, n);
    return 0;
}

/* Verify, then (dest != NULL) unpack.  Parsing only ever happens on authenticated data. */
static int verify_install(const char *pubpath, const char *bundle, const char *sigpath, const char *dest)
{
    size_t pl, bl, sl; uint8_t *pk = slurp(pubpath, &pl), *b = slurp(bundle, &bl), *sig = slurp(sigpath, &sl); char version[64] = "";
    if (!pk || !b || !sig || pl != 32 || sl != 64) { fprintf(stderr, "otatool: bad key, bundle or signature file\n"); return 1; }
    if (crypto_eddsa_check(sig, pk, b, bl)) { fprintf(stderr, "otatool: SIGNATURE DOES NOT VERIFY, nothing installed\n"); return 1; }

    const char *p = (const char *)b, *end = p + bl;
    if (bl < 7 || memcmp(p, "HMOTA1\n", 7)) { fprintf(stderr, "otatool: not a bundle\n"); return 1; }
    p += 7;
    if (sscanf(p, "version %63[^\n]\n", version) != 1) { fprintf(stderr, "otatool: no version\n"); return 1; }
    p = memchr(p, '\n', end - p) + 1;
    if (dest && mkdir(dest, 0755)) { fprintf(stderr, "otatool: %s: %s\n", dest, strerror(errno)); return 1; }
    if (dest) chmod(dest, 0755);                        /* root's umask on the device is 077; the daemon's user must get in */
    for (;;) {
        char name[80], path[512]; unsigned mode; size_t size; int used = 0;
        if (end - p >= 4 && !memcmp(p, "end\n", 4)) break;
        /* %n before the newline, not after: a "\n" in the format skips every whitespace byte, so a file starting with
         * one (a space, tab, newline) lost those bytes and the rest no longer parsed (2 % of otatool_test.sh's random blobs) */
        if (sscanf(p, "file %79s %o %zu%n", name, &mode, &size, &used) != 3 || !used || end - p <= used || p[used] != '\n' ||
            !name_ok(name) || size > (size_t)(end - p - used - 1)) {
            fprintf(stderr, "otatool: malformed bundle\n"); return 1;
        }
        p += used + 1;
        if (dest) { snprintf(path, sizeof path, "%s/%s", dest, name); if (spit(path, p, size, mode & 0755)) return 1; }
        p += size;
    }
    if (dest) { char vp[512]; snprintf(vp, sizeof vp, "%s/VERSION", dest); if (spit(vp, version, strlen(version), 0644)) return 1; sync(); }
    printf("%s\n", version);
    return 0;
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints = { 0 }, *ai;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai)) { fprintf(stderr, "otatool: cannot resolve %s\n", host); return -1; }
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) { fprintf(stderr, "otatool: cannot connect to %s:%s: %s\n", host, port, strerror(errno)); return -1; }
    return fd;
}

static size_t read_line(int fd, char *line, size_t cap)
{
    size_t n = 0;
    while (n < cap - 1 && read(fd, line + n, 1) == 1 && line[n] != '\n') n++;
    line[n] = 0;
    return n;
}

static int push(const char *host, const char *port, const char *bundle, const char *sigpath)
{
    size_t bl, sl; uint8_t *b = slurp(bundle, &bl), *sig = slurp(sigpath, &sl); char line[300];
    if (!b || !sig || sl != 64) return 1;
    int fd = connect_to(host, port);
    if (fd < 0) return 1;
    int hl = snprintf(line, sizeof line, "HMOTA-PUSH1 %zu\n", bl);
    if (write(fd, line, hl) != hl || write(fd, sig, 64) != 64) return 1;
    for (size_t off = 0; off < bl; ) { ssize_t w = write(fd, b + off, bl - off); if (w <= 0) { fprintf(stderr, "otatool: upload failed\n"); return 1; } off += w; }
    size_t n = read_line(fd, line, sizeof line); close(fd);
    printf("%s\n", n ? line : "FAILED no answer");
    return strncmp(line, "OK", 2) != 0;
}

/* Send REQ (one line, with its \n), sign "REQ" + the nonce the Echo answers with, print its verdict.
 * The protocol is described in src/hassmic/ota.c. */
static int challenge(const char *host, const char *port, const char *secret, const char *req)
{
    size_t skl, rl = strlen(req); uint8_t *sk = slurp(secret, &skl), msg[160], sig[64]; char line[300]; unsigned x;
    if (!sk || skl != 64) { fprintf(stderr, "otatool: bad secret key\n"); return 1; }
    if (rl + 32 > sizeof msg) { fprintf(stderr, "otatool: request too long\n"); return 1; }
    int fd = connect_to(host, port);
    if (fd < 0 || write(fd, req, rl) != (ssize_t)rl) return 1;
    if (!read_line(fd, line, sizeof line) || strncmp(line, "NONCE ", 6) || strlen(line) != 6 + 64) {
        printf("%s\n", line[0] ? line : "FAILED no challenge (an older hassmic?)"); return 1;
    }
    memcpy(msg, req, rl);
    for (int i = 0; i < 32; i++) { if (sscanf(line + 6 + 2 * i, "%2x", &x) != 1) return 1; msg[rl + i] = x; }
    crypto_eddsa_sign(sig, sk, msg, rl + 32);
    if (write(fd, sig, 64) != 64) return 1;
    size_t n = read_line(fd, line, sizeof line); close(fd);
    printf("%s\n", n ? line : "FAILED no answer");
    return strncmp(line, "OK", 2) != 0;
}

/* The system partition is mounted read-only at boot, and toybox cannot remount it: /proc/mounts names the device
 * /dev/root, which does not exist on these Echos, and naming dm-0 instead it finds no "/" in the table.  The kernel
 * needs neither for a remount, only the mount point: found here by walking up from PATH to where the device changes
 * (system-as-root on the supported Echos, so "/" for /system/hassmic).  Writable once remounted because boot-root
 * turned dm-verity off: dm-0 "system" maps the whole active slot, ro=0, ro.boot.veritymode=disabled (all three models). */
static int remount(const char *how, const char *path)
{
    char dir[512], up[512]; struct stat st, pst; int ro = !strcmp(how, "ro");
    if (!ro && strcmp(how, "rw")) { fprintf(stderr, "otatool: remount rw or ro\n"); return 2; }
    if (!realpath(path, dir) || stat(dir, &st)) { fprintf(stderr, "otatool: %s: %s\n", path, strerror(errno)); return 1; }
    while (strcmp(dir, "/")) {
        snprintf(up, sizeof up, "%s", dir);
        char *slash = strrchr(up, '/'); if (slash == up) slash[1] = 0; else *slash = 0;
        if (stat(up, &pst) || pst.st_dev != st.st_dev) break;
        memcpy(dir, up, sizeof dir);
    }
    if (mount(NULL, dir, NULL, MS_REMOUNT | (ro ? MS_RDONLY : 0), NULL)) {
        fprintf(stderr, "otatool: remount %s %s: %s\n", how, dir, strerror(errno)); return 1;
    }
    printf("%s\n", dir);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "keygen")) return keygen(argv[2], argv[3]);
    if (argc >= 6 && !strcmp(argv[1], "pack")) return pack(argc - 2, argv + 2);
    if (argc == 5 && !strcmp(argv[1], "verify")) return verify_install(argv[2], argv[3], argv[4], NULL);
    if (argc == 6 && !strcmp(argv[1], "push")) return push(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 6 && !strcmp(argv[1], "install")) return verify_install(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 5 && !strcmp(argv[1], "adb")) return challenge(argv[2], argv[3], argv[4], "HMOTA-ADB1\n");
    if (argc == 4 && !strcmp(argv[1], "remount")) return remount(argv[2], argv[3]);
    fprintf(stderr, "usage: otatool keygen SECRET PUBLIC | push HOST PORT BUNDLE SIG | adb HOST PORT SECRET | pack SECRET VERSION OUT FILE[:MODE]... | verify PUBLIC BUNDLE SIG | install PUBLIC BUNDLE SIG DESTDIR | remount rw|ro PATH\n");
    return 2;
}

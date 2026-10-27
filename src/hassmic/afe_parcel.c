/* afe_parcel.h: the "service call" tool's arguments and its printed reply */
#include "afe_parcel.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int add(struct afe_call *c, const char *fmt, long v)
{
    if (c->n + 1 >= AFE_SVC_MAXARGS) return -1;
    snprintf(c->words[c->n], sizeof c->words[c->n], fmt, v);
    c->argv[c->n] = c->words[c->n];
    c->n++;
    c->argv[c->n] = NULL;
    return 0;
}

int svc_i32(struct afe_call *c, long v) { return add(c, "i32", 0) || add(c, "%ld", v) ? -1 : 0; }

/* the bytes as little-endian words, padded to 4: writeByteArray's body and writeCString alike */
static int bytes(struct afe_call *c, const unsigned char *p, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4 && i + k < len; k++) w |= (uint32_t)p[i + k] << (8 * k);
        if (svc_i32(c, (long)(int32_t)w)) return -1;
    }
    return 0;
}

int svc_cstr(struct afe_call *c, const char *s) { return bytes(c, (const unsigned char *)s, strlen(s) + 1); }

int svc_blob(struct afe_call *c, const void *p, size_t len) { return svc_i32(c, (long)len) || bytes(c, p, len) ? -1 : 0; }

void svc_call_start(struct afe_call *c, const char *svc, const char *code)
{
    c->argv[0] = "/system/bin/service"; c->argv[1] = "call"; c->argv[2] = (char *)svc; c->argv[3] = (char *)code;
    c->argv[4] = NULL; c->n = 4;
}

int afe_call_build(struct afe_call *c, int cmd, const void *in, size_t in_len, int out_size)
{
    svc_call_start(c, "audiosignalprocessor", "3");
    if (svc_i32(c, cmd) || svc_blob(c, in, in_len) || svc_i32(c, out_size)) return -1;
    return 0;
}

/* Parcel's print through HexDump: up to 32 bytes on one line after "Parcel(", else lines "0x<offset>: " with up to four
 * words; a word is the little-endian uint32 as 8 hex digits, then the ASCII column in quotes (where a ' may stand for
 * itself, so only the words are read, from the left). */
int svc_reply_words(const char *text, uint32_t *w, size_t max)
{
    size_t nw = 0;
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        const char *s = strstr(line, "Parcel("), *nl = strchr(line, '\n');
        int words_here = s && (!nl || s < nl);
        s = words_here ? s + 7 : line;
        while (*s == ' ') s++;
        unsigned off; int used = 0;
        if (sscanf(s, "0x%8x: %n", &off, &used) >= 1 && used) { s += used; words_here = 1; }
        if (!words_here) continue;
        for (;;) {
            unsigned v; int used = 0;
            if (strspn(s, "0123456789abcdef") != 8 || (s[8] != ' ' && s[8] != '\n' && s[8] != ')' && s[8]) ||
                sscanf(s, "%8x%n", &v, &used) != 1 || used != 8 || nw >= max) break;
            w[nw++] = v;
            s += 8;
            if (*s == ' ') s++;
        }
    }
    return (int)nw;
}

int afe_call_reply(const char *text, unsigned char *buf, size_t cap)
{
    uint32_t w[1100]; size_t nw = (size_t)svc_reply_words(text, w, sizeof w / sizeof w[0]);
    if (nw < 3 || w[0] != 0 || w[1] != 0) return -1;                /* exception, then command()'s status */
    if (w[2] == 0xffffffffu) { if (cap) buf[0] = 0; return 0; }      /* a null array */
    size_t len = w[2];
    if (len > (nw - 3) * 4) return -1;
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(w[3 + i / 4] >> (8 * (i % 4)));
    buf[n] = 0;
    return (int)n;
}

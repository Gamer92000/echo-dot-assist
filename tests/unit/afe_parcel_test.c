/* afe_parcel.c: the arguments "service call" gets, and replies as Android 7's Parcel::print / HexDump show them */
#include "afe_parcel.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static void joined(struct afe_call *c, char *out, size_t n)
{
    out[0] = 0;
    for (int i = 0; c->argv[i]; i++) { strncat(out, c->argv[i], n - strlen(out) - 2); strcat(out, " "); }
}

int main(void)
{
    struct afe_call c; char s[2048]; unsigned char out[512]; int n;
    int on = 1;
    check(afe_call_build(&c, 146, &on, 4, 4) == 0, "listening mode: built");
    joined(&c, s, sizeof s);
    check(!strcmp(s, "/system/bin/service call audiosignalprocessor 3 i32 146 i32 4 i32 1 i32 4 "), s);
    const char *meta = "{\"timestamp_before_ww_start\":8823,\"timestamp_before_ww_end\":9559}";
    check(afe_call_build(&c, 114, meta, strlen(meta), 4) == 0, "wake word metadata: built");
    joined(&c, s, sizeof s);
    /* 65 bytes: 17 words, the last one "}" alone; '{"ti' = 0x6974227b */
    const char *pre = "/system/bin/service call audiosignalprocessor 3 i32 114 i32 65 i32 1769218683 ";
    check(!strncmp(s, pre, strlen(pre)) && strlen(s) > 7 && !strcmp(s + strlen(s) - 15, " i32 125 i32 4 "),
          "JSON as little-endian words, the last padded, then the out size");
    static char big[4096];
    check(afe_call_build(&c, 114, big, sizeof big, 4) == -1, "an input that needs too many words: refused");

    /* short reply: one line, no offset */
    n = afe_call_reply("Result: Parcel(00000000 00000000 00000000 '............')\n", out, sizeof out);
    check(n == 0, "an empty out array: 0 bytes");
    n = afe_call_reply("Result: Parcel(00000000 ffffffea ffffffff '............')\n", out, sizeof out);
    check(n == -1, "a status other than 0: none");
    n = afe_call_reply("Result: Parcel(Error: 0xffffffb5 \"Not a data message\")\n", out, sizeof out);
    check(n == -1, "an error parcel: none");
    /* long reply: offset lines, an ASCII column with a quote and hex-looking text in it */
    const char *json = "{\"sequenceID\":0,\"voiceEnergy\":8717,\"ambientEnergy\":62048,\"x\":\"'abcdef12\"}";
    char dump[4096] = "Result: Parcel(\n"; unsigned char bytes[512]; size_t jl = strlen(json), total;
    memset(bytes, 0, sizeof bytes);
    bytes[8] = (unsigned char)jl;                                 /* exception 0, status 0, length, data */
    memcpy(bytes + 12, json, jl);
    total = (12 + jl + 3) & ~3u;
    for (size_t off = 0; off < total; off += 16) {
        char line[160]; int k = snprintf(line, sizeof line, "  0x%08zx: ", off);
        for (size_t i = off; i < off + 16 && i < total; i += 4)
            k += snprintf(line + k, sizeof line - k, "%02x%02x%02x%02x ", bytes[i + 3], bytes[i + 2], bytes[i + 1], bytes[i]);
        k += snprintf(line + k, sizeof line - k, "'");
        for (size_t i = off; i < off + 16 && i < total; i++) line[k++] = bytes[i] >= ' ' && bytes[i] <= '~' ? (char)bytes[i] : '.';
        snprintf(line + k, sizeof line - k, "'\n");
        strcat(dump, line);
    }
    strcat(dump, ")\n");
    n = afe_call_reply(dump, out, sizeof out);
    check(n == (int)jl && !strcmp((char *)out, json), "arbitration JSON out of a multi-line dump");
    n = afe_call_reply(dump, out, 20);
    check(n == 19 && !strncmp((char *)out, json, 19) && out[19] == 0, "cut at the buffer's size, NUL-terminated");
    char lied[4096]; strcpy(lied, dump);
    char *len = strstr(lied, "0x00000000: ") + 12 + 18;            /* the length word: claims more than is there */
    memcpy(len, "00000400", 8);
    check(afe_call_reply(lied, out, sizeof out) == -1, "a length past the words that came: none");
    check(afe_call_reply("sh: service: not found\n", out, sizeof out) == -1, "no tool: none");
    return fails ? 1 : 0;
}

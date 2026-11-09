/* pb.h: varints of every length written and read back, a field of each wire type, and what is malformed, each message
 * in exactly its own bytes (under ASan/UBSan: no read past them, no shift past 63 bits): a varint cut off at the end,
 * a length past the end, a fixed field cut short, a varint of 11 bytes as the key, a value or a length (they came
 * from anyone on the network before a key was set: the shift went to 70), and a field number of 0 or past 2^29 - 1 */
#include "pb.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

/* the first field of m, read out of a copy that is exactly n bytes long (f->data points into it until the next call):
 * 1 read, 0 malformed or empty */
static unsigned char *held;
static int first(const unsigned char *m, size_t n, struct pbf *f)
{
    free(held); held = malloc(n ? n : 1); memcpy(held, m, n);
    const unsigned char *p = held;
    return pb_next(&p, held + n, f);
}

int main(void)
{
    unsigned char buf[64]; struct pb b; struct pbf f; char what[96];
    static const uint64_t vs[] = { 1, 127, 128, 16383, 16384, 1ull << 35, (1ull << 63) - 1, 1ull << 63, UINT64_MAX };
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++) {
        b = (struct pb){ buf, 0, sizeof buf }; pb_uint(&b, 100000, vs[i]);
        snprintf(what, sizeof what, "field 100000 = %llu (%zu bytes): read back", (unsigned long long)vs[i], b.n);
        check(first(buf, b.n, &f) && f.field == 100000 && f.wire == 0 && f.v == vs[i], what);
    }
    check(b.n == 3 + 10, "the largest varint takes 10 bytes");
    b = (struct pb){ buf, 0, sizeof buf }; pb_str(&b, 7, "abc");
    check(first(buf, b.n, &f) && f.field == 7 && f.wire == 2 && f.len == 3 && !memcmp(f.data, "abc", 3), "bytes: read back");
    b = (struct pb){ buf, 0, sizeof buf }; pb_float(&b, 5, 0.25f);
    { uint32_t u; float x = 0.25f; memcpy(&u, &x, 4); check(first(buf, b.n, &f) && f.field == 5 && f.wire == 5 && f.v == u, "fixed32: read back"); }
    b = (struct pb){ buf, 0, sizeof buf }; pb_sint(&b, 2, -3);
    check(first(buf, b.n, &f) && f.field == 2 && f.v == 5, "zigzag -3: 5");
    static const unsigned char fixed64[] = { 0x09, 1, 2, 3, 4, 5, 6, 7, 8 };
    check(first(fixed64, sizeof fixed64, &f) && f.field == 1 && f.wire == 1, "fixed64: skipped whole");

    /* malformed */
    static const unsigned char key_cut[] = { 0x80 }, value_cut[] = { 0x08, 0x80 }, len_cut[] = { 0x0a, 0x80 },
        len_past[] = { 0x0a, 0x05, 'a' }, f64_short[] = { 0x09, 1, 2, 3 }, f32_short[] = { 0x0d, 1, 2 }, group[] = { 0x0b };
    check(!first(key_cut, sizeof key_cut, &f), "a key cut off: malformed");
    check(!first(value_cut, sizeof value_cut, &f), "a value cut off: malformed");
    check(!first(len_cut, sizeof len_cut, &f), "a length cut off: malformed");
    check(!first(len_past, sizeof len_past, &f), "a length past the end: malformed");
    check(!first(f64_short, sizeof f64_short, &f) && !first(f32_short, sizeof f32_short, &f), "fixed fields cut short: malformed");
    check(!first(group, sizeof group, &f), "a group (wire type 3): malformed");
    static const unsigned char key11[] = { 0x88, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01 },
        value11[] = { 0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 },
        len11[] = { 0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
    check(!first(key11, sizeof key11, &f), "a key of 11 bytes: malformed");
    check(!first(value11, sizeof value11, &f), "a value of 11 bytes: malformed");
    check(!first(len11, sizeof len11, &f), "a length of 11 bytes: malformed");
    /* field numbers: 1 to 2^29 - 1 (keys as bytes: pb_uint's f << 3 is an int) */
    static const unsigned char field_top[] = { 0xf8, 0xff, 0xff, 0xff, 0x0f, 0x01 }, field_past[] = { 0x80, 0x80, 0x80, 0x80, 0x10, 0x01 },
        field_wrap[] = { 0x88, 0x80, 0x80, 0x80, 0x80, 0x01, 0x01 }, field0[] = { 0x00, 0x01 };
    check(first(field_top, sizeof field_top, &f) && f.field == PB_FIELD_MAX && f.wire == 0 && f.v == 1, "field 2^29 - 1: read back");
    check(!first(field_past, sizeof field_past, &f), "field 2^29: malformed");
    check(!first(field_wrap, sizeof field_wrap, &f), "field 2^32 + 1 (would wrap onto 1): malformed");
    check(!first(field0, sizeof field0, &f), "field 0: malformed");
    free(held);
    printf("%s\n", fails ? "pb: FAILED" : "pb: ok");
    return fails != 0;
}

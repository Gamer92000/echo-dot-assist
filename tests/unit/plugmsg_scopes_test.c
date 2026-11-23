/* plugmsg.h as tools/mkplugmsg.py makes it for a message that grew (tests/unit/plugmsg_scopes/demo.def: PUT gained a
 * field and a field of its list's element in demo 2, NEW came with demo 2): each version's reader and writer keep
 * that version's layout, a packet of another version is malformed (too short, or bytes after the last field), and
 * a type a peer does not speak is unknown to it.  Built against build/plugmsg_demo/plugmsg.h (make unit-plugmsg) */
#include "plugmsg.h"
#include <stdio.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

/* PUT at version V: KIND_A, "ab", two elements (KIND_A, then X), 0x1234 */
static long put_x(uint8_t *buf, size_t cap, unsigned v, unsigned x)
{
    struct pm_w w; struct pm_put m = { PM_KIND_A, { "ab", 2 }, { { 0 }, 2 }, 0x1234 }; struct pm_put_items e = { PM_KIND_A, 0x5566 };
    pm_winit(&w, buf, cap); pm_put_put(&w, 3, &m, v); pm_add_put_items(&w, &e, v); e.x = (uint8_t)x; pm_add_put_items(&w, &e, v);
    return pm_wlen(&w);
}
static long put(uint8_t *buf, size_t cap, unsigned v) { return put_x(buf, cap, v, PM_KIND_A); }

int main(void)
{
    uint8_t one[256], two[256]; long n1 = put(one, sizeof one, 1), n2 = put(two, sizeof two, 2);
    uint16_t v1[PM_SCOPES] = { 0 }, v2[PM_SCOPES] = { 0 }; struct pm_put m; struct pm_put_items e;
    v1[PM_SCOPE_CORE] = v2[PM_SCOPE_CORE] = 1; v1[PM_SCOPE_DEMO] = 1; v2[PM_SCOPE_DEMO] = 2;
    /* the layouts: version 1 is the fields of demo 1 only, version 2 its own two more (2 bytes each) */
    CHECK(n1 == PM_HEADER + 1 + 4 + 2 + 2 * 1 && n2 == n1 + 2 + 2 * 2, "each version's writer, its version's layout");
    CHECK(!memcmp(one, two, PM_HEADER + 1 + 4 + 2), "version 2 begins as version 1 does (the fields of 1, in their order)");
    CHECK(!pm_get_put(one + PM_HEADER, (size_t)n1 - PM_HEADER, &m, 1) && m.a == PM_KIND_A && m.b == 0 && m.items.n == 2, "version 1 read as version 1");
    CHECK(pm_next_put_items(&m.items, &e, 1) && e.x == PM_KIND_A && e.y == 0 && pm_next_put_items(&m.items, &e, 1) && e.x == PM_KIND_A && !pm_next_put_items(&m.items, &e, 1), "its list, the element of version 1");
    CHECK(!pm_get_put(two + PM_HEADER, (size_t)n2 - PM_HEADER, &m, 2) && m.b == 0x1234, "version 2 read as version 2");
    CHECK(pm_next_put_items(&m.items, &e, 2) && e.x == PM_KIND_A && e.y == 0x5566, "its list, the element of version 2");
    CHECK(pm_get_put(two + PM_HEADER, (size_t)n2 - PM_HEADER, &m, 1) == -1, "version 2's packet read as version 1: bytes after its last field, malformed");
    CHECK(pm_get_put(one + PM_HEADER, (size_t)n1 - PM_HEADER, &m, 2) == -1, "version 1's packet read as version 2: too short, malformed");
    CHECK(pm_check(PM_PUT, one + PM_HEADER, (size_t)n1 - PM_HEADER, v1) == 0 && pm_check(PM_PUT, two + PM_HEADER, (size_t)n2 - PM_HEADER, v1) == 1
          && pm_check(PM_PUT, two + PM_HEADER, (size_t)n2 - PM_HEADER, v2) == 0 && pm_check(PM_PUT, one + PM_HEADER, (size_t)n1 - PM_HEADER, v2) == 1,
          "pm_check by the version each peer speaks");
    CHECK(pm_check(PM_PUT, two + PM_HEADER, (size_t)n2 - PM_HEADER, NULL) == 0, "NULL: this build's own version (demo 2)");
    /* a type of a later version, or of a scope not spoken */
    { uint8_t z[] = { 9 };
      CHECK(!pm_spoken(PM_NEW, v1) && pm_check(PM_NEW, z, 1, v1) == -1 && pm_spoken(PM_NEW, v2) && pm_check(PM_NEW, z, 1, v2) == 0, "NEW is demo 2's");
      v1[PM_SCOPE_DEMO] = 0; v1[PM_SCOPE_OTHER] = 1;
      CHECK(!pm_spoken(PM_PUT, v1) && pm_spoken(PM_BOTH, v1) && pm_check(PM_BOTH, z, 1, v1) == 0, "BOTH with either of its scopes");
      CHECK(pm_scope_version(PM_SCOPE_DEMO) == 2 && PM_SCOPE_VERSION_DEMO == 2 && PM_MORE == 20 && PM_KIND_B == 2, "this build's versions, a constant and a value of demo 2"); }
    /* an enum's value of a later version: malformed to a peer of the version before it, as a field of that version
     * would be (finding 3 of a review of 4e0a202: values, constants and the manifest's keys went unversioned) */
    { uint8_t b[256]; long n = put(b, sizeof b, 2); uint16_t w1[PM_SCOPES] = { 0 }, w2[PM_SCOPES] = { 0 };
      w1[PM_SCOPE_CORE] = w2[PM_SCOPE_CORE] = 1; w1[PM_SCOPE_DEMO] = 1; w2[PM_SCOPE_DEMO] = 2;
      CHECK(pm_check(PM_PUT, b + PM_HEADER, (size_t)n - PM_HEADER, w2) == 0, "KIND_A at demo 2");
      b[PM_HEADER] = PM_KIND_B;
      CHECK(pm_check(PM_PUT, b + PM_HEADER, (size_t)n - PM_HEADER, w2) == 0, "KIND_B, demo 2's, at demo 2");
      n = put(b, sizeof b, 1); b[PM_HEADER] = PM_KIND_B;
      CHECK(pm_check(PM_PUT, b + PM_HEADER, (size_t)n - PM_HEADER, w1) == 1, "KIND_B at demo 1: malformed");
      b[PM_HEADER] = PM_KIND_C;
      CHECK(pm_check(PM_PUT, b + PM_HEADER, (size_t)n - PM_HEADER, w1) == 0, "KIND_C, demo 1's, at demo 1");
      n = put(b, sizeof b, 2); b[PM_HEADER] = 5;
      CHECK(pm_check(PM_PUT, b + PM_HEADER, (size_t)n - PM_HEADER, NULL) == 1, "a value KIND has in no version: malformed (the layout of demo 2, this build's own)");
      b[PM_HEADER] = PM_KIND_A;
      CHECK(pm_check(PM_PUT, b + PM_HEADER, (size_t)n - PM_HEADER, NULL) == 0, "... and with KIND_A in its place, well-formed");
      CHECK(pm_kind_ok(PM_KIND_B, w2) && !pm_kind_ok(PM_KIND_B, w1) && pm_kind_ok(PM_KIND_A, w1) && pm_kind_ok(PM_KIND_B, NULL) && !pm_kind_ok(0, NULL), "pm_kind_ok by version");
      CHECK(!PM_CONST_SPOKEN(MORE, w1) && PM_CONST_SPOKEN(MORE, w2) && PM_CONST_SPOKEN(LIM, w1) && PM_CONST_SPOKEN(MORE, NULL), "a constant of demo 2: spoken from demo 2 on");
      CHECK(pm_cap_ok(PM_CAP_OTHER, w1) && !pm_cap_ok(2, NULL), "a capability's value: this build's");
      /* what a sender sends a peer that lacks a value: its fallback (finding 4 of a review of 60a8589: the packet was
       * dropped), demo.def's B=2@2>A; a value no version has stays what it is, and pm_check refuses it */
      CHECK(pm_kind_for(PM_KIND_B, w1) == PM_KIND_A && pm_kind_for(PM_KIND_B, w2) == PM_KIND_B && pm_kind_for(PM_KIND_A, w1) == PM_KIND_A
            && pm_kind_for(PM_KIND_B, NULL) == PM_KIND_B && pm_kind_for(9, w1) == 9, "pm_kind_for: B to a peer of demo 1 is A");
      /* pm_fit over a whole packet, a field of the message and one of its list's second element: made what each peer
       * speaks in place, nothing else touched */
      { uint8_t c[256], d[256]; struct pm_put f; struct pm_put_items fe; long k = put_x(c, sizeof c, 1, PM_KIND_B), l = put(d, sizeof d, 1);
        c[PM_HEADER] = PM_KIND_B;
        CHECK(pm_check(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w1) == 1, "B in PUT and in its list, at demo 1: malformed as it is");
        CHECK(!pm_fit(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w1) && k == l && !memcmp(c, d, (size_t)k) && pm_check(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w1) == 0,
              "pm_fit at demo 1: A in both places, the rest as it was");
        k = put_x(c, sizeof c, 2, PM_KIND_B); c[PM_HEADER] = PM_KIND_B;
        CHECK(!pm_fit(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w2) && !pm_get_put(c + PM_HEADER, (size_t)k - PM_HEADER, &f, 2) && f.a == PM_KIND_B && f.b == 0x1234
              && pm_next_put_items(&f.items, &fe, 2) && pm_next_put_items(&f.items, &fe, 2) && fe.x == PM_KIND_B && fe.y == 0x5566, "pm_fit at demo 2: B stays B, version 2's fields read past");
        CHECK(pm_fit(PM_PUT, c + PM_HEADER, 5, w2) == -1 && !pm_fit(PM_NEW, c + PM_HEADER, 1, w2), "a packet cut short: -1, left to pm_check; a message without such a field: nothing to do");
        /* a value whose fallback is none (demo.def's D=4@2>-; finding 3 of a review of 4c55b54): no value of demo 1 is
         * right, so the packet is not for a peer of demo 1 at all (1: its sender drops it without a word), wherever it
         * stands (the message's field, its list's second element); to a peer of demo 2 it goes as it is */
        CHECK(pm_kind_for(PM_KIND_D, w1) == PM_NOT_SENT && pm_kind_for(PM_KIND_D, w2) == PM_KIND_D && pm_kind_for(PM_KIND_D, NULL) == PM_KIND_D, "pm_kind_for: D to a peer of demo 1 is none");
        k = put_x(c, sizeof c, 1, PM_KIND_D);
        CHECK(pm_check(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w1) == 1 && pm_fit(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w1) == 1, "D in PUT's list at demo 1: pm_fit 1, not sent");
        k = put_x(c, sizeof c, 1, PM_KIND_B); c[PM_HEADER] = PM_KIND_D;
        CHECK(pm_fit(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w1) == 1, "D in PUT's own field, B in its list, at demo 1: 1 (not sent, whatever else was made what demo 1 has)");
        k = put_x(c, sizeof c, 2, PM_KIND_D); c[PM_HEADER] = PM_KIND_D;
        CHECK(!pm_fit(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w2) && c[PM_HEADER] == PM_KIND_D && pm_check(PM_PUT, c + PM_HEADER, (size_t)k - PM_HEADER, w2) == 0, "D at demo 2: 0, as it is"); } }
    /* the manifest's keys, and the versions' marks (docs/plugins.md 3.3: a raised requires into a widening version) */
    { int sc = -1;
      CHECK(pm_key("demo.entry", "name", &sc) == 1 && sc == PM_SCOPE_DEMO && pm_key("demo.entry", "extra", &sc) == 2 && pm_key("demo.entry", "other", &sc) == 0 && pm_key("x", "name", NULL) == 0,
            "pm_key: the version of its scope a key came with");
      CHECK(pm_widens(PM_SCOPE_DEMO, 1, 2) && !pm_widens(PM_SCOPE_DEMO, 2, 2) && !pm_widens(PM_SCOPE_DEMO, 1, 1) && !pm_widens(PM_SCOPE_OTHER, 1, 2) && pm_widens(PM_SCOPE_DEMO, 0, 2),
            "pm_widens: demo 2 widens, from 1 to 2 and nothing else");
      CHECK(!strcmp(pm_version_doc(PM_SCOPE_DEMO, 2), "NEW, PUT's b and the key extra") && !pm_version_doc(PM_SCOPE_DEMO, 1)[0], "each version's words"); }
    printf("plugmsg scopes: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}

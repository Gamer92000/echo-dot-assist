/* The broker protocol's wire format (sdk/plugmsg.h, made from sdk/messages.def) on what a plugin may send: every
 * type written at its largest and read back, cut at every length, with every byte changed, and random packets.
 * Built with ASan and UBSan: a read past the packet ends the test. */
#include "plugmsg.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static char big[1100];
static struct pm_str S(unsigned n) { struct pm_str s = { big, (uint16_t)n }; return s; }

/* one packet of each type, lists full, strings at their limits (as far as 32 kB hold them) */
static long sample(uint16_t type, uint8_t *buf, size_t cap)
{
    struct pm_w w; pm_winit(&w, buf, cap);
    switch (type) {
    /* (the greeting at its bounds for good: every scope there will ever be, PM_SCOPES_MAX) */
    case PM_HELLO: { struct pm_hello m = { 7, { { 0 }, PM_SCOPES_MAX } }; struct pm_hello_scopes e = { S(24), 0xffff };
                     pm_put_hello(&w, 0, &m); for (int i = 0; i < PM_SCOPES_MAX; i++) pm_add_hello_scopes(&w, &e); break; }
    case PM_WELCOME: { struct pm_welcome m = { 1, S(48), S(64), S(64), S(64), { { 0 }, PM_SCOPES_MAX } }; struct pm_welcome_scopes e = { S(24), 1 };
                       pm_put_welcome(&w, 0, &m); for (int i = 0; i < PM_SCOPES_MAX; i++) pm_add_welcome_scopes(&w, &e); break; }
    case PM_REFUSED: { struct pm_refused m = { PM_REFUSED_SCOPES }; pm_put_refused(&w, 0, &m); break; }
    case PM_CONFIG: { struct pm_config m = { { { 0 }, 32 } }; struct pm_config_settings e = { S(32), S(512) };
                      pm_put_config(&w, 0, &m); for (int i = 0; i < 32; i++) pm_add_config_settings(&w, &e); break; }
    case PM_READY: pm_put_ready(&w, 0); break;
    case PM_PING: pm_put_ping(&w, 5); break;
    case PM_PONG: pm_put_pong(&w, 5); break;
    case PM_STOP: pm_put_stop(&w, 0); break;
    case PM_TIME: { struct pm_time m = { 1, S(64) }; pm_put_time(&w, 0, &m); break; }
    case PM_OK: pm_put_ok(&w, 9); break;
    case PM_ERR: { struct pm_err m = { PM_ERR_DENIED }; pm_put_err(&w, 9, &m); break; }
    case PM_FILES: { struct pm_files m = { S(32), { { 0 }, 16 } };
                     struct pm_files_items it = { S(40), 1, S(64), { { 0 }, 3 }, { { 0 }, 2 } };
                     struct pm_files_items_fields f = { S(32), S(256) }; struct pm_files_items_files fl = { S(64), 123456 };
                     pm_put_files(&w, 0, &m);
                     for (int i = 0; i < 16; i++) { pm_add_files_items(&w, &it); for (int k = 0; k < 3; k++) pm_add_files_items_fields(&w, &f);
                                                    for (int k = 0; k < 2; k++) pm_add_files_items_files(&w, &fl); }
                     break; }
    case PM_FILE_OPEN: { struct pm_file_open m = { S(32), S(40), S(64) }; pm_put_file_open(&w, 3, &m); break; }
    case PM_FILE_CHECK: { struct pm_file_check m = { S(32), S(40) }; pm_put_file_check(&w, 4, &m); break; }
    case PM_FILE_OK: { struct pm_file_ok m = { S(64), { { 0 }, 16 } }; struct pm_file_ok_fields f = { S(32), S(256) };
                       pm_put_file_ok(&w, 4, &m); for (int i = 0; i < 16; i++) pm_add_file_ok_fields(&w, &f); break; }
    case PM_FILE_BAD: { struct pm_file_bad m = { PM_FILE_BAD_FORMAT, S(200) }; pm_put_file_bad(&w, 4, &m); break; }
    case PM_WAKE_WORDS: { struct pm_wake_words m = { S(32), { { 0 }, 16 } }; struct pm_wake_words_words e = { S(40) };
                          pm_put_wake_words(&w, 0, &m); for (int i = 0; i < 16; i++) pm_add_wake_words_words(&w, &e); break; }
    case PM_WAKE_OPEN: { struct pm_wake_open m = { 1, S(40) }; pm_put_wake_open(&w, 0, &m); break; }
    case PM_WAKE_READY: { struct pm_wake_ready m = { 1 }; pm_put_wake_ready(&w, 0, &m); break; }
    case PM_WAKE_FAILED: { struct pm_wake_failed m = { 1, PM_WAKE_FAIL_BAD_MODEL }; pm_put_wake_failed(&w, 0, &m); break; }
    case PM_WAKE_CLOSE: { struct pm_wake_close m = { 1 }; pm_put_wake_close(&w, 0, &m); break; }
    case PM_WAKE_RESET: pm_put_wake_reset(&w, 0); break;
    case PM_WAKE_HEARD: { struct pm_wake_heard m = { 1, 200, 0x123456789abcULL, 0x12345678ffffULL }; pm_put_wake_heard(&w, 0, &m); break; }
    /* storage, the log, the network.  A value is bytes, NULs and all */
    case PM_STORE_GET: { struct pm_store_get m = { S(48) }; pm_put_store_get(&w, 6, &m); break; }
    case PM_STORE_VALUE: { static uint8_t v[16384]; struct pm_store_value m = { pm_b(v, sizeof v) }; pm_put_store_value(&w, 6, &m); break; }
    case PM_STORE_PUT: { static uint8_t v[16384]; struct pm_store_put m = { S(48), pm_b(v, sizeof v) }; pm_put_store_put(&w, 6, &m); break; }
    case PM_STORE_DEL: { struct pm_store_del m = { S(48) }; pm_put_store_del(&w, 6, &m); break; }
    case PM_LOG_FOLLOW: { struct pm_log_follow m = { S(40) }; pm_put_log_follow(&w, 7, &m); break; }
    case PM_LOG: { struct pm_log m = { S(40), 1, { { 0 }, 30 } }; struct pm_log_lines e = { S(1024) };
                   pm_put_log(&w, 0, &m); for (int i = 0; i < 30; i++) pm_add_log_lines(&w, &e); break; }
    case PM_CONNECT: { struct pm_connect m = { PM_CAP_NET_SERVER, 3, S(253), 65535 }; pm_put_connect(&w, 8, &m); break; }
    /* UDP, the search, proposals, music, the volume, the Echo's state, Home Assistant */
    case PM_UDP_OPEN: { struct pm_udp_open m = { PM_CAP_NET_UDP, 3 }; pm_put_udp_open(&w, 10, &m); break; }
    case PM_UDP_OPENED: { struct pm_udp_opened m = { 4, 28999 }; pm_put_udp_opened(&w, 10, &m); break; }
    case PM_UDP_PEER: { struct pm_udp_peer m = { 1, 0xC0A80114u, 65535 }; pm_put_udp_peer(&w, 11, &m); break; }
    case PM_SENDTO: { static uint8_t v[1472]; struct pm_sendto m = { 1, 0x0A000001u, 3483, pm_b(v, sizeof v) }; pm_put_sendto(&w, 0, &m); break; }
    case PM_BROADCAST: { static uint8_t v[1472]; struct pm_broadcast m = { 4, pm_b(v, sizeof v) }; pm_put_broadcast(&w, 0, &m); break; }
    case PM_PROPOSE: { struct pm_propose m = { S(32), S(15) }; pm_put_propose(&w, 12, &m); break; }
    case PM_NET: { struct pm_net m = { 0x0A000002u, 0xFFFFFF00u, 1 }; pm_put_net(&w, 0, &m); break; }
    case PM_MUSIC_OPEN: { struct pm_music_open m = { 48000, 2 }; pm_put_music_open(&w, 13, &m); break; }
    case PM_MUSIC_STATE: { struct pm_music_state m = { PM_MUSIC_PAUSED, S(128), S(128) }; pm_put_music_state(&w, 0, &m); break; }
    case PM_MUSIC_CLOSE: pm_put_music_close(&w, 14); break;
    case PM_MUSIC_POS: { struct pm_music_pos m = { 0x123456789aULL, 0xfedcba987654ULL, 70000 }; pm_put_music_pos(&w, 0, &m); break; }
    case PM_MUSIC_CMD: { struct pm_music_cmd m = { PM_MCMD_PREVIOUS, PM_MWHY_HOME_ASSISTANT }; pm_put_music_cmd(&w, 0, &m); break; }
    case PM_VOLUME_SET: { struct pm_volume_set m = { 100 }; pm_put_volume_set(&w, 15, &m); break; }
    case PM_VOLUME: { struct pm_volume m = { 55 }; pm_put_volume(&w, 0, &m); break; }
    case PM_STATE: { struct pm_state m = { PM_STATE_SPEAKING, 1, 42 }; pm_put_state(&w, 0, &m); break; }
    case PM_ENTITY_STATE: { struct pm_entity_state m = { S(32), S(255) }; pm_put_entity_state(&w, 0, &m); break; }
    case PM_EVENT: { struct pm_event m = { S(32), S(32) }; pm_put_event(&w, 0, &m); break; }
    case PM_ENTITY_SET: { struct pm_entity_set m = { S(32), S(255) }; pm_put_entity_set(&w, 0, &m); break; }
    case PM_ACTION: { struct pm_action m = { S(32), PM_FROM_PAGE, S(32), { { 0 }, 4 } }; struct pm_action_args e = { S(32), S(255) };
                      pm_put_action(&w, 0, &m); for (int i = 0; i < 4; i++) pm_add_action_args(&w, &e); break; }
    /* calls */
    case PM_CALL_INCOMING: { struct pm_call_incoming m = { S(64) }; pm_put_call_incoming(&w, 16, &m); break; }
    case PM_CALL_ANSWER: pm_put_call_answer(&w, 0); break;
    case PM_CALL_HANGUP: { struct pm_call_hangup m = { PM_HANGUP_STOPPED }; pm_put_call_hangup(&w, 0, &m); break; }
    case PM_CALL_OUT: { struct pm_call_out m = { S(32), S(64) }; pm_put_call_out(&w, 17, &m); break; }
    case PM_CALL_RINGING_THERE: pm_put_call_ringing_there(&w, 0); break;
    case PM_CALL_LIVE: { struct pm_call_live m = { S(32) }; pm_put_call_live(&w, 18, &m); break; }
    case PM_CALL_END: { struct pm_call_end m = { PM_RESULT_FAILED }; pm_put_call_end(&w, 19, &m); break; }
    case PM_REQ_START: { struct pm_req_start m = { 0xfedcba98u, PM_REQSRC_HA_WAKE, 1, S(64) }; pm_put_req_start(&w, 0, &m); break; }
    case PM_REQ_END: { struct pm_req_end m = { 0xfedcba98u, PM_REQEND_MUTED, 0x12345678u, 0x9abcdefu }; pm_put_req_end(&w, 0, &m); break; }
    case PM_REQ_TEXT: { struct pm_req_text m = { 0xfedcba98u, PM_REQTEXT_REPLY, S(1024) }; pm_put_req_text(&w, 0, &m); break; }
    case PM_BUTTON: { struct pm_button m = { PM_BUTTON_KEY_VOLUME_DOWN, PM_BUTTON_HOW_HOLD }; pm_put_button(&w, 0, &m); break; }
    default: return 0;
    }
    return pm_wlen(&w);
}

/* a packet as the broker takes one: the header, then the fields against the type.  Copied into a block of exactly
 * its size, so that ASan sees one byte too many */
static int take(const uint8_t *pkt, size_t len)
{
    uint8_t *copy = malloc(len ? len : 1); struct pm_hdr h; const uint8_t *body; size_t blen; int r = 2;
    memcpy(copy, pkt, len);
    if (!pm_head(copy, len, &h, &body, &blen)) r = pm_check(h.type, body, blen, NULL);
    free(copy);
    return r;
}

/* a bytes field takes what a string does not: NUL among them; a string never does */
static void bytes_and_strings(void)
{
    uint8_t buf[256], v[5] = { 'a', 0, 'b', 0, 0 }; struct pm_w w; struct pm_hdr h; const uint8_t *body; size_t blen;
    struct pm_store_put m = { pm_s("k"), pm_b(v, sizeof v) }, back; struct pm_store_get g = { { "a\0b", 3 } };
    pm_winit(&w, buf, sizeof buf); pm_put_store_put(&w, 1, &m);
    CHECK(pm_wlen(&w) > 0 && !pm_head(buf, (size_t)pm_wlen(&w), &h, &body, &blen) && !pm_get_store_put(body, blen, &back) && back.value.n == 5 && !memcmp(back.value.s, v, 5),
          "a value with NULs in it goes as it is");
    pm_winit(&w, buf, sizeof buf); pm_put_store_get(&w, 1, &g);
    CHECK(pm_wlen(&w) == -1, "a key with a NUL in it is no string: not written");
    pm_winit(&w, buf, sizeof buf); pm_put_store_put(&w, 1, &m); buf[PM_HEADER + 2] = 0;     /* (one put there by hand is not read) */
    CHECK(take(buf, (size_t)pm_wlen(&w)) == 1, "a key with a NUL in it, by hand: malformed");
}

int main(void)
{
    static uint8_t buf[PM_MAX_PACKET + 64]; int types = 0;
    memset(big, 'x', sizeof big);
    for (unsigned t = 0; t < 128; t++) {
        long n = sample((uint16_t)t, buf, sizeof buf);
        if (!n) { CHECK(pm_check((uint16_t)t, buf, 0, NULL) == -1, "a number without a type is unknown"); continue; }
        types++;
        CHECK(n >= PM_HEADER && n <= PM_MAX_PACKET, "the largest packet of a type fits");
        if (n < 0) { printf("     type %u (%s) does not fit\n", t, pm_name((uint16_t)t)); continue; }
        CHECK(take(buf, (size_t)n) == 0, "a full packet reads back"); if (take(buf, (size_t)n)) printf("     %s\n", pm_name((uint16_t)t));
        CHECK(pm_dir((uint16_t)t) && pm_name((uint16_t)t)[0] != '?', "every type has a name and a direction");
        /* every length short of the whole: malformed or, where the cut falls after the last field, well-formed, and
         * never a read past the end */
        for (long k = 0; k < n; k++) { int r = take(buf, (size_t)k); CHECK(r == 1 || r == 2 || (r == 0 && k >= PM_HEADER), "a cut packet"); }
        /* bytes after the last field: no layout of any version has them (both ends speak the same one), refused */
        buf[n] = 0xff; buf[n + 1] = 0xff;
        CHECK(n + 2 > PM_MAX_PACKET || take(buf, (size_t)n + 2) == 1, "bytes after the last field are refused");
        /* every byte of the fields changed in turn (lengths and counts among them) */
        for (long k = PM_HEADER; k < n; k++) for (int v = 0; v < 3; v++) {
            uint8_t old = buf[k]; buf[k] = (uint8_t)(v == 0 ? 0xff : v == 1 ? 0 : old + 1);
            take(buf, (size_t)n);
            buf[k] = old;
        }
    }
    CHECK(types == 60, "the count of types (tell docs/plugins.md 3.4 when it changes)");

    /* fields round trip */
    { struct pm_w w; struct pm_wake_heard a = { 1, 200, 0x123456789abcULL, 0x12345678ffffULL }, b; struct pm_hdr h; const uint8_t *body; size_t blen;
      pm_winit(&w, buf, sizeof buf); pm_put_wake_heard(&w, 0x01020304, &a);
      CHECK(!pm_head(buf, (size_t)pm_wlen(&w), &h, &body, &blen) && h.type == PM_WAKE_HEARD && h.id == 0x01020304, "the header");
      CHECK(!pm_get_wake_heard(body, blen, &b) && b.slot == 1 && b.score == 200 && b.begin == a.begin && b.end == a.end, "numbers");
      CHECK(buf[0] == PM_WAKE_HEARD && buf[1] == 0 && buf[4] == 4 && buf[7] == 1, "little-endian"); }
    { struct pm_w w; struct pm_config m = { { { 0 }, 2 } }, g; struct pm_config_settings e; char name[33], val[16]; int k = 0;
      pm_winit(&w, buf, sizeof buf); pm_put_config(&w, 0, &m);
      e.name = pm_s("server"); e.value = pm_s("10.0.0.2"); pm_add_config_settings(&w, &e);
      e.name = pm_s("port"); e.value = pm_s("514"); pm_add_config_settings(&w, &e);
      CHECK(!pm_get_config(buf + PM_HEADER, (size_t)pm_wlen(&w) - PM_HEADER, &g) && g.settings.n == 2, "a list's count");
      while (pm_next_config_settings(&g.settings, &e)) {
          CHECK(!pm_cstr(name, sizeof name, e.name) && !pm_cstr(val, sizeof val, e.value), "strings copy out");
          CHECK(!strcmp(name, k ? "port" : "server") && !strcmp(val, k ? "514" : "10.0.0.2"), "a list's elements, in order");
          k++;
      }
      CHECK(k == 2, "both elements"); }
    /* what must not be written: a string over its limit, one with a NUL, a list over its limit, a packet over 32 kB */
    { struct pm_w w; struct pm_file_bad m = { 1, S(201) };
      pm_winit(&w, buf, sizeof buf); pm_put_file_bad(&w, 1, &m); CHECK(pm_wlen(&w) == -1, "a string over its limit is not written");
      big[3] = 0; m.why = S(10); pm_winit(&w, buf, sizeof buf); pm_put_file_bad(&w, 1, &m); CHECK(pm_wlen(&w) == -1, "nor one with a NUL in it"); big[3] = 'x'; }
    { struct pm_w w; struct pm_wake_words m = { S(4), { { 0 }, 17 } };
      pm_winit(&w, buf, sizeof buf); pm_put_wake_words(&w, 0, &m); CHECK(pm_wlen(&w) == -1, "a list over its limit is not written"); }
    { struct pm_w w; struct pm_time m = { 1, S(10) };
      pm_winit(&w, buf, 10); pm_put_time(&w, 0, &m); CHECK(pm_wlen(&w) == -1, "a packet that does not fit its buffer is not written"); }
    /* what a peer speaks: a type of a scope it does not speak, or of a later version than it speaks, is unknown to it */
    { uint16_t ver[PM_SCOPES] = { 0 }; struct pm_w w; struct pm_hdr h; const uint8_t *body; size_t blen; struct pm_button m = { 1, 1 };
      ver[PM_SCOPE_CORE] = 1;
      pm_winit(&w, buf, sizeof buf); pm_put_button(&w, 0, &m); pm_head(buf, (size_t)pm_wlen(&w), &h, &body, &blen);
      CHECK(!pm_spoken(PM_BUTTON, ver) && pm_check(h.type, body, blen, ver) == -1, "BUTTON is not spoken without button");
      ver[PM_SCOPE_BUTTON] = 1;
      CHECK(pm_spoken(PM_BUTTON, ver) && pm_check(h.type, body, blen, ver) == 0 && pm_spoken(PM_PING, ver), "with button 1 it is");
      ver[PM_SCOPE_CORE] = 0; CHECK(!pm_spoken(PM_PING, ver), "PING is core's");
      CHECK(pm_spoken(PM_LOG, NULL) && pm_spoken(PM_CONNECT, NULL), "NULL: this build's own versions, every type");
      memset(ver, 0, sizeof ver); ver[PM_SCOPE_LOG_READ_FULL] = 1; ver[PM_SCOPE_NET_LAN] = 1;
      CHECK(pm_spoken(PM_LOG_FOLLOW, ver) && pm_spoken(PM_CONNECT, ver) && pm_spoken(PM_NET, ver) && !pm_spoken(PM_PROPOSE, ver) && !pm_spoken(PM_UDP_OPEN, ver),
            "a message of several scopes: spoken with any of them");
      CHECK(pm_caps(PM_UDP_OPEN) == (PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER)) && pm_caps(PM_HELLO) == 0, "a message's capabilities are its scopes'");
      CHECK(sizeof(pm_caps_t) * 8 >= PM_SCOPES, "a set of capabilities holds every one of this build's");
      CHECK(pm_scope_of("ha.entities", 11) == PM_SCOPE_HA_ENTITIES && pm_scope_of("ha.entitie", 10) == -1 && !strcmp(pm_scope_name(PM_SCOPE_LOG_READ_FULL), "log.read.full")
            && pm_scope_cap(PM_SCOPE_BUTTON) == PM_CAP_BUTTON && pm_scope_cap(PM_SCOPE_CORE) == -1 && pm_scope_name(PM_SCOPES)[0] == 0, "the scopes by name");
      for (int sc = 1; sc < PM_SCOPES; sc++) CHECK(pm_scope_of(pm_scope_name(sc), strlen(pm_scope_name(sc))) == sc && pm_scope_cap(sc) >= 0, "each capability is a scope"); }
    /* an enum's value: one of the values its scope has at the version a peer speaks, or the packet is malformed; a
     * capability's (CONNECT's kind) any of this build's (finding 3 of a review of 4e0a202: a value went unchecked) */
    { uint8_t p[] = { PM_ERR, 0, 0, 0, 9, 0, 0, 0, PM_ERR_UNSET, 0 };
      CHECK(take(p, sizeof p) == 0, "ERR with ERR_UNSET");
      p[8] = PM_ERR_UNSET + 1; CHECK(take(p, sizeof p) == 1, "ERR with a code ERR_* has not: malformed");
      p[8] = 0; CHECK(take(p, sizeof p) == 1, "ERR with code 0: malformed"); }
    { uint8_t p[] = { PM_MUSIC_CMD, 0, 0, 0, 0, 0, 0, 0, PM_MCMD_STOP, PM_MWHY_BUTTON };
      CHECK(take(p, sizeof p) == 0, "MUSIC_CMD with its values");
      p[9] = 0x40; CHECK(take(p, sizeof p) == 1, "MUSIC_CMD with a why MWHY_* has not: malformed");
      CHECK(pm_mwhy_ok(PM_MWHY_HOME_ASSISTANT, NULL) && !pm_mwhy_ok(PM_MWHY_HOME_ASSISTANT + 1, NULL), "pm_mwhy_ok"); }
    { struct pm_w w; struct pm_connect m = { PM_CAP_WAKE, 0, { "", 0 }, 0 }; uint16_t ver[PM_SCOPES] = { 0 }; struct pm_hdr h; const uint8_t *body; size_t blen;
      ver[PM_SCOPE_CORE] = 1; ver[PM_SCOPE_NET_LAN] = 1;
      pm_winit(&w, buf, sizeof buf); pm_put_connect(&w, 1, &m); pm_head(buf, (size_t)pm_wlen(&w), &h, &body, &blen);
      CHECK(pm_check(h.type, body, blen, ver) == 0, "CONNECT naming another capability: well-formed (the broker answers DENIED)");
      m.kind = 200; pm_winit(&w, buf, sizeof buf); pm_put_connect(&w, 1, &m); pm_head(buf, (size_t)pm_wlen(&w), &h, &body, &blen);
      CHECK(pm_check(h.type, body, blen, ver) == 1 && !pm_cap_ok(200, NULL) && pm_cap_ok(PM_CAP_BUTTON, NULL), "a kind that is no capability of this build's: malformed"); }
    CHECK(PM_CONST_SPOKEN(MAX_PACKET, NULL) && PM_CONST_SPOKEN(REQ_SAMPLES_MAX, NULL), "this build's constants");
    { uint16_t ver[PM_SCOPES] = { 0 }; ver[PM_SCOPE_CORE] = 1;
      CHECK(!PM_CONST_SPOKEN(REQ_SAMPLES_MAX, ver), "a constant of a scope not spoken"); }
    /* the manifest's keys and the versions' marks */
    { int sc = -1;
      CHECK(pm_key("net.lan", "ports", &sc) == 1 && sc == PM_SCOPE_NET_LAN && pm_key("net.lan", "tls", NULL) == 0 && pm_key("manifest", "requires", &sc) == 1 && sc == PM_SCOPE_CORE,
            "pm_key: each key's scope and version, none for one not there");
      CHECK(!pm_widens(PM_SCOPE_NET_LAN, 1, 1) && !pm_version_doc(PM_SCOPE_CORE, 1)[0], "no version above 1 yet: nothing widens"); }
    /* what must not be read */
    { uint8_t p[] = { PM_WAKE_WORDS, 0, 0, 0, 0, 0, 1, 0, 'm', 17, 0 };       /* a count over the limit, no elements */
      CHECK(take(p, sizeof p) == 1, "a list's count over its limit"); }
    { uint8_t p[] = { PM_FILE_BAD, 0, 0, 0, 0, 0, 1, 0, 3, 0, 'a', 0, 'b' };
      CHECK(take(p, sizeof p) == 1, "a NUL inside a string"); }
    { char out[4]; struct pm_str s = { "abcd", 4 }; CHECK(pm_cstr(out, sizeof out, s) == -1 && !out[0], "a string that does not fit its copy"); }

    /* random packets: a type, lengths anywhere */
    srand(1);
    for (int i = 0; i < 400000; i++) {
        size_t n = (size_t)(rand() % 200);
        for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)rand();
        if (n) buf[0] = (uint8_t)(rand() % 40);
        if (n > 1) buf[1] = 0;
        take(buf, n);
    }
    bytes_and_strings();
    printf("plugmsg: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}

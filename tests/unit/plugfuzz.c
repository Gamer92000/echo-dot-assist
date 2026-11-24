/* A fuzz target over the broker's dispatch (docs/plugins.md 6): message() of src/hassmic/plugin.c, which takes
 * whatever a plugin writes into its control socket, with a stand-in plugin's entry (tests/unit/plugin_harness.h) and
 * the store, the settings and an item behind it as on an Echo.  libFuzzer, ASan, UBSan: `make unit-plugfuzz`.
 *
 * An input is a plugin's whole life as far as the broker sees it: where it stands, then its packets.
 *   byte 0   the entry's state: before HELLO, before READY, running (byte 0 % 3); which plugin (byte 0 / 3 % 4): the
 *            stand-in wake word plugin, a music player with the capabilities (music, the volume, the
 *            Echo's state, UDP, the search, a server row to propose for, entities and actions), a phone (call:
 *            call, an action marked "calls"), or a listener (request.audio and request.text), for which a
 *            request runs around its packets: begun before the first, a block of its audio after each, its text and
 *            its end after the last, as the core says them
 *   byte 1   bit 0: every capability granted (else the manifest's two); bits 1-2 and 3-4: wake slots 0 and 1 closed,
 *            asked, open, failed; bit 5: a file check of the broker's under way; bit 6: its list of words is known;
 *            bit 7: the stream's clock far on (else at 2 s: a detection has to fit it); the phone: a call token of a
 *            person's is fresh ("0123456789abcdef0123456789abcdef", as an action marked "calls" brings one)
 *   then     packets: a length of two bytes, little-endian, and that many bytes
 * What the broker answers is read and thrown away after each input.  What must hold: no read or write out of place,
 * no descriptor left open by a FILE_OPEN, and a detection reaches the core only with positions that are a word's. */
#include "plugin_harness.h"

#define TOKEN "0123456789abcdef0123456789abcdef"

static int fds_open(void)
{
    int n = 0;
    for (int fd = 0; fd < 256; fd++) n += fcntl(fd, F_GETFD) != -1;
    return n;
}

/* one input per message a plugin sends, right as the SDK's client writes them: where the fuzzer starts from */
static void seeds(const char *dir)
{
    static uint8_t in[4096]; uint8_t b[1024]; struct pm_w w; char path[400]; FILE *f; int k = 0; size_t n;
#define PUT(STATE, FLAGS) do { n = 0; in[n++] = STATE; in[n++] = FLAGS; } while (0)
#define ADD() do { long l = pm_wlen(&w); if (l > 0) { in[n++] = (uint8_t)l; in[n++] = (uint8_t)(l >> 8); memcpy(in + n, b, (size_t)l); n += (size_t)l; } } while (0)
#define OUT() do { snprintf(path, sizeof path, "%s/seed-%02d", dir, k++); if ((f = fopen(path, "wb"))) { fwrite(in, 1, n, f); fclose(f); } } while (0)
    { struct pm_hello m = { 1, { { 0 }, 3 } }; struct pm_hello_scopes hs[3] = { { pm_s("core"), 1 }, { pm_s("files"), 1 }, { pm_s("wake"), 1 } }; struct pm_wake_words wm = { pm_s("models"), { { 0 }, 1 } }; struct pm_wake_words_words e = { pm_s("word") };
      PUT(0, 0); pm_winit(&w, b, sizeof b); pm_put_hello(&w, 1, &m); for (int i = 0; i < 3; i++) pm_add_hello_scopes(&w, &hs[i]); ADD();
      pm_winit(&w, b, sizeof b); pm_put_wake_words(&w, 0, &wm); pm_add_wake_words_words(&w, &e); ADD();
      pm_winit(&w, b, sizeof b); pm_put_ready(&w, 0); ADD(); OUT(); }
    { struct pm_hello m = { 9, { { 0 }, 1 } }; struct pm_hello_scopes hs = { pm_s("core"), 9 }; PUT(0, 0); pm_winit(&w, b, sizeof b); pm_put_hello(&w, 1, &m); pm_add_hello_scopes(&w, &hs); ADD(); OUT(); }
    { struct pm_file_open m = { pm_s("models"), pm_s("word"), pm_s(".model") }; PUT(2, 0); pm_winit(&w, b, sizeof b); pm_put_file_open(&w, 7, &m); ADD(); OUT(); }
    { struct pm_file_ok m = { pm_s("A word"), { { 0 }, 1 } }; struct pm_file_ok_fields e = { pm_s("level"), pm_s("5") };
      PUT(2, 1 << 5); pm_winit(&w, b, sizeof b); pm_put_file_ok(&w, 1, &m); pm_add_file_ok_fields(&w, &e); ADD(); OUT(); }
    { struct pm_file_bad m = { PM_FILE_BAD_FORMAT, pm_s("not a model") }; PUT(2, 1 << 5); pm_winit(&w, b, sizeof b); pm_put_file_bad(&w, 1, &m); ADD(); OUT(); }
    { struct pm_wake_ready m = { 0 }; struct pm_wake_failed fm = { 1, PM_WAKE_FAIL_BAD_MODEL };
      PUT(2, 1 << 1 | 1 << 3 | 1 << 6); pm_winit(&w, b, sizeof b); pm_put_wake_ready(&w, 0, &m); ADD(); pm_winit(&w, b, sizeof b); pm_put_wake_failed(&w, 0, &fm); ADD(); OUT(); }
    { struct pm_wake_heard m = { 0, 200, 16000, 28000 }; PUT(2, 2 << 1 | 1 << 6); pm_winit(&w, b, sizeof b); pm_put_wake_heard(&w, 0, &m); ADD(); OUT(); }
    { struct pm_err m = { PM_ERR_BAD }; PUT(2, 0); pm_winit(&w, b, sizeof b); pm_put_pong(&w, 1); ADD(); pm_winit(&w, b, sizeof b); pm_put_ok(&w, 2); ADD();
      pm_winit(&w, b, sizeof b); pm_put_err(&w, 3, &m); ADD(); pm_winit(&w, b, sizeof b); pm_put_ping(&w, 4); ADD(); OUT(); }
    /* the music player (byte 0: 5, running, the second plugin) */
    { struct pm_music_open mo = { 48000, 2 }; struct pm_music_state ms = { PM_MUSIC_PAUSED, pm_s("A title"), pm_s("An artist") }; struct pm_volume_set vs = { 42 };
      PUT(5, 0); pm_winit(&w, b, sizeof b); pm_put_music_open(&w, 1, &mo); ADD(); pm_winit(&w, b, sizeof b); pm_put_music_state(&w, 0, &ms); ADD();
      pm_winit(&w, b, sizeof b); pm_put_volume_set(&w, 2, &vs); ADD(); pm_winit(&w, b, sizeof b); pm_put_music_close(&w, 3); ADD(); OUT(); }
    { struct pm_udp_open u = { PM_CAP_NET_UDP, 0 }, dsc = { PM_CAP_NET_DISCOVER, 0 }; struct pm_udp_peer pr = { 0, 0x0A000009u, 4000 };
      struct pm_sendto st = { 0, 0x0A000009u, 4000, pm_b("x", 1) }; struct pm_broadcast bc = { 4, pm_b("?", 1) };
      PUT(5, 0); pm_winit(&w, b, sizeof b); pm_put_udp_open(&w, 1, &u); ADD(); pm_winit(&w, b, sizeof b); pm_put_udp_open(&w, 2, &dsc); ADD();
      pm_winit(&w, b, sizeof b); pm_put_udp_peer(&w, 3, &pr); ADD(); pm_winit(&w, b, sizeof b); pm_put_sendto(&w, 0, &st); ADD();
      pm_winit(&w, b, sizeof b); pm_put_broadcast(&w, 4, &bc); ADD(); OUT(); }
    { struct pm_propose pp = { pm_s("server"), pm_s("10.0.0.9") }; struct pm_entity_state es = { pm_s("level"), pm_s("7") }; struct pm_event ev = { pm_s("seen"), pm_s("ping") };
      PUT(5, 0); pm_winit(&w, b, sizeof b); pm_put_propose(&w, 1, &pp); ADD(); pm_winit(&w, b, sizeof b); pm_put_entity_state(&w, 2, &es); ADD();
      pm_winit(&w, b, sizeof b); pm_put_event(&w, 0, &ev); ADD(); OUT(); }
    /* the phone (byte 0: 8, running, the third plugin): a call that rings and goes live by a token (answered in
     * Home Assistant), one going out with the token, the far side's word, its end; and a ring that wants to go live
     * without anyone's answer */
    { struct pm_call_incoming ci = { pm_s("Mum") }; struct pm_call_live cl = { pm_s(TOKEN) }, bare = { pm_s("") }; struct pm_call_end ce = { PM_RESULT_HUNG_UP };
      struct pm_call_out co = { pm_s(TOKEN), pm_s("Bob") };
      PUT(8, 1 << 7); pm_winit(&w, b, sizeof b); pm_put_call_incoming(&w, 1, &ci); ADD(); pm_winit(&w, b, sizeof b); pm_put_call_live(&w, 2, &cl); ADD();
      pm_winit(&w, b, sizeof b); pm_put_call_end(&w, 3, &ce); ADD(); OUT();
      PUT(8, 1 << 7); pm_winit(&w, b, sizeof b); pm_put_call_out(&w, 1, &co); ADD(); pm_winit(&w, b, sizeof b); pm_put_call_ringing_there(&w, 2); ADD();
      pm_winit(&w, b, sizeof b); pm_put_call_live(&w, 3, &bare); ADD(); pm_winit(&w, b, sizeof b); pm_put_call_end(&w, 4, &ce); ADD(); OUT();
      PUT(8, 0); pm_winit(&w, b, sizeof b); pm_put_call_incoming(&w, 1, &ci); ADD(); pm_winit(&w, b, sizeof b); pm_put_call_live(&w, 2, &bare); ADD(); OUT(); }
    /* the listener (byte 0: 11, running, the fourth plugin): the request's messages are hassmic's, a plugin that
     * sends them is refused; and a run around its pings and a STORE_GET it has no capability for */
    { struct pm_req_start rs = { 1, PM_REQSRC_WAKE_WORD, 0, pm_s("Alexa") }; struct pm_req_end re = { 1, PM_REQEND_HA, 100, 0 }; struct pm_req_text rt = { 1, PM_REQTEXT_HEARD, pm_s("hello") };
      struct pm_store_get sg = { pm_s("k") };
      PUT(11, 0); pm_winit(&w, b, sizeof b); pm_put_req_start(&w, 1, &rs); ADD(); pm_winit(&w, b, sizeof b); pm_put_req_end(&w, 2, &re); ADD();
      pm_winit(&w, b, sizeof b); pm_put_req_text(&w, 3, &rt); ADD(); OUT();
      PUT(11, 0); pm_winit(&w, b, sizeof b); pm_put_pong(&w, 1); ADD(); pm_winit(&w, b, sizeof b); pm_put_store_get(&w, 2, &sg); ADD(); pm_winit(&w, b, sizeof b); pm_put_ok(&w, 3); ADD(); OUT(); }
#undef PUT
#undef ADD
#undef OUT
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *dir = getenv("PLUGFUZZ_SEEDS");
    (void)argc; (void)argv;
    h_install();
    h_add_full("musicplug", "\"audio.music\": true, \"audio.volume\": true, \"state\": true, \"net.udp\": 2, \"net.discover\": { \"udp\": 3483, \"to\": \"broadcast\" },"
               " \"net.server\": [ { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 9000 ] } ],"
               " \"ha.entities\": [ { \"name\": \"level\", \"kind\": \"number\", \"label\": \"Level\", \"min\": 0, \"max\": 10 },"
               "                    { \"name\": \"seen\", \"kind\": \"event\", \"label\": \"Seen\", \"types\": [ \"ping\" ] } ],"
               " \"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"page\": true } ]",
               "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" },", "the player's program\n");
    h_add_full("phone", "\"call\": true, \"ha.actions\": [ { \"name\": \"dial\", \"label\": \"Dial\", \"calls\": true } ]", "", "the phone's program\n");
    h_add_full("listener", "\"request.audio\": true, \"request.text\": true", "", "the listener's program\n");
    { const struct ps_plugin *l; ps_lock(); ps_scan(&l); ps_unlock(); }
    call_start();
    wake_user_cb = h_wake_cb;
    if (dir) seeds(dir);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static const int states[] = { P_HELLO, P_CONFIG, P_RUN }, wstates[] = { W_CLOSED, W_OPENING, W_OPEN, W_FAILED };
    struct plug *p; size_t at = 2; int before, player; static const int16_t blk[320];
    if (n < 2) return 0;
    {   /* the music player's own capabilities, as its manifest has them */
        uint32_t music = PM_CAPBIT(PM_CAP_AUDIO_MUSIC) | PM_CAPBIT(PM_CAP_AUDIO_VOLUME) | PM_CAPBIT(PM_CAP_STATE) | PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER)
                         | PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_HA_ENTITIES) | PM_CAPBIT(PM_CAP_HA_ACTIONS);
        player = d[0] / 3 % 4;
        p = h_plug(states[d[0] % 3], d[1] & 1 ? 0xffffffffu : player == 1 ? music : player == 2 ? PM_CAPBIT(PM_CAP_CALL) | PM_CAPBIT(PM_CAP_HA_ACTIONS)
                   : player == 3 ? PM_CAPBIT(PM_CAP_REQUEST_AUDIO) | PM_CAPBIT(PM_CAP_REQUEST_TEXT) : PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
        p->lane = -1;
        if (player == 1) h_as(p, "musicplug");
        if (player == 3) h_as(p, "listener");
        if (player == 2) { h_as(p, "phone"); snprintf(p->name, sizeof p->name, "Phone"); if (d[1] & 1 << 7) { snprintf(p->tok[0].t, sizeof p->tok[0].t, TOKEN); p->tok[0].at = now_ms(); } }
    }
    for (int s = 0; s < WAKE_SLOTS; s++) { p->ws[s].state = wstates[d[1] >> (1 + 2 * s) & 3]; snprintf(p->ws[s].word, sizeof p->ws[s].word, "word"); }
    if (d[1] & 1 << 5) { p->check.active = 1; p->check.id = 1; p->check.at = now_ms(); snprintf(p->check.list, sizeof p->check.list, "models"); snprintf(p->check.item, sizeof p->check.item, "word"); }
    if (d[1] & 1 << 6) { p->have_words = p->nwords = 1; snprintf(p->words[0], sizeof p->words[0], "word"); snprintf(p->words_list, sizeof p->words_list, "models"); }
    p->pos = d[1] & 1 << 7 ? 1ull << 40 : 32000;
    before = fds_open();
    if (player == 3) plugin_request_start(PLUGIN_REQ_WAKE_WORD, 0, "Alexa");
    while (at + 2 <= n) {
        size_t len = d[at] | (size_t)d[at + 1] << 8; uint8_t *pkt;
        at += 2;
        if (len > n - at) len = n - at;
        /* a block of exactly the packet's size: one byte read past it is seen */
        if (!(pkt = malloc(len ? len : 1))) break;
        memcpy(pkt, d + at, len); at += len;
        handling = 0; message(0, pkt, len); handling = -1;
        free(pkt);
        if (player == 3) plugin_request_audio(blk, sizeof blk);
        h_drain(NULL);
    }
    /* the run's end: its text, its end, the broker's queue (a process that was stopped meanwhile gets none of it, and
     * every descriptor the queue held is closed) */
    if (player == 3) { plugin_request_text(PLUGIN_TEXT_HEARD, "hello"); plugin_request_end(PLUGIN_REQEND_HA); }
    rq_drain();
    /* what its messages may leave behind in the broker's own rounds */
    check_done(0); timers(); h_drain(NULL);
    /* and what the process's end releases (exited): its stream (its thread ends and closes it), its UDP sockets, its lane */
    pthread_mutex_lock(&bl);
    mstream_end(p);
    for (int h = 0; h <= PS_UDP_MAX; h++) if (p->udp_open[h]) { close(p->udp[h]); p->udp_open[h] = 0; }
    pthread_mutex_unlock(&bl);
    for (int i = 0; i < 400 && atomic_load(&mstreams_live); i++) usleep(5000);
    if (atomic_load(&mstreams_live)) abort();   /* a stream thread that does not end when told */
    /* its call ends with the process (exited's call_over): the media's reader closes hassmic's end, the core is told,
     * the claim goes; a call left claimed would answer the next input BUSY */
    pthread_mutex_lock(&bl); if (pc.entry == 0) call_over(0, "the input ended"); pthread_mutex_unlock(&bl);
    for (int i = 0; i < 400 && (atomic_load(&media_readers) || call_claimed() != CALL_NOBODY); i++) usleep(5000);
    if (atomic_load(&media_readers) || call_claimed() != CALL_NOBODY || call_live()) abort();       /* a call that does not end when told */
    pthread_mutex_lock(&bl); if (lane_mine(p)) atomic_store(&lane_of[p->lane], -1); pthread_mutex_unlock(&bl);
    h_drain(NULL);
    if (fds_open() != before) abort();          /* a descriptor opened for the plugin and not closed again */
    return 0;
}

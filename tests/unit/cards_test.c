/* cards.c: what Home Assistant pushes (show_card v2, clear_card) checked and digested into the screen UI's
 * lines; rows read-only unless the card asks for a control (a lock's and an alarm's never); taps into Home Assistant
 * actions with their result at once; buttons as events; updates in place by id; notices stacked, under a voice
 * request's card and back after it; a request's card only for a request of this Echo's (and its conversation); clear
 * by id; pictures;
 * the screen held on, a card until tapped away only for a while; cuts at character boundaries; and a fuzz run of cards
 * cut and changed at random, and of the UI's lines (under ASan/UBSan).
 * cards.c is included with stand-ins for the screen (screen.c) and the ESPHome side (proto_esphome.c).
 * The cards are written with ' for " (Q()), to read. */
#include "../../src/hassmic/cards.c"
#include <assert.h>

pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
static char last[8192], service[96], kv[512], event[96], ekv[512];
static int nact, nevent, held = -1, event_fails;
int screen_present(void) { return 1; }
void screen_card(const char *l) { snprintf(last, sizeof last, "%s", l); }
void screen_card_hold(int h) { held = h; }
static int nwake;
void ring_screen_wake(void) { nwake++; }
static char bin_head[96];
void screen_bin(const char *l, const void *d, size_t n) { (void)d; (void)n; snprintf(bin_head, sizeof bin_head, "%s", l); }
uint8_t *esphome_fetch(const char *url, size_t max, size_t *n) { (void)max; if (strstr(url, "fail")) return NULL; *n = 3; uint8_t *b = malloc(3); memcpy(b, "jpg", 3); return b; }
static void kvs(char *o, size_t on, const char *const *k, int n)
{
    o[0] = 0;
    for (int i = 0; i < 2 * n; i++) { strncat(o, k[i], on - strlen(o) - 2); strcat(o, i % 2 ? ";" : "="); }
}
int esphome_action(const char *s, const char *const *k, int n) { snprintf(service, sizeof service, "%s", s); kvs(kv, sizeof kv, k, n); nact++; return 0; }
int esphome_event(const char *s, const char *const *k, int n)
{
    if (event_fails) return -1;
    snprintf(event, sizeof event, "%s", s); kvs(ekv, sizeof ekv, k, n); nevent++; return 0;
}

static char qbuf[CARD_JSON_MAX];
static const char *Q(const char *s) { size_t i = 0; for (; s[i] && i + 1 < sizeof qbuf; i++) qbuf[i] = s[i] == '\'' ? '"' : s[i]; qbuf[i] = 0; return qbuf; }
static char err[200];
static int show(const char *q) { return cards_show(Q(q), err, sizeof err); }
static int has(const char *line) { return strstr(last, line) != NULL; }
static int seq_now(void) { int s = -1; sscanf(last, "cb %d", &s); return s; }
static void tap(int row, const char *op) { char l[96]; snprintf(l, sizeof l, "card tap %d %d %s", seq_now(), row, op); cards_ui(l); }
static void later(long long ms) { for (int s = 0; s < NSLOT; s++) { if (sl[s].expires) sl[s].expires -= ms; sl[s].since -= ms; if (sl[s].img_next) sl[s].img_next -= ms; } idle_at -= ms; tick(mono_ms()); }

static const char *ROOM =
    "{'v':2,'id':'room','type':'entities','title':'Kitchen','timeout':0,'entities':["
    "{'entity_id':'light.kitchen','name':'Light','value':'On · 80 %','control':'dim','on':true,'level':80,'focus':true},"
    "{'entity_id':'switch.coffee','name':'Coffee','value':'Off','control':'toggle','on':false},"
    "{'entity_id':'lock.back','name':'Back door','value':'Locked','control':'toggle'},"
    "{'entity_id':'alarm_control_panel.house','name':'Alarm','value':'Armed','control':'toggle'},"
    "{'entity_id':'cover.blind','name':'Blind','value':'Open · 40 %','control':'cover','on':true,'level':40},"
    "{'entity_id':'climate.hall','name':'Hall','value':'20.5 now · set 21.0','control':'climate','setpoint':21,'step':0.5},"
    "{'entity_id':'scene.movie','name':'Movie','control':'run'},"
    "{'entity_id':'Sensor.T','name':'bad id'},"
    "{'entity_id':'sensor.t','name':'Temp','value':'21.5 °C','control':'toggle'}]}";
static const char *SAFE =                              /* switches asked for where a toggle would open or switch too much */
    "{'v':2,'id':'safe','type':'entities','timeout':0,'entities':["
    "{'entity_id':'cover.garage','name':'Garage','value':'Closed','control':'toggle','on':false},"
    "{'entity_id':'valve.water_main','name':'Water','value':'Open','control':'toggle','on':true},"
    "{'entity_id':'cover.garage','name':'Garage','value':'Closed','control':'dim'},"
    "{'entity_id':'switch.pump','name':'Pump','value':'Off','control':'climate','setpoint':1,'step':1}]}";

int main(void)
{
    /* rows: words as sent, a control only where asked and fitting (a lock's, an alarm's never; a bad id left out) */
    assert(!show(ROOM));
    assert(has("cb 1 e 8 0\tKitchen\t\n") && has("cr 0 s 1 80 1\tLight\tOn \xc2\xb7 80 %\n") && has("cr 1 t 0 -1 0\tCoffee\tOff\n"));
    assert(has("cr 2 r -1 -1 0\tBack door\tLocked\n") && has("cr 3 r -1 -1 0\tAlarm\tArmed\n") && has("cr 4 c 1 40 0\tBlind\tOpen \xc2\xb7 40 %\n"));
    assert(has("cr 5 h -1 -1 0\tHall\t20.5 now \xc2\xb7 set 21.0\n") && has("cr 6 b -1 -1 0\tMovie\t\n") && has("cr 7 r -1 -1 0\tTemp\t21.5 \xc2\xb0" "C\n"));
    assert(!has("bad id") && held == 1);                        /* (a toggle asked of a sensor: read-only) */
    assert(nwake == 1);                                         /* a notice: the screen woken */
    /* Home Assistant told what shows (the card thread's flush, outside lk), once per change, again after a failure */
    nevent = 0; flush_state(); assert(nevent == 1 && !strcmp(event, "esphome.hassmic_card_state") && !strcmp(ekv, "card=room;title=Kitchen;state=shown;"));
    flush_state(); assert(nevent == 1);
    event_fails = 1; cards_resend_state(); flush_state(); assert(nevent == 1 && st_dirty); event_fails = 0; flush_state(); assert(nevent == 2);
    /* taps: the action, shown at once; a lock's, an alarm's, an old card's: nothing */
    nact = 0; tap(1, "toggle");
    assert(nact == 1 && !strcmp(service, "switch.toggle") && !strcmp(kv, "entity_id=switch.coffee;") && has("cr 1 t 1 -1 0\tCoffee\tOff\n"));
    { char l[64]; snprintf(l, sizeof l, "card tap %d 0 level 40", seq_now()); cards_ui(l); }
    assert(nact == 2 && !strcmp(service, "light.turn_on") && !strcmp(kv, "entity_id=light.kitchen;brightness_pct=40;") && has("cr 0 s 1 40 1\t"));
    tap(4, "close"); assert(nact == 3 && !strcmp(service, "cover.close_cover"));
    tap(5, "up"); assert(nact == 4 && !strcmp(service, "climate.set_temperature") && strstr(kv, "temperature=21.5;"));
    tap(5, "up"); assert(nact == 5 && strstr(kv, "temperature=22.0;"));
    tap(6, "run"); assert(nact == 6 && !strcmp(service, "scene.turn_on"));
    tap(2, "toggle"); tap(3, "toggle"); tap(6, "toggle"); tap(99, "toggle");
    cards_ui("card tap 0 1 toggle"); cards_ui("card tap"); cards_ui("card tap x y z"); assert(nact == 6);
    tap(7, "toggle"); assert(nact == 6);
    /* a cover's or a valve's toggle, a control of another domain: read-only */
    { char keep[8192]; memcpy(keep, last, sizeof keep);
      assert(!show(SAFE) && has("cr 0 r -1 -1 0\tGarage\t") && has("cr 1 r -1 -1 0\tWater\t") && has("cr 2 r -1 -1 0\tGarage\t") && has("cr 3 r -1 -1 0\tPump\t"));
      cards_clear("safe", "test"); assert(!strcmp(last, keep)); }
    /* Home Assistant's update: in place (same seq), its words; an update of a card not shown: refused */
    int s0 = seq_now();
    assert(!show("{'v':2,'id':'room','update':true,'type':'entities','title':'Kitchen','timeout':0,'entities':["
                 "{'entity_id':'switch.coffee','name':'Coffee','value':'An','control':'toggle','on':true}]}"));
    assert(seq_now() == s0 && has("cb 1 e 1 0\tKitchen\t\n") && has("cr 0 t 1 -1 0\tCoffee\tAn\n"));
    assert(show("{'v':2,'id':'other','update':true,'type':'message','title':'x'}") && strstr(err, "not shown"));
    assert(show("{'v':2,'update':true,'type':'message','title':'x'}") && strstr(err, "id"));
    /* clear: by id (another id: nothing), the UI's tap away (an old seq: nothing) */
    cards_clear("nope", "test"); assert(has("cb 1 "));
    cards_clear("room", "test"); assert(!strcmp(last, "cx\n") && held == 0);
    nevent = 0; flush_state(); assert(nevent == 1 && !strcmp(ekv, "card=;title=;state=gone;"));
    assert(!show("{'v':2,'id':'a','type':'message','title':'A','message':'a','timeout':0}"));
    cards_ui("card clear 0"); assert(has("cm a\n"));
    { char l[32]; snprintf(l, sizeof l, "card clear %d", seq_now()); cards_ui(l); } assert(!strcmp(last, "cx\n"));
    /* versions, kinds: another version refused; a kind this Echo does not know shows its words */
    assert(show("{'v':1,'type':'message','message':'x'}") && strstr(err, "version 1"));
    assert(!show("{'v':2,'type':'carousel','title':'New','message':'from a newer side'}") && has(" m 0 0\tNew\t") && has("cm from a newer side\n"));
    assert(show("{'v':2,'type':'carousel'}") && show("[]") && show("{'v':2,'type':'message','scope':'later','title':'x'}"));
    cards_clear(NULL, "test");

    /* a notice under a voice request: hidden while it runs, the request's card over it, back after that card */
    assert(!show("{'v':2,'id':'laundry','type':'message','title':'Washer is done','timeout':0,"
                 "'buttons':[{'label':'Remind me','id':'snooze'},{'label':'Keep','id':'k','keep':true}]}"));
    int notice = seq_now();
    assert(has("cu 0\tRemind me\n") && has("cu 1\tKeep\n"));
    assert(show("{'v':2,'scope':'request','type':'message','title':'r'}") && strstr(err, "no request"));   /* none ran */
    cards_run(1); assert(!strcmp(last, "cx\n"));
    cards_conversation("conv1");
    assert(show("{'v':2,'scope':'request','conversation_id':'conv2','type':'message','title':'x'}") && strstr(err, "another"));
    assert(!show("{'v':2,'scope':'request','conversation_id':'conv1','type':'message','title':'Weather','message':'sunny','timeout':5}"));
    assert(has(" m 0 1\tWeather\t") && seq_now() != notice);
    cards_run(0); assert(has("\tWeather\t"));                  /* idle: its 5 s from now */
    later(6000); assert(seq_now() == notice && has("\tWasher is done\t"));   /* gone: the notice back */
    /* a notice that comes while a request's card stays after the request (the doorbell): over it at once, the screen
     * woken; the request's card back under it once it goes */
    cards_run(1);
    assert(!show("{'v':2,'scope':'request','type':'message','title':'Lights','timeout':0}"));
    cards_run(0); assert(has("\tLights\t"));
    { int w = nwake;
      assert(!show("{'v':2,'id':'door','type':'message','title':'Front door','timeout':0}") && has("\tFront door\t") && nwake == w + 1); }
    cards_clear("door", "test"); assert(has("\tLights\t"));
    cards_clear(NULL, "test"); assert(seq_now() == notice && has("\tWasher is done\t"));
    /* a request's card a moment after the request (it comes with the answer), not much later */
    cards_run(1); cards_run(0);
    assert(!show("{'v':2,'scope':'request','type':'message','title':'Late answer','timeout':5}") && has("\tLate answer\t"));
    cards_run(0); later(REQUEST_GRACE_MS + 1000); cards_run(0);          /* idle after an announcement: no request ended */
    assert(show("{'v':2,'scope':'request','type':'message','title':'After an announcement'}") && strstr(err, "no request"));
    cards_run(1); cards_run(0); later(REQUEST_GRACE_MS + 1000);
    assert(show("{'v':2,'scope':'request','type':'message','title':'Too late'}") && strstr(err, "no request"));
    assert(has("\tWasher is done\t"));
    /* buttons: the event (card, action), the card goes; keep: stays; no Home Assistant to tell: the card stays */
    nevent = 0; tap(1, "press");
    assert(nevent == 1 && !strcmp(event, "esphome.hassmic_card_action") && !strcmp(ekv, "card=laundry;action=k;") && has("\tWasher is done\t"));
    event_fails = 1; tap(0, "press"); assert(nevent == 1 && has("\tWasher is done\t")); event_fails = 0;
    tap(7, "press"); assert(nevent == 1);
    tap(0, "press"); assert(nevent == 2 && !strcmp(ekv, "card=laundry;action=snooze;") && !strcmp(last, "cx\n"));
    assert(show("{'v':2,'type':'message','title':'x','buttons':[{'label':'No id'}]}") && strstr(err, "id"));
    /* notices stack: the newest shows, the one under it back when it goes; the same id keeps its place; past NOTICES the
     * oldest goes */
    assert(!show("{'v':2,'id':'wash','type':'message','title':'Washer','timeout':0}"));
    assert(!show("{'v':2,'id':'bell','type':'message','title':'Doorbell','timeout':5}") && has("\tDoorbell\t"));
    later(6000); assert(has("\tWasher\t"));                   /* the doorbell's time up: the washer back */
    assert(!show("{'v':2,'id':'bell','type':'message','title':'Doorbell again','timeout':0}") && has("\tDoorbell again\t"));
    cards_clear("bell", "test"); assert(has("\tWasher\t"));
    for (int i = 0; i < NOTICES; i++) { char q[96]; snprintf(q, sizeof q, "{'v':2,'id':'n%d','type':'message','title':'N%d','timeout':0}", i, i); assert(!show(q)); }
    assert(has("\tN3\t")); cards_clear("n3", "t"); assert(has("\tN2\t")); cards_clear("n2", "t"); cards_clear("n1", "t"); cards_clear("n0", "t");
    assert(!strcmp(last, "cx\n"));                             /* the washer was the oldest of five: gone */
    /* a card until tapped away holds the screen on for HOLD_MAX_MS only; a timed one while it shows */
    assert(!show("{'v':2,'id':'n','type':'message','title':'Sticky','timeout':0}") && held == 1);
    later(HOLD_MAX_MS + 500); assert(held == 0 && has("\tSticky\t"));
    assert(!show("{'v':2,'id':'t','type':'message','title':'Timed','timeout':30}") && held == 1);
    later(31000); assert(has("\tSticky\t") && held == 0);     /* the timed one gone: the sticky one back, not holding */
    assert(!show("{'v':2,'id':'long','type':'message','title':'Long','timeout':3600}") && held == 1);
    later(HOLD_MAX_MS + 500); assert(held == 0 && has("\tLong\t"));   /* a long timeout holds the screen no longer either */
    cards_clear("long", "test");
    assert(!show("{'v':2,'id':'neg','type':'message','title':'Neg','timeout':-5}") && sl[told].c.timeout_s == NOTICE_TIMEOUT);
    cards_clear("neg", "test");
    cards_clear("n", "test"); assert(!strcmp(last, "cx\n"));
    /* a message's picture: "ci card<seq>.<n>", fetched when the UI asks for this card's; refreshed; an update keeps it */
    assert(!show("{'v':2,'id':'door','type':'message','title':'Front door','message':'Someone is at the front door',"
                 "'image':'http://ha:8123/api/hassmic_cards/camera/abc','image_refresh':2,'timeout':0}"));
    { int s = seq_now(); char ci[32], k[32];
      snprintf(ci, sizeof ci, "ci card%d.0\n", s); assert(has(ci));
      snprintf(k, sizeof k, "card%d.0", s); cards_image(k, 512); assert(!strcmp(img_want, k) && img_size == 512); img_want[0] = 0;
      snprintf(k, sizeof k, "card%d.0", s - 1); cards_image(k, 512); assert(!img_want[0] && strstr(bin_head, " 0 512")); cards_image("cardx", 1);
      later(2100); snprintf(ci, sizeof ci, "ci card%d.1\n", s); assert(has(ci));
      assert(!show("{'v':2,'id':'door','update':true,'type':'message','title':'Front door','message':'Still there',"
                   "'image':'http://ha:8123/api/hassmic_cards/camera/abc','image_refresh':2,'timeout':0}"));
      assert(seq_now() == s && has(ci) && has("cm Still there\n"));
      int w0 = nwake; assert(w0 >= 2);                          /* (an update wakes nothing) */
      sl[told].img_fails = IMAGE_FAILS; later(2100); assert(has(ci));        /* fetches failing: refreshes stop */
      sl[told].img_fails = 0; later(2100); snprintf(ci, sizeof ci, "ci card%d.2\n", s); assert(has(ci));
      later(HOLD_MAX_MS); later(2100); snprintf(ci, sizeof ci, "ci card%d.3\n", s); assert(!has(ci));   /* past its hold: none */
      assert(!show("{'v':2,'id':'door','update':true,'type':'message','title':'Front door','image':'http://ha:8123/x','image_refresh':0,'timeout':0}"));
      assert(sl[told].img_next == 0 && nwake == w0); }
    assert(show("{'v':2,'type':'message','message':'x','image':'file:///etc/passwd'}") && strstr(err, "http"));
    assert(show("{'v':2,'type':'message','message':'x','image':'http://a b'}"));
    cards_clear("door", "test");
    /* a list: text alone or with a detail, empty rows left out */
    assert(!show("{'v':2,'type':'list','title':'Today','subtitle':'3 events','items':[{'text':'Dentist','detail':'3:00 PM'},'Milk',"
                 "{'text':''},{'detail':'no text'},{'text':'Soccer\\tpractice'}]}"));
    assert(has(" l 3 0\tToday\t3 events\n") && has("cl 0\tDentist\t3:00 PM\n") && has("cl 1\tMilk\t\n") && has("cl 2\tSoccer practice\t\n"));
    assert(show("{'v':2,'type':'list','items':[]}"));
    cards_clear(NULL, "test");
    /* switched off: refused, nothing shows */
    cards_on(0); assert(show("{'v':2,'type':'message','title':'off'}") && strstr(err, "switched off")); cards_on(1);
    /* a cut never splits a character */
    { char js[600], n[300] = ""; for (int i = 0; i < 100; i++) strcat(n, "\xc3\xa4");
      snprintf(js, sizeof js, "{\"v\":2,\"type\":\"entities\",\"entities\":[{\"entity_id\":\"sensor.a\",\"name\":\"%s\",\"value\":\"1\"}]}", n);
      assert(!cards_show(js, err, sizeof err)); const char *t = strchr(strstr(last, "cr 0 "), '\t') + 1; size_t k = strcspn(t, "\t"); assert(k == 62); }
    cards_clear(NULL, "test");
    /* fuzz: cards cut and changed at random, the UI's lines too */
    srand(7);
    static char room[4096], buf[4096]; snprintf(room, sizeof room, "%s", Q(ROOM));
    size_t L = strlen(room);
    for (size_t cut_at = 0; cut_at <= L; cut_at++) { memcpy(buf, room, cut_at); buf[cut_at] = 0; cards_show(buf, err, sizeof err); }
    static const char *more[] = {
        "{'v':2,'id':'l','type':'list','title':'Shopping','items':['Milk',{'text':'Eggs','detail':'a dozen'}],'buttons':[{'label':'Done','id':'done'}]}",
        "{'v':2,'id':'m','scope':'request','type':'message','title':'Door','image':'http://x/y','image_refresh':1,'buttons':[{'label':'a','id':'b','keep':true}]}",
        "{'v':2,'id':'w','type':'weather','weather':{'condition':'sunny','temperature':'66','forecast':[{'label':'Fri','condition':'rainy','temps':'1','precip':'2'}]}}" };
    for (int i = 0; i < 30000; i++) {
        const char *src = i % 4 == 3 ? Q(more[(i / 4) % 3]) : room;
        size_t n = strlen(src); memcpy(buf, src, n + 1);
        for (int k = 0, m = 1 + rand() % 6; k < m; k++) buf[rand() % n] = (char)(rand() % 256 ? rand() % 256 : '"');
        buf[rand() % (n + 1)] = rand() % 4 ? buf[rand() % (n + 1)] : 0;
        cards_show(buf, err, sizeof err);
        if (!(i % 3)) { snprintf(buf, 64, "card tap %d %d %c%c%c %d", rand() % 4000, rand() % 12 - 2, 'a' + rand() % 26, 'a' + rand() % 26, rand() % 128, rand());
                        cards_ui(buf); }
        if (!(i % 11)) { snprintf(buf, 64, "card tap %d %d press", seq_now(), rand() % 5 - 1); cards_ui(buf); }
        if (!(i % 13)) cards_run(rand() % 2);
        if (!(i % 17)) cards_clear(rand() % 2 ? "room" : NULL, "fuzz");
        if (!(i % 19)) later(rand() % 40000);
    }
    puts("cards_test: ok");
    return 0;
}

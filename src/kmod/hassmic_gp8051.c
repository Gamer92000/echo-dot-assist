/*
 * hassmic_gp8051: a test, not a feature (PLAN.md "Thread 1.4" 2.1, real enhanced ACKs).  Does the Echo 4's GP712 run a
 * program of ours on its 8051, and how fast is that core?  The chip's 8051 runs whatever the driver uploads into its
 * RAM (0x2000-0x3fff, the host's own register addresses over SPI): Amazon's driver uploads Qorvo's Wi-Fi coexistence
 * program at 0x3600 (gpHal_k7c_CoexDrvOnChip_RamData, gpHal_CoexInitGpioInt) and starts it with gpRamProgram_Run,
 * whose argument >> 7 goes to 0x680 (the vector base, 128-byte aligned); 0x806 bit 1 holds the core.  One core: ours
 * runs in coex's place for the test, then coex is uploaded and started again as gpHal_Init does it.
 *
 * The program (0x77 bytes at 0x3f00, between coex's end 0x3eff and the mailbox 0x3fe0, which nothing uses): interrupts
 * off, 0xA5 into 0x3fd0, then on every tick of the time register's high byte (0x587: 512 us) a heartbeat into 0x3fd1
 * and the number of loops it counted since the last tick into 0x3fd2/3 (big-endian; one loop is 10 instructions).
 *
 * Every step runs on the driver's own thread (gpSched_ScheduleEvent takes the scheduler's lock and wakes it): the
 * register functions take no lock.  A client must have the driver open (its thread runs only then), the radio
 * otherwise idle.  /proc/hassmic_gp8051 says what happened.
 *
 * Any other program (the experiments of PLAN.md 2.1): hex=<bytes> at=<load address> [vector=<run address, 128-byte
 * aligned; at by default>] runms=<ms before the first read> reads=<n> every=<ms> peek=<addr:len,...> (each read: those
 * bytes, 32 at most each) poke=<addr:mask:val,...> (register writes before the start) unpoke=<...> (after coex is
 * back).  Never into coex (0x3600-0x3eff), the MAC filter (0x3000-0x34ab) or the mailbox (0x3fe0-): refused.
 * nouc=1: the 8051 left alone (no upload, coex not touched); write=<addr:hexbytes,...> chip RAM written before the
 * start (the sequencer's program among it: the filter's extension first, then its 4-byte hook, one stream write
 * each), undo=<...> the same after the last read.  Writes only in 0x2000-0x35ff and 0x3f00-0x3fdf.
 */
#include <linux/delay.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#if defined(CONFIG_ARM64) || !defined(HM_KALLSYMS)
#error "hassmic_gp8051 is laser's: 32-bit ARM, HM_KALLSYMS"
#endif

struct raminfo { u16 size; const u8 *data; u16 start; };       /* gpRamProgram_RamInfo_t: 12 bytes, .data+0x33c */

static const u8 probe[] = {
    0x02, 0x3f, 0x30, 0x32, 0, 0, 0, 0, 0, 0, 0, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 0x32,
    0, 0, 0, 0, 0, 0, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 0x32, 0, 0, 0, 0,
    0x75, 0x81, 0x60, 0xe4, 0xf5, 0xa8, 0x90, 0x3f, 0xd0, 0x74, 0xa5, 0xf0, 0x7d, 0x00, 0x12, 0x3f, 0x70, 0xff,
    0x12, 0x3f, 0x70, 0x6f, 0x60, 0xfa, 0x6f, 0xff, 0x7a, 0x00, 0x7b, 0x00, 0x0b, 0xbb, 0x00, 0x01, 0x0a, 0x12,
    0x3f, 0x70, 0x6f, 0x60, 0xf5, 0x6f, 0xfe, 0x90, 0x3f, 0xd2, 0xea, 0xf0, 0xa3, 0xeb, 0xf0, 0x0d, 0x90, 0x3f,
    0xd1, 0xed, 0xf0, 0xee, 0x80, 0xdd, 0, 0, 0, 0, 0x90, 0x05, 0x86, 0xe0, 0xa3, 0xe0, 0x22,
};
static struct raminfo probe_info = { sizeof probe, probe, 0x3f00 };

static char *hex = "", *peek = "3fd0:4", *poke = "", *unpoke = "", *wr = "", *wr2 = "", *wr3 = "", *undo = "";
static int nouc;
module_param_named(write, wr, charp, 0444); module_param_named(write2, wr2, charp, 0444);
module_param_named(write3, wr3, charp, 0444);         /* a string parameter is 1024 bytes at most: write, then 2, 3 */ module_param(undo, charp, 0444); module_param(nouc, int, 0444);
static int at = 0x3f00, vector = -1, runms = 20, nreads = 3, every = 5;
module_param(hex, charp, 0444); module_param(peek, charp, 0444); module_param(poke, charp, 0444);
module_param(unpoke, charp, 0444); module_param(at, int, 0444); module_param(vector, int, 0444);
module_param(runms, int, 0444); module_param_named(reads, nreads, int, 0444); module_param(every, int, 0444);
static u8 prog[0x1000];

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* the next number of a list ("3fd0:4,0x552:40:40"): hex, an optional 0x; *p moved past it and one separator */
static int num(const char **p, unsigned *v)
{
    int n = 0, d; *v = 0;
    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) *p += 2;
    while ((d = hexval(**p)) >= 0) { *v = *v << 4 | d; (*p)++; n++; }
    if (**p == ':' || **p == ',') (*p)++;
    return n ? 0 : -1;
}
static int overlaps(unsigned a, unsigned n)
{
    return a < 0x2000 || a + n > 0x3fe0 || (a < 0x34ac && a + n > 0x3000) || (a < 0x3f00 && a + n > 0x3600);
}

static void (*upload)(const struct raminfo *);
static void (*run)(u16 vector);
static u8 (*rreg)(u16 addr);
static void (*rmw)(u16 addr, u8 mask, u8 val);
static void (*rstream)(u16 addr, u8 *buf, u8 len);
static void (*wstream)(u16 addr, const u8 *buf, u8 len);
static void (*sched)(u32 us, void (*fn)(void));
static const struct raminfo *coex_info;

static const u8 coex_head[12] = { 0x02, 0x36, 0x09, 0x02, 0x37, 0x97, 0x02, 0x37, 0xd0, 0x75, 0x81, 0x35 };
static const u8 filt_head[4] = { 0x17, 0x00, 0x17, 0x01 };

static char out[2048];
static int outn, state;         /* 0 waiting for the driver's thread, 1 probe running, 2 done, -1 refused */
static int reads;

/* laser's kernel: only what stock modules import (devices/laser/Module.symvers), the rest by kallsyms */
static int (*hm_snprintf)(char *buf, size_t size, const char *fmt, ...);
static void (*unsched)(void (*fn)(void));
#define SAY(...) do { if (outn < (int)sizeof out - 1) { int k_ = hm_snprintf(out + outn, sizeof out - outn, __VA_ARGS__); \
    outn = k_ < 0 ? outn : outn + k_ > (int)sizeof out - 1 ? (int)sizeof out - 1 : outn + k_; } } while (0)
static int same(const u8 *a, const u8 *b, int n) { while (n--) if (*a++ != *b++) return 0; return 1; }

static void dump(const char *what, u16 addr, int n)
{
    u8 b[48]; int i;
    rstream(addr, b, (u8)n);
    SAY("%s %04x:", what, addr);
    for (i = 0; i < n; i++) SAY(" %02x", b[i]);
    SAY("\n");
}

static void restore(void)
{
    upload(coex_info);
    run(0x3600);
    rmw(0x552, 0x40, 0x40);                 /* what gpHal_Init sets after the coex program's start */
    SAY("restored: coex uploaded and started at 0x3600, 0x680 = %02x\n", rreg(0x680));
    dump("coex", 0x3600, 12);
}

static void pokes(const char *l)
{
    unsigned a, m, v;
    while (*l && !num(&l, &a) && !num(&l, &m) && !num(&l, &v)) {
        rmw((u16)a, (u8)m, (u8)v);
        SAY("poke %04x &%02x = %02x: now %02x\n", a, m, v, rreg((u16)a));
    }
}

/* "addr:hexbytes,...": each a stream write of at most 32 bytes, refused outside the RAM a test may change */
static void writes(const char *l)
{
    unsigned a;
    while (*l && !num(&l, &a)) {
        u8 b[32]; int n = 0, x, y, more = 0;
        while ((x = hexval(l[0])) >= 0 && (y = hexval(l[1])) >= 0) {
            if (n < 32) b[n++] = (u8)(x << 4 | y); else more = 1;      /* a chunk over 32 bytes: refused whole */
            l += 2;
        }
        while (*l && *l != ',') l++;
        if (*l == ',') l++;
        if (more || !n || a < 0x2000 || (a + n > 0x3600 && a < 0x3f00) || a + n > 0x3fe0) { SAY("write %04x (%d bytes): refused\n", a, n); continue; }
        wstream((u16)a, b, (u8)n);
        SAY("wrote %d bytes at %04x\n", n, a);
    }
}

static atomic_t busy = ATOMIC_INIT(0);          /* a step of ours on the driver's thread: the module is not left */

/* the end, every way: 2015 frames refused before the hooks go (unpoke first), the hooks undone, coex back; state 2
 * last, which the host's rmmod waits for */
static void finish(void)
{
    pokes(unpoke);
    writes(undo);
    if (!nouc) restore();
    state = 2;
}

static void step_abort(void) { atomic_inc(&busy); SAY("unloaded while running: undone\n"); finish(); atomic_dec(&busy); }

static void step_read(void)
{
    const char *l = peek;
    unsigned a, n;
    atomic_inc(&busy);
    SAY("read %d:\n", reads);
    while (*l && !num(&l, &a) && !num(&l, &n)) dump(" ", (u16)a, n > 32 ? 32 : (int)n);
    if (++reads < nreads) { sched(every * 1000, step_read); atomic_dec(&busy); return; }
    finish();
    atomic_dec(&busy);
}

static void step_start_(void);
static void step_start(void) { atomic_inc(&busy); step_start_(); atomic_dec(&busy); }
static void step_start_(void)
{
    u8 b[12]; int i, zero = 1;
    rstream(0x3600, b, 12);
    if (!same(b, coex_head, 12)) { SAY("0x3600 is not Qorvo's coex program: nothing done\n"); dump("at", 0x3600, 12); state = -1; return; }
    rstream(0x3000, b, 4);
    if (!same(b, filt_head, 4)) { SAY("0x3000 is not the MAC filter program: nothing done\n"); state = -1; return; }
    (void)i; (void)zero;
    /* never a stream read of the uc8051 block (0x680-0x6a7): the chip answers it with another event (0x0f for
     * 0x07), the read fails, and the driver's assert resets the chip once the step is over */
    SAY("before: 0x680 = %02x, 0x806 = %02x\n", rreg(0x680), rreg(0x806));
    pokes(poke);
    writes(wr); writes(wr2); writes(wr3);
    if (nouc) { SAY("the 8051 left as it is\n"); state = 1; sched(runms * 1000, step_read); return; }
    upload(&probe_info);
    run((u16)(vector >= 0 ? vector : at));
    SAY("program (%u bytes) uploaded at %04x and started at %04x, 0x680 = %02x, 0x806 = %02x\n", probe_info.size, at,
        vector >= 0 ? vector : at, rreg(0x680), rreg(0x806));
    state = 1;
    sched(runms * 1000, step_read);
}

/* gpHal_k7c_CoexDrvOnChip_RamDataInfo: a data symbol, which laser's kallsyms does not list for a module; the address
 * gpHal_CoexInitGpioInt loads by its movw/movt pair (+0xc, +0x10), taken only where the struct is the one analysed */
static const struct raminfo *find_coex(void)
{
    const u32 *f = (const u32 *)kallsyms_lookup_name("gpHal_CoexInitGpioInt");
    int i;
    if (!f) return NULL;
    for (i = 0; i < 32; i++) {
        u32 w = f[i], t = f[i + 1];
        if ((w & 0x0ff00000) == 0x03000000 && (t & 0x0ff00000) == 0x03400000 && (w & 0xf000) == (t & 0xf000)) {
            u32 a = ((w >> 4) & 0xf000) | (w & 0xfff) | (((t >> 4) & 0xf000) | (t & 0xfff)) << 16;
            const struct raminfo *r = (const struct raminfo *)a;
            if (a >= 0xbf000000 && r->size == 0x900 && r->start == 0x3600) return r;
        }
    }
    return NULL;
}

static int show(struct seq_file *m, void *v) { seq_printf(m, "STATE = %d\n%.*s", state, outn, out); return 0; }
static int open_fn(struct inode *inode, struct file *file) { return single_open(file, show, NULL); }
/* laser's struct file_operations (hassmic_rcpi4m.c says how it was found): 124 bytes, open at 0x34 */
static const void *const fops_words[31] = {
    [0] = THIS_MODULE, [1] = seq_lseek, [2] = seq_read, [0x34 / 4] = open_fn, [0x3c / 4] = single_release,
};

static int __init init_fn(void)
{
    upload = (void *)kallsyms_lookup_name("gpRamProgram_Upload");
    run = (void *)kallsyms_lookup_name("gpRamProgram_Run");
    rreg = (void *)kallsyms_lookup_name("readRegExternal");
    rmw = (void *)kallsyms_lookup_name("readModifyWriteRegExternal");
    rstream = (void *)kallsyms_lookup_name("readByteStreamExternal");
    wstream = (void *)kallsyms_lookup_name("writeByteStreamExternal");
    sched = (void *)kallsyms_lookup_name("gpSched_ScheduleEvent");
    coex_info = find_coex();
    hm_snprintf = (void *)kallsyms_lookup_name("snprintf");
    unsched = (void *)kallsyms_lookup_name("gpSched_UnscheduleEvent");
    if (!hm_snprintf || !unsched || !upload || !run || !rreg || !rmw || !rstream || !wstream || !sched || !coex_info) {
        pr_err("hassmic_gp8051: the GP712 driver's functions are not in this kernel\n");
        return -ENOENT;
    }
    if (*hex) {                                 /* a program of the parameters' */
        const char *h = hex; unsigned n = 0;
        while (h[0] && h[1] && n < sizeof prog) {
            int a = hexval(h[0]), b = hexval(h[1]);
            if (a < 0 || b < 0) { pr_err("hassmic_gp8051: hex: not hex at %d\n", (int)(h - hex)); return -EINVAL; }
            prog[n++] = (u8)(a << 4 | b); h += 2;
        }
        probe_info.data = prog; probe_info.size = (u16)n;
    }
    probe_info.start = (u16)at;
    if (overlaps(at, probe_info.size) || ((vector >= 0 ? vector : at) & 0x7f)) {
        pr_err("hassmic_gp8051: %u bytes at %04x: in coex, the filter or the mailbox, or a vector not 128-aligned\n", probe_info.size, at);
        return -EINVAL;
    }
    if (!proc_create("hassmic_gp8051", 0444, NULL, (const struct file_operations *)fops_words)) return -ENOMEM;
    sched(0, step_start);
    return 0;
}

static void __exit exit_fn(void)
{
    int i;
    /* a step running may schedule the next: wait for it, then take what is scheduled, twice over the window */
    for (i = 0; i < 200 && atomic_read(&busy); i++) msleep(10);
    unsched(step_start); unsched(step_read);
    for (i = 0; i < 200 && atomic_read(&busy); i++) msleep(10);
    unsched(step_start); unsched(step_read);                 /* nothing more of ours scheduled ... */
    if (state == 1) {                                        /* ... and what was patched undone, on its thread */
        sched(0, step_abort);
        for (i = 0; i < 200 && state == 1; i++) msleep(10);
        if (state == 1) { unsched(step_abort); pr_err("hassmic_gp8051: the driver's thread did not run the undo: the chip "
                                                    "keeps the test's changes until the driver starts again\n"); }
    }
    for (i = 0; i < 200 && atomic_read(&busy); i++) msleep(10);   /* a step still running: its text stays until done */
    remove_proc_entry("hassmic_gp8051", NULL);
}
module_init(init_fn);
module_exit(exit_fn);
MODULE_LICENSE("GPL");

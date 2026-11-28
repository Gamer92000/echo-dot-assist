/*
 * hassmic_gptime: the Echo 4's GP712 chip clock beside the host's, for timed transmit (PLAN.md "Thread 1.4", phase 2b;
 * docs/re-gp712.md).  Amazon's GP712 driver (DrvMacKernel_GP712_..._Laser, /system/lib/modules/gp712_mac.ko) times
 * a transmission on the chip's own clock (ScheduleTimedTx, command 0x8E: gpHal_ScheduleAbsoluteEvent at that time,
 * unconverted), and no command of its API 2.7 reads that clock.  The driver itself reads it for every frame it hands
 * up and every transmit it confirms: gpPd_DataIndication and gpPd_cbDataConfirm call gpHal_GetTime(&chip_now) and,
 * right after, gpOsal_TimeGetCurrent() (getrawmonotonic64, us, as u32), and turn the frame's chip time into the host's
 * by their difference.  Here each of those two calls to gpHal_GetTime is made a call to hm_gettime, which calls it
 * itself and the host clock as the driver will, and keeps the pair: a chip time is then host + (chip - pair's chip).
 * Nothing else is called from outside the driver's own context: gpHal_GetTime's register read (an SPI message through
 * the kernel's spi_sync) and its nesting counter stay the driver's thread's.
 *
 *   /proc/hassmic_gptime   "CHIP = <us, u32>\nHOST = <us, u32: CLOCK_MONOTONIC_RAW, the driver's>\nPAIRS = <n>\n
 *                           AGE = <ms since the pair>\nTX = <chip us of the last transmission>\nTXN = <n>\n
 *                           HOOKED = <calls patched, of 3>\n"; before the first pair "ERROR = no pair yet\nHOOKED =
 *                           n\n".  In RAM only.
 * TX: when the chip sent the last frame it confirmed, on its own clock (gpPd_cbDataConfirm's gpHal_GetTxTimestamp,
 * caught the same way): what a timed transmission is held to (tools/gp712's probe, gprcp).
 *
 * As hassmic_rcpi4m (Wi-Fi motion, the same kernel): laser's 4.9.77+ (MODVERSIONS, Amazon's structs) imports nothing
 * but what stock modules import and finds the rest by kallsyms_lookup_name (HM_KALLSYMS); no kprobes, no patch_text:
 * the call's word written under stop_machine after set_memory_rw, the caches flushed; the call found by its target in
 * each function's first scan_max instructions, exactly one, else nothing patched; a call already to hm_gettime counts as
 * done.  The driver is insmod'ed once at post-fs (init.zigbee.rc) and never unloaded: no module notifier.  No
 * module_exit: a thread may sit in hm_gettime.  Off by default; root loads it (main.sh) only where asked.
 *
 * Enhanced ACKs (PLAN.md "Thread 1.4" 2.1, docs/re-gp712.md "The MAC filter program"; enhack=0 leaves it out): the
 * chip's receive filter is a program of its sequence processor that gpHal_Init uploads at every start of the driver
 * (0x3000, its RAM), which parses headers by 802.15.4-2006 and answers with an Imm-Ack.  gpBaseComps_StackInit's one
 * call to gpHal_Init is made a call to hm_halinit, which calls it and then, on the driver's own thread as gpHal_Init
 * itself, extends that program (hassmic_gpfilter.h, made by tools/gp712/mkseq.py): 2015 frames taken (MACFILT 0x6a30
 * bit 2), parsed by 2015's PAN ID compression, and answered with an Enh-Ack the program writes into the hardware's
 * ACK buffer (sent at the hardware's turnaround).  Only where the 1196 bytes there are the program analysed (FNV-1a)
 * and each hook's place holds the bytes it replaces; the extensions before the hooks, each hook one stream write.
 */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

#if defined(CONFIG_ARM64) || !defined(HM_KALLSYMS)
#error "hassmic_gptime is laser's: 32-bit ARM, HM_KALLSYMS"
#endif


static char *get_sym = "gpHal_GetTime";
static char *host_sym = "gpOsal_TimeGetCurrent";
static char *callers = "gpPd_DataIndication,gpPd_cbDataConfirm";
static int scan_max = 160;         /* gpPd_DataIndication's call is its 68th instruction, gpPd_cbDataConfirm's 30th */
module_param(get_sym, charp, 0444);
module_param(host_sym, charp, 0444);
module_param(callers, charp, 0444);
module_param(scan_max, int, 0444);

static int enhack = 1;
module_param(enhack, int, 0644);     /* root's main.sh: 0 for the driver's next start where CSL is off */

static long (*hm_pkr)(void *dst, const void *src, size_t size);
static int (*hm_stop)(int (*fn)(void *), void *data, const struct cpumask *cpus);
static void (*hm_coherent)(unsigned long start, unsigned long end);
static int (*hm_set_rw)(unsigned long addr, int numpages);
#define probe_kernel_read(d, s, n) hm_pkr(d, s, n)

static void (*get_time)(u32 *chip);         /* void gpHal_GetTime(UInt32 *): us (register 0x586 x 2) */
static void (*get_txts)(u8 pbm, u32 *ts);   /* void gpHal_GetTxTimestamp(UInt8 pbm entry, UInt32 *): chip us */
static u32 (*host_time)(void);              /* uint32_t gpOsal_TimeGetCurrent(void): getrawmonotonic64, us */

static void (*hal_init)(u32 a);            /* gpHal_Init(UInt8) */
static void (*rstream)(u16 addr, u8 *buf, u8 len), (*wstream)(u16 addr, const u8 *buf, u8 len);
static void (*rmw)(u16 addr, u8 mask, u8 val);


static int resolve(void)
{
    hm_pkr = (void *)kallsyms_lookup_name("probe_kernel_read");
    hm_stop = (void *)kallsyms_lookup_name("stop_machine");
    hm_coherent = (void *)kallsyms_lookup_name("v7_coherent_kern_range");
    hm_set_rw = (void *)kallsyms_lookup_name("set_memory_rw");
    get_time = (void *)kallsyms_lookup_name(get_sym);
    host_time = (void *)kallsyms_lookup_name(host_sym);
    get_txts = (void *)kallsyms_lookup_name("gpHal_GetTxTimestamp");
    hal_init = (void *)kallsyms_lookup_name("gpHal_Init");
    rstream = (void *)kallsyms_lookup_name("readByteStreamExternal");
    wstream = (void *)kallsyms_lookup_name("writeByteStreamExternal");
    rmw = (void *)kallsyms_lookup_name("readModifyWriteRegExternal");
    if (!hal_init || !rstream || !wstream || !rmw) { enhack = 0; hal_init = NULL; }
    if (hm_pkr && hm_stop && hm_coherent && hm_set_rw && get_time && host_time && get_txts) return 0;
    pr_err("hassmic_gptime: probe_kernel_read, stop_machine, v7_coherent_kern_range, set_memory_rw, %s or %s not in this kernel (GP712 driver loaded?)\n",
           get_sym, host_sym);
    return -ENOENT;
}

/* the pair, one 64-bit word (chip << 32 | host): read whole by atomic64 (ldrexd), never torn */
static atomic64_t pair = ATOMIC64_INIT(0);
static atomic_t pairs = ATOMIC_INIT(0);
static unsigned long pair_jiffies;
static atomic_t tx_last = ATOMIC_INIT(0), tx_n = ATOMIC_INIT(0);

static void hm_txts(u8 pbm, u32 *ts)
{
    get_txts(pbm, ts);
    atomic_set(&tx_last, (int)*ts);
    atomic_inc(&tx_n);
}
static int hooked;

static void hm_gettime(u32 *chip)
{
    u32 c, h;
    get_time(chip);
    c = *chip; h = host_time();
    atomic64_set(&pair, (s64)((u64)c << 32 | h));
    WRITE_ONCE(pair_jiffies, jiffies);
    atomic_inc(&pairs);
}

#include "hassmic_gpfilter.h"

static atomic_t filter_done = ATOMIC_INIT(0), filter_refused = ATOMIC_INIT(0);
static int filter_why;          /* the last refusal: 1 the program differs, 2 a hook's place */
static int filter_hooked;       /* gpBaseComps_StackInit's call is ours: what root asks (enhack is its own switch) */

static void wblob(u16 at, const u8 *b, int n)
{
    int o;
    for (o = 0; o < n; o += 32) wstream((u16)(at + o), b + o, (u8)(n - o < 32 ? n - o : 32));
}

static void filter_patch(void)
{
    u8 b[32];
    u32 h = 0x811c9dc5;
    int o, i, k;
    for (o = 0; o < GPF_FILTER_LEN; o += 32) {
        int n = GPF_FILTER_LEN - o < 32 ? GPF_FILTER_LEN - o : 32;
        rstream((u16)(GPF_FILTER_AT + o), b, (u8)n);
        for (i = 0; i < n; i++) h = (h ^ b[i]) * 0x01000193;
    }
    if (h != GPF_FILTER_FNV) { filter_why = 1; atomic_inc(&filter_refused); return; }    /* not the one analysed */
    for (k = 0; k < (int)ARRAY_SIZE(gpf_hooks); k++) {                   /* gpHal_Init uploaded it anew: the originals are there */
        rstream(gpf_hooks[k].at, b, 4);
        for (i = 0; i < 4; i++) if (b[i] != gpf_hooks[k].orig[i]) { filter_why = 2; atomic_inc(&filter_refused); return; }
    }
    /* RX is not on yet at gpBaseComps_StackInit (gpMacCore_Init and the receiver come after): no frame meets a hook
     * half written, whatever the chip does with one stream write */
    wblob(GPF_ACK_AT, gpf_ack, sizeof gpf_ack);
    wblob(GPF_P2015_AT, gpf_p2015, sizeof gpf_p2015);
    for (k = 0; k < (int)ARRAY_SIZE(gpf_hooks); k++) wstream(gpf_hooks[k].at, gpf_hooks[k].hook, 4);
    rmw(0x6a30, 0x04, 0x04);                    /* frame version 2015 accepted */
    /* the ack wait: gpHal_Init writes 32 (in 32 us: 1024 us after the frame), and the chip takes an ack only if it has
     * ended by then, so nothing over 20 bytes (2026-11-28, tools/gp712/timedack.py: Enh-Acks of 7, 13, 17 bytes taken,
     * 21, 23 not).  A CSL child's Enh-Acks are secured and carry its CSL IE: 23 bytes, 31 to an extended address (up
     * to 1.4 ms); none was ever taken, and CSL never held.  63, the field's top (64 took no ack at all: six bits), is
     * 2 ms: with it every CSL frame to the XIAO was acked (cslsweep.py: 10 of 10 at any shift within +-1.5 ms) */
    rmw(0x408, 0xff, 63);
    atomic_inc(&filter_done);
}

static void hm_halinit(u32 a)
{
    int done_before = atomic_read(&filter_done);
    hal_init(a);
    if (READ_ONCE(enhack)) filter_patch();      /* root may switch it (main.sh radio_csl): read at each start */
    /* wherever the program runs unpatched: no 2015 frame taken (gpHal_Init writes 0x6a30's bits 0 and 1 only, and a
     * bit 2 left from a patch before, as long as the chip is not reset, would hand 2015 headers to 2006's parser) */
    if (atomic_read(&filter_done) == done_before) rmw(0x6a30, 0x04, 0x00);
}

static int show(struct seq_file *m, void *v)
{
    u64 p = (u64)atomic64_read(&pair);
    seq_printf(m, "ENHACK = %d\nFILTER_HOOKED = %d\nFILTER = %d\nFILTER_REFUSED = %d %d\n", READ_ONCE(enhack),
               filter_hooked, atomic_read(&filter_done), atomic_read(&filter_refused), filter_why);
    if (!atomic_read(&pairs)) { seq_printf(m, "ERROR = no pair yet\nHOOKED = %d\n", READ_ONCE(hooked)); return 0; }
    seq_printf(m, "CHIP = %u\nHOST = %u\nPAIRS = %d\nAGE = %u\nTX = %u\nTXN = %d\nHOOKED = %d\n", (u32)(p >> 32), (u32)p,
               atomic_read(&pairs), jiffies_to_msecs(jiffies - READ_ONCE(pair_jiffies)), (u32)atomic_read(&tx_last),
               atomic_read(&tx_n), READ_ONCE(hooked));
    return 0;
}

static int open_fn(struct inode *inode, struct file *file) { return single_open(file, show, NULL); }
/* laser's struct file_operations is not kernel.org's (hassmic_rcpi4m.c says how it was found): 124 bytes, open at 0x34 */
static const void *const fops_words[31] = {
    [0] = THIS_MODULE, [1] = seq_lseek, [2] = seq_read, [0x34 / 4] = open_fn, [0x3c / 4] = single_release,
};

static int is_bl_to(u32 insn, unsigned long at, unsigned long to)      /* bl, condition always; pc reads 8 ahead */
{
    long off = sign_extend32((insn & 0x00ffffff) << 2, 25);
    return (insn & 0xff000000) == 0xeb000000 && at + 8 + off == to;
}

static int make_bl(unsigned long at, unsigned long to, u32 *insn)
{
    long off = (long)to - (long)(at + 8);
    if (off < -SZ_32M || off >= SZ_32M) return -ERANGE;
    *insn = 0xeb000000 | ((off >> 2) & 0x00ffffff);
    return 0;
}

static int poke(void *arg)
{
    u32 *w = arg;
    *(u32 *)w[0] = w[1];
    hm_coherent(w[0], w[0] + 4);
    return 0;
}

static int patch(void *addr, u32 insn)
{
    u32 w[2] = { (u32)addr, insn };
    if (hm_set_rw((unsigned long)addr & PAGE_MASK, 1)) return -EPERM;
    return hm_stop(poke, w, NULL);
}

/* one caller: its one call to target made a call to wrapper; 1 hooked (or already), 0 not */
static int hook_one(const char *name, unsigned long target, unsigned long wrapper)
{
    unsigned long caller = kallsyms_lookup_name(name), site = 0;
    u32 insn; int i, n = 0, done = 0;
    if (!caller) { pr_err("hassmic_gptime: %s not in this kernel\n", name); return 0; }
    for (i = 0; i < scan_max; i++) {
        if (probe_kernel_read(&insn, (void *)(caller + 4 * i), 4)) break;
        if (is_bl_to(insn, caller + 4 * i, target)) { site = caller + 4 * i; n++; }
        if (is_bl_to(insn, caller + 4 * i, wrapper)) done++;
    }
    if (!n && done == 1) return 1;
    if (n != 1) { pr_err("hassmic_gptime: %d calls to %ps in %s's first %d instructions, not 1: not patched\n", n, (void *)target, name, scan_max); return 0; }
    if (make_bl(site, wrapper, &insn)) { pr_err("hassmic_gptime: %s out of branch range\n", name); return 0; }
    if (patch((void *)site, insn)) { pr_err("hassmic_gptime: patching %s failed\n", name); return 0; }
    pr_info("hassmic_gptime: %s+%#lx (call to %ps) hooked\n", name, site - caller, (void *)target);
    return 1;
}

static int __init init_fn(void)
{
    char name[64];
    const char *c = callers;
    int r, k;
    if ((r = resolve())) return r;
    while (*c) {                                    /* comma-separated, by hand: only what stock modules import */
        for (k = 0; *c && *c != ',' && k < (int)sizeof name - 1; ) name[k++] = *c++;
        name[k] = 0;
        while (*c && *c != ',') c++;
        if (*c == ',') c++;
        if (k) hooked += hook_one(name, (unsigned long)get_time, (unsigned long)hm_gettime);
    }
    hooked += hook_one("gpPd_cbDataConfirm", (unsigned long)get_txts, (unsigned long)hm_txts);
    if (hal_init) filter_hooked = hook_one("gpBaseComps_StackInit", (unsigned long)hal_init, (unsigned long)hm_halinit);
    if (!filter_hooked) enhack = 0;
    if (!hooked && !filter_hooked) return -EINVAL;
    /* the name the module was built under (a test copy beside a loaded one has its own) */
    if (!proc_create(KBUILD_MODNAME, 0444, NULL, (const struct file_operations *)fops_words)) pr_err("hassmic_gptime: no /proc entry\n");
    return 0;
}
module_init(init_fn);
MODULE_LICENSE("GPL");      /* kallsyms_lookup_name and stop_machine are GPL-only */

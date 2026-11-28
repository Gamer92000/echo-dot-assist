/* What ended the firmware last (tools/gp712/csltest.py's XIAO stopped answering, USB too, as it attached as a CSL
 * child: Zephyr halts on a fatal error, and its log never reached the port): the fatal error's reason, the faulting
 * PC and LR and, for an assert, its condition, file and line, kept in RAM that survives a reset; the reset done at
 * once; the shell command "lastfatal" says them after (and how often).  addr2line -e build/xiao/zephyr/zephyr.elf
 * <pc> names the place. */
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

#define MAGIC 0x48465433u                       /* "HFT3": a new one whenever the struct changes */

static struct { uint32_t magic, count, reason, pc, lr, ms, line; char file[48], cond[64]; } kept
	__attribute__((section(".noinit")));

static void fresh(void)
{
	if (kept.magic != MAGIC) { memset(&kept, 0, sizeof kept); kept.magic = MAGIC; }
}

static void keep_text(char *to, size_t cap, const char *s, int tail)
{
	size_t n = s ? strlen(s) : 0;
	const char *f = !s ? "" : tail && n >= cap ? s + n - (cap - 1) : s;
	strncpy(to, f, cap - 1); to[cap - 1] = 0;
}

/* an assert's place and condition, kept before the panic it ends in (zassert's own print never reaches the port) */
void zassert_fail(const char *cond, const char *file, unsigned int line, const char *fmt, ...)
{
	ARG_UNUSED(fmt);
	fresh();
	keep_text(kept.file, sizeof kept.file, file, 1);
	keep_text(kept.cond, sizeof kept.cond, cond, 0);
	kept.line = line ? line : 1;
	k_panic();
	CODE_UNREACHABLE;
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	fresh();
	kept.count++;
	kept.reason = reason;
	kept.pc = esf ? esf->basic.pc : 0;
	kept.lr = esf ? esf->basic.lr : 0;
	kept.ms = k_uptime_get_32();
	if (reason != K_ERR_KERNEL_PANIC) { kept.line = 0; kept.file[0] = kept.cond[0] = 0; }
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

static int cmd_lastfatal(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (kept.magic != MAGIC || !kept.count) { shell_print(sh, "none"); return 0; }
	shell_print(sh, "fatal errors: %u; last: reason %u, pc 0x%08x, lr 0x%08x, %u ms after its start",
		    kept.count, kept.reason, kept.pc, kept.lr, kept.ms);
	if (kept.line) shell_print(sh, "assert [%s] at %s:%u", kept.cond, kept.file, kept.line);
	return 0;
}

SHELL_CMD_REGISTER(lastfatal, NULL, "The last fatal error (reason, PC, LR, an assert's place), kept over the reset it caused",
		   cmd_lastfatal);

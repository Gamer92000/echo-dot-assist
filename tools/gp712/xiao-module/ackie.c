/* "ackie <short address hex> <bytes>": a header IE of that many bytes in the radio's Enh-Acks to that short address
 * (tools/gp712/timedack.py): the Echo 4's GP712 took the short unsecured Enh-Acks of a detached XIAO and none of a CSL
 * child's (secured, a CSL IE: about 25 bytes against 9), so the length is tried alone.  The IE is a vendor-specific
 * one (element 0x00: an OUI, then filler), which a receiver passes over; 0 bytes takes it out. */
#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <nrf_802154.h>

static int cmd_ackie(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 3) { shell_error(sh, "ackie <short address hex> <IE bytes, 0 or 5..40>"); return -EINVAL; }
	uint16_t a = (uint16_t)strtoul(argv[1], NULL, 16);
	long n = strtol(argv[2], NULL, 10);
	uint8_t addr[2] = { (uint8_t)a, (uint8_t)(a >> 8) };
	if (n == 0) {
		bool ok = nrf_802154_ack_data_clear(addr, false, NRF_802154_ACK_DATA_IE);
		shell_print(sh, "no IE for %04x: %s", a, ok ? "cleared" : "none was set");
		return 0;
	}
	if (n < 5 || n > 40) { shell_error(sh, "IE bytes: 0, or 5 to 40 (2 descriptor + 3 OUI + filler)"); return -EINVAL; }
	static uint8_t ie[40];
	uint16_t len = (uint16_t)(n - 2);            /* content: OUI + filler */
	ie[0] = (uint8_t)(len & 0x7f);               /* header IE: length 0..6, element id 7..14 (0x00 vendor), type 15 0 */
	ie[1] = 0;
	ie[2] = 0x00; ie[3] = 0x0e; ie[4] = 0x57;    /* an OUI of no one's: filler that says "test" to a sniffer */
	memset(ie + 5, 0xa5, (size_t)n - 5);
	bool ok = nrf_802154_ack_data_set(addr, false, ie, (uint16_t)n, NRF_802154_ACK_DATA_IE);
	shell_print(sh, "Enh-Acks to %04x carry a %ld byte IE: %s", a, n, ok ? "set" : "refused");
	return 0;
}

SHELL_CMD_REGISTER(ackie, NULL, "A header IE of N bytes in the Enh-Acks to a short address (tools/gp712/timedack.py)",
		   cmd_ackie);

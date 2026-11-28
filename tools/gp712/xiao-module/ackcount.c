/* "acks": what the radio acked and what it heard asking for an ack (tools/gp712/cslsweep.py): the Echo 4 heard none
 * of a CSL child's Enh-Acks on the air, so the XIAO says whether it sent them.  Nordic's driver calls
 * nrf_802154_tx_ack_started as each ack goes out and nrf_802154_received_timestamp_raw for each frame it took; both are
 * wrapped at link time (CMakeLists.txt) and passed on unchanged.  "acks clear" starts the counts again. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

void __real_nrf_802154_tx_ack_started(const uint8_t *data);
void __real_nrf_802154_received_timestamp_raw(uint8_t *data, int8_t power, uint8_t lqi, uint64_t time);

static struct {
	uint32_t imm, enh, enh_sec, enh_ie;             /* acks sent */
	uint32_t ar[2][2];                              /* frames heard asking for one: [2015][secured] */
	uint8_t last[4][40]; uint8_t last_len[4]; uint8_t n;
} c;

/* data: PHR (the length, FCS included), then the PSDU */
void __wrap_nrf_802154_tx_ack_started(const uint8_t *data)
{
	uint8_t len = data[0];
	uint16_t fcf = (uint16_t)(data[1] | data[2] << 8);
	if (((fcf >> 12) & 3) == 2) {
		c.enh++;
		if (fcf & 0x08) c.enh_sec++;
		if (fcf & 0x0200) c.enh_ie++;
	} else {
		c.imm++;
	}
	uint8_t k = c.n++ & 3, m = len > 40 ? 40 : len;
	memcpy(c.last[k], data + 1, m); c.last_len[k] = len;
	__real_nrf_802154_tx_ack_started(data);
}

void __wrap_nrf_802154_received_timestamp_raw(uint8_t *data, int8_t power, uint8_t lqi, uint64_t time)
{
	uint16_t fcf = (uint16_t)(data[1] | data[2] << 8);
	if ((fcf & 7) == 1 && (fcf & 0x20)) c.ar[((fcf >> 12) & 3) == 2][(fcf & 0x08) != 0]++;
	__real_nrf_802154_received_timestamp_raw(data, power, lqi, time);
}

static int cmd_acks(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "clear")) { memset(&c, 0, sizeof c); shell_print(sh, "cleared"); return 0; }
	shell_print(sh, "acks sent: Imm %u, Enh %u (secured %u, with IEs %u)", c.imm, c.enh, c.enh_sec, c.enh_ie);
	shell_print(sh, "data frames asking: 2006 %u (secured %u), 2015 %u (secured %u)", c.ar[0][0] + c.ar[0][1],
		    c.ar[0][1], c.ar[1][0] + c.ar[1][1], c.ar[1][1]);
	for (int i = 0; i < 4 && i < c.n; i++) {
		int k = (c.n - 1 - i) & 3;
		char hex[81]; int m = c.last_len[k] > 40 ? 40 : c.last_len[k];
		for (int j = 0; j < m; j++) snprintk(hex + 2 * j, 3, "%02x", c.last[k][j]);
		shell_print(sh, "ack -%d: %u bytes %s", i, c.last_len[k], hex);
	}
	return 0;
}

SHELL_CMD_REGISTER(acks, NULL, "Acks sent and frames asking for one ('acks clear')", cmd_acks);

/* The high-frequency crystal kept on for good, which the nRF52840's USB device needs: a sleepy end device's radio
 * driver lets it go while the radio sleeps.  Put in when the XIAO's USB died as it first slept (2026-11-28); that
 * was the upstream driver's assert (xiao-build.sh's head), not the crystal, and with it the firmware was then
 * tested: kept, USB-powered, so its current is of no account.  The clock driver's own request for a radio
 * controller: counted, no notification, never released. */
#include <zephyr/init.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

static int hfxo_hold(void)
{
	z_nrf_clock_bt_ctlr_hf_request();      /* counted by the clock driver; never released */
	return 0;
}

SYS_INIT(hfxo_hold, APPLICATION, 0);

/* "bootloader": the XIAO nRF52840 into its bootloader's serial DFU from the shell, so that a new firmware goes on
 * without the reset button's double press (tools/gp712/xiao-build.sh, xiao-flash.sh).  Seeed's bootloader is
 * Adafruit's nRF52 bootloader: a reset with GPREGRET 0x4E (DFU_MAGIC_SERIAL_ONLY_RESET) stays in its serial DFU,
 * which adafruit-nrfutil speaks; Seeed's Arduino firmware does the same on a 1200 baud open of its port. */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <hal/nrf_power.h>

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "into the bootloader's serial DFU");
	k_msleep(200);                          /* the line out before USB goes */
	nrf_power_gpregret_set(NRF_POWER, 0, 0x4E);
	NVIC_SystemReset();
	return 0;
}

SHELL_CMD_REGISTER(bootloader, NULL, "Reset into the bootloader's serial DFU (tools/gp712/xiao-flash.sh)", cmd_bootloader);

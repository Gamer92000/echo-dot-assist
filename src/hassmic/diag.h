/* SoC temperature and CPU usage for the settings page (no longer Home Assistant sensors: diagnostics live on the page) */
#ifndef DIAG_H
#define DIAG_H
float diag_soc_temp(void);              /* degrees C of the thermal zone board.thermal_type; NAN: none */
float diag_cpu(void);                   /* percent busy over the next 250 ms; NAN: no /proc/stat */
#endif

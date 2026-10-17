/* Device attestation: the token an Echo signs with its own key for Amazon's /auth/register (dha.c). */
#ifndef DHA_H
#define DHA_H
#include <stddef.h>
#include <time.h>

/* The device attestation token for device type DT, dated WHEN, into OUT; the serial comes from the HAL.  drvV1 where
 * the HAL's key is the RSA keymaster (biscuit, radar), donut's drvV3 around its certificate where it is the EC dhav2
 * key -- what each model's MAP builds.  0, or -1 with the reason in ERR.  Needs group drmrpc (the HAL's TEE session
 * refuses a process without it). */
int dha_jwt(const char *dt, time_t when, char *out, size_t cap, char *err, size_t errsz);
/* the device serial (DSN) as the HAL has it: 0, or -1 */
int dha_serial(char *out, size_t cap);
#endif

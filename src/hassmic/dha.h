/* Device attestation: the token an Echo signs with its own key for Amazon's /auth/register (dha.c). */
#ifndef DHA_H
#define DHA_H
#include <stddef.h>
#include <time.h>

/* The device attestation token for device type DT, dated WHEN, into OUT.  drvV1 where the HAL's key is the RSA
 * keymaster (biscuit, radar), donut's drvV3 around its certificate where it is the EC dhav2 key -- what each model's MAP
 * builds, with the serial from the HAL; Java MAP's drvV1 where the key is behind the fireos-dha service (checkers), with
 * DSN, the serial the registration sends.  0, or -1 with the reason in ERR.  The HAL needs group drmrpc (its TEE
 * session refuses a process without it); the service asks for none. */
int dha_jwt(const char *dt, const char *dsn, time_t when, char *out, size_t cap, char *err, size_t errsz);
/* the device serial (DSN) as the HAL has it: 0, or -1 */
int dha_serial(char *out, size_t cap);
#endif

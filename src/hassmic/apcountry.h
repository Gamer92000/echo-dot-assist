/* The Wi-Fi access point's country, from the 802.11d Country element of the network the Echo is on.  See apcountry.c. */
#ifndef APCOUNTRY_H
#define APCOUNTRY_H
#include <stddef.h>
#include <stdint.h>

int apcountry_get(char cc[3]);      /* 1 and cc = "DE"; 0 none (no Wi-Fi, no element, no nl80211), cc "".  One caller at a
                                     * time (static buffers): clock.c's zone thread.  ~1 ms (Dot 2); a few s at worst */

/* for tests/unit/apcountry_test.c */
int apcountry_ies(const uint8_t *ie, size_t len, char cc[3]);       /* the Country element in an IE list: 1 found */
#define APC_FOUND 1                 /* apcountry_parse: the associated BSS's country is in cc */
#define APC_END   2                 /* and the dump ended (done, or an error: no wireless on that interface) */
int apcountry_parse(const void *buf, size_t len, char cc[3]);       /* one datagram of a GET_SCAN dump: APC_* */
#endif

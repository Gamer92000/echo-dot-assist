/* Switching Wi-Fi networks from the settings page.  hassmic may not talk to wpa_supplicant and faces the network, so as
 * with adb (adbwifi.c) it only asks: a request in state/, which root's watcher hands to scripts/device/wifi.sh; the
 * answers come back in a directory only root writes.  See wifi.c.  Any thread; no lock needed from callers. */
#ifndef WIFI_H
#define WIFI_H
#include <stddef.h>
#include <stdint.h>

/* Ask root for a scan.  0 with *id (the answer that carries it is this scan's), or -1: ERR says why, *busy set when
 * root is switching networks or another request waits (answer 409, not 400) */
int    wifi_scan(unsigned *id, int *busy, char *err, size_t errsz);
/* Ask root to switch to SSID (1..32 bytes) with PASS: "" for an open network, 8..63 printable characters, or the PSK as
 * 64 hex digits.  The PSK is derived here (PBKDF2), so the password itself goes nowhere.  As wifi_scan otherwise */
int    wifi_join(const uint8_t *ssid, size_t n, const char *pass, unsigned *id, int *busy, char *err, size_t errsz);
/* {"current":{"ssid","state","ip"}|null,"busy","pending":{"kind","id","stale"}|null,"scan":{"id","networks":[...]}|null,
 *  "result":{"id","state","ssid","hex","ip","saved","reason","back"}|null}: see wifi.c */
size_t wifi_status_json(char *out, size_t cap);
/* {"ssid","ip"} of the network the Echo is on, or null: for the settings page's state */
size_t wifi_current_json(char *out, size_t cap);
#endif

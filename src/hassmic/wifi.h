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
/* For Improv (improv.c), which waits for the answers itself: */
struct wifi_net { uint8_t ssid[32]; size_t n; int signal, b24, b5; const char *sec; };     /* sec: open psk sae eap wep owe */
/* the networks of scan ID, strongest first, one per name; -1 while that scan has not answered */
int    wifi_scan_result(unsigned id, struct wifi_net *nets, int max);
/* 1 if the last scan (any) saw a network of that name, 0 if not, -1 if there is no scan */
int    wifi_seen(const uint8_t *ssid, size_t n);
/* switch ID: 0 under way, 1 on the network (IP its address), -1 failed (WHY: wifi.sh's reason, "unanswered" if root
 * never took the request) */
int    wifi_join_result(unsigned id, char *ip, size_t cap, char *why, size_t wcap);
/* {"current":{"ssid","state","ip"}|null,"busy","pending":{"kind","id","stale"}|null,"scan":{"id","networks":[...]}|null,
 *  "result":{"id","state","ssid","hex","ip","saved","reason","back"}|null}: see wifi.c */
size_t wifi_status_json(char *out, size_t cap);
/* {"ssid","ip"} of the network the Echo is on, or null: for the settings page's state */
size_t wifi_current_json(char *out, size_t cap);
#endif

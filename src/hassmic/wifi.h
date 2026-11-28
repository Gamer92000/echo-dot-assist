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
/* Ask root for the address on the network the Echo is on (issue #191): SPEC "auto" (DHCP) or "fixed <ip> <prefix 8..30>
 * <gateway> <dns> <dns2|->".  Checked here first (ERR in words for the page), then by root, which tries it and keeps it
 * only once the Echo was reached on it.  As wifi_scan otherwise */
int    wifi_address(const char *spec, unsigned *id, int *busy, char *err, size_t errsz);
/* For Improv (improv.c), which waits for the answers itself: */
struct wifi_net { uint8_t ssid[32]; size_t n; int signal, b24, b5; const char *sec; };     /* sec: open psk sae eap wep owe */
/* the networks of scan ID, strongest first, one per name; -1 while that scan has not answered */
int    wifi_scan_result(unsigned id, struct wifi_net *nets, int max);
/* 1 if the last scan (any) saw a network of that name, 0 if not, -1 if there is no scan */
int    wifi_seen(const uint8_t *ssid, size_t n);
/* switch ID: 0 under way, 1 on the network (IP its address), -1 failed (WHY: wifi.sh's reason, "unanswered" if root
 * never took the request) */
int    wifi_join_result(unsigned id, char *ip, size_t cap, char *why, size_t wcap);
/* {"current":{"ssid","hex","state","ip","netmask","gateway","dns":[]}|null,"busy","pending":{"kind","id","stale"}|null,
 *  "scan":{"id","networks":[...]}|null,"result":{"id","state","ssid","hex","ip","saved","reason","back"}|null,
 *  "address":{"mode":"auto"}|{"mode":"fixed","ip","prefix","gateway","dns":[],"ssid","here"},
 *  "address_result":{"id","state","ip","reason"}|null}: see wifi.c */
size_t wifi_status_json(char *out, size_t cap);
/* {"ssid","ip","fixed","ap":{"bssid","freq","since","signal"}|null,"moves":[{"at","how","from","from_freq","from_signal",
 *  "to","to_freq","to_signal","ms"}]} of the network the Echo is on, or null: for the settings page's state.  The access
 * point and the moves are root's record (scripts/device/roam.sh, issue #234), the signal /proc/net/wireless's */
size_t wifi_current_json(char *out, size_t cap);
#endif

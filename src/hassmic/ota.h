/* Push updates: receive a signed bundle over TCP and hand it to the root-side installer (scripts/system/main.sh). */
#ifndef OTA_H
#define OTA_H
int ota_start(int port);        /* listener thread; does nothing useful without /system/hassmic/update.pub */
#endif

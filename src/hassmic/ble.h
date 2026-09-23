/* Bluetooth LE on the Echo's MediaTek combo chip: raw HCI over /dev/stpbt, for Home Assistant's Bluetooth proxy.
 * Scanning, GATT client connections to up to BLE_MAX_CONN devices, pairing (Just Works) with bonds kept on the Echo. */
#ifndef BLE_H
#define BLE_H
#include <stddef.h>
#include <stdint.h>

#define BLE_BATCH 16
#define BLE_MAX_CONN 3

struct ble_adv { uint64_t addr; int rssi; unsigned addr_type, len; uint8_t data[31]; };

/* A device's GATT database.  UUIDs as ESPHome sends them: 128 bits, high and low half.  Characteristics of a service and
 * descriptors of a characteristic are contiguous: first, n.  Handles: service = start, characteristic = value handle. */
struct ble_svc { uint64_t uuid[2]; unsigned start, end; int first, n; };
struct ble_chr { uint64_t uuid[2]; unsigned decl, handle, props; int svc, first, n; };
struct ble_dsc { uint64_t uuid[2]; unsigned handle; };
struct ble_db { int nsvc, nchr, ndsc; struct ble_svc svc[48]; struct ble_chr chr[192]; struct ble_dsc dsc[256]; };

/* Everything is called on the controller thread, no lock held.  error: HCI reason / ATT error code, 0 = none.
 * addr: 48-bit device address as a number, like ESPHome. */
struct ble_handler {
    void (*adverts)(const struct ble_adv *a, int n);                  /* batches of up to BLE_BATCH, every 100 ms at most */
    void (*scan_changed)(void);                                         /* ble_scanning() changed */
    void (*slots_changed)(void);                                        /* ble_connections() changed */
    void (*connection)(uint64_t addr, int connected, unsigned mtu, int error);
    void (*services)(uint64_t addr, const struct ble_db *db);          /* valid during the call only */
    void (*read)(uint64_t addr, unsigned handle, const void *data, size_t len);
    void (*written)(uint64_t addr, unsigned handle);
    void (*notify)(uint64_t addr, unsigned handle, const void *data, size_t len);
    void (*error)(uint64_t addr, unsigned handle, int error);
    void (*paired)(uint64_t addr, int paired, int error);
    void (*unpaired)(uint64_t addr, int ok, int error);
};

int  ble_present(void);                 /* the radio exists (PC build: no) */
void ble_start(const struct ble_handler *h);    /* controller thread; takes the radio once btmanagerd has stopped.
                                                   Again to set the handler (NULL: none yet) */
const char *ble_mac(void);              /* "AA:BB:CC:DD:EE:FF", "" if unknown */
int  ble_scanning(void);                /* 1 while the controller scans */
int  ble_connections(uint64_t *addrs);  /* slots in use (connecting or connected), their addresses: BLE_MAX_CONN room */

/* Requests: queued for the controller thread, never block.  Each is answered through the handler. */
void ble_scan(int on, int active);      /* wanted state */
void ble_connect(uint64_t addr, unsigned addr_type);   /* -> connection(1, mtu) once the MTU is agreed, or (0, error) */
void ble_disconnect(uint64_t addr);     /* -> connection(0, 0) */
void ble_services(uint64_t addr);       /* -> services(), or error(addr, 0, e) */
void ble_read(uint64_t addr, unsigned handle);         /* characteristic value or descriptor -> read() / error() */
void ble_pair(uint64_t addr);          /* connected device: Just Works pairing + bond, or encrypt with the bond -> paired() */
void ble_unpair(uint64_t addr);        /* forget the bond, drop the link -> unpaired() */
void ble_write(uint64_t addr, unsigned handle, const void *data, size_t len, int response);   /* -> written() / error();
                                                                                                   nothing without response */
#endif

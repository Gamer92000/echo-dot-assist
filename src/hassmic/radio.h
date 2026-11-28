/* The Echo's 802.15.4 radio for Home Assistant (board.radio_rcp: the Echo 4's Qorvo GP712 behind Amazon's ot-rcp;
 * docs/re-thread.md).  Stock's Thread border router, its Thread service and its Zigbee hub are stopped (device.conf
 * RADIO_SERVICES, main.sh radio_watch); what runs in their place is chosen here and run by root:
 *   Thread, border router   our own otbr-agent (upstream ot-br-posix, scripts/otbr-build.sh) forms or joins a network
 *                           and routes it onto the Wi-Fi; Home Assistant's OTBR integration talks to its REST API,
 *                           which hassmic forwards on RADIO_REST_PORT to Home Assistant's own address
 *   Thread, router          the same, no border routing, no REST for Home Assistant: one more router of a network
 *                           the owner hands it (its dataset, sealed from the settings page)
 *   Zigbee, coordinator     our own Zigbee stack (zigbeed, src/zigbee/) runs the network as its trust center; Home
 *                           Assistant's ZHA or Zigbee2MQTT drive it as a ConBee (the deCONZ protocol) on RADIO_ZB_PORT,
 *                           which hassmic forwards and announces over mDNS (_zigbee-coordinator._tcp: ZHA finds it)
 *   Zigbee, router          zigbeed joins a network another coordinator runs (the first that lets it join) and extends
 *                           it; nothing to forward
 *   remote radio            the radio itself on RADIO_RCP_PORT (Spinel over HDLC-lite, as a USB stick's): Zigbee2MQTT's
 *                           zoh adapter, or an OpenThread border router of the owner's; one host at a time
 * Who may connect to either port: Home Assistant's own links (proto->peers) and one address the owner names. */
#ifndef RADIO_H
#define RADIO_H
#include <stddef.h>
#include <stdint.h>

enum { RADIO_OFF, RADIO_THREAD, RADIO_ZIGBEE, RADIO_MODES };
enum { THREAD_BR, THREAD_ROUTER, THREAD_REMOTE, THREAD_ROLES };
enum { ZIGBEE_COORD, ZIGBEE_ROUTER, ZIGBEE_REMOTE, ZIGBEE_ROLES };
extern const char *const radio_modes[RADIO_MODES];             /* "off", "thread", "zigbee": state/config's values */
extern const char *const radio_thread_roles[THREAD_ROLES];     /* "border_router", "router", "remote" */
extern const char *const radio_zigbee_roles[ZIGBEE_ROLES];     /* "coordinator", "router", "remote" */

#define RADIO_REST_PORT 28933           /* the border router's REST API, for Home Assistant's OTBR integration */
#define RADIO_RCP_PORT  28934           /* the radio itself, for a host elsewhere */
#define RADIO_ZB_PORT   28935           /* the Zigbee coordinator, deCONZ's protocol: ZHA's and Zigbee2MQTT's radio */

int  radio_present(void);               /* this Echo has one (board.radio_rcp, or HASSMIC_RADIO_DIR on the PC) */
void radio_start(void);                 /* after settings_load: the forwarding thread */
/* the settings (settings.c), lock held; -1 reads.  Root runs what they say from state/config */
int  radio_mode(int set);
int  radio_thread_role(int set);
int  radio_zigbee_role(int set);
/* the page's Radio card: what runs, the network (never its key), the addresses to give Home Assistant or a host.  No lock */
size_t radio_status_json(char *out, size_t cap);
/* the border router's ephemeral key (ePSKc) on (code: 9 digits and its NUL, for the page, sealed by web.c) or off;
 * 0, -1 refused or failed (err), -2 the border router does not answer */
int radio_share(int on, char code[16], char *err, size_t errsz);
/* the page's actions, no lock.  "form <name>": a new Thread network; "join": the network of the dataset TLV (tlv, n:
 * unsealed by web.c); "leave": the border router forgets its network; "permit <seconds>": Zigbee joining opened
 * (coordinator, router); "zb_leave": the Zigbee router leaves its network and looks for one again; "zb_prefer <extended PAN ID>|any": the network it joins (it leaves another one it is on); "zb_rotate": the coordinator gives the network a new key; "allow <IPv4>|-":
 * the one address besides Home Assistant's that may connect.  0 ok; -1 refused, err says why; -2 busy (try again) */
int  radio_action(const char *what, const char *arg, const uint8_t *tlv, size_t n, char *err, size_t errsz);
/* the mDNS service hassmic adds to its own while the Zigbee coordinator runs (avahi's XML), "" none.  No lock */
const char *radio_mdns_service(void);
#endif

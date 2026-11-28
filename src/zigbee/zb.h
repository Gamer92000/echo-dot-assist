/* zigbeed: a Zigbee PRO stack of our own on the Echo 4's 802.15.4 radio (docs/zigbee.md, docs/re-thread.md).
 * The radio is Amazon's OpenThread RCP (Spinel raw frames, through root's rcpbridge); everything above the PHY's
 * frames runs here: the MAC's association, data requests and indirect frames, the NWK layer (security, mesh routing:
 * route discovery, many-to-one routes and source routes, link status, broadcasts, end device children, leave and
 * rejoin), APS (acks, duplicates, link-key security, the trust center: transport, update device, tunnel, request,
 * verify and confirm key), a ZDO server and, as a router, a ZCL Basic cluster.
 * Roles: coordinator (the network's trust center, for Home Assistant's ZHA or Zigbee2MQTT through the deCONZ serial
 * protocol, deconz.c, which both speak) or router (joins a network another coordinator runs and extends it, alone).
 * Single-threaded: one poll loop over the radio, the host and timers (zigbeed.c). */
#ifndef ZB_H
#define ZB_H
#include <stddef.h>
#include <stdint.h>

#define ZB_MAX_NODES 512                /* the address map: every device this one knows (a coordinator's network) */
#define ZB_MAX_FRAME 127                /* an 802.15.4 PSDU, FCS included */
#define ZB_IND_MAX 32                   /* frames waiting for sleepy children, all of them */
#define ZB_BCAST_SLOTS 32               /* the broadcast transaction table */
#define ZB_DISC_SLOTS 16                /* route discoveries in flight (ours and those we pass on) */
#define ZB_PENDING_MAX 16               /* unicasts waiting for a route */
#define ZB_APS_WAIT 8                   /* APS frames waiting for their ack */
#define ZB_EP_SLOTS 4                   /* the host's endpoints (deCONZ's "configure endpoint" slots) */

enum role { R_COORD, R_ROUTER, R_ENDDEV };   /* R_ENDDEV: a sleepy end device, for the tests' joins */
enum netstate { NS_OFFLINE, NS_JOINING, NS_CONNECTED, NS_LEAVING };

/* a node of the network as this one knows it */
struct node {
    uint64_t ieee;
    uint16_t nwk;
    uint8_t cap;                        /* MAC capability flags from its association or device announce */
    uint8_t used;
    uint8_t child;                      /* an end device whose parent we are */
    uint8_t neighbor;                   /* a router or coordinator we hear (link status) */
    uint8_t has_key;                    /* its own TC link key (Zigbee 3.0 request key), or an install code's */
    uint8_t key_preconf;                /* that key was given by the host (install code), not made here */
    uint8_t key_verified;
    uint8_t key[16];
    uint32_t nwk_fc;                    /* the last NWK frame counter heard from it (where fc_known) */
    uint8_t fc_known;                   /* nwk_fc holds: a frame of that counter or lower is a replay */
    uint32_t prev_fc; uint8_t prev_fc_known;   /* the same under the network key before the last switch */
    uint8_t authed;                     /* it proved the network key (a secured frame of its own) or a link key */
    int64_t join_at;                    /* ms its association or rejoin was taken; cleared once authed */
    uint32_t aps_fc;                    /* the last APS frame counter (link-key secured) */
    int64_t seen;                       /* ms of the last frame from it */
    uint32_t timeout_s;                 /* an end device child's timeout (end device timeout request) */
    uint8_t lqi, in_cost, out_cost, out_age;   /* neighbor link: costs 1..7, 0 none */
    uint16_t next_hop;                  /* a route to it: 0xFFFF none */
    uint8_t route_cost;
    int64_t route_at;
    uint8_t nrelay;                     /* its source route (route record), relays from its side to ours */
    uint16_t relay[10];
    uint16_t parent;                    /* the router that announced it (update device), 0xFFFF unknown */
    int64_t heard;                      /* ms of the last secured frame it sent (any number of hops away) */
    int64_t poll_told;                  /* ms its last data request went to the host (deCONZ's MAC poll) */
};

/* a frame for the radio */
struct txf {
    uint8_t psdu[ZB_MAX_FRAME];
    uint8_t len;                        /* FCS included */
    uint8_t retries;                    /* MAC retries the RCP makes (unicast) */
    void (*done)(int status, void *arg); /* 0 sent (acked), else the RCP's status */
    void *arg;
};

/* the stack's state */
struct zb {
    enum role role;
    enum netstate state;
    const char *dir;                    /* its folder: zb.state */
    /* the radio */
    int rcp;
    uint64_t hw_ieee;                   /* the RCP's own EUI-64 */
    int channel;
    /* the network */
    uint64_t ieee;                      /* this node's (the host may set it: a backup restored) */
    uint16_t nwk, pan;
    uint64_t epan;                      /* the network's extended PAN ID */
    uint8_t nwk_key[16], key_seq;
    uint8_t prev_key[16], prev_key_seq; /* the key before the last switch, taken for a while (frames in flight) */
    int have_prev_key; int64_t prev_key_until;
    uint8_t tclk[16];                   /* the global TC link key ("ZigBeeAlliance09" unless the host says) */
    uint64_t tc_ieee;                   /* the trust center's EUI-64 */
    uint32_t nwk_fc, nwk_fc_saved;      /* our NWK frame counter, and what the state file says it may reach */
    uint32_t aps_fc;
    uint8_t nwk_seq, mac_seq, aps_seq, zdo_seq, zcl_seq, rreq_id;
    uint8_t update_id;
    uint32_t channel_mask;              /* deCONZ's; formation picks from it */
    int predefined_pan;
    uint8_t depth;
    int64_t permit_until;               /* ms: this node takes associations until then */
    int64_t net_permit_until;           /* ms: somewhere in the network joins are open (the TC takes update devices) */
    struct node nodes[ZB_MAX_NODES];
    int nnodes;
    /* routing */
    uint16_t mto_next;                  /* a router's route to the concentrator: next hop, 0xFFFF none */
    uint8_t mto_cost;
    int rr_needed;                      /* the concentrator asked for route records */
    /* the host's endpoints: simple descriptors as deCONZ keeps them (index, length, bytes) */
    uint8_t ep[ZB_EP_SLOTS][64];
    uint8_t ep_len[ZB_EP_SLOTS];
    /* joining as a router / end device */
    uint16_t parent;
    uint64_t parent_ieee;
    int joined;                          /* the network key arrived */
    int64_t poll_at;
    uint64_t prefer_epan;               /* a router's choice of network (the page's): 0 the first open one */
    struct seen_net { uint64_t epan; uint16_t pan; uint8_t channel, lqi, permit, room; int64_t at; } nets[16];   /* the last scans' */
    /* diagnostics */
    unsigned tx_ok, tx_fail, rx_frames, rx_bad;
};
extern struct zb zb;

/* zigbeed.c */
int64_t now_ms(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)
int  timer_at(int64_t when, void (*fn)(void *), void *arg);   /* one-shot; the same (fn, arg) moved, not added twice; -1 full */
void timer_cancel(void (*fn)(void *), void *arg);
void zb_save(void);                     /* the state file now */
void zb_save_soon(void);
uint32_t zb_random(void);

/* little-endian helpers */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static inline uint8_t *wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }
static inline uint8_t *wr32(uint8_t *p, uint32_t v) { p = wr16(p, (uint16_t)v); return wr16(p, (uint16_t)(v >> 16)); }
static inline uint8_t *wr64(uint8_t *p, uint64_t v) { p = wr32(p, (uint32_t)v); return wr32(p, (uint32_t)(v >> 32)); }

/* crypto.c */
void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* CCM* with a 4-byte MIC (Zigbee's security level 5): a = authenticated header, m = payload encrypted in place;
 * the MIC appended to m (m needs 4 bytes more).  Decrypt: 0 when the MIC holds */
void ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *a, size_t al, uint8_t *m, size_t ml);
int  ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *a, size_t al, uint8_t *m, size_t ml_with_mic);
void mmo_hash(const uint8_t *in, size_t n, uint8_t out[16]);  /* Matyas-Meyer-Oseas, Zigbee's B.6 */
void keyed_hash(const uint8_t key[16], uint8_t in, uint8_t out[16]);   /* HMAC-MMO of one byte (B.1.4) */
int  install_code_key(const uint8_t *code, size_t n, uint8_t key[16]);  /* the code with its CRC: 0 ok */

/* spinel.c: the RCP */
int  rcp_open(const char *path);
int  rcp_init(void);                    /* reset, versions, the EUI-64 */
int  rcp_set_u8(unsigned prop, uint8_t v);
int  rcp_set_u16(unsigned prop, uint16_t v);
int  rcp_set_eui(unsigned prop, uint64_t v);
int  rcp_src_match(int add, int ext, uint64_t addr);          /* the radio sets frame pending in its acks for these */
int  rcp_src_match_clear(void);
void rcp_input(void);                   /* the fd is readable */
void rcp_dispatch(void);                /* frames and transmission ends found meanwhile: acted on (main loop only) */
int  rcp_tx(struct txf *f);             /* queued; f is copied */
int  rcp_energy_scan(uint32_t mask, int ms_per_channel, int8_t out[27]);   /* blocking: formation only (nothing queued) */
/* a running network's scan, from timers: done(0, energies by channel) at its end, -1 none measured; -1 one runs */
int  rcp_energy_scan_async(uint32_t mask, int ms_per_channel, void (*done)(int st, const int8_t *e, void *arg), void *arg);
void rcp_promiscuous(int on);
int  rcp_tx_idle(void);
#define SP_PHY_ENABLED 0x20
#define SP_PHY_CHAN 0x21
#define SP_PHY_TX_POWER 0x25
#define SP_MAC_LADDR 0x34
#define SP_MAC_SADDR 0x35
#define SP_MAC_PANID 0x36
#define SP_MAC_RAW_STREAM 0x37
#define SP_MAC_PROMISC 0x38
#define SP_MAC_RX_ON_IDLE 0x3B
#define SP_SRC_MATCH_ENABLED 0x1303

/* mac.c */
struct rxinfo { int8_t rssi; uint8_t lqi; };
void mac_rx(const uint8_t *psdu, size_t len, const struct rxinfo *ri);
/* a MAC data frame with the NWK frame f (n bytes) to the short address dst (0xFFFF broadcast) or, with ext set, to
 * the EUI-64; an end device child that sleeps gets it when it polls */
int  mac_data(uint16_t dst, uint64_t ext, int use_ext, const uint8_t *f, size_t n, void (*done)(int, void *), void *arg);
int  mac_data_direct(uint16_t dst, uint64_t ext, int use_ext, const uint8_t *f, size_t n, void (*done)(int, void *), void *arg);
void mac_beacon_request(void);
int  mac_associate(uint16_t pan, uint16_t coord, uint8_t cap);
void mac_poll(void);
void mac_poll_then(void (*done)(int, void *));   /* the same, its MAC ack told */                    /* a data request to our parent (an end device, or a joiner waiting) */
void mac_indirect_expire(void *unused);
void mac_drop_indirect(uint64_t ieee);
int  mac_indirect_pending(uint64_t ieee);
int  mac_indirect_free(void);           /* slots of the indirect queue free */
int  mac_indirect_as(uint64_t ieee, uint16_t poll_nwk, const uint8_t *f, size_t n, int unproven);   /* held under the address it polls by */
void mac_scan_begin(void);
struct beacon { uint16_t pan, src; uint64_t epan; uint8_t channel, lqi, permit, router_cap, ed_cap, depth, update_id, stack; };
int  mac_scan_results(struct beacon *out, int max);

/* nwk.c */
struct node *node_by_nwk(uint16_t nwk);
struct node *node_by_ieee(uint64_t ieee);
struct node *node_add(uint64_t ieee, uint16_t nwk);   /* finds or makes; nwk 0xFFFF keeps what it had */
void node_forget(struct node *n);
uint16_t nwk_new_address(void);
void nwk_rx(const uint8_t *f, size_t n, uint16_t mac_src, uint64_t mac_src_ext, int src_ext, const struct rxinfo *ri);
/* an NWK data frame with APS frame a (n bytes) to dst; dst_ieee for the header where known (0: none); security as
 * Zigbee wants it (the network key) unless unsecured; done(status) once it went (or failed) to its next hop */
int  nwk_send(uint16_t dst, uint64_t dst_ieee, const uint8_t *a, size_t n, int secure, uint8_t radius,
              const uint16_t *relays, int nrelays, void (*done)(int, void *), void *arg);
int  nwk_send_cmd(uint16_t dst, const uint8_t *c, size_t n, int secure, uint8_t radius, int use_ext_dst, uint64_t ext);
void nwk_start(void);                   /* the network runs: link status, many-to-one routes */
void nwk_stop(void);
void nwk_leave(uint16_t nwk, uint64_t ieee, int rejoin, int remove_children);
void nwk_device_announced(uint16_t nwk, uint64_t ieee, uint8_t cap);
void nwk_join_found(void *unused);      /* router / end device: scan, associate */
void nwk_assoc_result(uint16_t addr, uint8_t status);
void nwk_joined(void);
void nwk_begin_join(void);
void nwk_resume_enddev(void);
int  nwk_deliver_unsecured(uint64_t ieee, uint16_t nwk, const uint8_t *f, size_t n);   /* a joiner's or rejoiner's key */
int  nwk_joiners(void);                 /* entries of joiners that have not proven the network key */
void nwk_address_seen(uint16_t nwk, uint64_t ieee);   /* the map, and address conflicts */
void nwk_beacon_heard(uint16_t pan, uint64_t epan);   /* in the network: a beacon of our PAN ID */
void nwk_switch_key(const uint8_t key[16], uint8_t seq);   /* the new network key active, the old one a while */
void zb_reset_network(int rejoin);
uint8_t lqi_cost(uint8_t lqi);
#define NWK_BCAST_ALL 0xFFFF
#define NWK_BCAST_RXON 0xFFFD
#define NWK_BCAST_ROUTERS 0xFFFC

/* aps.c */
struct apsind {                         /* what arrived for an endpoint here */
    uint16_t src; uint64_t src_ieee; uint8_t src_ep;
    uint16_t dst; uint8_t dst_mode;     /* 1 group, 2 nwk (incl. broadcast) */
    uint8_t dst_ep;
    uint16_t profile, cluster;
    const uint8_t *data; size_t n;
    uint8_t lqi; int8_t rssi;
    int aps_secured;
};
void aps_rx(const uint8_t *f, size_t n, uint16_t src, uint64_t src_ieee, uint16_t dst, int nwk_secured, const struct rxinfo *ri);
/* a data frame: dst_mode 1 group, 2 nwk (broadcasts too), 3 ieee.  ack: APS ack asked (unicast).  done(status):
 * 0 delivered (acked where asked), an APS/NWK status else */
int  aps_send(int dst_mode, uint16_t dst, uint64_t dst_ieee, uint8_t dst_ep, uint8_t src_ep, uint16_t profile, uint16_t cluster,
              const uint8_t *data, size_t n, int ack, uint8_t radius, const uint16_t *relays, int nrelays,
              void (*done)(int, void *), void *arg);
void aps_tc_joined(struct node *n, uint16_t parent, int rejoin);   /* send it the network key (direct or tunnelled) */
void aps_request_tclk(void *unused);    /* router / end device: a key of its own from the TC (Zigbee 3.0) */
void aps_update_device(uint64_t ieee, uint16_t nwk, uint8_t status);
int  aps_rotate_key(void);              /* the trust center: a new network key to all, the switch after; -1 one runs */
int  aps_rotating(void);
void aps_reset(void);                  /* a network left: what waited for it dropped */
int  aps_pending_key(uint8_t seq, uint8_t key[16]);   /* a joiner's key for the coming switch */
void aps_pending_taken(void);
#define APS_ST_SUCCESS 0x00
#define APS_ST_NO_ACK 0xA7
#define APS_ST_NO_ROUTE 0xD0            /* NWK route error, as deCONZ reports */
#define APS_ST_MAC_NO_ACK 0xE9
#define APS_ST_INVALID 0xC2
#define APS_ST_BUSY 0xE1                /* MAC channel access failure: nothing free to send it with */
#define APS_ST_FRAME_TOO_LONG 0xA0     /* APS ASDU_TOO_LONG: no room in a frame */

/* zdo.c: requests to this node; a router's ZCL Basic */
void zdo_rx(const struct apsind *in);   /* endpoint 0 here: returns whether it answered (coordinator: the host gets it too) */
int  zdo_local(const struct apsind *in, uint8_t *rsp, size_t *rn, uint16_t *rcluster);   /* 1: answered into rsp */
void zcl_rx(const struct apsind *in);
void zdo_device_announce(void);

/* deconz.c: the host */
int  dz_listen(const char *path);
void dz_input(int fd);
void dz_accept(int ls);
void dz_indication(const struct apsind *in);
void dz_green_power(const uint8_t *gpdf, size_t n);
/* gp.c: the router's Green Power proxy */
void gp_rx(const uint8_t *f, size_t n, uint8_t mac_seq, const struct rxinfo *ri);   /* a GPDF heard */
void gp_zcl_rx(const struct apsind *in);   /* the sink's commands (endpoint 242) */
void gp_save(void *file);
void gp_load(uint32_t src_id, unsigned sec, unsigned key_type, unsigned has_key, unsigned mode, unsigned mac_seq, uint32_t fc,
             unsigned group, const uint8_t key[16], unsigned fc_known);
void gp_reset(void);
int  gp_entries(void);
int  gp_commissioning(void);
extern uint8_t mac_rx_seq;             /* the MAC sequence number of the frame being handled */
void dz_mac_poll(struct node *c, const struct rxinfo *ri);   /* a child polled (deCONZ MAC poll indication) */
void dz_state_changed(void);
extern int dz_client;

/* store.c */
int  store_load(void);
void store_save(void);
#endif

/* RFCOMM (Bluetooth's serial port emulation over L2CAP): one multiplexer session and one data channel of ours on it,
 * for the Echo Wall Clock (wallclock.c) and its Alexa Gadgets traffic.  See rfcomm.c. */
#ifndef RFCOMM_H
#define RFCOMM_H
#include <stddef.h>

#define RFC_TXQ 2048                    /* bytes waiting for the peer's credits */

struct rfc {
    void *ctx;
    void (*out)(void *ctx, const unsigned char *pdu, size_t n);  /* one RFCOMM frame, the payload of an L2CAP packet */
    void (*data)(void *ctx, const unsigned char *p, size_t n);   /* bytes that came on our data channel */
    void (*state)(void *ctx, int up);                            /* our data channel opened (1), closed or refused (0) */
    unsigned l2mtu;                     /* what the L2CAP channel takes per packet */
    /* below: rfcomm.c's */
    int initiator;                      /* we sent the multiplexer's SABM (1) or the peer did (0); -1 none yet */
    int mux;                            /* 0 down, 1 our SABM sent, 2 up */
    int chan, dlci, dlc;                /* the peer's server channel we open, its DLCI; 0 none, 1 PN sent, 2 SABM sent, 3 open */
    int cfc, tx_credits, rx_credits;    /* credit based flow control (both sides agreed in PN); what each side may still send */
    unsigned mtu;                       /* agreed frame size of the data channel */
    int msc_sent;                       /* our modem status went out on the open channel */
    long long cmd_at;                   /* when our command awaiting its answer went (0: none) */
    unsigned char txq[RFC_TXQ]; size_t txn;
    unsigned short txlen[32]; int ntx;  /* each write's length: one write never shares a frame with the next */
};

void rfc_init(struct rfc *s);                                   /* callbacks, ctx and l2mtu set before */
void rfc_connect(struct rfc *s, int channel, long long now_ms); /* the peer's server channel: the multiplexer if it is not
                                                                    up, parameters, SABM; s->state says how it went */
void rfc_rx(struct rfc *s, const unsigned char *p, size_t n, long long now_ms);  /* one L2CAP payload from the peer */
int  rfc_send(struct rfc *s, const void *p, size_t n);          /* data on our channel, queued while it waits for credits;
                                                                    -1 = not open or no room */
int  rfc_tick(struct rfc *s, long long now_ms);                  /* -1: a command went unanswered (the caller drops it) */
int  rfc_open(const struct rfc *s);                             /* our data channel is open */
void rfc_close(struct rfc *s);                                  /* DISC of the channel and the multiplexer, state forgotten */
unsigned char rfc_fcs(const unsigned char *p, size_t n);        /* TS 07.10's frame check sequence (for the tests) */
#endif

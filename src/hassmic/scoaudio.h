/* A phone call's audio over Bluetooth (hfp.c): where the synchronous link's audio comes and goes on this model
 * (board.sco, docs/re-hfp.md), as 16 kHz mono s16 on hassmic's side, CVSD (narrowband) on the air.
 *   pcm  the chip's PCM pins on the SoC: ALSA devices of card 0 at 8 kHz, routed by controls (Dot 3, Dot 5, Show 5)
 *   ebc  an integrated core's BTCVSD (/dev/ebc): the air's bitstream, CVSD or mSBC coded here (cvsd.c, msbc.c; Dot 2)
 *   hci  SCO data over HCI as the standard has it (H4 type 3): the PC's tests ($HASSMIC_SCO=hci) */
#ifndef SCOAUDIO_H
#define SCOAUDIO_H
#include <stddef.h>
#include <stdint.h>

int  sco_present(void);                 /* this Echo has a way for a call's audio (any thread) */
const char *sco_kind(void);             /* "pcm", "ebc", "hci", "none" */
/* HFP's codec ids (AT+BAC, +BCS) */
enum { SCO_CVSD = 1, SCO_MSBC = 2 };
/* mSBC on this model: the link's bits are ours to code (ebc, hci), or the chip codes it from 16 kHz on its PCM pins
 * (board.sco_msbc_chip, measured per model; docs/re-hfp.md); else narrowband */
int  sco_wideband(void);
/* The HCI command that accepts a phone's synchronous link for CODEC (a2dp.c), its parameters after the address into p
 * (room for 64): Accept Synchronous Connection Request (0x0429) as before, or for mSBC the chip codes Enhanced Accept
 * (0x043e).  Returns the opcode; *n the parameters' length */
unsigned sco_accept_params(int codec, int esco, unsigned char *p, int *n);
/* After every HCI reset, on the controller thread (a2dp_setup): board.bt_sco_cmd, where the chip's driver does not set
 * its PCM side up.  -1: the controller is gone */
int  sco_chip_setup(void);
/* The link HANDLE (for hci) carries audio now: open the devices and run; sco_stop: close everything.  Both only ask a
 * thread of scoaudio's own and return at once (the devices' opening and the last call's threads' end take up to a
 * second or two); the newest wish wins.  CODEC: SCO_CVSD, or SCO_MSBC where sco_wideband().  rx gets what the phone sends, 16 kHz, from a thread of scoaudio's own, in
 * pieces of any length, and may be called a moment after sco_stop (the caller ties its pieces to the call it was
 * for: hfp.c by call.c's generation).  A start that fails says so in the log, the call then without audio */
int  sco_start(unsigned handle, int codec, void (*rx)(const int16_t *pcm, size_t n));
void sco_stop(void);
/* 16 kHz to the phone, any thread.  Queued; more than SCO_QUEUE_MS waiting drops the oldest (the call's clock and the
 * phone's are not one) */
#define SCO_QUEUE_MS 120
void sco_tx(const int16_t *pcm, size_t n);
/* ble.c's controller thread: an H4 SCO packet (handle, length, data) after its type byte */
void sco_hci_rx(const uint8_t *p, size_t n);
#endif

/* Phone calls over Bluetooth: the Echo as a phone's hands-free unit (HFP 1.7, HF role; issue #233, docs/re-hfp.md).
 * The phone (the audio gateway) connects to it as to any car kit, once paired through the Bluetooth pairing window
 * (a2dp.c) with the feature on; its calls ring here, are answered and ended with the action button, "stop", Home
 * Assistant, and talk through the Echo's speaker and call microphone (call.c, as Drop In's).  Wideband (mSBC) where the
 * model codes the link itself (scoaudio.h sco_wideband), else narrowband (CVSD); the phone's volume keys move the call's
 * volume and the Echo's go to the phone; a second call (waiting, on hold) is taken with hfp_switch. */
#ifndef HFP_H
#define HFP_H
#include <stddef.h>

enum { HFP_NONE, HFP_IDLE, HFP_RINGING, HFP_CALLING, HFP_TALKING, HFP_ON_PHONE, HFP_NSTATES };
extern const char *const hfp_states[HFP_NSTATES];   /* Home Assistant's "Phone call": "no phone", "idle", "ringing",
                                                       "calling", "connected", "on the phone" (keep: automations use them) */
int  hfp_present(void);                 /* this model has a way for a call's audio (scoaudio.c) and the Bluetooth radio */
int  hfp_enable(int set);               /* the feature (setting bluetooth_calls): 1 on, 0 off, -1 reads; off ends the link */
/* What Home Assistant shows: an HFP_* state, the caller (number or name as the phone gives it, "" none) and the
 * phone's name ("" none).  Any thread */
int  hfp_status(char *caller, size_t ccap, char *phone, size_t pcap);
/* The second call while one talks: HFP_WAITING (one waits to be answered) or HFP_HELD (one is on hold), its caller
 * (number or name, "" unknown) into who; 0 none.  Any thread.  Home Assistant's "Phone call waiting" */
enum { HFP_WAITING = 1, HFP_HELD };
int  hfp_second(char *who, size_t n);
/* main.c, whenever the call's volume (0..100) may have moved; any thread, any lock: only stored, the phone told by the
 * controller thread while a call's audio is here */
void hfp_call_volume(int pct);
/* What a person or Home Assistant does; any thread, no lock needed.  1: there was something to do */
int  hfp_button(void);                  /* the action button: answers a ringing call, a waiting one (this one held), else
                                           ends the one shown here */
int  hfp_switch(void);                  /* the waiting or held call taken, this one held (AT+CHLD=2) */
int  hfp_answer(void);                  /* a ringing call: answered, its audio here */
int  hfp_hangup(const char *why);       /* ends the call (rejects a ringing one), wherever its audio is; beside a held
                                           one that one comes back */
int  hfp_here(void);                    /* a call talking on the phone: its audio to the Echo */
int  hfp_dial(const char *number, char *err, size_t n);   /* the phone calls NUMBER (digits, + * #), its audio here.
                                                             0 ok, -1 why in err */
#endif

/* Amazon's front end on Android (checkers): its binder service "audiosignalprocessor", transaction 3 =
 * command(int cmd, byte[] in, byte[] out), reached through the firmware's own /system/bin/service tool (docs/re-checkers.md).
 * The parcel is int32s only, so "service call ... i32 ..." writes it as AIDL would: cmd, in as length + bytes padded to
 * 4, the out size; the reply is exception, status, out as length + bytes, which the tool prints as a hex dump.  Pure C,
 * for the PC's unit test (tests/unit/afe_parcel_test.c).  In every build: dha.c asks checkers' attestation service the
 * same way. */
#ifndef AFE_PARCEL_H
#define AFE_PARCEL_H
#include <stddef.h>
#include <stdint.h>

#define AFE_SVC_MAXARGS 96                  /* argv slots: 4096 bytes would not fit, and none of ours is near */

struct afe_call { char *argv[AFE_SVC_MAXARGS]; char words[AFE_SVC_MAXARGS][12]; int n; };

/* argv for "/system/bin/service call audiosignalprocessor 3 ...": 0, or -1 when in does not fit */
int afe_call_build(struct afe_call *c, int cmd, const void *in, size_t in_len, int out_size);

/* The tool's output: out's bytes into buf (NUL-terminated, cut at cap - 1).  Their count, or -1 when the reply is not
 * there, carries an exception or a status other than 0. */
int afe_call_reply(const char *text, unsigned char *buf, size_t cap);

/* Any other service the same way: svc_call_start ("/system/bin/service call <svc> <code>"; both strings must outlive
 * the call), then the parcel's int32s and C strings.  svc_cstr is Parcel::writeCString, the bytes and their NUL padded
 * to 4, which native interfaces (IAudioPolicyService) read where AIDL's would read a String16.  -1 when it does not
 * fit.  svc_reply_words: the printed reply's words, up to max; their count (0: no parcel, an error, no tool). */
void svc_call_start(struct afe_call *c, const char *svc, const char *code);
int  svc_i32(struct afe_call *c, long v);
int  svc_cstr(struct afe_call *c, const char *s);
int  svc_blob(struct afe_call *c, const void *p, size_t len);   /* AIDL's byte[]: its length, then the bytes padded */
int  svc_reply_words(const char *text, uint32_t *w, size_t max);
#endif

/* Amazon's front end on Android (checkers): its binder service "audiosignalprocessor", transaction 3 =
 * command(int cmd, byte[] in, byte[] out), reached through the firmware's own /system/bin/service tool (docs/re-checkers.md).
 * The parcel is int32s only, so "service call ... i32 ..." writes it as AIDL would: cmd, in as length + bytes padded to
 * 4, the out size; the reply is exception, status, out as length + bytes, which the tool prints as a hex dump.  Pure C,
 * for the PC's unit test (tests/unit/afe_parcel_test.c). */
#ifndef AFE_PARCEL_H
#define AFE_PARCEL_H
#include <stddef.h>

#define AFE_SVC_MAXARGS 96                  /* argv slots: 4096 bytes would not fit, and none of ours is near */

struct afe_call { char *argv[AFE_SVC_MAXARGS]; char words[AFE_SVC_MAXARGS][12]; };

/* argv for "/system/bin/service call audiosignalprocessor 3 ...": 0, or -1 when in does not fit */
int afe_call_build(struct afe_call *c, int cmd, const void *in, size_t in_len, int out_size);

/* The tool's output: out's bytes into buf (NUL-terminated, cut at cap - 1).  Their count, or -1 when the reply is not
 * there, carries an exception or a status other than 0. */
int afe_call_reply(const char *text, unsigned char *buf, size_t cap);
#endif

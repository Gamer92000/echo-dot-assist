/*
 * Client API of Amazon's `mixer` audio daemon (libmixerAPI.so), recovered from
 * donut_puffin NS65741 by disassembly.  Evidence: docs/FINDINGS.md.
 *
 * A handle is a mixer::DataTrans* (shared-memory block ring).  Opening a stream
 * creates the ring and drops a descriptor file in /data/mixer_streams/, which
 * the daemon picks up.  All Get/Release pairs are zero-copy.
 */
#ifndef MIXER_API_H
#define MIXER_API_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void *MixerHandle;

/* Capture stream types accepted by MixerOpenRecCh (strcmp table at 0xdadc..0xdb60). */
#define MIXER_REC_ASR          "micAsr"        /* post-AEC, post-beamformer, 16 kHz mono s16 */
#define MIXER_REC_MULTICH_ASR  "micMultiChAsr"
#define MIXER_REC_RAW          "micRaw"
#define MIXER_REC_HFP          "micHfp"
#define MIXER_REC_LINE_IN      "lineIn"
#define MIXER_REC_BT_A2DP      "btA2dp"

/* Playback stream types (table in /system/bin/mixer). */
#define MIXER_PLAY_MUSIC   "Music"
#define MIXER_PLAY_TTS     "TTS"
#define MIXER_PLAY_EARCON  "Earcon"
#define MIXER_PLAY_ALARM   "Alarm"
#define MIXER_PLAY_VOIP    "Voip"

/* MixerOpenRec(type) == MixerOpenRecCh(type, 1).  Block size is 1600 bytes per channel. */
MixerHandle MixerOpenRec(const char *type);
MixerHandle MixerOpenRecCh(const char *type, unsigned numCh);

/*
 * Returns a pointer to the next block, or NULL.  Blocks up to 1500 ms.
 * *status is an errno-style code (set to -1 before the wait), *numBytes the block length.
 * timestamp may be NULL.
 */
void *MixerGetBufRec(MixerHandle h, int *status, unsigned *numBytes);
void *MixerGetBufRecTimed(MixerHandle h, int *status, unsigned *numBytes, long long *timestamp);
int   MixerReleaseBufRec(MixerHandle h);              /* 0 ok, -1 error */

/*
 * rate: 4000..96000, numCh: 1..50, bits: see "Unsupported num bits" check (16 is safe).
 * MixerOpenPlay(r, c, b, type) == MixerOpenPlayAdv(r, c, b, 0, type).
 */
MixerHandle MixerOpenPlay(unsigned rate, unsigned numCh, unsigned bits, const char *type);
MixerHandle MixerOpenPlayAdv(unsigned rate, unsigned numCh, unsigned bits, unsigned adv, const char *type);

/* Returns a writable block or NULL.  *status preset to EINVAL, *blockBytes = capacity. */
void *MixerGetBufPlay(MixerHandle h, int *status, unsigned *blockBytes);
void *MixerGetBufPlayTimed(MixerHandle h, int *status, unsigned *blockBytes, unsigned long long timestamp);
int   MixerReleaseBufPlay(MixerHandle h, unsigned bytesWritten);   /* clamped to block size */

int MixerDrain(MixerHandle h);
int MixerFlush(MixerHandle h);
int MixerPause(MixerHandle h);
int MixerResume(MixerHandle h);
int MixerClose(MixerHandle h);

unsigned    MixerGetRate(MixerHandle h);
unsigned    MixerGetNumCh(MixerHandle h);
unsigned    MixerGetSampleSizeBits(MixerHandle h);
unsigned    MixerGetBufSize(MixerHandle h);
unsigned    MixerGetNumBytes(MixerHandle h);
unsigned    MixerGetUnderflowMs(MixerHandle h);
const char *MixerGetStreamName(MixerHandle h);

/* mode 0..3 */
int MixerLpmControl(MixerHandle h, int mode);

#ifdef __cplusplus
}
#endif
#endif

/* Speaker gain (issue #19): an analog gain stage in front of the speaker amplifier (board.speaker_gain), raised from the
 * settings page for an Echo that is too quiet at full volume.  Absent where the board names none or card 0 lacks it. */
#ifndef SPKGAIN_H
#define SPKGAIN_H

#define SPKGAIN_MAX 12                  /* dB over stock the page offers; see spkgain.c for why no more */

int  spkgain_present(void);             /* probed at the first call */
int  spkgain_get(void);                 /* dB over stock, 0..SPKGAIN_MAX */
void spkgain_set(int db);               /* applied at once, and again whenever the audio HAL puts stock back */
#endif

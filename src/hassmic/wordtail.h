/* Where the replay after a wake word starts (issue #72; main.c stream_ring).
 *
 * Pryon's end (the result's endSampleIndex, its kwSampleDetectionIndex) is where it was sure, not where the voice
 * stopped.  A user's Home Assistant debug recordings (2026-10-27, Echo Dot 2 in a kitchen; 50 ms frames, the gain
 * taken back out) after a keyword of -40.3 dBFS: -40 -34 -36 -38 -43 -48 -51 -54 dBFS, then the room until the
 * command at 0.9 s: the last vowel went on for 200 ms after the end, louder than the keyword, and decayed for 150 ms
 * more.  Another: one quiet frame (16 dB under), then 150 ms as loud, then the decay: the "ks" of "Ale-ks-a", Pryon
 * sure before the last syllable.  The gain starts at the keyword's level, so the tail reached speech-to-text as a word
 * of its own ("The", "Faint"; "Ja." for a child's "Alexaaa", forum 2026-10-19) and broke exact sentence matches.
 *
 * What ends the word is a pause: 10 ms frames (as 30 ms means, so the room's flicker does not count) 9 dB under the
 * word's loudest so far (the keyword's rms, or the tail's peak where louder) for 100 ms.  A consonant inside the word
 * is shorter (the "ks" above: one 50 ms frame).  The replay starts in that pause where it is 12 dB under, else at its
 * lowest.  The first version (a4fe0bd) stopped at the first 12 dB dip or 3 dB rise: on these recordings at 60 ms
 * (inside the word) and at 0 ms (the vowel rising after the end).
 *
 * No pause within WORDTAIL_FRAMES (a command spoken straight on, a room as loud as the word, a vowel longer than that)
 * says nothing about where the word ends: a dip there may be inside the command ("turn | on the light", when the word
 * ended before Pryon was sure), and a command word cut makes the assistant do the wrong thing, where a syllable of the
 * word left in only adds a stray word.  So then the least: the lowest point of the first dip if it starts within
 * WORDTAIL_EARLY, never later than the first version would have cut (what the releases since 2026-10-19 did), else
 * nothing (as the ring did from ad224f5 on).  The replay waits for the decision: up to 500 ms at the start of the
 * stream, caught up at once from the ring. */
#ifndef WORDTAIL_H
#define WORDTAIL_H

#define WORDTAIL_FRAME 160              /* samples: 10 ms at 16 kHz */
#define WORDTAIL_FRAMES 50              /* looked at for at most 500 ms after the end */
#define WORDTAIL_EARLY 15               /* frames: without a pause, only a dip starting this early may end the word */

/* db: level of each 10 ms frame from Pryon's end on (dBFS), n of them so far; keyword_db: the keyword's rms (dBFS).
 * Returns the frame the replay starts at, or -1 while more frames are needed (never once n >= WORDTAIL_FRAMES).
 * why: what decided, for the log. */
int wordtail_cut(const float *db, int n, float keyword_db, const char **why);
#endif

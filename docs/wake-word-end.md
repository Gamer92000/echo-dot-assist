# Where the request starts after the wake word

Issues #72, #79 and #80, PR #71. After a wake word the Echo streams the request to Home Assistant from its ring
buffer, starting where the word ends. Start too early and the word's tail goes along ("-xa", which speech to text
writes as a word); start too late and the command loses its first sound or its first word. And Home Assistant's
voice activity detection must hear the command at all. This page is how that is measured and what came out
(2026-10-29). The tables in full, generated: [`wake-word-end/corpus.md`](wake-word-end/corpus.md) and
[`wake-word-end/vad.md`](wake-word-end/vad.md), each request in `wake-word-end/*.jsonl`. Earlier measurements (220
requests of two cloned voices, the release's rule, rejected approaches): `PLAN.md`, the entries on the wake word's
end.

## What the Echo does

1. **The aligner** (`src/hassmic/wakealign.c`, `wakealign.h`): the Montreal Forced Aligner's first pass on the Echo,
   cut down to the wake word, finds where its phones end in the 2 s around Amazon's "heard" (Pryon's end), in the
   language spoken here (setting `speech_language`, else the time zone). Speaker adaptation learns the household's
   voices from the fifth request on.
2. **A pause after the word: its middle.** Where the aligned path runs into silence for 50 ms or more, the request
   starts in the middle of it; else, where the level finds a pause within 200 ms after the aligned end (80 ms or more
   within 6 dB of the room's floor), in the middle of that (`pause_after`).
3. **No answer near Pryon's end** (no model for the word, a misalignment): the release's rule, `wordtail.c`.
4. **800 ms of silence** open every request Home Assistant starts at speech to text (`main.c` `lead_in`, issue #80).

## How it is measured

- **Speech**: the corpus in `testdata/corpus` (its README): 30 voices designed by OmniVoice (children to elderly,
  whispering, accents), German, English, French, Spanish, every clip checked by speech to text, its words timed by
  MFA with the whole text known (the ground truth, from the dry clip). 4586 wake words: said in one breath with a
  command (2776), with a pause of 0.23-0.68 s put between the word and the command (1427, "joined"), the word alone
  (210), "<wake word>, stop" (173).
- **Room**: each clip with a 50 ms reverb, speech at -30 dBFS, noise at -58 dBFS, 1 s before and after.
- **Wake word engine**: stock Pryon under qemu (`build/donut/pryon_test` via `tools/qrun.sh`): the de-DE models, and
  for English the firmware's Alexa and computer-/echo-en-US (Amazon and Ziggy have no English model here; French and
  Spanish none at all, so their Amazon words go through the de-DE models, marked "proxy"). microWakeWord's three
  words through pymicro-wakeword. Pryon found 2899 of 3267 clips, microWakeWord 1687 of 2388.
- **Households**: each voice is one, its wake words in a fixed random order, the adaptation learning as it goes, as
  `main.c` `align_thread` does (`tools/wakealign/eval.c`).
- **Right**: the request starts from 40 ms before the wake word's end to 40 ms after the command's start; in a pause,
  anywhere in it; after the word alone, anywhere after its end.

**A pitfall, for whoever measures this next.** Pryon skips audio once its backlog passes ~7 s, and its sample
positions go wrong with it. Clips back to back in one stream, at `-x 4` with 24 qemu at once, had it skip up to 5 s:
only 5 of 30 ends were where a real-time run has them, and it found 36 % of the clips instead of 89 %. Even real time
skipped with 40 clips in one stream. Hence one clip a run at `-x 2`, and every result is checked for
`skipped_audio_backlog_since_last_ww_msec`: a run that skipped is made again in real time.

## Results

Right, by kind (from [`corpus.md`](wake-word-end/corpus.md)):

| | n | Pryon's end | release | aligner alone | adapted | the Echo |
|---|---|---|---|---|---|---|
| said in one breath | 2776 | 35 % | 30 % | 46 % | 64 % | **64 %** |
| a pause after the word | 1427 | 52 % | 77 % | 73 % | 90 % | **90 %** |
| the word alone | 210 | 55 % | 78 % | 56 % | 84 % | **85 %** |
| "<wake word>, stop" | 173 | 45 % | 43 % | 43 % | 80 % | **80 %** |
| **all** | 4586 | 42 % | 47 % | 55 % | 74 % | **74 %** |

By language the Echo is right in 75 % (de), 73 % (en), 79 % (fr), 79 % (es), 73 and 65 % through the proxies; by wake
word in 67-82 %. Without speaker adaptation 55 %: on synthetic voices in one room every household is very uniform,
so expect less from it at home (on the two cloned voices of the earlier measurements it gave +2 points).

How far off, all 4586: of the command cut (late) or of the wake word left in (early), ms:

| | cut >300 | 200-300 | 100-200 | 40-100 | **right** | tail 40-100 | 100-200 | >200 |
|---|---|---|---|---|---|---|---|---|
| Pryon's end | 0.7 % | 3.0 % | 9.6 % | 9.5 % | 41.8 % | 16.2 % | 13.0 % | 6.3 % |
| release | 11.1 % | 10.1 % | 8.4 % | 6.3 % | 47.1 % | 9.9 % | 5.3 % | 1.9 % |
| the Echo | 0.5 % | 1.0 % | 3.6 % | 11.9 % | 73.7 % | 4.1 % | 3.5 % | 1.7 % |

Roughly: 40-100 ms cut is part of the first sound, 100-200 ms the first sound or syllable, over 200 ms a short word
("an", "aus") gone; a tail of 100 ms or more is a syllable ("-xa"). The release lost 200 ms or more of the command in
21 % of the requests (it waits for a pause and, said in one breath, finds the one after the command's first word:
issue #79); the Echo in 1.5 %. The Echo's commonest miss is small, 40-100 ms of the first sound, nearly all in one
breath, where it starts 30 ms late in the median (the truth is the dry clip's, the aligner hears the reverb: not
corrected for, a real room would tell). With a pause it never cuts into the command, but where it misses (10 %) it
leaves 180 ms of the word in, in the median: the aligner ended far too early and no pause turned up within 200 ms.

### The pause rule, and what else was tried

| variant | all | pause | word alone | one breath | stop | starts > 300 ms into the command |
|---|---|---|---|---|---|---|
| without either pause rule | 63 % | 63 % | 56 % | 62 % | 79 % | - |
| pause search 150 ms | 71 % | 84 % | 72 % | 64 % | 79 % | 22 |
| **pause search 200 ms (chosen)** | 74 % | 90 % | 85 % | 64 % | 80 % | 23 |
| pause search 300 ms | 76 % | 97 % | 93 % | 63 % | 80 % | 45 |
| 300 ms, stopped by a rising level (6 dB) | 74 % | 92 % | 87 % | 64 % | 80 % | |
| 300 ms, stopped by a rising level (10 dB) | 75 % | 94 % | 90 % | 64 % | 80 % | |
| 300 ms, at most 6 frames at speech level before it | 71 % | 84 % | 77 % | 64 % | 79 % | 25 |
| 300 ms, at most 10 frames | 73 % | 89 % | 84 % | 64 % | 80 % | 26 |
| 300 ms, at most 15 frames | 75 % | 93 % | 90 % | 64 % | 80 % | 30 |

Searching 300 ms ahead scores best but finds the silence after a one-word command and takes it for the pause after
the wake word: "Alexa, an", "Echo on", "Hey Jarvis off" lose the whole command (all 13 requests it got wrong where
200 ms got them right were such, 330-500 ms into the command). Neither a guard on a rising level nor one on the time
at speech level before the pause told them apart, so the search stops at 200 ms. The pause is found at the room's
floor, not under the word's level: a command's "s" or "sch" lies 10-20 dB under its vowels, and a threshold relative
to the word took those for a pause ("<wake word>, stop" 83 -> 58 % in the first run, the one with Pryon's skips). (Variants: `corpus_eval.py --cflags
-DPAUSE_LOOK=…`, wakealign.c's constants.)

## Issue #80: Home Assistant's VAD

Home Assistant's own classes (2026.10.1: `MicroVadSpeexEnhancer` as `default_pipeline.py` makes it for a satellite,
its VAD on the audio before speex, then `VoiceCommandSegmenter`), on each command alone of the corpus as it would
follow a right cut, in a room (speech at -26 dBFS, the room at -55 dBFS, 3 s of it after the command). Heard: the
segmenter went into the command; ended: it found its end within those 3 s, else Home Assistant waits for its 15 s
timeout ([`vad.md`](wake-word-end/vad.md)):

| | n | ended, no silence | ended, 800 ms of silence first |
|---|---|---|---|
| commands under 0.4 s | 23 | 17 % | 57 % |
| 0.4-0.8 s | 199 | 63 % | 93 % |
| 0.8 s and more | 551 | 99 % | 100 % |
| **all** | 773 | 87 % | **97 %** |
| with the wake word in the request (`wake_word_in_request`), commands under 0.6 s | 335 | 100 % | 100 % |

Its VAD is a new one per request and blind for its first 760 ms; the silence covers that. What is left are one-word
commands ("on", "off", "an") shorter than the 0.3 s of speech the segmenter wants before a command starts: no silence
can help those, the wake word in the request does (334 of 335), and so would "End of request heard on the Echo"
(setting `end_of_request`), which ends the request on the Echo itself.

## What this does not tell

- **The Echo's own audio**: the front end (echo cancelling, beamforming, its gain) is not simulated, and the room is
  a synthetic one. Real recordings from Echos (issue #79's, #72's) are the next test.
- **Real people**: the voices are synthetic; the speaker adaptation's gain is likely smaller with real households.
- **Time on the device**: under qemu-arm 0.2 s a request with the adaptation's update, in its own thread.
- **French and Spanish Pryon models**, and English ones for Amazon and Ziggy: not at hand (proxies above);
  Japanese: not measured.
- **Speech to text**: "right" here is acoustic (where the words are), not what an STT engine makes of a 50 ms cut.

## Making it again

```sh
git lfs pull --include "testdata/corpus/**"                         # the corpus (or tools/speechcorpus/run.sh)
.venv/bin/python tools/wakealign/corpus_eval.py --publish docs/wake-word-end
build/ha-venv/bin/python tools/vad_leadin.py --publish docs/wake-word-end
```

`corpus_eval.py` needs `build/donut/pryon_test`, the firmware's rootfs and the de-DE / en-US Pryon models in
`device-logs/models/` (`scripts/artifacts.sh` or the settings page's Download from Amazon); about 10 minutes with 24
jobs. `build/ha-venv` is made by `tests/ha_real_dropin.py --setup`. Both stamp their tables with the commit they ran
at.

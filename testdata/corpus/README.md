# Speech corpus

Wake words and commands said by 30 voices that OmniVoice designed from a description (gender, age, pitch, whisper,
English accents): German, English, French and Spanish, six voices in a second language too. For measurements and test
fixtures with speech in them, such as where the wake word ends (`src/hassmic/wakealign.h`). Made by
`tools/speechcorpus/run.sh` (DEVELOPMENT.md "Speech corpus"), 2026-10-29.

The clips are in Git LFS: a clone has pointers only. To get them:

```sh
git lfs pull --include "testdata/corpus/**"
```

- `<voice>/<language>-<wake word>-<n>.wav`: 16 kHz mono 16-bit, the Echo's rate. `joined-…` are a wake word alone and a
  command alone of the same voice put together with a pause of known length; `<language>-bare-<n>` a command alone.
- `voices/<voice>.wav`: the sentence each voice was designed with; every clip of it is cloned from that, so a voice
  is one speaker throughout. `voices/voices.json`: its description and what speech to text heard in it.
- `manifest.jsonl`: one line a clip. `text`, `voice`, `voice_description`, `language`, `wake`, `command`, `kind`
  (`command`, `joined`, `alone`, `stop`, `bare`), `comma`; `heard` (canary-1b-v2) and `ok` (it says its text:
  `tools/speechcorpus/verify.py`, `why` when not); `words` (`[start, end, word]` in seconds, by the Montreal Forced
  Aligner with the whole text), `wake_end`, `command_start`, `gap`. Take the ones with `ok`: 7110 of 7393.

## Licence

The audio was made with OmniVoice (k2-fsa), whose code is Apache 2.0 and whose pretrained model is "licensed under
the CC-BY-NC due to constraints from its training data (e.g., Emilia)" (its model card). Whether audio a model makes
is bound by the model's licence is not settled; these clips are handled as if they were: **CC BY-NC 4.0**, not under
the project's MIT licence. That holds for `testdata/alexa_omnivoice.raw` too, which is cut from one of them. The model's terms forbid using it for voice
cloning without consent, impersonation or fraud; the voices here are designed, nobody's.

The word times come from the Montreal Forced Aligner's models (CC BY 4.0, `wakealign/README.md`), the transcripts from
NVIDIA's canary-1b-v2.

# Wake word aligners

Where the wake word ends, for `src/hassmic/wakealign.c` (issues #72, #79). `<word>-<locale>.wa` is one wake word in
one spoken language: its pronunciations as a graph of phones, and which Gaussian mixtures (GMMs) of its acoustic model
those phones use. The GMMs are in `<acoustic model>.was` (`german_mfa.was`, `english_mfa.was`, ...), once for every
word of that model, cut out of one of the Montreal Forced Aligner's acoustic models. `main.c` `align_resolve` picks a
word file by wake word and the language spoken; en-CA, en-AU and fr-CA use en-US's, en-GB's and fr-FR's (the same
models and dictionary). They ship next to hassmic in every bundle (`scripts/bundle.sh`): 2.5 MB.

Made by `tools/wakealign/build.sh wakealign [ID ...]` (MFA 3.4.3 from conda-forge; how in the script's header).

## Licence

Derived from the [Montreal Forced Aligner's pretrained models](https://mfa-models.readthedocs.io) by Michael
McAuliffe et al., licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). These files are under the
same licence (not the project's MIT licence).

Used:

| Locales | Acoustic model | Dictionary | G2P model |
|---|---|---|---|
| de-DE | german_mfa 3.0.0 | german_mfa | german_mfa |
| en-US (en-CA) | english_mfa 3.1.0 | english_us_mfa | english_us_mfa |
| en-GB (en-AU) | english_mfa 3.1.0 | english_uk_mfa | english_uk_mfa |
| en-IN | english_mfa 3.1.0 | english_india_mfa | english_india_mfa |
| fr-FR (fr-CA) | french_mfa 3.0.0 | french_mfa | french_mfa |
| es-ES | spanish_mfa 3.2.0 | spanish_spain_mfa | spanish_spain_mfa |
| es-MX | spanish_mfa 3.2.0 | spanish_latin_america_mfa | spanish_latin_america_mfa |
| ja-JP | japanese_mfa 3.0 | japanese_mfa | japanese_katakana_mfa |

`okay_nabu`, `hey_jarvis` and `hey_mycroft` (microWakeWord's) are in en-US and de-DE; the firmware's own "alexa" uses
`alexa-en-US.wa`.

Changes made: only the Gaussian mixtures, decision tree leaves, transitions and LDA the wake words' phones (in their
contexts), silence and `spn` need are kept; each mixture is cut to its 16 heaviest Gaussians (weights renormalised),
means and variances stored as float16, once per acoustic model; the pronunciations (up to three from the dictionary,
else the G2P model's two best) are stored as phone sequences.

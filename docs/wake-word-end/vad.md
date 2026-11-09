# Home Assistant's VAD on requests that open on the command: the speech corpus

Made by `build/ha-venv/bin/python tools/vad_leadin.py` at e3253ba, 2026-10-29 (Home Assistant 2026.10.1). What it is: [`../wake-word-end.md`](../wake-word-end.md). Heard: HA's VoiceCommandSegmenter went into the command; ended: it also found the command's end within the 3 s of room after it (else HA waits for its 15 s timeout).

## The command alone opening the request, by its length

|  | n | heard, no silence | ended, no silence | heard, 800 ms | ended, 800 ms |
|---|---|---|---|---|---|
| 0.4-0.8 s | 199 | 63 % | 63 % | 93 % | 93 % |
| 0.8 s and more | 551 | 99 % | 99 % | 100 % | 100 % |
| under 0.4 s | 23 | 17 % | 17 % | 57 % | 57 % |

## The command alone opening the request, by language

|  | n | heard, no silence | ended, no silence | heard, 800 ms | ended, 800 ms |
|---|---|---|---|---|---|
| de | 250 | 90 % | 90 % | 98 % | 98 % |
| en | 364 | 86 % | 86 % | 96 % | 96 % |
| es | 84 | 76 % | 76 % | 96 % | 96 % |
| fr | 75 | 96 % | 96 % | 100 % | 100 % |

## The command alone opening the request, all

|  | n | heard, no silence | ended, no silence | heard, 800 ms | ended, 800 ms |
|---|---|---|---|---|---|
| all | 773 | 87 % | 87 % | 97 % | 97 % |

## With the wake word in the request (wake_word_in_request), commands under 0.6 s

|  | n | heard, no silence | ended, no silence | heard, 800 ms | ended, 800 ms |
|---|---|---|---|---|---|
| all | 335 | 100 % | 100 % | 100 % | 100 % |

## Not ended even with the silence

de-f-elderly: "off", de-f-elderly: "pause", de-f-teen: "gute Nacht", de-f-whisper: "abbrechen", de-f-whisper: "an", de-m-child: "an", de-m-elderly: "lauter", de-m-middle: "on", de-m-whisper: "lauter", de-m-young: "off", en-au-f-middle: "off", en-gb-f-elderly: "off", en-gb-f-elderly: "on", en-gb-m-young: "off", en-gb-m-young: "on", en-ja-m-young: "off", en-ja-m-young: "on", en-us-f-whisper: "off", en-us-m-elderly: "off", en-us-m-elderly: "on", en-zh-f-young: "off", en-zh-f-young: "on", es-f-middle-high: "enciende la luz", es-f-middle-high: "está cerrada la puerta", es-f-young: "más alto"

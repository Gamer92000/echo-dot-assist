# Where the request starts after the wake word: the speech corpus

Made by `tools/wakealign/corpus_eval.py` at b18c489 with changes, 2026-10-30; 4586 wake words of 6337 clips. What it is and how to read it: [`../wake-word-end.md`](../wake-word-end.md). Right: from 40 ms before the wake word's end to 40 ms after the command's start (in a pause anywhere in it; the word alone: anywhere after its end). Columns: Pryon's end as it is, the release's rule (wordtail.c), the aligner alone (else the rule), with the household's adaptation, and the Echo (the aligner where it answers, else the rule).

## Right in all

|  | n | pryon | release | aligner | adapted | echo | Echo: tail | Echo: cut | Pryon's end - word's, ms (median) |
|---|---|---|---|---|---|---|---|---|---|
| all | 4586 | 42 % | 47 % | 55 % | 74 % | 74 % | 426 | 782 | +0 |

## Right by kind (joined: a pause of 0.23-0.68 s put in)

|  | n | pryon | release | aligner | adapted | echo | Echo: tail | Echo: cut | Pryon's end - word's, ms (median) |
|---|---|---|---|---|---|---|---|---|---|
| alone | 210 | 55 % | 78 % | 56 % | 84 % | 85 % | 32 | 0 | -30 |
| command | 2776 | 35 % | 30 % | 46 % | 64 % | 64 % | 244 | 761 | +20 |
| joined | 1427 | 52 % | 77 % | 73 % | 90 % | 90 % | 136 | 0 | -40 |
| stop | 173 | 45 % | 43 % | 43 % | 80 % | 80 % | 14 | 21 | +0 |

## Right by language (proxy: Amazon's words in fr/es through de-DE Pryon)

|  | n | pryon | release | aligner | adapted | echo | Echo: tail | Echo: cut | Pryon's end - word's, ms (median) |
|---|---|---|---|---|---|---|---|---|---|
| de | 1661 | 35 % | 45 % | 56 % | 75 % | 75 % | 166 | 254 | -30 |
| en | 1866 | 39 % | 43 % | 49 % | 73 % | 73 % | 149 | 346 | +0 |
| es | 124 | 48 % | 47 % | 54 % | 78 % | 79 % | 6 | 20 | +120 |
| es proxy | 365 | 58 % | 60 % | 58 % | 65 % | 65 % | 44 | 82 | +20 |
| fr | 190 | 50 % | 51 % | 62 % | 79 % | 79 % | 9 | 30 | +55 |
| fr proxy | 380 | 62 % | 64 % | 68 % | 73 % | 73 % | 52 | 50 | +10 |

## Right by wake word

|  | n | pryon | release | aligner | adapted | echo | Echo: tail | Echo: cut | Pryon's end - word's, ms (median) |
|---|---|---|---|---|---|---|---|---|---|
| alexa | 764 | 39 % | 46 % | 45 % | 67 % | 67 % | 130 | 119 | -30 |
| amazon | 369 | 50 % | 53 % | 64 % | 76 % | 76 % | 6 | 82 | +0 |
| computer | 748 | 32 % | 46 % | 41 % | 67 % | 67 % | 121 | 126 | -60 |
| echo | 684 | 44 % | 51 % | 59 % | 74 % | 74 % | 52 | 127 | +40 |
| hey_jarvis | 385 | 54 % | 52 % | 72 % | 82 % | 82 % | 4 | 67 | +120 |
| hey_mycroft | 758 | 27 % | 33 % | 51 % | 78 % | 78 % | 45 | 121 | -90 |
| okay_nabu | 544 | 53 % | 51 % | 62 % | 77 % | 77 % | 46 | 80 | +150 |
| ziggy | 334 | 56 % | 58 % | 65 % | 75 % | 75 % | 22 | 60 | +20 |

## Right by voice

|  | n | pryon | release | aligner | adapted | echo | Echo: tail | Echo: cut | Pryon's end - word's, ms (median) |
|---|---|---|---|---|---|---|---|---|---|
| de-f-elderly | 213 | 40 % | 46 % | 52 % | 69 % | 69 % | 47 | 18 | -40 |
| de-f-middle-high | 175 | 41 % | 59 % | 38 % | 59 % | 59 % | 28 | 43 | -30 |
| de-f-teen | 156 | 34 % | 59 % | 52 % | 73 % | 73 % | 22 | 20 | -60 |
| de-f-whisper | 143 | 34 % | 38 % | 61 % | 71 % | 71 % | 16 | 25 | +40 |
| de-f-young | 204 | 36 % | 40 % | 59 % | 78 % | 78 % | 14 | 31 | -10 |
| de-m-child | 155 | 39 % | 53 % | 46 % | 81 % | 81 % | 13 | 16 | -20 |
| de-m-elderly | 190 | 28 % | 37 % | 62 % | 79 % | 79 % | 15 | 24 | -70 |
| de-m-middle | 195 | 34 % | 33 % | 68 % | 75 % | 75 % | 8 | 40 | -30 |
| de-m-whisper | 142 | 38 % | 55 % | 65 % | 80 % | 80 % | 4 | 24 | +20 |
| de-m-young | 222 | 34 % | 44 % | 63 % | 77 % | 77 % | 18 | 32 | -20 |
| en-au-f-middle | 103 | 30 % | 37 % | 40 % | 65 % | 65 % | 21 | 15 | +10 |
| en-ca-f-teen | 141 | 35 % | 38 % | 34 % | 72 % | 72 % | 8 | 31 | +10 |
| en-gb-f-elderly | 147 | 32 % | 31 % | 43 % | 67 % | 67 % | 30 | 18 | -30 |
| en-gb-m-young | 136 | 46 % | 51 % | 49 % | 75 % | 75 % | 10 | 24 | +35 |
| en-in-m-young | 137 | 56 % | 62 % | 53 % | 69 % | 69 % | 10 | 32 | +10 |
| en-ja-m-young | 148 | 31 % | 40 % | 64 % | 86 % | 86 % | 1 | 20 | -40 |
| en-ru-m-middle | 145 | 49 % | 41 % | 59 % | 77 % | 77 % | 3 | 30 | +10 |
| en-us-f-whisper | 125 | 27 % | 53 % | 27 % | 69 % | 69 % | 19 | 20 | -60 |
| en-us-f-young | 182 | 38 % | 37 % | 38 % | 72 % | 72 % | 11 | 40 | +10 |
| en-us-m-elderly | 147 | 30 % | 29 % | 56 % | 75 % | 75 % | 3 | 34 | +0 |
| en-us-m-middle | 148 | 39 % | 32 % | 60 % | 75 % | 75 % | 3 | 34 | +20 |
| en-zh-f-young | 137 | 47 % | 50 % | 46 % | 75 % | 75 % | 9 | 25 | +0 |
| es-f-middle-high | 72 | 68 % | 65 % | 58 % | 67 % | 68 % | 16 | 7 | +90 |
| es-f-young | 132 | 54 % | 51 % | 56 % | 73 % | 73 % | 12 | 24 | +40 |
| es-m-elderly | 149 | 52 % | 58 % | 60 % | 66 % | 66 % | 7 | 44 | +20 |
| es-m-middle | 136 | 52 % | 56 % | 54 % | 69 % | 69 % | 15 | 27 | +10 |
| fr-f-elderly | 127 | 58 % | 52 % | 67 % | 76 % | 76 % | 8 | 22 | +50 |
| fr-f-young | 146 | 65 % | 64 % | 62 % | 77 % | 77 % | 15 | 18 | +30 |
| fr-m-middle | 186 | 53 % | 62 % | 71 % | 76 % | 76 % | 19 | 25 | +30 |
| fr-m-teen | 147 | 59 % | 61 % | 64 % | 73 % | 73 % | 21 | 19 | +10 |

## How far off, all (4586): the command cut or the wake word's tail left in, ms

|  | cut >300 | cut 200-300 | cut 100-200 | cut 40-100 | right | tail 40-100 | tail 100-200 | tail >200 |
|---|---|---|---|---|---|---|---|---|
| pryon | 0.7 % | 3.0 % | 9.6 % | 9.5 % | 41.8 % | 16.2 % | 13.0 % | 6.3 % |
| release | 11.1 % | 10.1 % | 8.4 % | 6.3 % | 47.1 % | 9.9 % | 5.3 % | 1.9 % |
| aligner | 2.4 % | 3.2 % | 5.9 % | 10.8 % | 57.4 % | 5.3 % | 7.0 % | 8.1 % |
| adapted | 0.5 % | 1.0 % | 3.6 % | 11.9 % | 73.7 % | 4.1 % | 3.5 % | 1.7 % |
| echo | 0.5 % | 1.0 % | 3.6 % | 12.0 % | 73.7 % | 4.1 % | 3.5 % | 1.7 % |

## How far off, command (2776): the command cut or the wake word's tail left in, ms

|  | cut >300 | cut 200-300 | cut 100-200 | cut 40-100 | right | tail 40-100 | tail 100-200 | tail >200 |
|---|---|---|---|---|---|---|---|---|
| pryon | 1.0 % | 4.7 % | 15.5 % | 15.0 % | 35.2 % | 17.0 % | 10.4 % | 1.2 % |
| release | 16.8 % | 16.1 % | 13.7 % | 9.8 % | 29.5 % | 8.9 % | 4.6 % | 0.5 % |
| aligner | 4.0 % | 5.1 % | 8.9 % | 16.4 % | 45.8 % | 7.8 % | 6.7 % | 5.3 % |
| adapted | 0.8 % | 1.5 % | 5.7 % | 19.4 % | 63.8 % | 5.5 % | 2.3 % | 0.9 % |
| echo | 0.8 % | 1.5 % | 5.7 % | 19.4 % | 63.8 % | 5.5 % | 2.3 % | 0.9 % |

## How far off, joined (1427): the command cut or the wake word's tail left in, ms

|  | cut >300 | cut 200-300 | cut 100-200 | cut 40-100 | right | tail 40-100 | tail 100-200 | tail >200 |
|---|---|---|---|---|---|---|---|---|
| pryon | 0.0 % | 0.0 % | 0.0 % | 0.1 % | 52.3 % | 14.7 % | 17.5 % | 15.4 % |
| release | 0.0 % | 0.2 % | 0.0 % | 0.1 % | 77.4 % | 11.7 % | 6.2 % | 4.3 % |
| aligner | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 79.7 % | 0.6 % | 6.9 % | 12.8 % |
| adapted | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 90.5 % | 1.3 % | 5.3 % | 3.0 % |
| echo | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 90.5 % | 1.3 % | 5.3 % | 3.0 % |

## How far off, alone (210): the command cut or the wake word's tail left in, ms

|  | cut >300 | cut 200-300 | cut 100-200 | cut 40-100 | right | tail 40-100 | tail 100-200 | tail >200 |
|---|---|---|---|---|---|---|---|---|
| pryon | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 54.8 % | 13.3 % | 16.7 % | 15.2 % |
| release | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 77.6 % | 11.0 % | 6.2 % | 5.2 % |
| aligner | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 71.0 % | 2.9 % | 11.4 % | 14.8 % |
| adapted | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 84.8 % | 3.3 % | 9.0 % | 2.9 % |
| echo | 0.0 % | 0.0 % | 0.0 % | 0.0 % | 84.8 % | 3.3 % | 9.0 % | 2.9 % |

## How far off, stop (173): the command cut or the wake word's tail left in, ms

|  | cut >300 | cut 200-300 | cut 100-200 | cut 40-100 | right | tail 40-100 | tail 100-200 | tail >200 |
|---|---|---|---|---|---|---|---|---|
| pryon | 0.6 % | 4.6 % | 5.8 % | 11.0 % | 44.5 % | 19.7 % | 12.1 % | 1.7 % |
| release | 23.1 % | 7.5 % | 1.7 % | 8.1 % | 42.8 % | 9.2 % | 7.5 % | 0.0 % |
| aligner | 0.6 % | 2.3 % | 13.9 % | 22.0 % | 42.8 % | 6.4 % | 5.8 % | 6.4 % |
| adapted | 0.6 % | 1.7 % | 4.0 % | 5.8 % | 79.8 % | 4.6 % | 1.7 % | 1.7 % |
| echo | 0.6 % | 1.7 % | 4.0 % | 5.8 % | 79.8 % | 4.6 % | 1.7 % | 1.7 % |

## Where the starts lie against the wake word's end, ms (percentiles)

|  | n | 10th | 25th | 50th | 75th | 90th |
|---|---|---|---|---|---|---|
| command pryon | 2776 | -110 | -60 | +20 | +100 | +180 |
| command release | 2776 | -70 | +0 | +100 | +250 | +390 |
| command aligner | 2716 | -130 | -20 | +30 | +100 | +190 |
| command adapted | 2767 | -40 | +0 | +30 | +70 | +130 |
| command echo | 2776 | -40 | +0 | +30 | +70 | +130 |
| joined pryon | 1427 | -230 | -140 | -40 | +60 | +160 |
| joined release | 1427 | -130 | -40 | +10 | +70 | +160 |
| joined aligner | 1337 | -330 | +70 | +170 | +230 | +280 |
| joined adapted | 1426 | +30 | +150 | +190 | +250 | +290 |
| joined echo | 1427 | +30 | +150 | +190 | +250 | +290 |
| alone pryon | 210 | -220 | -137 | -30 | +60 | +160 |
| alone release | 210 | -130 | -40 | +10 | +90 | +180 |
| alone aligner | 178 | -370 | -150 | +150 | +270 | +330 |
| alone adapted | 209 | -122 | +80 | +200 | +280 | +330 |
| alone echo | 210 | -120 | +80 | +200 | +280 | +330 |
| stop pryon | 173 | -120 | -70 | +0 | +80 | +164 |
| stop release | 173 | -86 | -20 | +50 | +320 | +410 |
| stop aligner | 173 | -120 | -10 | +90 | +140 | +190 |
| stop adapted | 173 | -30 | -10 | +30 | +100 | +168 |
| stop echo | 173 | -30 | -10 | +30 | +100 | +168 |

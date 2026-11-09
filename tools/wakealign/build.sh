#!/bin/sh
# The wake word aligners (src/hassmic/wakealign.h) from the Montreal Forced Aligner's pretrained models: a
# <word>-<locale>.wa per wake word and language spoken, and the GMMs of each acoustic model once, in <model>.was beside
# them (walign.py write_wa), next to hassmic on the Echo (scripts/bundle.sh takes wakealign/*.wa *.was, in git: run
# with OUTDIR wakealign to remake them; with IDs only those, added to the .was files).
#
#   MFA_RUN="<runs a command in MFA's env>" MFA_ROOT_DIR=<MFA's root> tools/wakealign/build.sh OUTDIR [ID ...]
#
# e.g. MFA_RUN="micromamba run -n mfa" (MFA 3.4.3 from conda-forge); the models it needs are downloaded into
# MFA_ROOT_DIR (`mfa model download`).  MFA's models are CC BY 4.0 (mfa-models), credited in wakealign/README.md.
# Ids: Amazon's wake words (davs.c, web/app.js WW_KEYS) in every locale the page offers (LOCALES) where MFA has an
# acoustic model (the firmware's own "alexa" is alexa-en-US), and microWakeWord's standard models (English, and
# German speakers').  en-CA, en-AU, fr-CA would be byte for byte en-US's, en-GB's, fr-FR's (the same models and
# dictionary): main.c align_resolve takes those.  Italian (only MFA's small Common Voice model, and no spelling model)
# and Hindi, Arabic (none) are left out.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:?usage: tools/wakealign/build.sh OUTDIR [ID ...]}; shift
: "${MFA_RUN:?MFA_RUN: how to run a command in the Montreal Forced Aligner env}"
: "${MFA_ROOT_DIR:?MFA_ROOT_DIR: MFA's root (pretrained_models/ in it)}"
PM=$MFA_ROOT_DIR/pretrained_models
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
DEST=$OUT
# all of them: the .was files anew (without GMMs no word uses), made aside and put in place only when every word was
# (a download that fails, GitHub's 60 an hour, left an empty wakealign/ otherwise)
[ $# -gt 0 ] || { OUT=$TMP/out; mkdir -p "$OUT"; }

# locale: acoustic model, dictionary, spelling (G2P) model
models() {
    case "$1" in
    de-DE) echo german_mfa german_mfa german_mfa;;
    en-US|en-CA) echo english_mfa english_us_mfa english_us_mfa;;
    en-GB|en-AU) echo english_mfa english_uk_mfa english_uk_mfa;;
    en-IN) echo english_mfa english_india_mfa english_india_mfa;;
    fr-FR|fr-CA) echo french_mfa french_mfa french_mfa;;
    es-ES) echo spanish_mfa spanish_spain_mfa spanish_spain_mfa;;
    es-MX) echo spanish_mfa spanish_latin_america_mfa spanish_latin_america_mfa;;
    ja-JP) echo japanese_mfa japanese_mfa japanese_katakana_mfa;;
    esac
}
# how a locale's dictionary writes the word (Japanese: katakana)
spelt() {
    case "$1:$2" in
    ja-JP:alexa) echo アレクサ;; ja-JP:echo) echo エコー;; ja-JP:computer) echo コンピューター;;
    ja-JP:amazon) echo アマゾン;; ja-JP:ziggy) echo ジギー;;
    *) echo "$2" | tr _ ' ';;
    esac
}
have() {    # kind name: download into MFA_ROOT_DIR once
    case "$1" in acoustic|g2p) f=$PM/$1/$2.zip;; dictionary) f=$PM/$1/$2.dict;; esac
    [ -s "$f" ] || $MFA_RUN mfa model download "$1" "$2" >/dev/null </dev/null
    [ -s "$f" ] || { echo "no $1 model $2" >&2; return 1; }
}
one() {     # id locale word
    set -- "$1" $(models "$2") "$3" "$2"
    id=$1 am=$2 dict=$3 g2p=$4 word=$5 loc=$6
    have acoustic "$am" && have dictionary "$dict" && have g2p "$g2p" || return 1
    [ -d "$TMP/$am" ] || (mkdir -p "$TMP/$am.z" && cd "$TMP/$am.z" && unzip -q "$PM/acoustic/$am.zip" && mv */ "$TMP/$am")
    # shellcheck disable=SC2046
    # (stdin away from the loop that reads the ids: micromamba run would eat them)
    $MFA_RUN python tools/wakealign/extract.py "$TMP/$am" "$PM/dictionary/$dict.dict" "$PM/g2p/$g2p.zip" "$TMP/$id.npz" $(spelt "$loc" "$word") </dev/null
    python3 tools/wakealign/walign.py write "$TMP/$id.npz" "$OUT/$id.wa" "$OUT/$am.was" </dev/null
}

# pt-BR: MFA's portuguese_mfa is a 2.0 model with position-dependent phones and no phone table to map them by
LOCALES="de-DE en-US en-GB fr-FR es-ES ja-JP en-IN es-MX"
all() {
    for loc in $LOCALES; do for w in alexa echo computer amazon ziggy; do echo "$w-$loc $loc $w"; done; done
    for w in okay_nabu hey_jarvis hey_mycroft; do echo "$w-en-US en-US $w"; echo "$w-de-DE de-DE $w"; done   # microWakeWord's
}
all | while read -r id loc w; do
    if [ $# -gt 0 ]; then case " $* " in *" $id "*) ;; *) continue;; esac; fi
    one "$id" "$loc" "$w" || { echo "$id: failed" >&2; : >"$TMP/failed"; }
done
if [ -e "$TMP/failed" ]; then
    if [ $# -gt 0 ]; then echo "not all of them made (the others are in $DEST)" >&2; else echo "not all of them made: $DEST left as it was" >&2; fi
    exit 1
fi
if [ $# -eq 0 ]; then rm -f "$DEST"/*.wa "$DEST"/*.was; mv "$OUT"/*.wa "$OUT"/*.was "$DEST"/; fi

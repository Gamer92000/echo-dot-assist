#!/bin/sh
set -eu
case "$0" in */*) cd "${0%/*}" ;; esac
[ -x ./hassmic ] || { echo 'Place run-local.sh and mww/ beside the existing hassmic executable.' >&2; exit 1; }
[ -s ./mww/okay_nabu/manifest.json ] && [ -s ./mww/okay_nabu/model.tflite ] || { echo 'Okay Nabu model files are missing.' >&2; exit 1; }
export HASSMIC_STATE="$PWD/state-local"
export HASSMIC_WIFI="$PWD/wifi-local"
export HASSMIC_MWW="$PWD/mww"
export HASSMIC_MODELS="$PWD/models"
if [ -f /etc/avahi/services/echo1-assist.service ]; then
    export HASSMIC_MDNS_FILE=/etc/avahi/services/echo1-assist.service
fi
export MISTO_SHMBUF=/usr/local/bin/shmbuf_tool
MISTO_PAPLAY=$(command -v paplay) || { echo 'paplay not found' >&2; exit 1; }
export MISTO_PAPLAY
mkdir -p "$HASSMIC_STATE"
if [ ! -f "$HASSMIC_STATE/config" ]; then
    printf 'wake_engine=microwakeword\n' > "$HASSMIC_STATE/config"
fi
if [ ! -f "$HASSMIC_STATE/mww_word" ]; then
    printf 'okay_nabu\n' > "$HASSMIC_STATE/mww_word"
fi
# Echo 1 native ledd backend, local microWakeWord, activation sound enabled.
# Separate state leaves the original run.sh/server-wake settings available.
exec ./hassmic -P esphome -p 6053 -n 'Echo 1' -w local \
    -V -B -z 28928 -o 0 -a 28930 -i 28932 -W 28931

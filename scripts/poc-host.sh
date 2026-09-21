#!/bin/sh
# Phase 4 shortcut: reference wyoming-satellite runs on this PC, the Echo only moves audio.
#   device:  /data/local/hassmic/mixcap -l 16701 &  /data/local/hassmic/mixplay -l 16702 -r 22050 &
#   HA:      add Wyoming integration -> this PC's IP, port 10700; wake word handled by HA (openWakeWord).
# Ports 16701/16702 are inside the range the stock firewall admits.
set -e
IP=${1:?usage: poc-host.sh <echo-ip>}
cd "$(dirname "$0")/.."
[ -x .venv/bin/wyoming-satellite ] || .venv/bin/pip -q install wyoming-satellite
exec .venv/bin/python -m wyoming_satellite --name "Echo Dot PoC" --uri tcp://0.0.0.0:10700 \
    --mic-command "nc $IP 16701" --snd-command "nc $IP 16702" --snd-command-rate 22050 --debug

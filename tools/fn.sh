#!/bin/sh
# usage: fn.sh file.asm 'symbol substring (non-plt definition)'  -> prints function body
awk -v f="$2" 'index($0,f) && /^[0-9a-f]+ </ && !/@plt/ {p=1} p{print} p&&/^$/{exit}' "$1"

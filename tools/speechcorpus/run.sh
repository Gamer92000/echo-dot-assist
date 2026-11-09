#!/bin/sh
# The speech corpus from scratch or on top of what OUT has (make.py's header): made, checked by speech to text, the
# ones turned down made again (a voice whose reference fails: that anew first, its clips once it passes), aligned by
# MFA, the joins checked too.  OUT defaults to testdata/corpus (Git LFS).  From scratch: ~1.5 h on a 16 GB GPU
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-testdata/corpus}
P=toolchain/omnivoice/venv/bin/python
$P tools/speechcorpus/make.py "$OUT"
$P tools/speechcorpus/verify.py "$OUT"
for i in 1 2 3 4 5; do $P tools/speechcorpus/make.py "$OUT" --redo; $P tools/speechcorpus/verify.py "$OUT"; done
$P tools/speechcorpus/align.py "$OUT"
$P tools/speechcorpus/verify.py "$OUT"

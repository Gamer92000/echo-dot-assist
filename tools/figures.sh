#!/bin/sh
# Renders the README figures: docs/img/src/<name>.typ -> docs/img/<name>-light.svg and -dark.svg.
# Needs typst (0.13 or newer); it fetches CeTZ from packages.typst.org on first use. Text becomes
# outlines in the SVG, so the figures look the same without the font (Noto Sans) installed.
set -e
cd "$(dirname "$0")/../docs/img"
for src in src/*.typ; do
    name=$(basename "$src" .typ)
    [ "$name" = theme ] && continue
    for theme in light dark; do
        typst compile --root . --input theme=$theme "$src" "$name-$theme.svg"
    done
    echo "$name"
done

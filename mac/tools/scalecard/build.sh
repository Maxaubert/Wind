#!/bin/zsh
# Builds tools/scalecard/scalecard: the Render scaling test card and zoomed-screen grab (scripts/e2e_draw.py).
set -euo pipefail
cd "${0:A:h}"
swiftc -O scalecard.swift -o scalecard
echo "built tools/scalecard/scalecard"

#!/bin/zsh
# Builds tools/upscaleref/up: the CPU reference for the Render engine's scaling filters (scripts/e2e_draw.py).
# xBR and NIS come straight from the Windows shaders (gen_ported.py); nearest, bilinear and bicubic are plain.
set -euo pipefail
cd "${0:A:h}"
python3 gen_ported.py ../../.. build-ported.inc
clang++ -O2 -std=c++17 -include vec.h -DPORTED='"build-ported.inc"' up.cpp -o up
echo "built tools/upscaleref/up"

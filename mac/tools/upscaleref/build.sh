#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/upscaleref/up: the CPU reference for the Render engine's scaling filters (scripts/e2e_draw.py).
# xBR comes straight from the Windows shader (gen_ported.py); nearest is plain.
set -euo pipefail
cd "${0:A:h}"
python3 gen_ported.py ../../.. build-ported.inc
clang++ -O2 -std=c++17 -include vec.h -DPORTED='"build-ported.inc"' up.cpp -o up
echo "built tools/upscaleref/up"

#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/scalecard/scalecard: the Render scaling test card and zoomed-screen grab (scripts/e2e_draw.py).
set -euo pipefail
cd "${0:A:h}"
swiftc -O scalecard.swift -o scalecard
echo "built tools/scalecard/scalecard"

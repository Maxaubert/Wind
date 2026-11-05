#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/cursorcap/cursorcap: the zoomed-screen capture behind scripts/cursor_truth.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O cursorcap.swift -o cursorcap
echo "built tools/cursorcap/cursorcap"

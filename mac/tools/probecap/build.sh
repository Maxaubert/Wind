#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/probecap/probecap: the zoomed-screen capture behind scripts/drawshot_truth.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O probecap.swift -o probecap
echo "built tools/probecap/probecap"

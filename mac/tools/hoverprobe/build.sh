#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/hoverprobe/hoverprobe: the hover probes behind scripts/e2e_hover.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O hoverprobe.swift -o hoverprobe
echo "built tools/hoverprobe/hoverprobe"

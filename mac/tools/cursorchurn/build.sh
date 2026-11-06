#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/cursorchurn/cursorchurn: the hover probes behind scripts/e2e_hover.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O cursorchurn.swift -o cursorchurn
echo "built tools/cursorchurn/cursorchurn"
clang -fobjc-arc -O1 -framework Cocoa -framework IOSurface dispcap.m -o dispcap
clang -fobjc-arc -O1 -framework Cocoa -framework IOSurface curseen.m -o curseen

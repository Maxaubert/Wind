#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/shotkit/shotkit: window, keys, session probe, colour sampler and PNG stats for
# scripts/e2e_screenshot.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O shotkit.swift -o shotkit
echo "built tools/shotkit/shotkit"

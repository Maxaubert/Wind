#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/fakeprompt/fakeprompt: a window above Wind's draw overlay for a few seconds, like a
# system prompt, for scripts/e2e_draw.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O fakeprompt.swift -o fakeprompt
echo "built tools/fakeprompt/fakeprompt"

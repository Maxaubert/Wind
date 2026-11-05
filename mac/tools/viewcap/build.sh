#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/viewcap/viewcap: zoomed-screen view capture behind scripts/edge_truth.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O viewcap.swift -o viewcap
echo "built tools/viewcap/viewcap"

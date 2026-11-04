#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Compiles assets/Wind.icon (an Icon Composer icon: black fill, the white wind strokes as a layer)
# into assets/icon/Assets.car (macOS 26+, no grey plate) and assets/icon/Wind.icns (older macOS).
# Needs Xcode (actool). The results are committed, so a normal build does not need Xcode; rerun
# this after changing assets/Wind.icon (edit it in Xcode's Icon Composer or by hand).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p assets/icon
DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer} xcrun actool assets/Wind.icon \
  --compile assets/icon --platform macosx --minimum-deployment-target 13.0 --app-icon Wind \
  --output-partial-info-plist /dev/null --output-format human-readable-text >/dev/null
echo "built assets/icon/Assets.car and assets/icon/Wind.icns"

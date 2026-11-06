#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Builds tools/hoverprobe/hoverprobe: the hover probes behind scripts/e2e_hover.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O hoverprobe.swift -o hoverprobe
echo "built tools/hoverprobe/hoverprobe"
# TipProbe.app: the same binary as an app bundle, so `open` makes it the active app (AppKit tooltips need that).
A=TipProbe.app; mkdir -p $A/Contents/MacOS && cp hoverprobe $A/Contents/MacOS/TipProbe
cat > $A/Contents/Info.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleExecutable</key><string>TipProbe</string><key>CFBundleIdentifier</key><string>com.windtest.tipprobe</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>
PL
codesign -fs - $A 2>/dev/null

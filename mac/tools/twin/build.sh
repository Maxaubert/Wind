#!/bin/zsh
# Builds tools/twin/TwinA.app and TwinB.app: two plain window apps (separate bundle ids, so Cmd+Tab
# switches between them) for scripts/e2e_draw.py's app-switch check. Launch the binary directly:
# Contents/MacOS/Twin <title> <x> <seconds> [hide].
set -euo pipefail
cd "${0:A:h}"
swiftc -O twin.swift -o twin.bin
for n in A B; do
  mkdir -p Twin$n.app/Contents/MacOS
  cp twin.bin Twin$n.app/Contents/MacOS/Twin
  cat > Twin$n.app/Contents/Info.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.maxaubert.wind.twin$n</string>
  <key>CFBundleExecutable</key><string>Twin</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleName</key><string>Twin$n</string>
</dict></plist>
PLIST
  codesign --force -s - Twin$n.app
done
rm twin.bin
echo "built tools/twin/TwinA.app, TwinB.app"

#!/bin/zsh
# Builds tools/fakegame/FakeGame.app: a foreground app that locks the pointer like a game's
# mouse-look, for scripts/e2e_mouse.py. Launch with `open -W FakeGame.app --args <seconds>`.
set -euo pipefail
cd "${0:A:h}"
mkdir -p FakeGame.app/Contents/MacOS
swiftc -O fakegame.swift -o FakeGame.app/Contents/MacOS/FakeGame
cat > FakeGame.app/Contents/Info.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.maxaubert.wind.fakegame</string>
  <key>CFBundleExecutable</key><string>FakeGame</string>
  <key>CFBundlePackageType</key><string>APPL</string>
</dict></plist>
PLIST
codesign --force -s - FakeGame.app
echo "built tools/fakegame/FakeGame.app"

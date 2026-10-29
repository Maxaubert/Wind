#!/bin/zsh
# Build Wind.app into build/. `scripts/build.sh install` also installs it to /Applications and
# relaunches it. `scripts/build.sh test` builds build/test/Wind.app with the e2e command channel
# compiled in (WIND_TEST_CHANNEL); the installed app never has it. Signs with the local "Wind Mac Dev" identity when present (a stable identity keeps
# the Accessibility grant across rebuilds), otherwise ad-hoc.
set -euo pipefail
cd "${0:A:h}/.."
VERSION=$(cat VERSION 2>/dev/null || echo 0.1.0)
FLAGS=(-c release --product Wind)
APP=build/Wind.app
if [[ "${1:-}" == "test" ]]; then
  FLAGS+=(-Xswiftc -DWIND_TEST_CHANNEL --scratch-path .build/test)
  APP=build/test/Wind.app
fi
swift build "${FLAGS[@]}" 2>&1 | grep -vE '^\[[0-9]+/[0-9]+\]' || true
BIN=$(swift build "${FLAGS[@]}" --show-bin-path)/Wind
[[ -x "$BIN" ]] || { echo "build failed"; exit 1; }

rm -rf "$APP"
mkdir -p "${APP:h}"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Wind"
cp "$BIN" "$APP/Contents/MacOS/WindGuard"     # the crash watchdog runs under its own name
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.maxaubert.wind</string>
  <key>CFBundleName</key><string>Wind</string>
  <key>CFBundleDisplayName</key><string>Wind</string>
  <key>CFBundleExecutable</key><string>Wind</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSUIElement</key><true/>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

KC="$HOME/Library/Keychains/windmac-dev.keychain-db"
if [[ -f "$KC" ]]; then
  security unlock-keychain -p "$PW" "$KC" 2>/dev/null || true
  codesign --force --options runtime --keychain "$KC" -s "Wind Mac Dev" "$APP"
else
  codesign --force -s - "$APP"
fi
codesign -dv "$APP" 2>&1 | grep -E 'Authority|Signature' || true
echo "built $APP ($VERSION)"

if [[ "${1:-}" == "install" ]]; then
  osascript -e 'tell application id "com.maxaubert.wind" to quit' 2>/dev/null || true
  pkill -x Wind 2>/dev/null || true
  sleep 0.5
  rm -rf /Applications/Wind.app
  cp -R "$APP" /Applications/Wind.app
  open /Applications/Wind.app
  echo "installed and launched /Applications/Wind.app"
fi

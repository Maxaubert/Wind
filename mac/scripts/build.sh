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
mkdir -p .build                                 # a fresh checkout or worktree has none yet
# A failed compile must stop here: never package a stale binary from an earlier build.
if ! swift build "${FLAGS[@]}" > .build/last-build.log 2>&1; then
  grep -E 'error' .build/last-build.log | head -20
  echo "build failed (full log: .build/last-build.log)"; exit 1
fi
BIN=$(swift build "${FLAGS[@]}" --show-bin-path)/Wind

rm -rf "$APP"
mkdir -p "${APP:h}"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Wind"
cp "$BIN" "$APP/Contents/MacOS/WindGuard"     # the crash watchdog runs under its own name
# The settings UI (ui/, Svelte): rebuilt when its sources are newer than the last build, then bundled.
if [[ ! -f ui/dist/index.html || -n "$(find ui/src ui/index.html ui/public ui/vite.config.js VERSION -newer ui/dist/index.html -print -quit)" ]]; then
  [[ -d ui/node_modules ]] || (cd ui && npm ci --silent) || { echo "ui: npm ci failed"; exit 1; }
  (cd ui && npm run build --silent > /dev/null) || { echo "ui: build failed"; exit 1; }
fi
cp -R ui/dist "$APP/Contents/Resources/ui"
cp assets/icon/Assets.car assets/icon/Wind.icns "$APP/Contents/Resources/"   # app icon (scripts/make_icon.sh)
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.maxaubert.wind</string>
  <key>CFBundleName</key><string>Wind</string>
  <key>CFBundleIconFile</key><string>Wind</string>
  <key>CFBundleIconName</key><string>Wind</string>
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
if [[ "${WIND_SIGN:-}" == "adhoc" ]]; then
  codesign --force --options runtime -s - "$APP"     # release builds (scripts/release.sh): no local identity
elif [[ -f "$KC" ]]; then
  # The keychain's password lives in the login keychain (item windmac-dev-keychain), never in the repo.
  security unlock-keychain -p "$(security find-generic-password -s windmac-dev-keychain -w 2>/dev/null)" "$KC" 2>/dev/null || true
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

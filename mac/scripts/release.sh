#!/bin/zsh
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# The public Mac release: build/Wind.app signed ad hoc (no Developer ID yet, so Gatekeeper asks once),
# with the licence and notices inside, zipped with ditto to dist/Wind-mac-<version>.zip.
# Run by .github/workflows/mac.yml on a macOS runner; works the same locally.
set -euo pipefail
cd "${0:A:h}/.."
VERSION=$(cat VERSION)
WIND_SIGN=adhoc scripts/build.sh
APP=build/Wind.app
cp ../LICENSE "$APP/Contents/Resources/LICENSE.txt"
cp ../THIRD-PARTY-NOTICES.md "$APP/Contents/Resources/THIRD-PARTY-NOTICES.md"
codesign --force --options runtime -s - "$APP"       # again: the copies above changed the bundle
codesign --verify --deep --strict "$APP"
# The test-only command channel must never ship (CLAUDE.md). Its short literals are inlined (strings
# cannot see them); this log text is in a test build only (checked both ways 2026-11-02).
if strings "$APP/Contents/MacOS/Wind" | grep -q 'settoggle result'; then
  echo "release build contains the test command channel"; exit 1
fi
mkdir -p dist
ZIP="dist/Wind-mac-$VERSION.zip"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
shasum -a 256 "$ZIP"
echo "released $ZIP"

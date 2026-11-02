#!/bin/zsh
# Builds tools/shotcap/shotcap: the zoomed-screen capture behind scripts/screenshot_truth.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O shotcap.swift -o shotcap
echo "built tools/shotcap/shotcap"

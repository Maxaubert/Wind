#!/bin/zsh
# Builds tools/cursorchurn/cursorchurn: the hover probes behind scripts/e2e_hover.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O cursorchurn.swift -o cursorchurn
echo "built tools/cursorchurn/cursorchurn"

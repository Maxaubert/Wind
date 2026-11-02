#!/bin/zsh
# Builds tools/hoverprobe/hoverprobe: the hover probes behind scripts/e2e_hover.py.
set -euo pipefail
cd "${0:A:h}"
swiftc -O hoverprobe.swift -o hoverprobe
echo "built tools/hoverprobe/hoverprobe"

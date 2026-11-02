#!/bin/zsh
# Builds tools/vmouse/vmouse: real HID pointer motion through Karabiner-Elements' virtual HID
# device, for the pointer e2e tests (scripts/e2e_mouse.py). Needs Karabiner-Elements installed
# (its DriverKit virtual HID device active); vmouse runs as root because the device socket is
# root-only. Headers come from pqrs-org/Karabiner-DriverKit-VirtualHIDDevice (public domain).
set -euo pipefail
cd "${0:A:h}"
SRC=../../.build/kvhid
[[ -d $SRC ]] || git clone -q --depth 1 https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice.git $SRC
clang++ -std=c++23 -O2 -Wno-deprecated-declarations -isystem $SRC/vendor/vendor/include -isystem $SRC/include \
  vmouse.cpp -framework IOKit -framework CoreFoundation -o vmouse
echo "built tools/vmouse/vmouse"

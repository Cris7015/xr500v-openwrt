#!/usr/bin/env bash
# build-local.sh — build the XR500v image LOCALLY in WSL (Ryzen), no Azure.
#
# Tree: ~/openwrt (cjdelisle base @f3605b31 + this overlay applied).
# Re-sync the overlay from this repo before building so package/target edits
# made here land in the build tree.
#
# Usage:
#   ./scripts/build-local.sh              # full/incremental build + trendchip-patch
#   ./scripts/build-local.sh <make-args>  # e.g. package/kernel/mt76/compile
set -euo pipefail

OWRT="${OWRT:-$HOME/openwrt}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="$(nproc)"
IMG="$OWRT/bin/targets/econet/en751221/openwrt-econet-en751221-tplink_archer-xr500v-squashfs-sysupgrade.bin"

echo "==> [1/3] sync overlay ($REPO -> $OWRT)"
cp -r "$REPO"/package/* "$OWRT/package/"
cp -r "$REPO"/target/*  "$OWRT/target/"
# kmod source edits don't auto-rebuild; clean the kmods we tweak so they recompile
make -C "$OWRT" package/kernel/econet-eth/clean package/kernel/econet-pcm/clean package/kernel/mt76/clean >/dev/null 2>&1 || true

echo "==> [2/3] make -j$JOBS ${*:-world}"
cd "$OWRT"
make -j"$JOBS" "${@:-world}"

if [ -f "$IMG" ]; then
  echo "==> [3/3] trendchip-patch the sysupgrade image"
  python3 "$REPO/scripts/patch_trendchip_header.py" "$IMG" "${IMG%.bin}-patched.bin"
  echo
  echo "  RAW    : $IMG"
  echo "  FLASH  : ${IMG%.bin}-patched.bin   <- usa ESTE (header trendchip)"
  echo "  -> flash: ./scripts/flash-from-wsl.sh  (router en stock telnet :2323)"
else
  echo "==> (no se generó imagen sysupgrade — probablemente un build parcial de paquete)"
fi

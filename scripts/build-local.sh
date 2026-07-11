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
KERNEL_BIN="$OWRT/build_dir/target-mips_24kc_musl/linux-econet_en751221/tplink_archer-xr500v-kernel.bin"

# WSL strips: the Windows PATH leaks into WSL and 'C:\Program Files\PowerShell\7'
# becomes a *relative* entry 'Files/PowerShell/7', which makes find -execdir
# (used by OpenWrt's reproducible-build timestamp step) refuse to run and the
# build fails at package/install. Drop /mnt/* and any non-absolute entry.
export PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -E '^/' | grep -vE '^/mnt/' | paste -sd:)"

echo "==> [1/3] sync overlay ($REPO -> $OWRT)"
cp -r "$REPO"/package/* "$OWRT/package/"
cp -r "$REPO"/target/*  "$OWRT/target/"
# kmod source edits don't auto-rebuild; clean the kmods we tweak so they recompile
make -C "$OWRT" package/kernel/econet-eth/clean package/kernel/econet-pcm/clean package/kernel/mt76/clean \
  package/kernel/xr500v-en7570-diag/clean package/kernel/xr500v-gpon-diag/clean \
  package/kernel/xr500v-xpon-rx-init/clean >/dev/null 2>&1 || true

# re-apply local feed patches (LuCI/etc. fixes not yet upstream). Idempotent —
# git-apply --check skips ones already present — so this survives `feeds update`.
# luci-base is cleaned afterwards so the patched source actually recompiles.
applied=0
for p in "$REPO"/patches/feeds/*/*.patch; do
  [ -e "$p" ] || continue
  feed="$(basename "$(dirname "$p")")"
  if git -C "$OWRT/feeds/$feed" apply -p1 --check "$p" 2>/dev/null; then
    git -C "$OWRT/feeds/$feed" apply -p1 "$p" && echo "  applied feed patch: ${p#"$REPO"/}" && applied=1
  fi
done
[ "$applied" = 1 ] && make -C "$OWRT" package/feeds/luci/luci-base/clean >/dev/null 2>&1 || true

echo "==> [2/3] make -j$JOBS ${*:-world}"
cd "$OWRT"
make -j"$JOBS" "${@:-world}"

if [ -f "$IMG" ]; then
  PATCHED="${IMG%.bin}-patched.bin"
  echo "==> [3/3] validate layout, trendchip-patch, validate patched image"
  python3 "$REPO/scripts/validate_xr500v_image.py" "$IMG" --kernel-bin "$KERNEL_BIN"
  python3 "$REPO/scripts/patch_trendchip_header.py" "$IMG" "$PATCHED"
  python3 "$REPO/scripts/validate_xr500v_image.py" "$PATCHED" \
    --kernel-bin "$KERNEL_BIN" --require-trendchip
  echo
  echo "  RAW    : $IMG"
  echo "  FLASH  : $PATCHED   <- use THIS (validated TrendChip header)"
  echo "  -> flash: ./scripts/flash-from-wsl.sh  (router in stock telnet :2323)"
else
  echo "==> (no sysupgrade image generated — probably a partial package build)"
fi

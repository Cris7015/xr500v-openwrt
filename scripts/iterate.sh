#!/usr/bin/env bash
# iterate.sh — full iter loop for XR500v OpenWrt PoC.
#
# Edits to the DTS or image Makefile are assumed already committed locally.
# This script: push → build on Azure → fetch image → flash slot B → wait for
# user to power-cycle + bflag set 1 + autoboot → capture UART.
#
# Usage:
#   ./scripts/iterate.sh <iter-tag>
# Example:
#   ./scripts/iterate.sh iter2-pcie-off
#
# The iter-tag is used to name build/boot artifacts so they don't overwrite
# previous runs.

set -euo pipefail

ITER_TAG="${1:?usage: iterate.sh <iter-tag>}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARTIFACT_DIR="$REPO_ROOT/codex-runs/$ITER_TAG"
IMAGE_NAME="openwrt-econet-en751221-tplink_archer-xr500v-squashfs-sysupgrade.bin"
LOCAL_TFTP="/mnt/c/tftp"
AZURE_HOST="azure-xr500v"

mkdir -p "$ARTIFACT_DIR"

echo "==> [1/5] push local commits"
cd "$REPO_ROOT"
git push --quiet

echo "==> [2/5] build on Azure"
ssh "$AZURE_HOST" '
set -e
cd ~/xr500v-openwrt && git pull --rebase --quiet
bash scripts/apply-xr500v-files.sh
cd ~/openwrt
make -j$(nproc) world V=s 2>&1 | tail -3
ls -la bin/targets/econet/en751221/'"$IMAGE_NAME"'
' 2>&1 | tee "$ARTIFACT_DIR/build.log"

echo "==> [3/5] fetch image from Azure → WSL → tftp dir"
scp "$AZURE_HOST:openwrt/bin/targets/econet/en751221/$IMAGE_NAME" \
    "$LOCAL_TFTP/openwrt-xr500v-$ITER_TAG.bin"
cp "$LOCAL_TFTP/openwrt-xr500v-$ITER_TAG.bin" "$ARTIFACT_DIR/image.bin"
ls -la "$LOCAL_TFTP/openwrt-xr500v-$ITER_TAG.bin"

echo "==> [4/5] flash slot B"
echo "(launching flash-from-wsl.sh — assumes router is currently on stock slot A,"
echo " telnet :2323 active, IP reachable)"
bash "$REPO_ROOT/scripts/flash-from-wsl.sh" "$LOCAL_TFTP/openwrt-xr500v-$ITER_TAG.bin" 2>&1 \
  | tee "$ARTIFACT_DIR/flash.log"

echo "==> [5/5] now manual: power-cycle, intercept bldr, 'bflag set 1', power-cycle no intercept"
echo "    keep picocom open to capture the boot. Save the log to:"
echo "    $ARTIFACT_DIR/bootlog.txt"
echo
echo "When you have the bootlog, paste it back so we can analyze."

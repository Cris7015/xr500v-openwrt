#!/bin/bash
# Aplica los files XR500v-específicos del repo en un clone de openwrt
# Uso: ./apply-xr500v-files.sh [openwrt_dir]
#   openwrt_dir defaults to ~/openwrt

set -e
OPENWRT_DIR="${1:-$HOME/openwrt}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [[ ! -d "$OPENWRT_DIR/target/linux/econet" ]]; then
    echo "ERROR: $OPENWRT_DIR no parece ser un openwrt tree con target econet" >&2
    echo "       (no existe $OPENWRT_DIR/target/linux/econet)" >&2
    exit 1
fi

echo "[+] Applying XR500v files to $OPENWRT_DIR..."

# DTS
SRC_DTS="$REPO_DIR/target/linux/econet/dts/en751221_tplink_archer-xr500v.dts"
if [[ -f "$SRC_DTS" ]]; then
    cp -v "$SRC_DTS" "$OPENWRT_DIR/target/linux/econet/dts/"
else
    echo "WARN: $SRC_DTS not found, skipping" >&2
fi

# image build files (could be Makefile or en751221.mk)
for f in "$REPO_DIR"/target/linux/econet/image/*; do
    if [[ -f "$f" ]]; then
        cp -v "$f" "$OPENWRT_DIR/target/linux/econet/image/"
    fi
done

echo
echo "[+] Done. Now run on the OpenWrt tree:"
echo "    cd $OPENWRT_DIR && make defconfig"

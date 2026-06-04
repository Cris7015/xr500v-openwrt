#!/bin/bash
# Downloads the latest image from the Azure VM and flashes it to slot B of the XR500v
# Usage: ./flash-from-wsl.sh

set -e
ROUTER_IP="${ROUTER_IP:-192.168.68.99}"
PC_IP="${PC_IP:-192.168.68.248}"
ROUTER_PORT="${ROUTER_PORT:-2323}"
TFTP_DIR="${TFTP_DIR:-/mnt/c/tftp}"
AZURE_HOST="${AZURE_HOST:-azure-xr500v}"

# Detect the image filename (most recently built)
IMAGE_NAME=$(ssh "$AZURE_HOST" 'ls -t ~/openwrt/bin/targets/econet/en751221/*xr500v*sysupgrade.bin 2>/dev/null | head -1' || true)

if [[ -z "$IMAGE_NAME" ]]; then
    echo "ERROR: no XR500v image found in ~/openwrt/bin/targets/" >&2
    exit 1
fi

LOCAL_NAME=$(basename "$IMAGE_NAME")
echo "[+] Image: $LOCAL_NAME"

echo "[+] scp from Azure to WSL/$TFTP_DIR..."
scp "$AZURE_HOST:$IMAGE_NAME" "$TFTP_DIR/$LOCAL_NAME"
ls -la "$TFTP_DIR/$LOCAL_NAME"

echo "[+] Extract kernel and rootfs sections from image..."
python3 <<PY
import os, hashlib
src = open('$TFTP_DIR/$LOCAL_NAME', 'rb').read()
print(f"  Total: {len(src)} bytes ({len(src)/1024/1024:.2f} MB)")

# Kernel partition (3MB) - take first 3MB of image
kernel = src[:0x300000]
if len(kernel) < 0x300000:
    kernel += b'\xff' * (0x300000 - len(kernel))
open('$TFTP_DIR/openwrt_kernel1_v3.bin', 'wb').write(kernel)
print(f"  kernel1 payload: {len(kernel)} bytes md5={hashlib.md5(kernel).hexdigest()}")

# Rootfs (16MB) — slice from actual squashfs magic offset.
# tplink-v2-header recipe places squashfs at 0x200 (header) + KERNEL_SIZE = 0x300200,
# NOT at 0x400000 as the original script hardcoded. Detect magic to be safe.
sqsh_off = src.find(b'hsqs')
if sqsh_off < 0:
    raise SystemExit("squashfs hsqs magic not found in image")
rootfs = src[sqsh_off:]
if len(rootfs) < 0x1000000:
    rootfs += b'\xff' * (0x1000000 - len(rootfs))
elif len(rootfs) > 0x1000000:
    rootfs = rootfs[:0x1000000]
open('$TFTP_DIR/openwrt_rootfs1_v3.bin', 'wb').write(rootfs)
print(f"  rootfs1 payload: {len(rootfs)} bytes md5={hashlib.md5(rootfs).hexdigest()}")
PY

echo
echo "[+] Push to router via TFTP and flash..."
( 
    echo "rm -f /tmp/k1.bin /tmp/r1.bin"
    sleep 0.3
    echo "tftp -g -l /tmp/k1.bin -r openwrt_kernel1_v3.bin $PC_IP"
    sleep 8
    echo "tftp -g -l /tmp/r1.bin -r openwrt_rootfs1_v3.bin $PC_IP"
    sleep 80
    echo "ls -la /tmp/k1.bin /tmp/r1.bin"
    sleep 0.5
    echo "/userfs/bin/mtd -f -e kernel1 write /tmp/k1.bin kernel1"
    sleep 8
    echo "echo K_RC_\$?"
    sleep 0.5
    echo "/userfs/bin/mtd writeflash /tmp/r1.bin 16777216 28311552 /dev/mtd0"
    sleep 30
    echo "echo R_RC_\$?"
    sleep 0.5
    echo "exit"
) | timeout 180 telnet "$ROUTER_IP" "$ROUTER_PORT" 2>&1 | tail -30

echo
echo "[+] Flash complete. To boot: power cycle 30s, intercept bldr, 'bflag set 1', power cycle again, no intercept."

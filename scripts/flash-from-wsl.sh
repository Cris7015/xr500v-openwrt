#!/bin/bash
# Flashes the latest LOCAL build (the trendchip-patched image) to slot B of the XR500v.
# Builds are local now (NOT Azure). Run build-local.sh first. Device must be in stock telnet :2323.
# Usage: ./flash-from-wsl.sh

set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROUTER_IP="${ROUTER_IP:-192.168.68.99}"
PC_IP="${PC_IP:-192.168.68.248}"
ROUTER_PORT="${ROUTER_PORT:-2323}"
TFTP_DIR="${TFTP_DIR:-/mnt/c/tftp}"
OWRT="${OWRT:-$HOME/openwrt}"
FLASH_LOG_DIR="${FLASH_LOG_DIR:-$TFTP_DIR}"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

require_log_line() {
    local log="$1"
    local pattern="$2"
    local description="$3"

    if ! grep -aEq "$pattern" "$log"; then
        echo "ERROR: $description" >&2
        echo "----- router transcript ($log) -----" >&2
        tail -80 "$log" >&2
        exit 1
    fi
}

# IMAGE_NAME may select one exact audited artifact. Otherwise use the newest
# TrendChip-patched local image (a raw image crashes bldr).
IMAGE_NAME="${IMAGE_NAME:-}"
if [[ -z "$IMAGE_NAME" ]]; then
    shopt -s nullglob
    for candidate in \
        "$OWRT"/bin/targets/econet/en751221/*xr500v*sysupgrade-patched.bin; do
        if [[ -z "$IMAGE_NAME" || "$candidate" -nt "$IMAGE_NAME" ]]; then
            IMAGE_NAME="$candidate"
        fi
    done
    shopt -u nullglob
fi

if [[ -z "$IMAGE_NAME" ]]; then
    die "no patched XR500v image found in $OWRT/bin/targets/ (run build-local.sh first)"
fi
[[ -f "$IMAGE_NAME" ]] || die "selected IMAGE_NAME does not exist: $IMAGE_NAME"

LOCAL_NAME=$(basename "$IMAGE_NAME")
echo "[+] Local patched image: $LOCAL_NAME"

echo "[+] Validate XR500v header, 3 MiB layout and 512-byte file gap..."
python3 "$SCRIPT_DIR/validate_xr500v_image.py" \
    "$IMAGE_NAME" --require-trendchip

echo "[+] copy to $TFTP_DIR (TFTP-served by the Windows host)..."
cp "$IMAGE_NAME" "$TFTP_DIR/$LOCAL_NAME"
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

# Rootfs (16MB) — the validator already requires the exact XR500v offset.
# Do not use bytes.find(): an accidental "hsqs" sequence inside the compressed
# kernel must never move the destructive partition slice.
sqsh_off = 0x300200
if src[sqsh_off:sqsh_off + 4] != b'hsqs':
    raise SystemExit("squashfs hsqs magic is not at required offset 0x300200")
rootfs = src[sqsh_off:]
if len(rootfs) < 0x1000000:
    rootfs += b'\xff' * (0x1000000 - len(rootfs))
elif len(rootfs) > 0x1000000:
    rootfs = rootfs[:0x1000000]
open('$TFTP_DIR/openwrt_rootfs1_v3.bin', 'wb').write(rootfs)
print(f"  rootfs1 payload: {len(rootfs)} bytes md5={hashlib.md5(rootfs).hexdigest()}")
PY

KERNEL_PAYLOAD="$TFTP_DIR/openwrt_kernel1_v3.bin"
ROOTFS_PAYLOAD="$TFTP_DIR/openwrt_rootfs1_v3.bin"
[[ $(stat -c %s "$KERNEL_PAYLOAD") -eq 3145728 ]] || \
    die "kernel1 payload is not exactly 3145728 bytes"
[[ $(stat -c %s "$ROOTFS_PAYLOAD") -eq 16777216 ]] || \
    die "rootfs1 payload is not exactly 16777216 bytes"

echo
echo "[+] Transfer payloads to the stock firmware..."
mkdir -p "$FLASH_LOG_DIR"
FLASH_STAMP=$(date +%Y%m%d-%H%M%S)
TRANSFER_LOG="$FLASH_LOG_DIR/xr500v-flash-$FLASH_STAMP-transfer.log"
WRITE_LOG="$FLASH_LOG_DIR/xr500v-flash-$FLASH_STAMP-write.log"
: >"$TRANSFER_LOG"
: >"$WRITE_LOG"

(
    echo "rm -f /tmp/k1.bin /tmp/r1.bin"
    sleep 0.3
    echo "tftp -g -l /tmp/k1.bin -r openwrt_kernel1_v3.bin $PC_IP"
    sleep 20
    echo "echo K_DL_\$?"
    echo "tftp -g -l /tmp/r1.bin -r openwrt_rootfs1_v3.bin $PC_IP"
    sleep 80
    echo "echo R_DL_\$?"
    echo "ls -la /tmp/k1.bin /tmp/r1.bin"
    echo "echo TRANSFER_OK"
    sleep 1
    echo "exit"
) | timeout 150 telnet "$ROUTER_IP" "$ROUTER_PORT" 2>&1 | \
    tr -d '\r' >"$TRANSFER_LOG" || {
        echo "----- router transcript ($TRANSFER_LOG) -----" >&2
        tail -80 "$TRANSFER_LOG" >&2
        die "telnet/TFTP transfer session failed"
    }

require_log_line "$TRANSFER_LOG" '^K_DL_0$' \
    "kernel1 TFTP download did not return RC=0"
require_log_line "$TRANSFER_LOG" '^R_DL_0$' \
    "rootfs1 TFTP download did not return RC=0"
require_log_line "$TRANSFER_LOG" '[[:space:]]3145728[[:space:]]+/tmp/k1\.bin$' \
    "router did not receive an exact 3145728-byte kernel1"
require_log_line "$TRANSFER_LOG" '[[:space:]]16777216[[:space:]]+/tmp/r1\.bin$' \
    "router did not receive an exact 16777216-byte rootfs1"
require_log_line "$TRANSFER_LOG" '^TRANSFER_OK$' \
    "router transfer session did not reach its completion marker"

echo "[+] Transfer RCs and router-side sizes are exact; write slot B..."
(
    echo "/userfs/bin/mtd -f -e kernel1 write /tmp/k1.bin kernel1; KRC=\$?; echo K_RC_\$KRC; if [ \"\$KRC\" -ne 0 ]; then echo FLASH_ABORT_KERNEL; exit; fi"
    sleep 20
    echo "/userfs/bin/mtd writeflash /tmp/r1.bin 16777216 28311552 /dev/mtd0; RRC=\$?; echo R_RC_\$RRC; if [ \"\$RRC\" -ne 0 ]; then echo FLASH_ABORT_ROOTFS; exit; fi; echo FLASH_OK"
    sleep 60
    echo "exit"
) | timeout 120 telnet "$ROUTER_IP" "$ROUTER_PORT" 2>&1 | \
    tr -d '\r' >"$WRITE_LOG" || {
        echo "----- router transcript ($WRITE_LOG) -----" >&2
        tail -80 "$WRITE_LOG" >&2
        die "telnet/MTD write session failed"
    }

require_log_line "$WRITE_LOG" '^K_RC_0$' \
    "kernel1 MTD write did not return RC=0; rootfs1 was not authorized"
require_log_line "$WRITE_LOG" '^R_RC_0$' \
    "rootfs1 MTD write did not return RC=0"
require_log_line "$WRITE_LOG" '^FLASH_OK$' \
    "router write session did not reach its completion marker"

tail -30 "$WRITE_LOG"

echo
echo "[+] Flash complete: transfer sizes and both MTD return codes verified."
echo "[+] Full transcripts: $TRANSFER_LOG and $WRITE_LOG"
echo "[+] To boot: intercept bldr, 'bflag set 1', then cold power-cycle and do not intercept."

#!/usr/bin/env python3
"""Fail closed on the XR500v's non-obvious kernel/header/rootfs layout."""

import argparse
import struct
from pathlib import Path


HEADER_SIZE = 0x200
KERNEL_PARTITION_SIZE = 0x300000
KERNEL_PAYLOAD_LIMIT = KERNEL_PARTITION_SIZE - HEADER_SIZE
ROOTFS_FILE_OFFSET = HEADER_SIZE + KERNEL_PARTITION_SIZE
TRENDCHIP_MAGIC = bytes.fromhex("4c3d2e1faa55aa55")
TRENDCHIP_SUB_MAGIC = bytes.fromhex("55aa0101")


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument(
        "--kernel-bin",
        type=Path,
        help="the pre-padding tplink_archer-xr500v-kernel.bin build artifact",
    )
    parser.add_argument("--require-trendchip", action="store_true")
    args = parser.parse_args()

    data = args.image.read_bytes()
    require(len(data) > ROOTFS_FILE_OFFSET + 4, "image is truncated")

    header_size = be32(data, 0x74)
    payload_size = be32(data, 0x78)
    require(header_size == HEADER_SIZE,
            f"header size is 0x{header_size:x}, expected 0x{HEADER_SIZE:x}")
    require(payload_size == len(data) - HEADER_SIZE,
            f"tplink payload field 0x{payload_size:x} != file size minus header "
            f"0x{len(data) - HEADER_SIZE:x}")
    require(data[ROOTFS_FILE_OFFSET:ROOTFS_FILE_OFFSET + 4] == b"hsqs",
            f"SquashFS magic is not at file offset 0x{ROOTFS_FILE_OFFSET:x}")

    kernel_size = None
    headroom = None
    if args.kernel_bin:
        kernel_size = args.kernel_bin.stat().st_size
        headroom = KERNEL_PAYLOAD_LIMIT - kernel_size
        require(headroom >= 0,
                f"compressed kernel is {kernel_size} bytes, exceeds safe "
                f"0x{KERNEL_PAYLOAD_LIMIT:x} limit by {-headroom} bytes")

    if args.require_trendchip:
        require(data[0x60:0x68] == TRENDCHIP_MAGIC,
                "TrendChip primary magic is absent")
        require(be32(data, 0x7c) == KERNEL_PARTITION_SIZE,
                f"TrendChip rootfs offset is 0x{be32(data, 0x7c):x}, expected "
                f"0x{KERNEL_PARTITION_SIZE:x}")
        require(data[0x8c:0x90] == TRENDCHIP_SUB_MAGIC,
                "TrendChip sub-magic is absent")

    print(f"image:                 {args.image}")
    print(f"file_size:             {len(data)} bytes")
    print(f"header_size:           0x{header_size:x} (512 bytes)")
    print(f"kernel_partition:      0x{KERNEL_PARTITION_SIZE:x} (3 MiB)")
    print(f"kernel_payload_limit:  0x{KERNEL_PAYLOAD_LIMIT:x} bytes")
    if kernel_size is not None:
        print(f"compressed_kernel:     {kernel_size} bytes")
        print(f"kernel_headroom:       {headroom} bytes")
    print(f"rootfs_header_offset:  0x{KERNEL_PARTITION_SIZE:x}")
    print(f"rootfs_file_offset:    0x{ROOTFS_FILE_OFFSET:x}")
    print(f"required_512B_gap:     present")
    print(f"trendchip_header:      {'valid' if args.require_trendchip else 'not required'}")


if __name__ == "__main__":
    main()

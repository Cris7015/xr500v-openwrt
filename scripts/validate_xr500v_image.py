#!/usr/bin/env python3
"""Fail closed on the XR500v's non-obvious kernel/header/rootfs layout."""

import argparse
import lzma
import struct
from pathlib import Path


HEADER_SIZE = 0x200
KERNEL_PARTITION_SIZE = 0x300000
KERNEL_PAYLOAD_LIMIT = KERNEL_PARTITION_SIZE - HEADER_SIZE
ROOTFS_FILE_OFFSET = HEADER_SIZE + KERNEL_PARTITION_SIZE
ROOTFS_PARTITION_SIZE = 0x1000000
KERNEL_LOADADDR = 0x80020000
KERNEL_SIZE_HINT = 0x01300000
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
    require(len(data) >= ROOTFS_FILE_OFFSET + 96, "image is truncated")
    require(
        len(data) - ROOTFS_FILE_OFFSET <= ROOTFS_PARTITION_SIZE,
        "rootfs payload exceeds the 16 MiB rootfs1 partition",
    )

    header_size = be32(data, 0x74)
    payload_size = be32(data, 0x78)
    require(header_size == HEADER_SIZE,
            f"header size is 0x{header_size:x}, expected 0x{HEADER_SIZE:x}")
    require(payload_size == len(data) - HEADER_SIZE,
            f"tplink payload field 0x{payload_size:x} != file size minus header "
            f"0x{len(data) - HEADER_SIZE:x}")
    require(data[ROOTFS_FILE_OFFSET:ROOTFS_FILE_OFFSET + 4] == b"hsqs",
            f"SquashFS magic is not at file offset 0x{ROOTFS_FILE_OFFSET:x}")
    require(be32(data, 0x68) == KERNEL_LOADADDR,
            f"decompress address is 0x{be32(data, 0x68):08x}, expected "
            f"0x{KERNEL_LOADADDR:08x}")
    require(be32(data, 0x6c) == KERNEL_LOADADDR,
            f"kernel wrapper entry is 0x{be32(data, 0x6c):08x}, expected "
            f"0x{KERNEL_LOADADDR:08x}")
    require(data[KERNEL_PARTITION_SIZE:ROOTFS_FILE_OFFSET] == bytes(HEADER_SIZE),
            "the required 512-byte gap before SquashFS is not all zero")

    compressed_region = data[HEADER_SIZE:KERNEL_PARTITION_SIZE]
    try:
        decompressor = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        kernel = decompressor.decompress(compressed_region)
    except lzma.LZMAError as exc:
        raise SystemExit(f"ERROR: kernel LZMA stream is invalid: {exc}") from exc
    require(decompressor.eof, "kernel LZMA stream is truncated")
    require(not any(decompressor.unused_data),
            "nonzero data follows the kernel LZMA stream inside kernel1")

    kernel_size = len(kernel)
    headroom = KERNEL_PAYLOAD_LIMIT - kernel_size
    require(headroom >= 0,
            f"decompressed kernel is {kernel_size} bytes, exceeds safe "
            f"0x{KERNEL_PAYLOAD_LIMIT:x} limit by {-headroom} bytes")

    if args.kernel_bin:
        expected_kernel = args.kernel_bin.read_bytes()
        require(kernel == expected_kernel,
                "decompressed image kernel does not match --kernel-bin")

    squashfs_bytes = struct.unpack_from("<Q", data, ROOTFS_FILE_OFFSET + 40)[0]
    rootfs_payload_size = len(data) - ROOTFS_FILE_OFFSET
    require(squashfs_bytes == rootfs_payload_size,
            f"SquashFS bytes_used is {squashfs_bytes}, but image carries "
            f"{rootfs_payload_size} rootfs bytes")

    if args.require_trendchip:
        require(data[0x60:0x68] == TRENDCHIP_MAGIC,
                "TrendChip primary magic is absent")
        require(be32(data, 0x70) == KERNEL_SIZE_HINT,
                f"TrendChip kernel size hint is 0x{be32(data, 0x70):x}, expected "
                f"0x{KERNEL_SIZE_HINT:x}")
        require(be32(data, 0x7c) == KERNEL_PARTITION_SIZE,
                f"TrendChip rootfs offset is 0x{be32(data, 0x7c):x}, expected "
                f"0x{KERNEL_PARTITION_SIZE:x}")
        require(be32(data, 0x80) == ROOTFS_PARTITION_SIZE,
                f"TrendChip rootfs size is 0x{be32(data, 0x80):x}, expected "
                f"0x{ROOTFS_PARTITION_SIZE:x}")
        require(data[0x88:0x8c] == bytes(4),
                "TrendChip reserved word at 0x88 is not zero")
        require(data[0x8c:0x90] == TRENDCHIP_SUB_MAGIC,
                "TrendChip sub-magic is absent")

    print(f"image:                 {args.image}")
    print(f"file_size:             {len(data)} bytes")
    print(f"header_size:           0x{header_size:x} (512 bytes)")
    print(f"kernel_partition:      0x{KERNEL_PARTITION_SIZE:x} (3 MiB)")
    print(f"kernel_payload_limit:  0x{KERNEL_PAYLOAD_LIMIT:x} bytes")
    print(f"decompressed_kernel:   {kernel_size} bytes")
    print(f"kernel_headroom:       {headroom} bytes")
    print(f"rootfs_header_offset:  0x{KERNEL_PARTITION_SIZE:x}")
    print(f"rootfs_file_offset:    0x{ROOTFS_FILE_OFFSET:x}")
    print(f"rootfs_payload:        {rootfs_payload_size} bytes")
    print(f"required_512B_gap:     verified all zero")
    print(f"trendchip_header:      {'valid' if args.require_trendchip else 'not required'}")


if __name__ == "__main__":
    main()

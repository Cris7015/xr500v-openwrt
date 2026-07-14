#!/usr/bin/env python3
"""
Patch the 512-byte tplink-v2 header on an OpenWrt sysupgrade.bin with
TrendChip/EcoNet-bldr-specific fields the OEM bldr reads post-decompress.

Stock XR500v kernel partition header (mtd3_kernel.bin) layout (BE 32-bit):
  @0x00: 03 00 00 03 cc cc 22 33 ...    (TP-Link sub-magic)
  @0x60: 4c 3d 2e 1f aa 55 aa 55        TrendChip primary magic
  @0x68: <decompress address>           where bldr places the LZMA-decompressed wrapper
  @0x6c: <kernel entry point>           address bldr jumps to after decompress
  @0x70: <kernel size hint>             stock has 0x01300000
  @0x74: 00 00 02 00                    header size = 0x200
  @0x78: <tplink payload size>          raw OpenWrt image uses file_size-0x200
  @0x7c: <rootfs offset>                stock has 0x00300000 (= kernel partition size)
  @0x80: <rootfs size>                  stock has 0x00bdc087
  @0x88: 00 00 00 00
  @0x8c: 55 aa 01 01                    sub-magic

Without these fields the bldr crashes on slave-CPU rootfs setup because it
reads garbage (0xFFFFFFFF and 0) and dereferences into KSEG2 (0xc0000000).

Usage:
  patch_trendchip_header.py <input> <output> [--entry 0x80020000]
"""
import argparse
import struct
import sys


STOCK_MAGIC = bytes.fromhex("4c3d2e1faa55aa55")
SUB_MAGIC = bytes.fromhex("55aa0101")
KERNEL_LOADADDR = 0x80020000


def patch(data: bytearray, entry: int, kernel_size_hint: int,
          rootfs_offset: int, rootfs_size: int) -> bytearray:
    if len(data) < 0x200:
        sys.exit("image too small to contain a 512-byte header")

    # decompress address: read existing tplink-v2 value at 0x68 and keep it.
    # tplink-v2-header writes KERNEL_LOADADDR there in BE, which already
    # matches what the bldr should use to place the wrapper.
    decompress_addr = struct.unpack(">I", bytes(data[0x68:0x6c]))[0]
    if decompress_addr != KERNEL_LOADADDR:
        sys.exit(
            f"unexpected tplink-v2 load address 0x{decompress_addr:08x}; "
            f"XR500v requires 0x{KERNEL_LOADADDR:08x}"
        )
    # kernel entry: keep whatever tplink-v2-header wrote (KERNEL_LOADADDR).
    # The LZMA wrapper's _start lives at the load address and decompresses
    # the real kernel itself, so jumping to load addr is correct.
    # The --entry override is only used if explicitly requested.
    existing_entry = struct.unpack(">I", bytes(data[0x6c:0x70]))[0]
    if entry == 0:
        entry = existing_entry
    if entry != KERNEL_LOADADDR:
        sys.exit(
            f"unsafe kernel wrapper entry 0x{entry:08x}; "
            f"XR500v requires 0x{KERNEL_LOADADDR:08x}, not the inner ELF entry"
        )

    # TrendChip magic at 0x60.
    data[0x60:0x68] = STOCK_MAGIC
    # 0x68 already correct (kept from tplink-v2).
    # 0x6c kernel entry — keep existing unless --entry given.
    data[0x6c:0x70] = struct.pack(">I", entry)
    # 0x70 kernel size hint.
    data[0x70:0x74] = struct.pack(">I", kernel_size_hint)
    # 0x74 header size — keep tplink-v2 value (already 0x200).
    # 0x78 tplink payload-size field — keep tplink-v2 value.
    # 0x7c rootfs offset within concatenated kernel+rootfs image.
    data[0x7c:0x80] = struct.pack(">I", rootfs_offset)
    # 0x80 rootfs size.
    data[0x80:0x84] = struct.pack(">I", rootfs_size)
    # 0x88 padding zeros — keep.
    # 0x8c sub-magic.
    data[0x8c:0x90] = SUB_MAGIC

    print(
        f"  decompress_addr  = 0x{decompress_addr:08x}\n"
        f"  kernel_entry     = 0x{entry:08x}\n"
        f"  kernel_size_hint = 0x{kernel_size_hint:08x}\n"
        f"  rootfs_offset    = 0x{rootfs_offset:08x}\n"
        f"  rootfs_size      = 0x{rootfs_size:08x}",
        file=sys.stderr,
    )
    return data


def main():
    p = argparse.ArgumentParser()
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--entry", type=lambda s: int(s, 0), default=KERNEL_LOADADDR,
                   help="kernel wrapper entry; XR500v requires 0x80020000")
    p.add_argument("--kernel-size", type=lambda s: int(s, 0), default=0x01300000,
                   help="kernel size hint at @0x70 (default: stock value 0x01300000)")
    p.add_argument("--rootfs-offset", type=lambda s: int(s, 0), default=0x00300000,
                   help="rootfs offset within image at @0x7c (default: 0x300000 = 3 MB kernel partition)")
    p.add_argument("--rootfs-size", type=lambda s: int(s, 0), default=0x01000000,
                   help="rootfs size at @0x80 (default: 0x1000000 = 16 MB rootfs partition)")
    args = p.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())

    patch(data, args.entry, args.kernel_size, args.rootfs_offset, args.rootfs_size)

    with open(args.output, "wb") as f:
        f.write(data)
    print(f"wrote {args.output} ({len(data)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()

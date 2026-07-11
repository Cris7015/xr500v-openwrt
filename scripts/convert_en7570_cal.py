#!/usr/bin/env python3
"""Convert and validate the XR500v EN7570 100-word calibration matrix.

The OEM 2.6.36 driver reads /etc/7570_bob.conf directly into a native u32
array on the big-endian EN751221.  The new Airoha LDDLA driver imports its
firmware with get_unaligned_le32(), so the byte order must be converted per
word before the OEM matrix can be used as airoha/en7570_cal.bin.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path


WORD_COUNT = 100
IMAGE_SIZE = WORD_COUNT * 4
MAGIC_OFFSET = 0x94
MAGIC_GPON = 0x07050700
MAGIC_EPON = 0xE7050700
ERASED = 0xFFFFFFFF

IMPORTANT_FIELDS = {
    0x000: "initial_ibias",
    0x004: "initial_imod",
    0x008: "p0_target",
    0x00C: "p1_target",
    0x020: "tx_calibration",
    0x024: "rx_calibration",
    0x030: "t0_t1_delay",
    0x034: "t0c_t1c",
    0x050: "ddmi_tx_points",
    0x054: "ddmi_rx_points_1",
    0x058: "ddmi_rx_points_2",
    0x094: "pon_mode_magic",
}


class CalibrationError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unpack_words(data: bytes, byte_order: str) -> list[int]:
    if len(data) != IMAGE_SIZE:
        raise CalibrationError(
            f"expected exactly {IMAGE_SIZE} bytes ({WORD_COUNT} u32 words), "
            f"got {len(data)}"
        )
    prefix = ">" if byte_order == "big" else "<"
    return list(struct.unpack(f"{prefix}{WORD_COUNT}I", data))


def pack_words(words: list[int], byte_order: str) -> bytes:
    if len(words) != WORD_COUNT:
        raise CalibrationError(f"expected {WORD_COUNT} words, got {len(words)}")
    prefix = ">" if byte_order == "big" else "<"
    return struct.pack(f"{prefix}{WORD_COUNT}I", *words)


def validate(words: list[int]) -> str:
    magic = words[MAGIC_OFFSET // 4]
    if magic == MAGIC_GPON:
        mode = "GPON"
    elif magic == MAGIC_EPON:
        mode = "EPON"
    else:
        raise CalibrationError(
            f"invalid PON magic at 0x{MAGIC_OFFSET:03x}: 0x{magic:08x}; "
            f"expected 0x{MAGIC_GPON:08x} (GPON) or 0x{MAGIC_EPON:08x} (EPON)"
        )

    for offset in (0x000, 0x004, 0x008, 0x00C):
        if words[offset // 4] == ERASED:
            raise CalibrationError(
                f"required calibration field at 0x{offset:03x} is erased"
            )

    return mode


def print_report(path: Path, data: bytes, words: list[int], byte_order: str) -> None:
    mode = validate(words)
    print(f"file          : {path}")
    print(f"size          : {len(data)} bytes / {len(words)} words")
    print(f"byte order    : {byte_order}")
    print(f"sha256        : {sha256(data)}")
    print(f"PON mode      : {mode}")
    print("selected words:")
    for offset, name in IMPORTANT_FIELDS.items():
        print(f"  0x{offset:03x} {name:<20} 0x{words[offset // 4]:08x}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="400-byte calibration image")
    parser.add_argument(
        "output",
        type=Path,
        nargs="?",
        help="converted output; omit to validate/report only",
    )
    parser.add_argument(
        "--input-endian",
        choices=("big", "little"),
        default="big",
        help="word byte order in the input (default: OEM big-endian)",
    )
    parser.add_argument(
        "--output-endian",
        choices=("big", "little"),
        default="little",
        help="word byte order in the output (default: LDDLA little-endian)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        source = args.input.read_bytes()
        words = unpack_words(source, args.input_endian)
        print_report(args.input, source, words, args.input_endian)

        if args.output is None:
            return 0

        converted = pack_words(words, args.output_endian)
        roundtrip = unpack_words(converted, args.output_endian)
        if roundtrip != words:
            raise CalibrationError("internal round-trip verification failed")
        validate(roundtrip)

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(converted)
        print(f"output        : {args.output}")
        print(f"output endian : {args.output_endian}")
        print(f"output sha256 : {sha256(converted)}")
        return 0
    except (OSError, CalibrationError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

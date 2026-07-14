#!/usr/bin/env python3
"""Regression tests for the destructive XR500v image contract."""

from __future__ import annotations

import lzma
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts/validate_xr500v_image.py"
PATCHER = ROOT / "scripts/patch_trendchip_header.py"
PLATFORM = ROOT / "target/linux/econet/base-files/lib/upgrade/platform.sh"
HEADER_SIZE = 0x200
KERNEL_PARTITION_SIZE = 0x300000
ROOTFS_OFFSET = 0x300200
LOADADDR = 0x80020000


def make_image(*, trendchip: bool) -> tuple[bytes, bytes]:
    kernel = bytes(range(256)) * 4
    compressed = lzma.compress(kernel, format=lzma.FORMAT_ALONE)
    data = bytearray(ROOTFS_OFFSET + 96)
    data[HEADER_SIZE : HEADER_SIZE + len(compressed)] = compressed
    struct.pack_into(">I", data, 0x68, LOADADDR)
    struct.pack_into(">I", data, 0x6C, LOADADDR)
    struct.pack_into(">I", data, 0x74, HEADER_SIZE)
    struct.pack_into(">I", data, 0x78, len(data) - HEADER_SIZE)
    data[ROOTFS_OFFSET : ROOTFS_OFFSET + 4] = b"hsqs"
    struct.pack_into("<Q", data, ROOTFS_OFFSET + 40, 96)
    if trendchip:
        data[0x60:0x68] = bytes.fromhex("4c3d2e1faa55aa55")
        struct.pack_into(">I", data, 0x70, 0x01300000)
        struct.pack_into(">I", data, 0x7C, KERNEL_PARTITION_SIZE)
        struct.pack_into(">I", data, 0x80, 0x01000000)
        data[0x8C:0x90] = bytes.fromhex("55aa0101")
    return bytes(data), kernel


class ValidateXr500vImageTests(unittest.TestCase):
    def run_validator(self, image: bytes, kernel: bytes, *, trendchip: bool) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            image_path = Path(temporary) / "image.bin"
            kernel_path = Path(temporary) / "kernel.bin"
            image_path.write_bytes(image)
            kernel_path.write_bytes(kernel)
            command = [
                sys.executable,
                str(VALIDATOR),
                str(image_path),
                "--kernel-bin",
                str(kernel_path),
            ]
            if trendchip:
                command.append("--require-trendchip")
            return subprocess.run(command, text=True, capture_output=True, check=False)

    def test_valid_raw_and_patched_contracts(self) -> None:
        for trendchip in (False, True):
            with self.subTest(trendchip=trendchip):
                image, kernel = make_image(trendchip=trendchip)
                result = self.run_validator(image, kernel, trendchip=trendchip)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_wrapper_entry_is_rejected(self) -> None:
        image, kernel = make_image(trendchip=True)
        damaged = bytearray(image)
        struct.pack_into(">I", damaged, 0x6C, 0x804CF940)
        result = self.run_validator(bytes(damaged), kernel, trendchip=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kernel wrapper entry", result.stderr)

    def test_nonzero_gap_is_rejected(self) -> None:
        image, kernel = make_image(trendchip=True)
        damaged = bytearray(image)
        damaged[KERNEL_PARTITION_SIZE] = 1
        result = self.run_validator(bytes(damaged), kernel, trendchip=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("512-byte gap", result.stderr)

    def test_squashfs_length_and_trendchip_size_are_rejected(self) -> None:
        image, kernel = make_image(trendchip=True)
        damaged = bytearray(image)
        struct.pack_into("<Q", damaged, ROOTFS_OFFSET + 40, 95)
        result = self.run_validator(bytes(damaged), kernel, trendchip=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SquashFS bytes_used", result.stderr)

        damaged = bytearray(image)
        struct.pack_into(">I", damaged, 0x80, 0x00300000)
        result = self.run_validator(bytes(damaged), kernel, trendchip=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TrendChip rootfs size", result.stderr)

    def test_patcher_and_target_platform_gate_share_the_contract(self) -> None:
        raw, kernel = make_image(trendchip=False)
        with tempfile.TemporaryDirectory() as temporary:
            raw_path = Path(temporary) / "raw.bin"
            patched_path = Path(temporary) / "patched.bin"
            kernel_path = Path(temporary) / "kernel.bin"
            raw_path.write_bytes(raw)
            kernel_path.write_bytes(kernel)
            patch = subprocess.run(
                [
                    sys.executable,
                    str(PATCHER),
                    str(raw_path),
                    str(patched_path),
                    "--entry",
                    "0x80020000",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(patch.returncode, 0, patch.stderr)
            validate = subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    str(patched_path),
                    "--kernel-bin",
                    str(kernel_path),
                    "--require-trendchip",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(validate.returncode, 0, validate.stderr)
            shell = '. "$1"; board_name(){ echo tplink,archer-xr500v; }; platform_check_image "$2"'
            accepted = subprocess.run(
                ["sh", "-c", shell, "sh", str(PLATFORM), str(patched_path)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            rejected = subprocess.run(
                ["sh", "-c", shell, "sh", str(PLATFORM), str(raw_path)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(rejected.returncode, 0)

            unsafe = subprocess.run(
                [
                    sys.executable,
                    str(PATCHER),
                    str(raw_path),
                    str(Path(temporary) / "unsafe.bin"),
                    "--entry",
                    "0x804cf940",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(unsafe.returncode, 0)
            self.assertIn("not the inner ELF entry", unsafe.stderr)


if __name__ == "__main__":
    unittest.main()

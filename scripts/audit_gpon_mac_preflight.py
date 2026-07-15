#!/usr/bin/env python3
"""Fail-closed source audit for the XR500v GPON MAC bus preflight."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


EXPECTED_SCU_REGS = {
    "EN751221_SCU_WAN_CONF": 0x070,
    "EN751221_SCU_RESET_CTRL2": 0x830,
    "EN751221_SCU_RESET_CTRL1": 0x834,
}

FORBIDDEN_CALLS = [
    r"\b(?:ioread|read[blqw]|__raw_read)[a-z0-9_]*\s*\(",
    r"\b(?:iowrite|write[blqw]|__raw_write)[a-z0-9_]*\s*\(",
    r"\b(?:devm_)?ioremap[a-z0-9_]*\s*\(",
    r"\b(?:devm_)?request_(?:threaded_)?irq\s*\(",
    r"\breset_control_[a-z0-9_]+\s*\(",
    r"\bregmap_(?:write|update_bits|set_bits|clear_bits)\s*\(",
    r"\bclk_[a-z0-9_]+\s*\(",
    r"\bgpiod_(?:set|direction_output)[a-z0-9_]*\s*\(",
    r"\bi2c_[a-z0-9_]+\s*\(",
    r"\b(?:schedule_work|queue_work|timer_setup|hrtimer_init)\s*\(",
]


def audit(source: pathlib.Path, package_makefile: pathlib.Path) -> list[str]:
    text = source.read_text(encoding="utf-8")
    makefile = package_makefile.read_text(encoding="utf-8")
    errors: list[str] = []

    defines = {
        name: int(value, 16)
        for name, value in re.findall(
            r"^#define\s+(EN751221_SCU_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+)$",
            text,
            re.MULTILINE,
        )
    }
    if defines != EXPECTED_SCU_REGS:
        errors.append(f"SCU allowlist mismatch: {defines!r}")

    for pattern in FORBIDDEN_CALLS:
        if re.search(pattern, text):
            errors.append(f"forbidden call matched: {pattern}")

    if "0x1fb64000" not in text:
        errors.append("known-stall address must remain documented")
    if text.count("regmap_read(") != 3:
        errors.append("expected exactly three SCU regmap reads")
    if "gpon_mac_mmio_reads: 0" not in text:
        errors.append("zero GPON read assertion missing")
    if "of_machine_is_compatible(\"tplink,archer-xr500v\")" not in text:
        errors.append("XR500v machine guard missing")
    if "AUTOLOAD" in makefile:
        errors.append("package must not autoload")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("package_makefile", type=pathlib.Path)
    args = parser.parse_args()

    errors = audit(args.source, args.package_makefile)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: three SCU reads; GPON MMIO/write/IRQ/reset paths absent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

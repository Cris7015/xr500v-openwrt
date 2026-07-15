#!/usr/bin/env python3
"""Fail-closed source audit for the XR500v six-register GPON snapshot."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


EXPECTED_READS = [
    "GPON_G_ONU_ID",
    "GPON_G_GBL_CFG",
    "GPON_G_INT_ENABLE",
    "GPON_G_PLOAMU_FIFO_STS",
    "GPON_G_PLOAMD_FIFO_STS",
    "GPON_G_ACTIVATION_ST",
]

EXPECTED_OFFSETS = {
    "GPON_G_ONU_ID": 0x000,
    "GPON_G_GBL_CFG": 0x004,
    "GPON_G_INT_ENABLE": 0x00C,
    "GPON_G_PLOAMU_FIFO_STS": 0x050,
    "GPON_G_PLOAMD_FIFO_STS": 0x058,
    "GPON_G_ACTIVATION_ST": 0x0BC,
}

FORBIDDEN_CALLS = [
    r"\bioread(?!32\s*\()[a-z0-9_]*\s*\(",
    r"\b(?:read[blqw]|__raw_read[blqw])\s*\(",
    r"\b(?:iowrite|write[blqw]|__raw_write)[a-z0-9_]*\s*\(",
    r"\b(?:devm_)?request_(?:threaded_)?irq\s*\(",
    r"\breset_control_[a-z0-9_]+\s*\(",
    r"\bregmap_(?:write|set_bits|clear_bits)\s*\(",
    r"\bclk_[a-z0-9_]+\s*\(",
    r"\bgpiod_(?:set|direction_output)[a-z0-9_]*\s*\(",
    r"\bi2c_[a-z0-9_]+\s*\(",
    r"\b(?:schedule_work|queue_work|timer_setup|hrtimer_init)\s*\(",
    r"\b(?:msleep|usleep_range|fsleep|udelay|ndelay)\s*\(",
]


def audit(source: pathlib.Path, package_makefile: pathlib.Path) -> list[str]:
    text = source.read_text(encoding="utf-8")
    makefile = package_makefile.read_text(encoding="utf-8")
    errors: list[str] = []

    for pattern in FORBIDDEN_CALLS:
        if re.search(pattern, text):
            errors.append(f"forbidden call matched: {pattern}")

    offsets = {
        name: int(value, 16)
        for name, value in re.findall(
            r"^#define\s+(GPON_G_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+)$",
            text,
            re.MULTILINE,
        )
    }
    if offsets != EXPECTED_OFFSETS:
        errors.append(f"GPON read allowlist mismatch: {offsets!r}")

    reads = re.findall(r"ioread32\(ctx->base \+ (GPON_G_[A-Z0-9_]+)\)", text)
    if reads != EXPECTED_READS or text.count("ioread32(") != 6:
        errors.append(f"expected exactly the six ordered reads: {reads!r}")

    if "#define GPON_MAC_PHYS\t\t\t0x1fb64000" not in text:
        errors.append("exact GPON MAC physical base missing")
    if "#define GPON_MAC_SIZE\t\t\t0x000000c0" not in text:
        errors.append("exact 0xc0 mapping constraint missing")
    if text.count("request_mem_region(") != 1:
        errors.append("expected one bounded region claim")
    if text.count("ioremap(") != 1 or (
        "ioremap(GPON_MAC_PHYS, GPON_MAC_SIZE)" not in text
    ):
        errors.append("exact bounded mapping missing")
    if text.count("iounmap(ctx.base);") != 1:
        errors.append("mapping teardown missing")
    if "GPON_G_PLOAMU_WDATA" in text or "GPON_G_PLOAMD_RDATA" in text:
        errors.append("FIFO data register token present")
    if text.count("regmap_update_bits(") != 2:
        errors.append("expected exactly two WAN-mux update call sites")
    if text.count("regmap_read(") != 5:
        errors.append("expected exactly five SCU read call sites")
    if "goto restore_mode;" not in text:
        errors.append("failed-set/read rollback path missing")
    if "result.wan_after != result.wan_before" not in text:
        errors.append("exact restore readback gate missing")
    if "if (regmap_might_sleep(ctx.scu))" not in text:
        errors.append("atomic regmap guard missing")
    if text.count("stop_machine(") != 1:
        errors.append("expected one global two-VPE stop_machine window")
    if "of_machine_is_compatible(\"tplink,archer-xr500v\")" not in text:
        errors.append("XR500v machine guard missing")
    if "of_machine_is_compatible(\"econet,en751221\")" not in text:
        errors.append("EN751221 machine guard missing")
    if "if (!allow_snapshot)" not in text:
        errors.append("manual arm gate missing")
    if "gpon_mac_mmio_writes: 0" not in text:
        errors.append("zero GPON write assertion missing")
    if "fifo_data_reads: 0" not in text:
        errors.append("zero FIFO data read assertion missing")
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

    print("PASS: six allowlisted GPON reads in a global guarded mux window")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

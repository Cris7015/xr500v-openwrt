#!/usr/bin/env python3
"""Fail-closed source audit for the XR500v GPON MAC single-read probe."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


FORBIDDEN_CALLS = [
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

    if "#define GPON_ONU_ID_PHYS\t\t0x1fb64000" not in text:
        errors.append("exact G_ONU_ID physical address missing")
    if "#define GPON_ONU_ID_SIZE\t\t0x00000004" not in text:
        errors.append("four-byte mapping constraint missing")
    if text.count("ioread32(") != 1 or (
        "result.onu_id = ioread32(ctx->base);" not in text
    ):
        errors.append("expected exactly one direct G_ONU_ID read")
    if text.count("request_mem_region(") != 1:
        errors.append("expected one four-byte region claim")
    if text.count("ioremap(") != 1 or (
        "ioremap(GPON_ONU_ID_PHYS, GPON_ONU_ID_SIZE)" not in text
    ):
        errors.append("exact four-byte mapping missing")
    if text.count("iounmap(ctx.base);") != 1:
        errors.append("mapping teardown missing")
    if re.search(r"\bbase\s*\+", text):
        errors.append("offset access outside the single mapped register")
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
    if "#include <linux/stop_machine.h>" not in text:
        errors.append("stop_machine API include missing")
    if "preempt_disable();" in text or "local_irq_save(" in text:
        errors.append("CPU-local exclusion must not replace stop_machine")
    if "if (!allow_single_read)" not in text:
        errors.append("manual arm gate missing")
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

    print("PASS: one four-byte GPON read in a global guarded mux window")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

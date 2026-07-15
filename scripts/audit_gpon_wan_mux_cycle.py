#!/usr/bin/env python3
"""Fail-closed source audit for the XR500v WAN-mux cycle module."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


FORBIDDEN_CALLS = [
    r"\b(?:ioread|read[blqw]|__raw_read)[a-z0-9_]*\s*\(",
    r"\b(?:iowrite|write[blqw]|__raw_write)[a-z0-9_]*\s*\(",
    r"\b(?:devm_)?ioremap[a-z0-9_]*\s*\(",
    r"\b(?:devm_)?request_(?:threaded_)?irq\s*\(",
    r"\breset_control_[a-z0-9_]+\s*\(",
    r"\bregmap_(?:write|set_bits|clear_bits)\s*\(",
    r"\bclk_[a-z0-9_]+\s*\(",
    r"\bgpiod_(?:set|direction_output)[a-z0-9_]*\s*\(",
    r"\bi2c_[a-z0-9_]+\s*\(",
    r"\b(?:msleep|usleep_range|fsleep|udelay|ndelay)\s*\(",
    r"\b(?:schedule_work|queue_work|timer_setup|hrtimer_init)\s*\(",
]


def audit(source: pathlib.Path, package_makefile: pathlib.Path) -> list[str]:
    text = source.read_text(encoding="utf-8")
    makefile = package_makefile.read_text(encoding="utf-8")
    errors: list[str] = []

    for pattern in FORBIDDEN_CALLS:
        if re.search(pattern, text):
            errors.append(f"forbidden call matched: {pattern}")

    if text.count("regmap_update_bits(") != 2:
        errors.append("expected exactly two WAN-mux update call sites")
    if text.count("regmap_read(") != 5:
        errors.append("expected exactly five SCU read call sites")
    if "original_mode != WAN_MODE_ATM" not in text:
        errors.append("frozen ATM-mode guard missing")
    if "result.wan_after != result.wan_before" not in text:
        errors.append("restore readback gate missing")
    if "goto restore_mode;" not in text:
        errors.append("failed-set rollback path missing")
    if "if (regmap_might_sleep(ctx.scu))" not in text:
        errors.append("atomic regmap guard missing")
    if text.count("stop_machine(") != 1:
        errors.append("expected one global two-VPE stop_machine window")
    if "#include <linux/stop_machine.h>" not in text:
        errors.append("stop_machine API include missing")
    if "preempt_disable();" in text or "local_irq_save(" in text:
        errors.append("CPU-local exclusion must not replace stop_machine")
    if "gpon_mac_mmio_reads: 0" not in text:
        errors.append("zero GPON read assertion missing")
    if "if (!allow_wan_mux_cycle)" not in text:
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

    print("PASS: two WAN-mux writes in a global window with restoration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Capture and validate XR500v phase-27 evidence without actuating hardware.

The ``capture`` subcommand performs read-only SSH commands only.  It never
loads a module, changes ``driver_override``, reboots, powers down, writes MMIO,
touches GPIO, or issues an I2C transaction.  Its purpose is to preserve the
debugfs report immediately after the separately authorised one-shot test.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


STATUS_PATH = "/sys/kernel/debug/xr500v-en7570-los-trace-observer/status"
FACTORY_SHA256 = "401dfdaee77c84649bda100fd5dd85be01c7ea126d0a5cc2116b141c1a07a5e4"
ECANCELED_MIPS = -158
ERANGE = -34

EXPECTED_WRITES = [
    (0x0300, 4, "01000000"),
    (0x0014, 2, "0034"),
    (0x0014, 2, "0074"),
    (0x0024, 1, "02"),
    (0x0159, 1, "10"),
    (0x0014, 2, "0034"),
    (0x0159, 1, "10"),
    (0x0014, 2, "0024"),
    (0x0024, 1, "00"),
    (0x0014, 4, "00240500"),
    (0x011C, 4, "071f3c36"),
    (0x0024, 4, "00000104"),
    (0x0024, 4, "00004104"),
    (0x0120, 4, "051f0000"),
    (0x011C, 4, "061f1c10"),
]

SAMPLE_PLAN = [
    (0, "dense"),
    (5, "dense"),
    (10, "dense"),
    (20, "dense"),
    (50, "critical"),
    (100, "full"),
    (250, "full"),
    (500, "full"),
    (1000, "full"),
    (2000, "full"),
    (5000, "full"),
    (10000, "full"),
]

EXPECTED_GUARDS = [
    ("tiamux", 0x0000, "08001002"),
    ("mpd_targets", 0x0004, "00020000"),
    ("t1delay", 0x0008, "99000020"),
    ("tx_sd", 0x000C, "40000000"),
    ("la_pwd", 0x0014, "00240500"),
    ("bgcken", 0x001C, "555555a5"),
    ("pi_tgen", 0x0020, "06000000"),
    ("p0_cs1", 0x0134, "00020410"),
    ("p0_cs3", 0x013C, "30120010"),
    ("p0_latch", 0x0140, "00000000"),
    ("p1_cs1", 0x0144, "00020410"),
    ("p1_cs3", 0x014C, "30120010"),
    ("p1_latch", 0x0150, "00000000"),
    ("rogue_tx", 0x0168, "34020000"),
    ("erc_filter", 0x016C, "3f2f0f00"),
    ("sw_reset", 0x0300, "00000000"),
]

EXPECTED_STATE_LAYOUT = [
    ("tiamux", 0x0000),
    ("mpd_targets", 0x0004),
    ("t1delay", 0x0008),
    ("tx_sd", 0x000C),
    ("la_pwd", 0x0014),
    ("bgcken", 0x001C),
    ("pi_tgen", 0x0020),
    ("svadc_pd", 0x0024),
    ("apd_dac", 0x0030),
    ("safe_protect", 0x0100),
    ("los_ctrl1", 0x011C),
    ("los_ctrl2", 0x0120),
    ("los_timer", 0x0124),
    ("los_timeout_count", 0x0128),
    ("los_timeout", 0x012C),
    ("p0_cs1", 0x0134),
    ("p0_cs2", 0x0138),
    ("p0_cs3", 0x013C),
    ("p0_latch", 0x0140),
    ("p1_cs1", 0x0144),
    ("p1_cs2", 0x0148),
    ("p1_cs3", 0x014C),
    ("p1_latch", 0x0150),
    ("adc_probe", 0x0154),
    ("probe_control", 0x0158),
    ("apd_ovp_latch", 0x0164),
    ("rogue_tx", 0x0168),
    ("erc_filter", 0x016C),
    ("sw_reset", 0x0300),
]

EXPECTED_COLD_STATE = {
    "tiamux": "08001002",
    "mpd_targets": "00020000",
    "t1delay": "99000020",
    "tx_sd": "40000000",
    "la_pwd": "00240000",
    "bgcken": "555555a5",
    "pi_tgen": "06000000",
    "svadc_pd": "00000100",
    "apd_dac": "00080000",
    "safe_protect": "ff8fff0f",
    "los_ctrl1": "06083c36",
    "los_ctrl2": "10050000",
    "los_timer": "ffffffff",
    "los_timeout_count": "00000000",
    "los_timeout": "00000000",
    "p0_cs1": "00020410",
    "p0_cs2": "00000000",
    "p0_cs3": "30120010",
    "p0_latch": "00000000",
    "p1_cs1": "00020410",
    "p1_cs2": "00000000",
    "p1_cs3": "30120010",
    "p1_latch": "00000000",
    "adc_probe": "00000000",
    "probe_control": "00000000",
    "apd_ovp_latch": "00000000",
    "rogue_tx": "34020000",
    "erc_filter": "3f2f0f00",
    "sw_reset": "00000000",
}

OPERATION = "fixed OEM EN7570 reset/RSSI/gain/LOS prefix + terminal timestamped trace"
TRACE_POLICY = "dense@0/5/10/20 critical@50 full@100/250/500/1000/2000/5000/10000 ms"
HEX_BYTES_PATTERN = r"[0-9a-f]{2}(?: [0-9a-f]{2})*"
HEX4_PATTERN = r"[0-9a-f]{2}(?: [0-9a-f]{2}){3}"

WRITE_RE = re.compile(
    rf"^write_(\d{{2}}): reg=([0-9a-f]{{4}}) len=(\d+) payload=({HEX_BYTES_PATTERN}) "
    r"attempted=(yes|no) result=(-?\d+) start_ns=(\d+) done_ns=(\d+)$",
    re.M,
)
SAMPLE_RE = re.compile(
    rf"^sample_(?P<index>\d{{2}}): level=(?P<level>dense|critical|full) "
    rf"capture=(?P<capture>-?\d+) verify=(?P<verify>-?\d+) outcome=(?P<outcome>-?\d+) "
    rf"target_ms=(?P<target_ms>\d+) start_us=(?P<start_us>\d+) "
    rf"los2_done_us=(?P<los2_done_us>\d+) end_us=(?P<end_us>\d+) "
    rf"los2=(?P<los2>{HEX4_PATTERN})\(r=(?P<los2_rc>-?\d+)\) "
    rf"timer=(?P<timer>{HEX4_PATTERN})\(r=(?P<timer_rc>-?\d+)\) "
    rf"count=(?P<count>{HEX4_PATTERN})\(r=(?P<count_rc>-?\d+)\) "
    rf"timeout=(?P<timeout>{HEX4_PATTERN})\(r=(?P<timeout_rc>-?\d+)\) "
    rf"debug=(?P<debug>{HEX4_PATTERN})\(r=(?P<debug_rc>-?\d+)\) "
    rf"los1=(?P<los1>{HEX4_PATTERN})\(r=(?P<los1_rc>-?\d+)\) "
    rf"svadc=(?P<svadc>{HEX4_PATTERN})\(r=(?P<svadc_rc>-?\d+)\)$",
    re.M,
)
SNAPSHOT_RE = re.compile(
    r"^(cold|post_reset|terminal): capture=(-?\d+) verify=(-?\d+) "
    r"gpio\(active_low=(\d+) direction=(-?\d+) logical=(-?\d+) raw=(-?\d+)\)$",
    re.M,
)
REGISTER_RE = re.compile(
    rf"^\s{{2}}([a-z0-9_]+)\s+@([0-9a-f]{{4}})=({HEX4_PATTERN})\(r=(-?\d+)\)$",
    re.M,
)
GUARD_RE = re.compile(
    rf"^\s{{2}}guard_([a-z0-9_]+)@([0-9a-f]{{4}})=({HEX4_PATTERN})\(r=(-?\d+)\)$",
    re.M,
)
SAFETY_RE = re.compile(
    rf"^\s{{2}}safety ovp=({HEX4_PATTERN})\(r=(-?\d+)\) "
    rf"apd=({HEX4_PATTERN})\(r=(-?\d+)\) safe=({HEX4_PATTERN})\(r=(-?\d+)\) "
    rf"ibias=({HEX4_PATTERN})\(r=(-?\d+)\) imod=({HEX4_PATTERN})\(r=(-?\d+)\)$",
    re.M,
)
SAMPLE_XPON_RE = re.compile(
    r"^\s{2}gpio\(active_low=(\d+) direction=(-?\d+) logical=(-?\d+) raw=(-?\d+)\) "
    r"physet3=([0-9a-f]{8}) physet10=([0-9a-f]{8}) physta1=([0-9a-f]{8}) "
    r"fsm=(\d+) setting=([0-9a-f]{8}) misc=([0-9a-f]{8}) rx=([0-9a-f]{8}) "
    r"sync=([0-9a-f]+) sync_ok=(\d+) rx_hi=([0-9a-f]{2}) bit15=(\d+) "
    r"sta=([0-9a-f]{8}) los=(\d+) prbs=([0-9a-f]{8}) test=([0-9a-f]{8}) "
    r"irq=([0-9a-f]{8})$",
    re.M,
)
SNAPSHOT_XPON_RE = re.compile(
    r"^\s{2}xpon physet3=([0-9a-f]{8}) physet10=([0-9a-f]{8}) physta1=([0-9a-f]{8}) "
    r"fsm=(\d+) setting=([0-9a-f]{8}) misc=([0-9a-f]{8}) rx=([0-9a-f]{8}) "
    r"sync=([0-9a-f]+) sync_ok=(\d+) rx_hi=([0-9a-f]{2}) bit15=(\d+) "
    r"sta=([0-9a-f]{8}) los=(\d+) prbs=([0-9a-f]{8}) test=([0-9a-f]{8}) "
    r"irq=([0-9a-f]{8})$",
    re.M,
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _compact_hex(value: str) -> str:
    return value.replace(" ", "")


def _scalar(raw: str, key: str, errors: list[str]) -> str | None:
    matches = re.findall(rf"^{re.escape(key)}:\s*(.*?)\s*$", raw, re.M)
    if len(matches) != 1:
        errors.append(f"{key}: expected exactly one field, found {len(matches)}")
        return None
    return matches[0]


def _integer(value: str | None, label: str, errors: list[str]) -> int | None:
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        errors.append(f"{label}: invalid integer {value!r}")
        return None


def _reject_positive_result(value: int | None, label: str, errors: list[str]) -> None:
    if value is not None and value > 0:
        errors.append(f"{label}: helper result cannot be positive ({value})")


def _check_xpon_gate(
    block: str,
    label: str,
    errors: list[str],
    *,
    sample_gpio: bool = True,
    enforce_safe: bool = True,
) -> list[int] | None:
    pattern = SAMPLE_XPON_RE if sample_gpio else SNAPSHOT_XPON_RE
    matches = list(pattern.finditer(block))
    if len(matches) != 1:
        errors.append(f"{label}: expected one GPIO/xPON gate line, found {len(matches)}")
        return None
    if sample_gpio:
        bases = (10, 10, 10, 10, 16, 16, 16, 10, 16, 16, 16, 16, 10, 16, 10, 16, 10, 16, 16, 16)
    else:
        bases = (16, 16, 16, 10, 16, 16, 16, 16, 10, 16, 10, 16, 10, 16, 16, 16)
    values = [int(item, base) for item, base in zip(matches[0].groups(), bases)]
    if sample_gpio:
        if values[0] not in (0, 1):
            errors.append(f"{label}: GPIO active_low is not boolean")
        for field, value in zip(("direction", "logical", "raw"), values[1:4]):
            if value > 1:
                errors.append(f"{label}: GPIO {field} cannot exceed 1 ({value})")
    if not _xpon_derived_fields_are_consistent(values, sample_gpio=sample_gpio):
        errors.append(f"{label}: xPON derived fields do not match their source registers")
    if enforce_safe and not _xpon_gate_is_safe(values, sample_gpio=sample_gpio):
        errors.append(f"{label}: GPIO/xPON safety gate is not retained")
    return values


def _xpon_derived_fields_are_consistent(values: list[int], *, sample_gpio: bool) -> bool:
    if sample_gpio:
        physta1, fsm = values[6], values[7]
        rx, sync, sync_ok, rx_hi, bit15 = values[10:15]
        sta, los = values[15], values[16]
    else:
        physta1, fsm = values[2], values[3]
        rx, sync, sync_ok, rx_hi, bit15 = values[6:11]
        sta, los = values[11], values[12]
    return bool(
        fsm == ((physta1 >> 18) & 0x7)
        and sync == (rx & 0xF)
        and sync_ok == int(sync == 0xA)
        and rx_hi == ((rx >> 8) & 0xFF)
        and bit15 == int(bool(rx & (1 << 15)))
        and los == int(bool(sta & 1))
    )


def _xpon_gate_is_safe(values: list[int], *, sample_gpio: bool) -> bool:
    if sample_gpio:
        active_low, direction, logical, raw = values[:4]
        physet3, physet10 = values[4], values[5]
        setting, misc = values[8], values[9]
        prbs, test, irq = values[17], values[18], values[19]
        gpio_safe = (active_low, direction, logical, raw) == (0, 0, 1, 1)
    else:
        physet3, physet10 = values[0], values[1]
        setting, misc = values[4], values[5]
        prbs, test, irq = values[13], values[14], values[15]
        gpio_safe = True
    return bool(
        _xpon_derived_fields_are_consistent(values, sample_gpio=sample_gpio)
        and gpio_safe
        and not (physet3 & (1 << 5))
        and (physet10 & (1 << 31))
        and setting == 0x14F
        and not (misc & (1 << 28))
        and not prbs
        and not test
        and not irq
    )


def _snapshot_blocks(raw: str, errors: list[str]) -> dict[str, dict[str, Any]]:
    matches = list(SNAPSHOT_RE.finditer(raw))
    names = [match.group(1) for match in matches]
    if names != ["cold", "post_reset", "terminal"]:
        errors.append(f"snapshots: expected cold/post_reset/terminal once and in order, got {names}")
    first_write = raw.find("write_01:")
    result: dict[str, dict[str, Any]] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else first_write
        if end < 0:
            end = len(raw)
        block = raw[match.end() : end]
        register_rows = [
            (name, int(reg, 16), _compact_hex(value), int(rc))
            for name, reg, value, rc in REGISTER_RE.findall(block)
        ]
        registers = {name: (reg, value, rc) for name, reg, value, rc in register_rows}
        name = match.group(1)
        capture_result = int(match.group(2))
        verify_result = int(match.group(3))
        _reject_positive_result(capture_result, f"{name} capture", errors)
        _reject_positive_result(verify_result, f"{name} verify", errors)
        gpio = tuple(int(match.group(i)) for i in range(4, 8))
        if gpio[0] not in (0, 1):
            errors.append(f"{name}: GPIO active_low is not boolean")
        for field, value in zip(("direction", "logical", "raw"), gpio[1:]):
            if value > 1:
                errors.append(f"{name}: GPIO {field} cannot exceed 1 ({value})")
        for register_name, _reg, _value, rc in register_rows:
            _reject_positive_result(rc, f"{name} {register_name} read", errors)
        xpon_values = _check_xpon_gate(
            block,
            name,
            errors,
            sample_gpio=False,
            enforce_safe=capture_result == 0 and verify_result == 0,
        )
        result[name] = {
            "capture": capture_result,
            "verify": verify_result,
            "gpio": gpio,
            "registers": registers,
            "xpon": xpon_values,
        }
        if len(registers) != 29:
            errors.append(f"{name}: expected 29 EN7570 registers, found {len(registers)}")
        if [(row[0], row[1]) for row in register_rows] != EXPECTED_STATE_LAYOUT:
            errors.append(f"{name}: EN7570 register layout/order differs from the frozen map")
        if result[name]["capture"] == 0 and result[name]["verify"] == 0:
            if result[name]["gpio"] != (0, 0, 1, 1):
                errors.append(f"{name}: snapshot GPIO gate is not active-high/output/high/high")
            if any(row[3] != 0 for row in register_rows):
                errors.append(f"{name}: verify=0 snapshot contains a failed register read")
            if name in ("cold", "post_reset"):
                for field, expected in EXPECTED_COLD_STATE.items():
                    row = registers.get(field)
                    if row is None or row[1] != expected:
                        errors.append(f"{name}: {field} differs from cold value {expected}")
            elif name == "terminal":
                terminal_fixed = dict(EXPECTED_COLD_STATE)
                terminal_fixed.update(
                    la_pwd="00240500",
                    svadc_pd="00004104",
                    los_ctrl1="061f1c10",
                )
                for field in ("los_ctrl2", "los_timer", "los_timeout_count", "los_timeout", "adc_probe"):
                    terminal_fixed.pop(field)
                for field, expected in terminal_fixed.items():
                    row = registers.get(field)
                    if row is None or row[1] != expected:
                        errors.append(f"terminal: {field} differs from fixed value {expected}")
                los2 = registers.get("los_ctrl2")
                if los2 is None or los2[1][:4] != "051f" or los2[1][6:] != "00":
                    errors.append("terminal: LOS_CTRL2 must be 05 1f ?? 00")
            for field, expected in (
                ("apd_dac", "00080000"),
                ("safe_protect", "ff8fff0f"),
                ("apd_ovp_latch", "00000000"),
            ):
                row = registers.get(field)
                if row is None or row[1:] != (expected, 0):
                    errors.append(f"{name}: {field} is absent, failed, or differs from {expected}")
    return result


def _snapshot_state_matches(
    snapshot: dict[str, Any], stage: str, *, rssi_value: int | None = None
) -> bool:
    registers = snapshot["registers"]
    xpon_values = snapshot.get("xpon")
    if snapshot["gpio"] != (0, 0, 1, 1):
        return False
    if xpon_values is None or not _xpon_gate_is_safe(xpon_values, sample_gpio=False):
        return False
    if any(row[2] != 0 for row in registers.values()):
        return False
    if stage in ("cold", "post_reset"):
        return all(registers.get(name, (None, None, None))[1] == value for name, value in EXPECTED_COLD_STATE.items())
    if stage != "terminal" or rssi_value is None:
        return False
    expected = dict(EXPECTED_COLD_STATE)
    expected.update(
        la_pwd="00240500",
        svadc_pd="00004104",
        los_ctrl1="061f1c10",
        adc_probe=f"{rssi_value & 0xff:02x}{rssi_value >> 8:02x}0000",
    )
    for dynamic in ("los_ctrl2", "los_timer", "los_timeout_count", "los_timeout"):
        expected.pop(dynamic)
    if not all(registers.get(name, (None, None, None))[1] == value for name, value in expected.items()):
        return False
    los2 = registers.get("los_ctrl2")
    return bool(los2 and los2[1][:4] == "051f" and los2[1][6:] == "00")


def _snapshot_expected_capture(snapshot: dict[str, Any]) -> int:
    for name, _reg in EXPECTED_STATE_LAYOUT:
        row = snapshot["registers"].get(name)
        if row is not None and row[2] != 0:
            return row[2]
    for value in snapshot["gpio"][1:]:
        if value < 0:
            return value
    return 0


def _snapshot_is_zero_initialized(snapshot: dict[str, Any]) -> bool:
    return bool(
        snapshot["gpio"] == (0, 0, 0, 0)
        and len(snapshot["registers"]) == len(EXPECTED_STATE_LAYOUT)
        and all(value == "00000000" and rc == 0 for _reg, value, rc in snapshot["registers"].values())
        and snapshot.get("xpon") is not None
        and all(value == 0 for value in snapshot["xpon"])
    )


def _check_rssi_progress(
    attempted_count: int,
    write_results: list[int],
    sequence_result: int,
    rssi_match: re.Match[str] | None,
    readbacks_match: re.Match[str] | None,
    errors: list[str],
) -> None:
    if rssi_match is None or readbacks_match is None:
        return
    vref, value = (int(rssi_match.group(index), 16) for index in (1, 2))
    latch_initial, latch_second = rssi_match.group(4), rssi_match.group(5)
    readbacks = tuple(_compact_hex(item) for item in readbacks_match.groups())
    allowed_pairs = {(0x020A, 0x0284), (0x020A, 0x0285), (0x020B, 0x0285), (0x020B, 0x0286)}

    def boundary_write_failed(one_based_index: int) -> bool:
        return bool(
            attempted_count == one_based_index
            and len(write_results) >= one_based_index
            and write_results[one_based_index - 1] != 0
        )

    if attempted_count == 0:
        if (vref, value, int(rssi_match.group(3), 16)) != (0, 0, 0):
            errors.append("rssi_oracle: zero-write abort must retain zero-initialized values")
        if (latch_initial, latch_second) != ("00", "00") or any(
            readback != "00000000" for readback in readbacks
        ):
            errors.append("rssi_readbacks: zero-write abort must retain zero-initialized evidence")
    if attempted_count <= 4 and vref != 0:
        errors.append("rssi_oracle: Vref changed before its first possible read")
    if boundary_write_failed(5) and vref != 0:
        errors.append("rssi_oracle: failed write_05 cannot produce a Vref read")
    if attempted_count <= 6 and value != 0:
        errors.append("rssi_oracle: V changed before its first possible read")
    if boundary_write_failed(7) and value != 0:
        errors.append("rssi_oracle: failed write_07 cannot produce a V read")
    if boundary_write_failed(1) and latch_initial != "00":
        errors.append("rssi_oracle: failed write_01 cannot produce an initial latch read")
    if attempted_count <= 5 and latch_second != "00":
        errors.append("rssi_oracle: second ADC latch changed before its first possible read")
    if boundary_write_failed(6) and latch_second != "00":
        errors.append("rssi_oracle: failed write_06 cannot produce a second latch read")
    if attempted_count >= 2 and latch_initial != "00":
        errors.append("rssi_oracle: reaching write_02 requires a clear initial ADC latch")
    if attempted_count >= 6 and vref not in (0x020A, 0x020B):
        errors.append("rssi_oracle: reaching write_06 requires an accepted Vref")
    if attempted_count == 5 and vref != 0 and vref not in (0x020A, 0x020B):
        if sequence_result != ERANGE:
            errors.append(
                "sequence_result: a nonzero rejected Vref after write_05 must return -ERANGE"
            )
    if attempted_count >= 7 and latch_second != "00":
        errors.append("rssi_oracle: reaching write_07 requires a clear second ADC latch")
    if attempted_count >= 8 and (vref, value) not in allowed_pairs:
        errors.append("rssi_oracle: reaching write_08 requires one accepted Vref/V pair")
    if attempted_count == 7 and value != 0 and (vref, value) not in allowed_pairs:
        if sequence_result != ERANGE:
            errors.append(
                "sequence_result: a nonzero rejected V after write_07 must return -ERANGE"
            )
    if attempted_count >= 10:
        adc = f"{value & 0xff:02x}{value >> 8:02x}0000"
        expected_calibration = ("00240000", "00000100", adc, "00000000")
        if readbacks[:4] != expected_calibration:
            errors.append("rssi_readbacks: reaching write_10 requires exact calibration readbacks")
    elif attempted_count <= 8 and any(readback != "00000000" for readback in readbacks[:4]):
        errors.append("rssi_readbacks: calibration evidence changed before its first possible read")
    if boundary_write_failed(9) and any(readback != "00000000" for readback in readbacks[:4]):
        errors.append("rssi_readbacks: failed write_09 cannot produce calibration evidence")
    if attempted_count >= 11 and readbacks[4] != "00240500":
        errors.append("rssi_readbacks: reaching write_11 requires the exact gain readback")
    elif attempted_count <= 9 and readbacks[4] != "00000000":
        errors.append("rssi_readbacks: gain evidence changed before its first possible read")
    if boundary_write_failed(10) and readbacks[4] != "00000000":
        errors.append("rssi_readbacks: failed write_10 cannot produce gain evidence")
    if attempted_count >= 15 and readbacks[5] != "061f3c36":
        errors.append("rssi_readbacks: reaching write_15 requires the exact LOS trigger readback")
    elif attempted_count <= 13 and readbacks[5] != "00000000":
        errors.append("rssi_readbacks: LOS trigger changed before its first possible read")
    if boundary_write_failed(14) and readbacks[5] != "00000000":
        errors.append("rssi_readbacks: failed write_14 cannot produce LOS trigger evidence")


def validate_status(raw: str) -> dict[str, Any]:
    errors: list[str] = []
    warnings: list[str] = []
    if "\x00" in raw:
        errors.append("status contains a NUL byte")
    if not raw.endswith("\n"):
        warnings.append("status does not end with a newline; capture may be truncated")

    exact_fields = {
        "operation": OPERATION,
        "trace_policy": TRACE_POLICY,
        "silicon_id": "0x03",
        "silicon_variant": "0x01",
        "factory_length": "0x190",
        "factory_sha256": FACTORY_SHA256,
        "factory_hash_matched": "yes",
        "mmio_write_attempts": "0 / 0 maximum",
        "apd_write_attempts": "0 / 0 maximum",
        "observer_retry_count": "0",
        "software_rollback": "impossible/not attempted",
        "esd_or_deglitch_write": "no",
        "periodic_worker": "no",
        "xpon_mmio_write": "no",
        "apd_write": "no",
        "tx_current_laser_tgen": "no",
        "arbitrary_write_path": "no",
    }
    scalars: dict[str, str | None] = {}
    for key, expected in exact_fields.items():
        scalars[key] = _scalar(raw, key, errors)
        if scalars[key] is not None and scalars[key] != expected:
            errors.append(f"{key}: got {scalars[key]!r}, expected {expected!r}")

    tx_gpio = _scalar(raw, "tx_disable_gpio", errors)
    if tx_gpio is not None and not re.fullmatch(r"\d+ \(hardware offset 16\)", tx_gpio):
        errors.append(f"tx_disable_gpio: unexpected value {tx_gpio!r}")
    module_pinned = _scalar(raw, "module_pinned", errors)
    retries = _scalar(raw, "adapter_retries", errors)
    if retries is not None and not re.fullmatch(r"saved=\d+ during=0 restored=yes", retries):
        errors.append(f"adapter_retries: unsafe or malformed value {retries!r}")

    sequence_result = _integer(_scalar(raw, "sequence_result", errors), "sequence_result", errors)
    halted_step = _integer(_scalar(raw, "halted_step", errors), "halted_step", errors)
    halted_sample = _integer(_scalar(raw, "halted_sample", errors), "halted_sample", errors)
    trace_result = _integer(_scalar(raw, "trace_result", errors), "trace_result", errors)
    outcome_result = _integer(_scalar(raw, "outcome_result", errors), "outcome_result", errors)
    terminal_outcome = _integer(_scalar(raw, "terminal_outcome", errors), "terminal_outcome", errors)
    prefix_end_ns = _integer(_scalar(raw, "prefix_end_ns", errors), "prefix_end_ns", errors)
    for label, value in (
        ("sequence_result", sequence_result),
        ("trace_result", trace_result),
        ("outcome_result", outcome_result),
        ("terminal_outcome", terminal_outcome),
    ):
        _reject_positive_result(value, label, errors)

    rssi_oracle = _scalar(raw, "rssi_oracle", errors)
    rssi_match = re.fullmatch(
        r"vref=0x([0-9a-f]{4}) v=0x([0-9a-f]{4}) delta=0x([0-9a-f]{4}) "
        r"latch_initial=([0-9a-f]{2}) latch_second=([0-9a-f]{2})",
        rssi_oracle or "",
    )
    rssi_readbacks = _scalar(raw, "rssi_readbacks", errors)
    readbacks_match = re.fullmatch(
        rf"la=({HEX4_PATTERN}) svadc=({HEX4_PATTERN}) adc=({HEX4_PATTERN}) "
        rf"probe=({HEX4_PATTERN}) gain_la=({HEX4_PATTERN}) los_trigger=({HEX4_PATTERN})",
        rssi_readbacks or "",
    )
    if rssi_match is None:
        errors.append(f"rssi_oracle: malformed value {rssi_oracle!r}")
    else:
        vref, value, delta = (int(rssi_match.group(i), 16) for i in range(1, 4))
        if delta != (value - vref if value >= vref else 0):
            errors.append("rssi_oracle: delta does not match v and vref")
    if readbacks_match is None:
        errors.append(f"rssi_readbacks: malformed value {rssi_readbacks!r}")

    snapshots = _snapshot_blocks(raw, errors)

    write_matches = list(WRITE_RE.finditer(raw))
    if len(write_matches) != 15:
        errors.append(f"writes: expected 15 report rows, found {len(write_matches)}")
    attempted_flags: list[bool] = []
    write_results: list[int] = []
    write_done_ns: list[int] = []
    last_done = 0
    for position, match in enumerate(write_matches):
        index, reg, length, payload, attempted, rc, start_ns, done_ns = match.groups()
        expected_index = position + 1
        if int(index) != expected_index:
            errors.append(f"writes: row {position + 1} is write_{index}, expected write_{expected_index:02d}")
        if position < len(EXPECTED_WRITES):
            exp_reg, exp_len, exp_payload = EXPECTED_WRITES[position]
            if (int(reg, 16), int(length), _compact_hex(payload)) != (exp_reg, exp_len, exp_payload):
                errors.append(f"write_{expected_index:02d}: fixed register/length/payload differs")
        is_attempted = attempted == "yes"
        attempted_flags.append(is_attempted)
        rc_i, start_i, done_i = int(rc), int(start_ns), int(done_ns)
        _reject_positive_result(rc_i, f"write_{expected_index:02d} result", errors)
        write_results.append(rc_i)
        write_done_ns.append(done_i)
        if is_attempted:
            if start_i <= 0 or done_i < start_i:
                errors.append(f"write_{expected_index:02d}: invalid attempted timestamps")
            if last_done and start_i < last_done:
                errors.append(f"write_{expected_index:02d}: timestamp overlaps prior write")
            last_done = done_i
        elif rc_i != ECANCELED_MIPS or start_i or done_i:
            errors.append(
                f"write_{expected_index:02d}: unattempted row must be rc={ECANCELED_MIPS} with zero timestamps"
            )
    attempted_count = sum(attempted_flags)
    if attempted_flags != [True] * attempted_count + [False] * (len(attempted_flags) - attempted_count):
        errors.append("writes: attempted rows do not form one contiguous prefix")

    write_count_text = _scalar(raw, "i2c_write_attempts", errors)
    write_count_match = re.fullmatch(r"(\d+) / 15 maximum", write_count_text or "")
    if not write_count_match:
        errors.append(f"i2c_write_attempts: malformed value {write_count_text!r}")
    elif int(write_count_match.group(1)) != attempted_count:
        errors.append("i2c_write_attempts does not equal attempted=yes rows")

    failed_writes = [
        position + 1
        for position, match in enumerate(write_matches)
        if match.group(5) == "yes" and int(match.group(6)) != 0
    ]
    if failed_writes and failed_writes != [attempted_count]:
        errors.append(f"writes: failed attempts must be only the final attempted row, got {failed_writes}")
    if failed_writes:
        failed_rc = write_results[failed_writes[0] - 1]
        if sequence_result != failed_rc:
            errors.append(
                f"sequence_result is {sequence_result}, expected failed write result {failed_rc}"
            )
    _check_rssi_progress(
        attempted_count,
        write_results,
        sequence_result,
        rssi_match,
        readbacks_match,
        errors,
    )

    # A surviving status file means los_trace_run() passed its global claim.
    # It pins the module and sets the physical recovery boundary before even
    # the first fast gate, so these are mandatory with zero attempts too.
    if module_pinned != "yes":
        errors.append("module_pinned must be yes whenever phase-27 status exists")

    sample_matches = list(SAMPLE_RE.finditer(raw))
    samples_text = _scalar(raw, "samples_taken", errors)
    samples_count_match = re.fullmatch(r"(\d+) / 12", samples_text or "")
    samples_count = int(samples_count_match.group(1)) if samples_count_match else None
    if samples_count is None:
        errors.append(f"samples_taken: malformed value {samples_text!r}")
    elif samples_count != len(sample_matches):
        errors.append("samples_taken does not equal the number of sample rows")
    if len(sample_matches) > len(SAMPLE_PLAN):
        errors.append(f"samples: found {len(sample_matches)}, maximum is {len(SAMPLE_PLAN)}")

    accepted: list[int] = []
    failed_samples: list[int] = []
    previous_start = -1
    previous_end = -1
    footer_at = raw.find("tx_disable_asserted:")
    for position, match in enumerate(sample_matches):
        fields = match.groupdict()
        index = fields["index"]
        level = fields["level"]
        target_ms = fields["target_ms"]
        end = sample_matches[position + 1].start() if position + 1 < len(sample_matches) else footer_at
        if end < 0:
            end = len(raw)
        block = raw[match.end() : end]
        if int(index) != position:
            errors.append(f"samples: row {position} is sample_{index}, expected sample_{position:02d}")
        if position < len(SAMPLE_PLAN):
            exp_target, exp_level = SAMPLE_PLAN[position]
            if (int(target_ms), level) != (exp_target, exp_level):
                errors.append(f"sample_{position:02d}: target/level differs from the frozen schedule")
        capture_i = int(fields["capture"])
        verify_i = int(fields["verify"])
        outcome_i = int(fields["outcome"])
        _reject_positive_result(capture_i, f"sample_{position:02d} capture", errors)
        _reject_positive_result(verify_i, f"sample_{position:02d} verify", errors)
        _reject_positive_result(outcome_i, f"sample_{position:02d} outcome", errors)
        for field in ("los2", "timer", "count", "timeout", "debug", "los1", "svadc"):
            _reject_positive_result(
                int(fields[f"{field}_rc"]), f"sample_{position:02d} {field} read", errors
            )
        if capture_i != 0 and verify_i != capture_i:
            errors.append(f"sample_{position:02d}: failed capture/result does not propagate to verify")
        start_i = int(fields["start_us"])
        los2_done_i = int(fields["los2_done_us"])
        end_i = int(fields["end_us"])
        if not start_i <= los2_done_i <= end_i:
            errors.append(f"sample_{position:02d}: timestamps are not ordered")
        if start_i < previous_start:
            errors.append(f"sample_{position:02d}: start timestamp moved backwards")
        if previous_end >= 0 and start_i < previous_end:
            errors.append(f"sample_{position:02d}: overlaps the preceding synchronous sample")
        previous_start = start_i
        previous_end = end_i
        if position < len(SAMPLE_PLAN) and start_i < SAMPLE_PLAN[position][0] * 1000:
            errors.append(f"sample_{position:02d}: started before its absolute deadline")
        lateness = start_i - int(target_ms) * 1000
        if lateness > 5000:
            warnings.append(f"sample_{position:02d}: start is {lateness} us late")

        los2_compact = _compact_hex(fields["los2"])
        byte2 = int(los2_compact[4:6], 16)
        accepted_sample = capture_i == 0 and verify_i == 0
        gate_values = _check_xpon_gate(
            block,
            f"sample_{position:02d}",
            errors,
            enforce_safe=accepted_sample,
        )
        safety_matches = list(SAFETY_RE.finditer(block))
        guards = [
            (name, int(reg, 16), _compact_hex(value), int(rc))
            for name, reg, value, rc in GUARD_RE.findall(block)
        ]
        for safety_match in safety_matches:
            for group_index in (2, 4, 6, 8, 10):
                _reject_positive_result(
                    int(safety_match.group(group_index)),
                    f"sample_{position:02d} safety read",
                    errors,
                )
        for guard_name, _reg, _value, rc in guards:
            _reject_positive_result(rc, f"sample_{position:02d} guard_{guard_name} read", errors)
        block_lines = block.splitlines()
        dense_marker_count = block_lines.count("  safety EN7570=not-sampled (dense level)")
        static_marker_count = block_lines.count("  static_tx_guards=not-sampled")
        expected_safety = (
            "00000000", 0,
            "00080000", 0,
            "ff8fff0f", 0,
            "00000000", 0,
            "00000000", 0,
        )
        safety_groups = safety_matches[0].groups() if len(safety_matches) == 1 else ()
        normalized_safety: tuple[str | int, ...] = tuple(
            _compact_hex(value) if group_index % 2 == 0 else int(value)
            for group_index, value in enumerate(safety_groups)
        )
        level_structure_safe = False
        if level == "dense":
            level_structure_safe = (
                dense_marker_count == 1
                and not safety_matches
                and static_marker_count == 1
                and not guards
            )
            if dense_marker_count != 1 or safety_matches:
                errors.append(f"sample_{position:02d}: dense safety structure differs")
            if static_marker_count != 1 or guards:
                errors.append(f"sample_{position:02d}: dense static-guard structure differs")
            for field in ("count", "los1", "svadc"):
                if _compact_hex(fields[field]) != "00000000" or int(fields[f"{field}_rc"]) != ECANCELED_MIPS:
                    errors.append(f"sample_{position:02d}: dense {field} was unexpectedly sampled")
        elif level == "critical":
            level_structure_safe = (
                len(safety_matches) == 1
                and not guards
                and static_marker_count == 1
            )
            if len(safety_matches) != 1:
                errors.append(f"sample_{position:02d}: critical safety row is absent or duplicated")
            if guards or static_marker_count != 1:
                errors.append(f"sample_{position:02d}: critical static-guard structure differs")
        else:
            guard_layout = [(name, reg) for name, reg, _value, _rc in guards]
            expected_guard_layout = [(name, reg) for name, reg, _value in EXPECTED_GUARDS]
            level_structure_safe = (
                len(safety_matches) == 1
                and guard_layout == expected_guard_layout
                and static_marker_count == 0
            )
            if len(safety_matches) != 1:
                errors.append(f"sample_{position:02d}: full safety row is absent or duplicated")
            if guard_layout != expected_guard_layout:
                errors.append(f"sample_{position:02d}: full static-guard layout differs")
            if static_marker_count:
                errors.append(f"sample_{position:02d}: full sample contains omission marker")

        base_map_safe = bool(
            int(fields["los2_rc"]) == 0
            and int(fields["timer_rc"]) == 0
            and int(fields["timeout_rc"]) == 0
            and int(fields["debug_rc"]) == 0
            and los2_compact[:4] == "051f"
            and los2_compact[6:] == "00"
        )
        if level == "dense":
            level_map_safe = all(
                _compact_hex(fields[field]) == "00000000"
                and int(fields[f"{field}_rc"]) == ECANCELED_MIPS
                for field in ("count", "los1", "svadc")
            )
        else:
            level_map_safe = bool(
                int(fields["count_rc"]) == 0
                and _compact_hex(fields["los1"]) == "061f1c10"
                and int(fields["los1_rc"]) == 0
                and _compact_hex(fields["svadc"]) == "00004104"
                and int(fields["svadc_rc"]) == 0
                and normalized_safety == expected_safety
                and (level != "full" or guards == [(*guard, 0) for guard in EXPECTED_GUARDS])
            )
        gate_safe = bool(
            gate_values is not None
            and _xpon_gate_is_safe(gate_values, sample_gpio=True)
        )
        verification_map_safe = bool(
            level_structure_safe and base_map_safe and level_map_safe and gate_safe
        )

        capture_order = ["los2", "timer", "timeout", "debug"]
        if level != "dense":
            capture_order.extend(("count", "los1", "svadc"))
        capture_results = [int(fields[f"{field}_rc"]) for field in capture_order]
        if level != "dense" and safety_matches:
            capture_results.extend(int(safety_matches[0].group(index)) for index in (2, 4, 6, 8, 10))
        if level == "full":
            capture_results.extend(rc for _name, _reg, _value, rc in guards)
        if gate_values is not None:
            capture_results.extend(value for value in gate_values[1:4] if value < 0)
        expected_capture = next((result for result in capture_results if result != 0), 0)
        if capture_i != expected_capture:
            errors.append(
                f"sample_{position:02d}: capture={capture_i} does not equal first read/GPIO error {expected_capture}"
            )
        expected_verify = capture_i if capture_i != 0 else (0 if verification_map_safe else -1)
        if verify_i != expected_verify:
            errors.append(
                f"sample_{position:02d}: verify={verify_i} does not follow capture/map result {expected_verify}"
            )

        if accepted_sample:
            expected_outcome = 0 if byte2 in (0x22, 0x23) else ERANGE
            if (
                int(fields["los2_rc"]) != 0
                or los2_compact[:4] != "051f"
                or los2_compact[6:] != "00"
                or outcome_i != expected_outcome
            ):
                errors.append(f"sample_{position:02d}: accepted outcome/result is inconsistent")
            for field in ("timer", "timeout", "debug"):
                if int(fields[f"{field}_rc"]) != 0:
                    errors.append(f"sample_{position:02d}: accepted {field} read failed")
            if level != "dense":
                if int(fields["count_rc"]) != 0:
                    errors.append(f"sample_{position:02d}: critical timeout-count read failed")
                if _compact_hex(fields["los1"]) != "061f1c10" or int(fields["los1_rc"]) != 0:
                    errors.append(f"sample_{position:02d}: critical LOS_CTRL1 differs")
                if _compact_hex(fields["svadc"]) != "00004104" or int(fields["svadc_rc"]) != 0:
                    errors.append(f"sample_{position:02d}: critical SVADC differs")
            accepted.append(byte2)
            if level != "dense":
                if normalized_safety != expected_safety:
                    errors.append(f"sample_{position:02d}: critical EN7570 safety row differs")
                if level == "full" and guards != [(*guard, 0) for guard in EXPECTED_GUARDS]:
                    errors.append(f"sample_{position:02d}: full static TX guard set differs")
        else:
            failed_samples.append(position)
            if outcome_i != ECANCELED_MIPS:
                errors.append(f"sample_{position:02d}: rejected sample outcome must remain {ECANCELED_MIPS}")

    if failed_samples:
        if failed_samples != [len(sample_matches) - 1]:
            errors.append(f"samples: failed capture/verify must be the final sample, got {failed_samples}")
        if halted_sample != failed_samples[0]:
            errors.append("halted_sample does not identify the rejected final sample")
    elif halted_sample not in (-1, None):
        errors.append("halted_sample is set despite every reported sample being accepted")

    terminal = snapshots.get("terminal")
    if terminal and terminal["capture"] == 0 and terminal["verify"] == 0:
        terminal_los2 = terminal["registers"].get("los_ctrl2")
        if terminal_los2 is None or terminal_los2[2] != 0:
            errors.append("terminal: accepted LOS_CTRL2 is absent or failed")
        else:
            terminal_byte2 = int(terminal_los2[1][4:6], 16)
            accepted.append(terminal_byte2)
            expected_terminal = 0 if terminal_byte2 in (0x22, 0x23) else ERANGE
            if terminal_outcome != expected_terminal:
                errors.append("terminal_outcome is inconsistent with terminal LOS_CTRL2.byte2")

    observations = _scalar(raw, "outcome_observations", errors)
    obs_match = re.fullmatch(
        r"22=(\d+) 23=(\d+) other=(\d+) transitions=(\d+) first=([0-9a-f]{2}) "
        r"final=([0-9a-f]{2}) valid=(\d+) \(samples plus terminal\)",
        observations or "",
    )
    if obs_match:
        count22 = accepted.count(0x22)
        count23 = accepted.count(0x23)
        other = len(accepted) - count22 - count23
        transitions = sum(a != b for a, b in zip(accepted, accepted[1:]))
        expected_obs = (
            count22,
            count23,
            other,
            transitions,
            accepted[0] if accepted else 0,
            accepted[-1] if accepted else 0,
            1 if accepted else 0,
        )
        actual_obs = (
            int(obs_match.group(1)),
            int(obs_match.group(2)),
            int(obs_match.group(3)),
            int(obs_match.group(4)),
            int(obs_match.group(5), 16),
            int(obs_match.group(6), 16),
            int(obs_match.group(7)),
        )
        if actual_obs != expected_obs:
            errors.append(f"outcome_observations: got {actual_obs}, recomputed {expected_obs}")
        # run_los_trace() initializes the global outcome to zero as soon as
        # the complete prefix has established prefix_end_ns.  A capture may
        # then abort before accepting even one observation, so an empty set is
        # zero after that boundary and -ECANCELED only before it.
        expected_global = ERANGE if other else (0 if prefix_end_ns else ECANCELED_MIPS)
        if outcome_result != expected_global:
            errors.append(f"outcome_result: got {outcome_result}, recomputed {expected_global}")
    else:
        errors.append(f"outcome_observations: malformed value {observations!r}")

    tx_asserted = _scalar(raw, "tx_disable_asserted", errors)
    powercut = _scalar(raw, "physical_powercut_required", errors)
    if tx_asserted != "yes":
        errors.append("TX_DISABLE is not asserted after the claimed sequence")
    if powercut != "yes":
        errors.append("physical_powercut_required must be yes whenever phase-27 status exists")

    cold = snapshots.get("cold")
    post_reset = snapshots.get("post_reset")
    terminal = snapshots.get("terminal")
    first_write_succeeded = bool(attempted_count and write_results and write_results[0] == 0)
    for snapshot_name, snapshot in snapshots.items():
        if snapshot_name == "post_reset" and not first_write_succeeded:
            continue
        expected_capture = _snapshot_expected_capture(snapshot)
        if snapshot["capture"] != expected_capture:
            errors.append(
                f"{snapshot_name}: capture={snapshot['capture']} does not equal first register/GPIO error {expected_capture}"
            )
    if cold and (cold["capture"], cold["verify"]) != (0, 0):
        errors.append("cold: surviving status requires a successful cold preflight")
    if post_reset:
        if not first_write_succeeded:
            if (post_reset["capture"], post_reset["verify"]) != (ECANCELED_MIPS, ECANCELED_MIPS):
                errors.append("post_reset: must remain canceled when write_01 did not succeed")
            if not _snapshot_is_zero_initialized(post_reset):
                errors.append("post_reset: unattempted snapshot is not the zero-initialized C structure")
        else:
            expected_post_verify = (
                post_reset["capture"]
                if post_reset["capture"] != 0
                else (0 if _snapshot_state_matches(post_reset, "post_reset") else -1)
            )
            if post_reset["verify"] != expected_post_verify:
                errors.append(
                    f"post_reset: verify={post_reset['verify']} does not follow capture/map result {expected_post_verify}"
                )
            if post_reset["verify"] != 0:
                if attempted_count != 1:
                    errors.append("post_reset: later writes are impossible after a failed reset snapshot")
                if sequence_result != post_reset["verify"]:
                    errors.append("sequence_result does not match failed post_reset verification")
    if terminal:
        if attempted_count == 15:
            terminal_rssi = int(rssi_match.group(2), 16) if rssi_match is not None else None
            expected_terminal_verify = (
                terminal["capture"]
                if terminal["capture"] != 0
                else (
                    0
                    if _snapshot_state_matches(terminal, "terminal", rssi_value=terminal_rssi)
                    else -1
                )
            )
            if terminal["verify"] != expected_terminal_verify:
                errors.append(
                    f"terminal: verify={terminal['verify']} does not follow capture/map result {expected_terminal_verify}"
                )
        elif terminal["verify"] != ECANCELED_MIPS:
            errors.append("terminal: verification is only legal after all 15 write attempts")
        if terminal["verify"] != 0 and terminal_outcome != ECANCELED_MIPS:
            errors.append("terminal_outcome must remain canceled when terminal verification failed")
        if terminal["verify"] == 0 and rssi_match is not None:
            terminal_adc = terminal["registers"].get("adc_probe")
            value = int(rssi_match.group(2), 16)
            expected_adc = f"{value & 0xff:02x}{value >> 8:02x}0000"
            if terminal_adc is None or terminal_adc[1] != expected_adc:
                errors.append(f"terminal: ADC probe differs from RSSI oracle value {expected_adc}")

    all_writes_ok = len(write_matches) == 15 and attempted_count == 15 and all(
        result == 0 for result in write_results
    )
    prefix_complete = bool(prefix_end_ns)
    if prefix_complete:
        if not all_writes_ok:
            errors.append("prefix_end_ns is set without 15 successful writes")
        elif prefix_end_ns != write_done_ns[-1]:
            errors.append("prefix_end_ns does not equal write_15 done_ns")
        if not sample_matches:
            errors.append("a completed prefix must capture at least sample_00")
    else:
        if all_writes_ok:
            errors.append("15 successful writes must establish prefix_end_ns")
        if sample_matches:
            errors.append("trace samples are impossible before prefix_end_ns")
        if trace_result != ECANCELED_MIPS:
            errors.append("trace_result must remain canceled when the prefix did not complete")

    if prefix_complete:
        if failed_samples:
            final_verify = int(sample_matches[-1].group("verify"))
            if trace_result != final_verify or sequence_result != final_verify:
                errors.append("trace/sequence result does not match the rejected final sample")
        elif len(sample_matches) != len(SAMPLE_PLAN):
            errors.append("a trace without a rejected sample must contain all 12 samples")
        elif trace_result != 0:
            errors.append("12 accepted samples require trace_result=0")
        elif terminal and terminal["verify"] == 0:
            if sequence_result != 0:
                errors.append("accepted trace and terminal snapshot require sequence_result=0")
        elif terminal and sequence_result != terminal["verify"]:
            errors.append("sequence_result does not match failed terminal verification")

    if sequence_result == 0:
        if halted_step != 0:
            errors.append("successful sequence must have halted_step=0")
    else:
        expected_halted_step = attempted_count or 1
        if halted_step != expected_halted_step:
            errors.append(f"halted_step is {halted_step}, expected {expected_halted_step}")

    all_samples_ok = len(sample_matches) == 12 and all(
        int(match.group("capture")) == 0 and int(match.group("verify")) == 0
        for match in sample_matches
    )
    all_snapshots_ok = set(snapshots) == {"cold", "post_reset", "terminal"} and all(
        item["capture"] == 0 and item["verify"] == 0 for item in snapshots.values()
    )
    complete = (
        sequence_result == 0
        and trace_result == 0
        and all_writes_ok
        and prefix_complete
        and all_samples_ok
        and all_snapshots_ok
    )
    if sequence_result == 0 and not complete:
        errors.append("sequence_result=0 but the evidence is incomplete or contains a failed stage")
    if complete:
        if halted_step != 0 or halted_sample != -1:
            errors.append("complete sequence has a nonzero halted_step/halted_sample")
        classification = "complete-new-outcome" if any(v not in (0x22, 0x23) for v in accepted) else "complete-known-outcome"
    else:
        classification = "aborted-needs-powercut"
        warnings.append(f"sequence aborted with result {sequence_result}; physical power cut remains mandatory")

    return {
        "schema": "xr500v-phase27-status-validation-v1",
        "valid": not errors,
        "classification": classification,
        "electrical_sequence_result": sequence_result,
        "trace_result": trace_result,
        "scientific_outcome_result": outcome_result,
        "terminal_outcome_result": terminal_outcome,
        "attempted_writes": attempted_count,
        "accepted_observations": [f"0x{value:02x}" for value in accepted],
        "physical_powercut_required": powercut == "yes",
        "errors": errors,
        "warnings": warnings,
    }


def _print_report(report: dict[str, Any]) -> None:
    state = "VALID" if report["valid"] else "INVALID"
    print(f"{state}: {report['classification']}")
    print(
        f"writes={report['attempted_writes']} sequence={report['electrical_sequence_result']} "
        f"trace={report['trace_result']} outcome={report['scientific_outcome_result']}"
    )
    for warning in report["warnings"]:
        print(f"WARNING: {warning}")
    for error in report["errors"]:
        print(f"ERROR: {error}")


def _exit_code(report: dict[str, Any]) -> int:
    # A coherent abort is valid evidence and must not look like a transport or
    # parser failure to an operator tempted to retry the one-shot sequence.
    return 0 if report["valid"] else 1


def _run_ssh(host: str, command: str, timeout: int) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            host,
            command,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def _run_ssh_captured(host: str, command: str, timeout: int) -> subprocess.CompletedProcess[bytes]:
    try:
        return _run_ssh(host, command, timeout)
    except subprocess.TimeoutExpired as exc:
        return subprocess.CompletedProcess(
            exc.cmd,
            124,
            stdout=exc.stdout or b"",
            stderr=(exc.stderr or b"") + b"\nTIMEOUT\n",
        )
    except OSError as exc:
        return subprocess.CompletedProcess(
            ["ssh", host, command],
            127,
            stdout=b"",
            stderr=f"SSH_EXEC_ERROR: {exc}\n".encode("utf-8", errors="backslashreplace"),
        )


def _write_bytes_durable(path: Path, data: bytes) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("xb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    directory_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def _write_text_durable(path: Path, text: str, *, encoding: str) -> None:
    _write_bytes_durable(path, text.encode(encoding))


def _print_powercut_instruction() -> None:
    print(
        "CORTAR CORRIENTE FISICAMENTE AHORA POR >=35 SEGUNDOS; NO REINTENTAR LA SECUENCIA",
        file=sys.stderr,
        flush=True,
    )


def _capture_impl(args: argparse.Namespace, announce_powercut: Callable[[], None]) -> int:
    output = Path(args.output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = {
        "metadata": (
            "uname -a; cat /proc/sys/kernel/random/boot_id; cat /proc/uptime; "
            "tr '\\000' '\\n' </sys/firmware/devicetree/base/xpon-phy@1faf0000/compatible; "
            "sha256sum /tmp/phase27-los-trace.ko 2>/dev/null || true"
        ),
        "dmesg": "dmesg 2>&1 || true",
        "logread": "logread 2>&1 || true",
    }
    results: dict[str, subprocess.CompletedProcess[bytes]] = {
        "status": _run_ssh_captured(args.host, f"cat {STATUS_PATH}", args.timeout)
    }
    try:
        _write_bytes_durable(output / "status.raw", results["status"].stdout)
        _write_bytes_durable(output / "status.stderr.raw", results["status"].stderr)
    finally:
        # A local disk/fsync failure cannot be allowed to hide the physical
        # recovery boundary. On the success path this still runs only after
        # the raw status and stderr are durable.
        announce_powercut()
    for name, command in commands.items():
        results[name] = _run_ssh_captured(args.host, command, args.timeout)
        _write_bytes_durable(output / f"{name}.raw", results[name].stdout)
        _write_bytes_durable(output / f"{name}.stderr.raw", results[name].stderr)

    try:
        status_text = results["status"].stdout.decode("ascii")
    except UnicodeDecodeError:
        status_text = results["status"].stdout.decode("ascii", errors="replace")
    report = validate_status(status_text)
    if results["status"].returncode != 0:
        report["valid"] = False
        report["errors"].append(f"status SSH command returned {results['status'].returncode}")
    _write_text_durable(
        output / "validation.json",
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    manifest = {
        "schema": "xr500v-phase27-read-only-capture-v1",
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "host": args.host,
        "status_path": STATUS_PATH,
        "hardware_action_performed": False,
        "commands": {
            name: {"returncode": result.returncode, "stdout_sha256": sha256_bytes(result.stdout)}
            for name, result in results.items()
        },
        "validation": {
            "valid": report["valid"],
            "classification": report["classification"],
            "physical_powercut_required": report["physical_powercut_required"],
        },
        # capture-only may be invoked after a transport timeout, so absence or
        # malformed status can never weaken the recovery boundary.
        "next_required_action": "physical power cut for at least 35 seconds; assume invocation was committed",
    }
    _write_text_durable(
        output / "manifest.json",
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    checksum_lines = []
    for path in sorted(output.iterdir()):
        if path.name == "SHA256SUMS" or not path.is_file():
            continue
        checksum_lines.append(f"{sha256_bytes(path.read_bytes())} *{path.name}")
    _write_text_durable(
        output / "SHA256SUMS", "\n".join(checksum_lines) + "\n", encoding="ascii"
    )
    _print_report(report)
    print(f"capture: {output}")
    return _exit_code(report)


def capture(args: argparse.Namespace) -> int:
    announced = False

    def announce_once() -> None:
        nonlocal announced
        if not announced:
            _print_powercut_instruction()
            announced = True

    # The one-shot is committed separately before this capture-only tool is
    # invoked. Therefore *every* exit path, including Ctrl-C before the first
    # SSH byte or a local path error, must retain the physical-cut boundary.
    try:
        return _capture_impl(args, announce_once)
    finally:
        announce_once()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser("validate", help="validate an existing raw status file")
    validate_parser.add_argument("status", type=Path)
    validate_parser.add_argument("--json", dest="json_path", type=Path)
    capture_parser = subparsers.add_parser("capture", help="read and preserve status/logs over read-only SSH")
    capture_parser.add_argument("--host", default="root@192.168.68.222")
    capture_parser.add_argument("--output-dir", required=True)
    capture_parser.add_argument("--timeout", type=int, default=20)
    args = parser.parse_args()

    if args.command == "capture":
        return capture(args)
    raw = args.status.read_text(encoding="ascii")
    report = validate_status(raw)
    if args.json_path:
        args.json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    _print_report(report)
    return _exit_code(report)


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Static safety audit for the compile-only XR500v phase-27 observer."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = (
    ROOT
    / "package/kernel/xr500v-en7570-los-trace-observer/src"
    / "xr500v-en7570-los-trace-observer.c"
)

EXPECTED_WRITES = [
    ("SW_RESET", 4, "01 00 00 00"),
    ("LA_PWD", 2, "00 34 00 00"),
    ("LA_PWD", 2, "00 74 00 00"),
    ("SVADC_PD", 1, "02 00 00 00"),
    ("ADC_LATCH", 1, "10 00 00 00"),
    ("LA_PWD", 2, "00 34 00 00"),
    ("ADC_LATCH", 1, "10 00 00 00"),
    ("LA_PWD", 2, "00 24 00 00"),
    ("SVADC_PD", 1, "00 00 00 00"),
    ("LA_PWD", 4, "00 24 05 00"),
    ("LOS_CTRL1", 4, "07 1f 3c 36"),
    ("SVADC_PD", 4, "00 00 01 04"),
    ("SVADC_PD", 4, "00 00 41 04"),
    ("LOS_CTRL2", 4, "05 1f 00 00"),
    ("LOS_CTRL1", 4, "06 1f 1c 10"),
]

EXPECTED_TARGETS_MS = [0, 5, 10, 20, 50, 100, 250, 500, 1000, 2000, 5000, 10000]
EXPECTED_LEVELS = [
    "DENSE",
    "DENSE",
    "DENSE",
    "DENSE",
    "CRITICAL",
    "FULL",
    "FULL",
    "FULL",
    "FULL",
    "FULL",
    "FULL",
    "FULL",
]
EXPECTED_ORACLE = {
    (0x020A, 0x0284),
    (0x020A, 0x0285),
    (0x020B, 0x0285),
    (0x020B, 0x0286),
}

EXPECTED_GUARDS = [
    ("tiamux", 0x0000, "08 00 10 02"),
    ("mpd_targets", 0x0004, "00 02 00 00"),
    ("t1delay", 0x0008, "99 00 00 20"),
    ("tx_sd", 0x000C, "40 00 00 00"),
    ("la_pwd", 0x0014, "00 24 05 00"),
    ("bgcken", 0x001C, "55 55 55 a5"),
    ("pi_tgen", 0x0020, "06 00 00 00"),
    ("p0_cs1", 0x0134, "00 02 04 10"),
    ("p0_cs3", 0x013C, "30 12 00 10"),
    ("p0_latch", 0x0140, "00 00 00 00"),
    ("p1_cs1", 0x0144, "00 02 04 10"),
    ("p1_cs3", 0x014C, "30 12 00 10"),
    ("p1_latch", 0x0150, "00 00 00 00"),
    ("rogue_tx", 0x0168, "34 02 00 00"),
    ("erc_filter", 0x016C, "3f 2f 0f 00"),
    ("sw_reset", 0x0300, "00 00 00 00"),
]


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def define_int(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)\s*$", source, re.M)
    require(match is not None, f"missing integer define {name}")
    return int(match.group(1), 0)


def parse_fixed_writes(source: str) -> list[tuple[str, int, str]]:
    match = re.search(
        r"static const struct fixed_write fixed_writes\[FIXED_WRITE_COUNT\] = \{(.*?)\n\};",
        source,
        re.S,
    )
    require(match is not None, "fixed_writes initializer is absent")
    rows = re.findall(
        r"\{\s*EN7570_([A-Z0-9_]+),\s*(\d+),\s*\{([^}]*)\}\s*\}",
        match.group(1),
    )
    parsed: list[tuple[str, int, str]] = []
    for register, length, payload in rows:
        values = re.findall(r"0x([0-9a-fA-F]{2})", payload)
        parsed.append((register, int(length), " ".join(value.lower() for value in values)))
    return parsed


def parse_schedule(source: str) -> list[tuple[int, str]]:
    match = re.search(
        r"static const struct trace_schedule_entry trace_schedule"
        r"\[TRACE_SAMPLE_COUNT\] = \{(.*?)\n\};",
        source,
        re.S,
    )
    require(match is not None, "trace schedule initializer is absent")
    rows = re.findall(
        r"\{\s*(\d+),\s*TRACE_GUARD_(DENSE|CRITICAL|FULL)\s*\}",
        match.group(1),
    )
    return [(int(target), level) for target, level in rows]


def parse_guards(source: str) -> list[tuple[str, int, str]]:
    match = re.search(
        r"static const struct en7570_guard_reg en7570_tx_guards\[\] = \{(.*?)\n\};",
        source,
        re.S,
    )
    require(match is not None, "per-sample TX guard initializer is absent")
    rows = re.findall(
        r'\{\s*"([^"]+)",\s*(0x[0-9a-fA-F]+),\s*\{([^}]*)\}\s*\}',
        match.group(1),
    )
    parsed: list[tuple[str, int, str]] = []
    for name, register, payload in rows:
        values = re.findall(r"0x([0-9a-fA-F]{2})", payload)
        parsed.append((name, int(register, 0), " ".join(value.lower() for value in values)))
    return parsed


def enumerate_oracle(source: str) -> set[tuple[int, int]]:
    vref_min = define_int(source, "RSSI_VREF_MIN")
    vref_max = define_int(source, "RSSI_VREF_MAX")
    value_min = define_int(source, "RSSI_V_MIN")
    value_max = define_int(source, "RSSI_V_MAX")
    delta_min = define_int(source, "RSSI_DELTA_MIN")
    delta_max = define_int(source, "RSSI_DELTA_MAX")
    accepted: set[tuple[int, int]] = set()
    for vref in range(0x10000):
        if not vref_min <= vref <= vref_max:
            continue
        for value in range(value_min, value_max + 1):
            if value > vref and delta_min <= value - vref <= delta_max:
                accepted.add((vref, value))
    return accepted


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE
    source = path.read_text(encoding="utf-8")

    require(define_int(source, "FIXED_WRITE_COUNT") == 15, "write budget is not 15")
    require(define_int(source, "TRACE_SAMPLE_COUNT") == 12, "sample budget is not 12")
    require(parse_fixed_writes(source) == EXPECTED_WRITES, "fixed write table differs from oracle")
    require(
        parse_schedule(source) == list(zip(EXPECTED_TARGETS_MS, EXPECTED_LEVELS)),
        "trace target/guard-level schedule differs from oracle",
    )
    require(enumerate_oracle(source) == EXPECTED_ORACLE, "RSSI oracle accepts a different pair set")
    require(parse_guards(source) == EXPECTED_GUARDS, "per-sample TX guards differ from oracle")

    calls = [int(value) for value in re.findall(r"en7570_fixed_write_once\(observer,\s*(\d+)\)", source)]
    require(calls == list(range(15)), "fixed write call sequence is not exactly 0..14")
    require(source.count("ret = __i2c_transfer") == 2, "unexpected raw I2C transfer call count")
    require("&message, 1" in source, "fixed write transfer call is absent")
    require("messages," in source, "pointer-read transfer call is absent")

    forbidden = [
        "iowrite",
        "gpiod_set",
        "regulator_",
        "schedule_work",
        "delayed_work",
        "timer_setup",
        "kthread_",
        "APD_STEP_COUNT",
        "APD_FIRST_WRITE",
        "XPON_SETTING_EN7570",
        "DEFINE_SHOW_STORE_ATTRIBUTE",
        ".write =",
    ]
    for token in forbidden:
        require(token not in source, f"forbidden token present: {token}")

    fixed_block = re.search(
        r"static const struct fixed_write fixed_writes\[FIXED_WRITE_COUNT\] = \{(.*?)\n\};",
        source,
        re.S,
    ).group(1)
    require("EN7570_APD_DAC" not in fixed_block, "APD register appears in write table")

    fast_gate = re.search(
        r"static int fast_tx_gate\(.*?\n\}\n\nstatic void full_snapshot_capture",
        source,
        re.S,
    )
    require(fast_gate is not None, "fast TX gate is absent")
    fast_gate_reads = re.findall(
        r"en7570_read4\(observer,\s*(EN7570_[A-Z0-9_]+)",
        fast_gate.group(0),
    )
    require(
        fast_gate_reads
        == [
            "EN7570_APD_OVP_LATCH",
            "EN7570_SAFE_PROTECT",
            "EN7570_IBIAS",
            "EN7570_IMOD",
        ],
        "fast gate read order differs from phase 26",
    )
    require('debugfs_create_file("status", 0444' in source, "debugfs status is not read-only")
    require("module_param(arm_en7570_los_trace, bool, 0444)" in source, "arm parameter differs")
    for required in (
        "atomic_cmpxchg(&los_trace_sequence_claimed, 0, 1)",
        "__module_get(THIS_MODULE)",
        "observer->en7570->adapter->retries = 0",
        "i2c_lock_bus(observer->en7570->adapter, I2C_LOCK_SEGMENT)",
        "observer->physical_powercut_required = true",
        "SNAPSHOT_TERMINAL",
    ):
        require(required in source, f"missing guard: {required}")

    capture = re.search(
        r"static void trace_sample_capture\(.*?\n\}\n\nstatic int trace_sample_verify",
        source,
        re.S,
    )
    require(capture is not None, "trace sample capture function is absent")
    sample_reads = re.findall(r"en7570_read4\(observer,\s*(EN7570_[A-Z0-9_]+)", capture.group(0))
    require(sample_reads and sample_reads[0] == "EN7570_LOS_CTRL2", "LOS_CTRL2 is not the first sample read")
    for register in (
        "EN7570_APD_OVP_LATCH",
        "EN7570_APD_DAC",
        "EN7570_SAFE_PROTECT",
        "EN7570_IBIAS",
        "EN7570_IMOD",
    ):
        require(register in sample_reads, f"sample does not capture safety register {register}")
    require("en7570_tx_guards[i].reg" in capture.group(0), "sample does not capture all TX guards")

    dense_block, remainder = capture.group(0).split(
        "if (guard_level >= TRACE_GUARD_CRITICAL)", 1
    )
    critical_block, full_block = remainder.split(
        "if (guard_level == TRACE_GUARD_FULL)", 1
    )
    dense_reads = re.findall(
        r"en7570_read4\(observer,\s*(EN7570_[A-Z0-9_]+)", dense_block
    )
    critical_reads = re.findall(
        r"en7570_read4\(observer,\s*(EN7570_[A-Z0-9_]+)", critical_block
    )
    require(
        dense_reads
        == [
            "EN7570_LOS_CTRL2",
            "EN7570_LOS_CAL_TIMER",
            "EN7570_LOS_TIMEOUT",
            "EN7570_LOS_DBG",
        ],
        "dense level is not exactly the four-read oracle",
    )
    require(
        critical_reads
        == [
            "EN7570_LOS_TIMEOUT_COUNT",
            "EN7570_LOS_CTRL1",
            "EN7570_SVADC_PD",
            "EN7570_APD_OVP_LATCH",
            "EN7570_APD_DAC",
            "EN7570_SAFE_PROTECT",
            "EN7570_IBIAS",
            "EN7570_IMOD",
        ],
        "critical level does not add exactly eight reads",
    )
    require(
        full_block.count("en7570_read4(observer, en7570_tx_guards[i].reg") == 1,
        "full level does not add exactly the static guard loop",
    )
    require("? 0 : -ERANGE" not in capture.group(0), "capture classifies outcome before verify")
    require(
        "if (guard_level >= TRACE_GUARD_CRITICAL)" in capture.group(0),
        "critical reads are not level-gated",
    )
    require(
        "if (guard_level == TRACE_GUARD_FULL)" in capture.group(0),
        "static guards are not full-level-gated",
    )
    for omitted_result in (
        "timeout_count_result",
        "los_ctrl1_result",
        "svadc_result",
        "apd_result",
        "ovp_result",
        "safe_result",
        "ibias_result",
        "imod_result",
        "tx_guard_result[i]",
    ):
        require(
            f"sample->{omitted_result} = -ECANCELED" in capture.group(0),
            f"omitted field lacks sentinel: {omitted_result}",
        )

    run_trace = re.search(
        r"static int run_los_trace\(.*?\n\}\n\nstatic int verify_factory_hash",
        source,
        re.S,
    )
    require(run_trace is not None, "trace runner is absent")
    verify_pos = run_trace.group(0).find("trace_sample_verify")
    classify_pos = run_trace.group(0).find("classify_trace_outcome")
    require(0 <= verify_pos < classify_pos, "sample is classified before its safety verdict")
    require("observer->halted_sample = i" in run_trace.group(0), "failed sample index is not retained")
    require(
        "trace_schedule[i].guard_level" in run_trace.group(0),
        "sample level is not sourced from the immutable schedule",
    )

    debugfs_pos = source.find('debugfs_create_file("status", 0444')
    irreversible_pos = source.find("ret = los_trace_run(observer);")
    require(0 <= debugfs_pos < irreversible_pos, "evidence channel is created after the write sequence")
    require("mutex_lock(&observer->status_lock)" in source, "evidence channel lacks serialization")
    require("Unknown byte 2 is a recorded outcome" in source, "unknown byte-2 semantics are not explicit")
    require("if (observer->outcome_result)" not in source, "outcome classification still fails the electrical sequence")

    print(f"PASS: {path}")
    print("fixed_writes: 15 exact; APD/MMIO write paths absent")
    print(
        "trace_schedule:",
        " ".join(
            f"{target}:{level.lower()}"
            for target, level in zip(EXPECTED_TARGETS_MS, EXPECTED_LEVELS)
        ),
    )
    print("RSSI oracle pairs:", " ".join(f"{a:04x}/{b:04x}" for a, b in sorted(EXPECTED_ORACLE)))
    print("dense=4 reads; critical=12 reads; full=28 reads; LOS_CTRL2 always first")
    print("critical checkpoints validate APD/OVP/SAFE/currents; full adds 16 TX guards")
    print("evidence channel precedes writes; verdict precedes outcome classification")
    print("one-shot, pinning, zero retries, bus lock and physical recovery guards present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

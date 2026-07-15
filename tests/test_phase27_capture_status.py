#!/usr/bin/env python3
"""Offline contract tests for scripts/phase27_capture_status.py."""

from __future__ import annotations

import importlib.util
import io
import json
import subprocess
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "phase27_capture_status", ROOT / "scripts/phase27_capture_status.py"
)
assert SPEC and SPEC.loader
CAPTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAPTURE)

STATE_REGS = [
    ("tiamux", 0x0000, "08001002"),
    ("mpd_targets", 0x0004, "00020000"),
    ("t1delay", 0x0008, "99000020"),
    ("tx_sd", 0x000C, "40000000"),
    ("la_pwd", 0x0014, "00240000"),
    ("bgcken", 0x001C, "555555a5"),
    ("pi_tgen", 0x0020, "06000000"),
    ("svadc_pd", 0x0024, "00000100"),
    ("apd_dac", 0x0030, "00080000"),
    ("safe_protect", 0x0100, "ff8fff0f"),
    ("los_ctrl1", 0x011C, "06083c36"),
    ("los_ctrl2", 0x0120, "10050000"),
    ("los_timer", 0x0124, "ffffffff"),
    ("los_timeout_count", 0x0128, "00000000"),
    ("los_timeout", 0x012C, "00000000"),
    ("p0_cs1", 0x0134, "00020410"),
    ("p0_cs2", 0x0138, "00000000"),
    ("p0_cs3", 0x013C, "30120010"),
    ("p0_latch", 0x0140, "00000000"),
    ("p1_cs1", 0x0144, "00020410"),
    ("p1_cs2", 0x0148, "00000000"),
    ("p1_cs3", 0x014C, "30120010"),
    ("p1_latch", 0x0150, "00000000"),
    ("adc_probe", 0x0154, "00000000"),
    ("probe_control", 0x0158, "00000000"),
    ("apd_ovp_latch", 0x0164, "00000000"),
    ("rogue_tx", 0x0168, "34020000"),
    ("erc_filter", 0x016C, "3f2f0f00"),
    ("sw_reset", 0x0300, "00000000"),
]


def ph(value: str) -> str:
    return " ".join(value[index : index + 2] for index in range(0, len(value), 2))


def xpon_line(sample: bool, *, zero: bool = False) -> str:
    prefix = "  gpio(active_low=0 direction=0 logical=1 raw=1)" if sample else "  xpon"
    if zero:
        return (
            f"{prefix} physet3=00000000 physet10=00000000 physta1=00000000 fsm=0 "
            "setting=00000000 misc=00000000 rx=00000000 sync=0 sync_ok=0 rx_hi=00 "
            "bit15=0 sta=00000000 los=0 prbs=00000000 test=00000000 irq=00000000"
        )
    return (
        f"{prefix} physet3=00000000 physet10=80000000 physta1=000c0000 fsm=3 "
        "setting=0000014f misc=00000000 rx=00000000 sync=0 sync_ok=0 rx_hi=00 "
        "bit15=0 sta=00000001 los=1 prbs=00000000 test=00000000 irq=00000000"
    )


def snapshot(
    name: str,
    *,
    terminal_byte: int = 0x22,
    capture_result: int = 0,
    verify_result: int = 0,
    unattempted: bool = False,
) -> list[str]:
    gpio = "gpio(active_low=0 direction=0 logical=0 raw=0)" if unattempted else (
        "gpio(active_low=0 direction=0 logical=1 raw=1)"
    )
    lines = [
        f"{name}: capture={capture_result} verify={verify_result} {gpio}"
    ]
    for reg_name, reg, value in STATE_REGS:
        if unattempted:
            value = "00000000"
        elif name == "terminal":
            terminal_values = {
                "la_pwd": "00240500",
                "svadc_pd": "00004104",
                "los_ctrl1": "061f1c10",
                "los_ctrl2": f"051f{terminal_byte:02x}00",
                "adc_probe": "84020000",
            }
            value = terminal_values.get(reg_name, value)
        lines.append(f"  {reg_name:<18}@{reg:04x}={ph(value)}(r=0)")
    lines.append(xpon_line(sample=False, zero=unattempted))
    return lines


def make_status(
    *,
    sample_bytes: list[int] | None = None,
    terminal_byte: int = 0x22,
    attempted_writes: int = 15,
) -> str:
    if sample_bytes is None:
        sample_bytes = [0x22] * 12
    prefix_complete = attempted_writes == 15
    complete = prefix_complete and len(sample_bytes) == 12
    trace_aborted = prefix_complete and not complete
    sequence_result = 0 if complete else (-1 if trace_aborted else -5)
    trace_result = -1 if trace_aborted else (0 if complete else CAPTURE.ECANCELED_MIPS)
    sample_rows = len(sample_bytes) + (1 if trace_aborted else 0)
    terminal_verified = prefix_complete
    accepted = list(sample_bytes)
    if terminal_verified:
        accepted.append(terminal_byte)
    count22 = accepted.count(0x22)
    count23 = accepted.count(0x23)
    other = len(accepted) - count22 - count23
    transitions = sum(left != right for left, right in zip(accepted, accepted[1:]))
    outcome_result = CAPTURE.ERANGE if other else (0 if prefix_complete else CAPTURE.ECANCELED_MIPS)
    terminal_outcome = (
        (0 if terminal_byte in (0x22, 0x23) else CAPTURE.ERANGE)
        if terminal_verified
        else CAPTURE.ECANCELED_MIPS
    )
    if attempted_writes >= 8:
        rssi_oracle = "vref=0x020a v=0x0284 delta=0x007a latch_initial=00 latch_second=00"
    elif attempted_writes >= 6:
        rssi_oracle = "vref=0x020a v=0x0000 delta=0x0000 latch_initial=00 latch_second=00"
    else:
        rssi_oracle = "vref=0x0000 v=0x0000 delta=0x0000 latch_initial=00 latch_second=00"
    calibration = (
        "la=00 24 00 00 svadc=00 00 01 00 adc=84 02 00 00 probe=00 00 00 00"
        if attempted_writes >= 10
        else "la=00 00 00 00 svadc=00 00 00 00 adc=00 00 00 00 probe=00 00 00 00"
    )
    gain = "00 24 05 00" if attempted_writes >= 11 else "00 00 00 00"
    trigger = "06 1f 3c 36" if attempted_writes >= 15 else "00 00 00 00"
    rssi_readbacks = f"{calibration} gain_la={gain} los_trigger={trigger}"
    lines = [
        f"operation:             {CAPTURE.OPERATION}",
        f"trace_policy:          {CAPTURE.TRACE_POLICY}",
        "silicon_id:            0x03",
        "silicon_variant:       0x01",
        "factory_length:        0x190",
        f"factory_sha256:        {CAPTURE.FACTORY_SHA256}",
        "factory_hash_matched:  yes",
        "tx_disable_gpio:       528 (hardware offset 16)",
        "module_pinned:         yes",
        "adapter_retries:       saved=3 during=0 restored=yes",
        f"i2c_write_attempts:    {attempted_writes} / 15 maximum",
        "mmio_write_attempts:   0 / 0 maximum",
        "apd_write_attempts:    0 / 0 maximum",
        f"sequence_result:       {sequence_result}",
        f"halted_step:           {0 if complete else (attempted_writes or 1)}",
        f"halted_sample:         {len(sample_bytes) if trace_aborted else -1}",
        f"trace_result:          {trace_result}",
        f"outcome_result:        {outcome_result}",
        f"terminal_outcome:      {terminal_outcome}",
        (
            f"outcome_observations:  22={count22} 23={count23} other={other} "
            f"transitions={transitions} first={accepted[0] if accepted else 0:02x} "
            f"final={accepted[-1] if accepted else 0:02x} valid={1 if accepted else 0} "
            "(samples plus terminal)"
        ),
        f"prefix_end_ns:         {2450 if prefix_complete else 0}",
        f"rssi_oracle:          {rssi_oracle}",
        f"rssi_readbacks:       {rssi_readbacks}",
    ]
    lines += snapshot("cold")
    lines += snapshot(
        "post_reset",
        capture_result=0 if attempted_writes else CAPTURE.ECANCELED_MIPS,
        verify_result=0 if attempted_writes else CAPTURE.ECANCELED_MIPS,
        unattempted=not attempted_writes,
    )
    lines += snapshot(
        "terminal",
        terminal_byte=terminal_byte,
        verify_result=0 if terminal_verified else CAPTURE.ECANCELED_MIPS,
    )

    for index, (reg, length, payload) in enumerate(CAPTURE.EXPECTED_WRITES, start=1):
        attempted = index <= attempted_writes
        start = 1000 + (index - 1) * 100 if attempted else 0
        done = start + 50 if attempted else 0
        rc = 0 if attempted else CAPTURE.ECANCELED_MIPS
        lines.append(
            f"write_{index:02d}: reg={reg:04x} len={length} payload={ph(payload)} "
            f"attempted={'yes' if attempted else 'no'} result={rc} start_ns={start} done_ns={done}"
        )

    lines.append(f"samples_taken:         {sample_rows} / 12")
    rows = [(byte2, False) for byte2 in sample_bytes]
    if trace_aborted:
        rows.append((0x22, True))
    for index, (byte2, failed) in enumerate(rows):
        target, level = CAPTURE.SAMPLE_PLAN[index]
        start = target * 1000 + 100
        outcome = CAPTURE.ECANCELED_MIPS if failed else (0 if byte2 in (0x22, 0x23) else CAPTURE.ERANGE)
        if level == "dense":
            staged_fields = (
                f"count=00 00 00 00(r={CAPTURE.ECANCELED_MIPS}) "
                f"timeout=00 00 00 00(r=0) debug=00 00 00 00(r=0) "
                f"los1=00 00 00 00(r={CAPTURE.ECANCELED_MIPS}) "
                f"svadc=00 00 00 00(r={CAPTURE.ECANCELED_MIPS})"
            )
        else:
            staged_fields = (
                "count=00 00 00 00(r=0) timeout=00 00 00 00(r=0) "
                "debug=00 00 00 00(r=0) los1=06 1f 1c 10(r=0) svadc=00 00 41 04(r=0)"
            )
        lines.append(
            f"sample_{index:02d}: level={level} capture=0 verify={-1 if failed else 0} outcome={outcome} "
            f"target_ms={target} start_us={start} los2_done_us={start + 200} end_us={start + 400} "
            f"los2={'04' if failed else '05'} 1f {byte2:02x} 00(r=0) "
            f"timer=ff ff ff ff(r=0) {staged_fields}"
        )
        if level == "dense":
            lines += [
                "  safety EN7570=not-sampled (dense level)",
                "  static_tx_guards=not-sampled",
            ]
        else:
            lines.append(
                "  safety ovp=00 00 00 00(r=0) apd=00 08 00 00(r=0) safe=ff 8f ff 0f(r=0) "
                "ibias=00 00 00 00(r=0) imod=00 00 00 00(r=0)"
            )
            if level == "full":
                for name, reg, value in CAPTURE.EXPECTED_GUARDS:
                    lines.append(f"  guard_{name}@{reg:04x}={ph(value)}(r=0)")
            else:
                lines.append("  static_tx_guards=not-sampled")
        lines.append(xpon_line(sample=True))

    lines += [
        "tx_disable_asserted:   yes",
        "physical_powercut_required: yes",
        "observer_retry_count:  0",
        "software_rollback:     impossible/not attempted",
        "esd_or_deglitch_write: no",
        "periodic_worker:       no",
        "xpon_mmio_write:       no",
        "apd_write:             no",
        "tx_current_laser_tgen: no",
        "arbitrary_write_path:  no",
    ]
    return "\n".join(lines) + "\n"


class Phase27CaptureStatusTests(unittest.TestCase):
    def test_complete_known_transition_is_valid(self) -> None:
        raw = make_status(sample_bytes=[0x22] * 4 + [0x23] * 4 + [0x22] * 4)
        report = CAPTURE.validate_status(raw)
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["classification"], "complete-known-outcome")

    def test_exact_en7570_variant_is_required(self) -> None:
        raw = make_status().replace("silicon_variant:       0x01", "silicon_variant:       0x03", 1)
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("silicon_variant" in error for error in report["errors"]))

    def test_terminal_unknown_is_scientific_not_electrical_failure(self) -> None:
        report = CAPTURE.validate_status(make_status(terminal_byte=0x24))
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["classification"], "complete-new-outcome")
        self.assertEqual(report["scientific_outcome_result"], CAPTURE.ERANGE)

    def test_prefix_abort_is_valid_evidence_and_requires_powercut(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=5)
        report = CAPTURE.validate_status(raw)
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["classification"], "aborted-needs-powercut")
        self.assertTrue(report["physical_powercut_required"])
        self.assertEqual(CAPTURE._exit_code(report), 0)
        invented = raw.replace(
            "la=00 00 00 00 svadc=00 00 00 00 adc=00 00 00 00 "
            "probe=00 00 00 00 gain_la=00 00 00 00 los_trigger=00 00 00 00",
            "la=de ad be ef svadc=de ad be ef adc=de ad be ef "
            "probe=de ad be ef gain_la=de ad be ef los_trigger=de ad be ef",
            1,
        )
        report = CAPTURE.validate_status(invented)
        self.assertFalse(report["valid"])
        self.assertTrue(any("before its first possible read" in error for error in report["errors"]))
        failed_boundary = raw.replace(
            "write_05: reg=0159 len=1 payload=10 attempted=yes result=0",
            "write_05: reg=0159 len=1 payload=10 attempted=yes result=-5",
            1,
        ).replace(
            "vref=0x0000 v=0x0000 delta=0x0000",
            "vref=0x020a v=0x0000 delta=0x0000",
            1,
        )
        report = CAPTURE.validate_status(failed_boundary)
        self.assertFalse(report["valid"])
        self.assertTrue(any("failed write_05" in error for error in report["errors"]))

    def test_trace_abort_before_first_accepted_sample_is_valid(self) -> None:
        report = CAPTURE.validate_status(make_status(sample_bytes=[], attempted_writes=15))
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["classification"], "aborted-needs-powercut")
        self.assertEqual(report["scientific_outcome_result"], 0)

    def test_tampered_write_is_rejected(self) -> None:
        raw = make_status().replace("write_11: reg=011c", "write_11: reg=011d", 1)
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("write_11" in error for error in report["errors"]))

    def test_failed_write_must_cause_sequence_result(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=5).replace(
            "write_05: reg=0159 len=1 payload=10 attempted=yes result=0",
            "write_05: reg=0159 len=1 payload=10 attempted=yes result=-5",
            1,
        )
        self.assertTrue(CAPTURE.validate_status(raw)["valid"])
        impossible = raw.replace("sequence_result:       -5", "sequence_result:       -1", 1)
        report = CAPTURE.validate_status(impossible)
        self.assertFalse(report["valid"])
        self.assertTrue(any("expected failed write result" in error for error in report["errors"]))
        positive = raw.replace("result=-5", "result=1", 1).replace(
            "sequence_result:       -5", "sequence_result:       1", 1
        )
        report = CAPTURE.validate_status(positive)
        self.assertFalse(report["valid"])
        self.assertTrue(any("cannot be positive" in error for error in report["errors"]))

    def test_rejected_nonzero_rssi_read_must_cause_erange(self) -> None:
        bad_vref = make_status(sample_bytes=[], attempted_writes=5).replace(
            "vref=0x0000 v=0x0000 delta=0x0000",
            "vref=0x1234 v=0x0000 delta=0x0000",
            1,
        )
        report = CAPTURE.validate_status(bad_vref)
        self.assertFalse(report["valid"])
        self.assertTrue(any("rejected Vref" in error for error in report["errors"]))
        self.assertTrue(
            CAPTURE.validate_status(
                bad_vref.replace("sequence_result:       -5", "sequence_result:       -34", 1)
            )["valid"]
        )

        bad_v = make_status(sample_bytes=[], attempted_writes=7).replace(
            "vref=0x020a v=0x0000 delta=0x0000",
            "vref=0x020a v=0x1234 delta=0x102a",
            1,
        )
        report = CAPTURE.validate_status(bad_v)
        self.assertFalse(report["valid"])
        self.assertTrue(any("rejected V after write_07" in error for error in report["errors"]))
        self.assertTrue(
            CAPTURE.validate_status(
                bad_v.replace("sequence_result:       -5", "sequence_result:       -34", 1)
            )["valid"]
        )

    def test_last_write_failure_retains_prior_rssi_evidence(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=15)
        replacements = {
            "sequence_result:       -1": "sequence_result:       -5",
            "halted_sample:         0": "halted_sample:         -1",
            "trace_result:          -1": f"trace_result:          {CAPTURE.ECANCELED_MIPS}",
            "outcome_result:        0": f"outcome_result:        {CAPTURE.ECANCELED_MIPS}",
            "terminal_outcome:      0": f"terminal_outcome:      {CAPTURE.ECANCELED_MIPS}",
            "outcome_observations:  22=1 23=0 other=0 transitions=0 first=22 final=22 valid=1 "
            "(samples plus terminal)":
                "outcome_observations:  22=0 23=0 other=0 transitions=0 first=00 final=00 valid=0 "
                "(samples plus terminal)",
            "prefix_end_ns:         2450": "prefix_end_ns:         0",
            "terminal: capture=0 verify=0": "terminal: capture=0 verify=-1",
            "write_15: reg=011c len=4 payload=06 1f 1c 10 attempted=yes result=0":
                "write_15: reg=011c len=4 payload=06 1f 1c 10 attempted=yes result=-5",
            "samples_taken:         1 / 12": "samples_taken:         0 / 12",
        }
        for original, replacement in replacements.items():
            self.assertIn(original, raw)
            raw = raw.replace(original, replacement, 1)
        terminal_start = raw.index("terminal:")
        writes_start = raw.index("write_01:")
        terminal = raw[terminal_start:writes_start].replace(
            "  los_ctrl1         @011c=06 1f 1c 10(r=0)",
            "  los_ctrl1         @011c=07 1f 3c 36(r=0)",
            1,
        )
        raw = raw[:terminal_start] + terminal + raw[writes_start:]
        sample_start = raw.index("sample_00:")
        footer_start = raw.index("tx_disable_asserted:")
        raw = raw[:sample_start] + raw[footer_start:]
        report = CAPTURE.validate_status(raw)
        self.assertTrue(report["valid"], report["errors"])

        tampered = raw.replace("los_trigger=06 1f 3c 36", "los_trigger=00 00 00 00", 1)
        report = CAPTURE.validate_status(tampered)
        self.assertFalse(report["valid"])
        self.assertTrue(any("reaching write_15" in error for error in report["errors"]))

    def test_unsafe_sample_gpio_is_rejected(self) -> None:
        raw = make_status().replace(
            "gpio(active_low=0 direction=0 logical=1 raw=1) physet3=",
            "gpio(active_low=0 direction=0 logical=0 raw=1) physet3=",
            1,
        )
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("GPIO/xPON safety gate" in error for error in report["errors"]))

    def test_snapshot_layout_and_terminal_adc_tampering_are_rejected(self) -> None:
        layout = make_status().replace("@0000=08 00 10 02", "@0001=08 00 10 02", 1)
        self.assertFalse(CAPTURE.validate_status(layout)["valid"])
        adc = make_status().replace("@0154=84 02 00 00", "@0154=de ad be ef", 1)
        report = CAPTURE.validate_status(adc)
        self.assertFalse(report["valid"])
        self.assertTrue(any("ADC probe" in error for error in report["errors"]))

    def test_sample_fixed_bytes_and_complete_body_are_required(self) -> None:
        los2 = make_status().replace("los2=05 1f 22 00", "los2=04 1f 22 00", 1)
        self.assertFalse(CAPTURE.validate_status(los2)["valid"])
        missing = make_status().replace(" timer=ff ff ff ff(r=0)", "", 1)
        self.assertFalse(CAPTURE.validate_status(missing)["valid"])

    def test_impossible_complete_state_machine_is_rejected(self) -> None:
        raw = make_status().replace("sequence_result:       0", "sequence_result:       -5", 1)
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("require sequence_result=0" in error for error in report["errors"]))

    def test_synchronous_samples_cannot_overlap(self) -> None:
        raw = make_status().replace("end_us=500 ", "end_us=6000 ", 1)
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("overlaps the preceding" in error for error in report["errors"]))

    def test_real_ecanceled_terminal_capture_is_not_an_unattempted_sentinel(self) -> None:
        raw = make_status()
        replacements = {
            "sequence_result:       0": f"sequence_result:       {CAPTURE.ECANCELED_MIPS}",
            "halted_step:           0": "halted_step:           15",
            "terminal_outcome:      0": f"terminal_outcome:      {CAPTURE.ECANCELED_MIPS}",
            "outcome_observations:  22=13 23=0 other=0 transitions=0 first=22 final=22 valid=1 "
            "(samples plus terminal)":
                "outcome_observations:  22=12 23=0 other=0 transitions=0 first=22 final=22 valid=1 "
                "(samples plus terminal)",
            "terminal: capture=0 verify=0":
                f"terminal: capture={CAPTURE.ECANCELED_MIPS} verify={CAPTURE.ECANCELED_MIPS}",
        }
        for original, replacement in replacements.items():
            self.assertIn(original, raw)
            raw = raw.replace(original, replacement, 1)
        terminal_start = raw.index("terminal:")
        writes_start = raw.index("write_01:")
        terminal = raw[terminal_start:writes_start].replace(
            "  tiamux            @0000=08 00 10 02(r=0)",
            f"  tiamux            @0000=00 00 00 00(r={CAPTURE.ECANCELED_MIPS})",
            1,
        )
        raw = raw[:terminal_start] + terminal + raw[writes_start:]
        report = CAPTURE.validate_status(raw)
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["classification"], "aborted-needs-powercut")

    def test_zero_write_fast_gate_abort_still_requires_powercut(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=0)
        report = CAPTURE.validate_status(raw)
        self.assertTrue(report["valid"], report["errors"])
        self.assertEqual(report["classification"], "aborted-needs-powercut")
        self.assertTrue(report["physical_powercut_required"])
        post_reset = raw.index("post_reset:")
        tampered = raw[:post_reset] + raw[post_reset:].replace(
            "@0000=00 00 00 00", "@0000=08 00 10 02", 1
        )
        report = CAPTURE.validate_status(tampered)
        self.assertFalse(report["valid"])
        self.assertTrue(any("zero-initialized" in error for error in report["errors"]))
        invented = raw.replace(
            "vref=0x0000 v=0x0000 delta=0x0000",
            "vref=0x020a v=0x0284 delta=0x007a",
            1,
        ).replace(
            "la=00 00 00 00 svadc=00 00 00 00 adc=00 00 00 00 "
            "probe=00 00 00 00 gain_la=00 00 00 00 los_trigger=00 00 00 00",
            "la=00 24 00 00 svadc=00 00 01 00 adc=84 02 00 00 "
            "probe=00 00 00 00 gain_la=00 24 05 00 los_trigger=06 1f 3c 36",
            1,
        )
        report = CAPTURE.validate_status(invented)
        self.assertFalse(report["valid"])
        self.assertTrue(any("zero-write abort" in error for error in report["errors"]))

    def test_rejected_sample_still_requires_complete_evidence_block(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=15)
        missing = raw.replace("  safety EN7570=not-sampled (dense level)\n", "", 1).replace(
            xpon_line(sample=True) + "\n", "", 1
        )
        self.assertFalse(CAPTURE.validate_status(missing)["valid"])
        mismatch = raw.replace("sample_00: level=dense capture=0 verify=-1", "sample_00: level=dense capture=-5 verify=-1", 1)
        report = CAPTURE.validate_status(mismatch)
        self.assertFalse(report["valid"])
        self.assertTrue(any("does not propagate" in error for error in report["errors"]))
        unrecorded = raw.replace(
            "sample_00: level=dense capture=0 verify=-1",
            "sample_00: level=dense capture=-5 verify=-5",
            1,
        )
        report = CAPTURE.validate_status(unrecorded)
        self.assertFalse(report["valid"])
        self.assertTrue(
            any("does not equal first read/GPIO error" in error for error in report["errors"])
        )

    def test_decimal_gpio_errno_is_preserved(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=15)
        raw = raw.replace(
            "sample_00: level=dense capture=0 verify=-1",
            "sample_00: level=dense capture=-110 verify=-110",
            1,
        ).replace(
            "gpio(active_low=0 direction=0 logical=1 raw=1) physet3=",
            "gpio(active_low=0 direction=-110 logical=1 raw=1) physet3=",
            1,
        ).replace(
            "sequence_result:       -1", "sequence_result:       -110", 1
        ).replace(
            "trace_result:          -1", "trace_result:          -110", 1
        )
        report = CAPTURE.validate_status(raw)
        self.assertTrue(report["valid"], report["errors"])

    def test_reported_derived_fields_must_match_source_values(self) -> None:
        raw = make_status().replace("fsm=3", "fsm=9", 1)
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("xPON derived fields" in error for error in report["errors"]))
        raw = make_status(sample_bytes=[], attempted_writes=0).replace(
            "delta=0x0000", "delta=0x0001", 1
        )
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("delta does not match" in error for error in report["errors"]))

    def test_post_reset_verify_must_follow_capture_and_map(self) -> None:
        raw = make_status(sample_bytes=[], attempted_writes=1)
        propagated_wrong = raw.replace(
            "post_reset: capture=0 verify=0", "post_reset: capture=-5 verify=-1", 1
        )
        self.assertFalse(CAPTURE.validate_status(propagated_wrong)["valid"])
        exact_map_wrong = raw.replace(
            "post_reset: capture=0 verify=0", "post_reset: capture=0 verify=-1", 1
        )
        report = CAPTURE.validate_status(exact_map_wrong)
        self.assertFalse(report["valid"])
        self.assertTrue(any("does not follow capture/map" in error for error in report["errors"]))

    def test_capture_transport_failure_still_orders_physical_cut(self) -> None:
        failed = subprocess.CompletedProcess(["ssh"], 255, stdout=b"", stderr=b"unreachable\n")
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=str(output), timeout=1)
            stderr = io.StringIO()
            stdout = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh_captured", return_value=failed):
                with redirect_stderr(stderr), redirect_stdout(stdout):
                    rc = CAPTURE.capture(args)
            self.assertEqual(rc, 1)
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertIn("physical power cut", manifest["next_required_action"])
            self.assertEqual((output / "status.raw").read_bytes(), b"")
        with tempfile.TemporaryDirectory() as existing:
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=existing, timeout=1)
            stderr = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh_captured", return_value=failed):
                with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                    rc = CAPTURE.capture(args)
            self.assertEqual(rc, 1)
            self.assertTrue((Path(existing) / "manifest.json").is_file())
            self.assertTrue((Path(existing) / "status.raw").is_file())
            self.assertEqual(
                (Path(existing) / ".capture.claim").read_text(encoding="ascii"),
                "xr500v-phase27-capture-claim-v1\n",
            )
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())
        with tempfile.TemporaryDirectory() as existing:
            (Path(existing) / "occupied").write_text("do not overwrite\n", encoding="utf-8")
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=existing, timeout=1)
            stderr = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh_captured") as run_ssh:
                with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                    with self.assertRaises(FileExistsError):
                        CAPTURE.capture(args)
            run_ssh.assert_not_called()
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            output.write_text("existing evidence\n", encoding="utf-8")
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=str(output), timeout=1)
            stderr = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh_captured") as run_ssh:
                with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                    with self.assertRaises(FileExistsError):
                        CAPTURE.capture(args)
            run_ssh.assert_not_called()
            self.assertEqual(output.read_text(encoding="utf-8"), "existing evidence\n")
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=str(output), timeout=1)
            stderr = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh_captured", side_effect=KeyboardInterrupt):
                with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                    with self.assertRaises(KeyboardInterrupt):
                        CAPTURE.capture(args)
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=str(output), timeout=1)
            stderr = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh", side_effect=OSError("ssh unavailable")):
                with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                    rc = CAPTURE.capture(args)
            self.assertEqual(rc, 1)
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())
            self.assertTrue((output / "manifest.json").is_file())
            self.assertIn("SSH_EXEC_ERROR", (output / "status.stderr.raw").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            args = SimpleNamespace(host="root@192.0.2.1", output_dir=str(output), timeout=1)
            succeeded = subprocess.CompletedProcess(["ssh"], 0, stdout=b"status\n", stderr=b"")
            stderr = io.StringIO()
            with mock.patch.object(CAPTURE, "_run_ssh_captured", return_value=succeeded):
                with mock.patch.object(
                    CAPTURE, "_write_bytes_durable", side_effect=OSError("disk full")
                ):
                    with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                        with self.assertRaises(OSError):
                            CAPTURE.capture(args)
            self.assertIn("CORTAR CORRIENTE FISICAMENTE AHORA", stderr.getvalue())

    def test_output_directory_has_one_atomic_owner(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            output.mkdir()
            barrier = threading.Barrier(2)

            def claim() -> str:
                barrier.wait()
                try:
                    CAPTURE._prepare_output_directory(output)
                except FileExistsError:
                    return "rejected"
                return "claimed"

            with ThreadPoolExecutor(max_workers=2) as executor:
                results = list(executor.map(lambda _: claim(), range(2)))

            self.assertCountEqual(results, ["claimed", "rejected"])
            self.assertEqual(
                (output / ".capture.claim").read_text(encoding="ascii"),
                "xr500v-phase27-capture-claim-v1\n",
            )

    def test_missing_powercut_boundary_is_rejected(self) -> None:
        raw = make_status().replace("physical_powercut_required: yes", "physical_powercut_required: no")
        report = CAPTURE.validate_status(raw)
        self.assertFalse(report["valid"])
        self.assertTrue(any("physical_powercut_required" in error for error in report["errors"]))


if __name__ == "__main__":
    unittest.main()

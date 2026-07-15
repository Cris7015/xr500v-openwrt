#!/usr/bin/env python3
"""Mutation tests for the XR500v digital-handoff source auditor."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDITOR = ROOT / "scripts/audit_xpon_digital_handoff_probe.py"
SOURCE = (
    ROOT
    / "package/kernel/xr500v-xpon-digital-handoff-probe/src"
    / "xr500v-xpon-digital-handoff-probe.c"
)


class XponDigitalHandoffAuditMutationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not SOURCE.exists():
            raise unittest.SkipTest("digital-handoff C source has not been added yet")
        cls.current = SOURCE.read_text(encoding="utf-8")

    def run_audit(self, source: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "probe.c"
            path.write_text(source, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(AUDITOR), str(path)],
                text=True,
                capture_output=True,
                check=False,
            )

    def assert_rejected(self, source: str, expected: str) -> None:
        result = self.run_audit(source)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(expected, output)

    def replace_once(self, old: str, new: str) -> str:
        self.assertEqual(self.current.count(old), 1, old)
        return self.current.replace(old, new, 1)

    def sub_once(self, pattern: str, replacement: str) -> str:
        mutant, count = re.subn(pattern, replacement, self.current, count=1, flags=re.M)
        self.assertEqual(count, 1, pattern)
        return mutant

    def test_current_source_passes(self) -> None:
        result = self.run_audit(self.current)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_extra_mmio_write(self) -> None:
        mutant = self.current + "\nstatic void bad(void __iomem *p) { iowrite32(0, p); }\n"
        self.assert_rejected(mutant, "exactly two iowrite32")

    def test_rejects_i2c_access(self) -> None:
        mutant = self.current + (
            "\nstatic int bad(struct i2c_adapter *a) "
            "{ return i2c_transfer(a, 0, 0); }\n"
        )
        self.assert_rejected(mutant, "forbidden I2C access")

    def test_rejects_control_and_status_mask_mutations(self) -> None:
        cases = (
            ("PHYSET3_ESD_PRO", 2, 3),
            ("PHYSET2_FW_READY", 0, 1),
            ("PHYSET2_PHY_READY", 2, 3),
        )
        for name, old, new in cases:
            with self.subTest(mask=name):
                mutant = self.sub_once(
                    rf"^(#define\s+{name}\s+)BIT\({old}\)$",
                    rf"\g<1>BIT({new})",
                )
                self.assert_rejected(mutant, f"{name} changed")

    def test_rejects_directional_noop_semantics_mutations(self) -> None:
        cases = (
            (
                "return set ? 0 : -EALREADY;",
                "return 0;",
                "ESD no-op semantics",
            ),
            (
                "return set ? -EALREADY : 0;",
                "return 0;",
                "FWRDY no-op semantics",
            ),
        )
        for old, new, expected in cases:
            with self.subTest(primitive=expected):
                self.assert_rejected(self.replace_once(old, new), expected)

    def test_rejects_removed_single_bit_proofs(self) -> None:
        cases = (
            (
                "if ((old ^ new) & ~PHYSET3_ESD_PRO)\n\t\treturn -EPERM;",
                "ESD primitive single-bit change proof",
            ),
            (
                "if ((old ^ new) & ~PHYSET2_FW_READY)\n\t\treturn -EPERM;",
                "FWRDY primitive single-bit change proof",
            ),
        )
        for proof, expected in cases:
            with self.subTest(primitive=expected):
                self.assert_rejected(self.replace_once(proof, ""), expected)

    def test_rejects_timestamp_after_fw_write(self) -> None:
        old = (
            "if (set)\n"
            "\t\tprobe->handoff_start_ns = ktime_get_ns();\n"
            "\tiowrite32(new, probe->base + PHYSET2);"
        )
        new = (
            "iowrite32(new, probe->base + PHYSET2);\n"
            "\tif (set)\n"
            "\t\tprobe->handoff_start_ns = ktime_get_ns();"
        )
        self.assert_rejected(
            self.replace_once(old, new),
            "timestamp is not immediately before",
        )

    def test_rejects_timestamp_outside_set_branch(self) -> None:
        old = "if (set)\n\t\tprobe->handoff_start_ns = ktime_get_ns();"
        new = "probe->handoff_start_ns = ktime_get_ns();"
        self.assert_rejected(
            self.replace_once(old, new),
            "timestamp is not immediately before",
        )

    def test_rejects_extra_raw_primitive_caller(self) -> None:
        needle = "return update_esd_pro_bit(probe, true);"
        mutant = self.replace_once(
            needle,
            "update_esd_pro_bit(probe, true);\n\t" + needle,
        )
        self.assert_rejected(mutant, "ESD rollback wrapper is not unconditional")

    def test_rejects_apply_wrapper_guard_bypass(self) -> None:
        old = (
            "static int apply_fw_ready(struct xr500v_digital_handoff *probe)\n"
            "{\n\tint ret;\n\n\tret = "
            "static_guard(probe, COLD_PHYSET2, ACTIVE_PHYSET3, false);"
        )
        new = old.replace("ACTIVE_PHYSET3, false", "ACTIVE_PHYSET3, true")
        self.assert_rejected(
            self.replace_once(old, new),
            "FWRDY apply wrapper lost its exact preflight guard",
        )

    def test_rejects_apply_order_mutation(self) -> None:
        esd = "ret = apply_esd_disable(probe);"
        fw = "ret = apply_fw_ready(probe);"
        self.assertEqual(self.current.count(esd), 1)
        self.assertEqual(self.current.count(fw), 1)
        mutant = self.current.replace(esd, "APPLY_PLACEHOLDER", 1)
        mutant = mutant.replace(fw, esd, 1)
        mutant = mutant.replace("APPLY_PLACEHOLDER", fw, 1)
        self.assert_rejected(mutant, "apply order is not exactly")

    def test_rejects_apply_latch_after_raw_write(self) -> None:
        old = (
            "probe->fw_ready_rollback_needed = true;\n"
            "\tret = update_fw_ready_bit(probe, true);"
        )
        new = (
            "ret = update_fw_ready_bit(probe, true);\n"
            "\tprobe->fw_ready_rollback_needed = true;"
        )
        self.assert_rejected(
            self.replace_once(old, new),
            "FWRDY rollback-needed latch is not set before raw apply",
        )

    def test_rejects_unlatched_esd_apply(self) -> None:
        mutant = self.replace_once("probe->esd_rollback_needed = true;\n", "")
        self.assert_rejected(mutant, "ESD rollback-needed latch")

    def test_rejects_guard_added_to_protective_rollback(self) -> None:
        old = "return update_fw_ready_bit(probe, false);"
        new = (
            "if (tx_disable_guard(probe))\n"
            "\t\treturn -EPERM;\n"
            "\treturn update_fw_ready_bit(probe, false);"
        )
        self.assert_rejected(
            self.replace_once(old, new),
            "FWRDY rollback wrapper is not unconditional",
        )

    def test_rejects_rollback_order_mutation(self) -> None:
        fw = "err = rollback_fw_ready_disable(probe);"
        esd = "err = rollback_esd_restore(probe);"
        self.assertEqual(self.current.count(fw), 1)
        self.assertEqual(self.current.count(esd), 1)
        mutant = self.current.replace(fw, "ROLLBACK_PLACEHOLDER", 1)
        mutant = mutant.replace(esd, fw, 1)
        mutant = mutant.replace("ROLLBACK_PLACEHOLDER", esd, 1)
        self.assert_rejected(mutant, "rollback order")

    def test_rejects_else_if_that_can_skip_esd_rollback(self) -> None:
        old = "\t}\n\tif (probe->esd_rollback_needed) {"
        new = "\t} else if (probe->esd_rollback_needed) {"
        self.assert_rejected(
            self.replace_once(old, new),
            "both reverse rollback operations are not independently attempted",
        )

    def test_rejects_early_return_between_rollback_writes(self) -> None:
        old = (
            "err = rollback_fw_ready_disable(probe);\n"
            "\t\tif (err && !write_ret)\n"
            "\t\t\twrite_ret = err;"
        )
        new = old + "\n\t\tif (err)\n\t\t\treturn err;"
        self.assert_rejected(
            self.replace_once(old, new),
            "return before both protective writes",
        )

    def test_rejects_phyready_as_fatal_sample_state(self) -> None:
        needle = "static int verify_sample(const struct handoff_sample *sample)\n{"
        insertion = (
            needle
            + "\n\tif (sample->physet2 & PHYSET2_PHY_READY)\n"
            + "\t\treturn -EIO;"
        )
        self.assert_rejected(
            self.replace_once(needle, insertion),
            "sample verifier no longer permits only PHYRDY variability",
        )

    def test_rejects_phyready_as_fatal_final_state(self) -> None:
        needle = (
            "static int verify_final_state("
            "const struct xr500v_digital_handoff *probe)\n{"
        )
        insertion = (
            needle
            + "\n\tif (probe->final_physet2 & PHYSET2_PHY_READY)\n"
            + "\t\treturn -EIO;"
        )
        self.assert_rejected(
            self.replace_once(needle, insertion),
            "final verifier no longer ignores only",
        )

    def test_rejects_broader_physet2_sample_mask(self) -> None:
        old = "(sample->physet2 & ~PHYSET2_PHY_READY) != ACTIVE_PHYSET2"
        new = "(sample->physet2 & ~(PHYSET2_PHY_READY | BIT(3))) != ACTIVE_PHYSET2"
        self.assert_rejected(
            self.replace_once(old, new),
            "sample verifier no longer permits only PHYRDY variability",
        )

    def test_rejects_missing_final_tx_guard(self) -> None:
        old = "probe->final_prbs_tx || probe->final_test_frame ||"
        new = "probe->final_prbs_tx || false ||"
        self.assertEqual(self.current.count(old), 2)
        mutant = self.current.replace(old, new, 1)
        self.assert_rejected(
            mutant,
            "final protected-state verdict lost final_test_frame",
        )

    def test_rejects_settle_duration_mutation(self) -> None:
        mutant = self.sub_once(
            r"^(#define\s+PHY_READY_SETTLE_MS\s+)500$",
            r"\g<1>400",
        )
        self.assert_rejected(mutant, "PHY_READY_SETTLE_MS changed")

    def test_rejects_settle_before_esd_restore(self) -> None:
        sleep = "\t\tmsleep(PHY_READY_SETTLE_MS);"
        self.assertEqual(self.current.count(sleep), 1)
        mutant = self.current.replace(sleep, "", 1)
        marker = "\t\terr = rollback_esd_restore(probe);"
        self.assertEqual(mutant.count(marker), 1)
        mutant = mutant.replace(marker, sleep + "\n" + marker, 1)
        self.assert_rejected(mutant, "rollback write, settle")

    def test_rejects_powercut_from_write_error(self) -> None:
        old = "probe->physical_powercut_required = probe->rollback_result != 0;"
        new = "probe->physical_powercut_required = probe->rollback_write_result != 0;"
        self.assert_rejected(
            self.replace_once(old, new),
            "power-cut verdict is not derived only",
        )

    def test_rejects_phyready_settle_in_powercut_verdict(self) -> None:
        old = "probe->physical_powercut_required = probe->rollback_result != 0;"
        new = (
            "probe->physical_powercut_required = probe->rollback_result != 0 ||\n"
            "\t\tprobe->phy_ready_settle_result != 0;"
        )
        self.assert_rejected(
            self.replace_once(old, new),
            "power-cut verdict is not derived only",
        )

    def test_rejects_removed_debugfs_safety_field(self) -> None:
        old = "powercut_reason_writable_baseline_mismatch: %s\\n"
        new = "writable_baseline_mismatch_removed: %s\\n"
        self.assert_rejected(
            self.replace_once(old, new),
            "debugfs report lost field powercut_reason_writable_baseline_mismatch:",
        )

    def test_rejects_schedule_mutation(self) -> None:
        match = re.search(
            r"(static\s+const\s+(?:u32|unsigned\s+int|unsigned\s+long)\s+"
            r"[A-Za-z_]\w*(?:schedule|target)[A-Za-z_]\w*\s*\[[^]]+\]\s*=\s*\{)"
            r"(.*?)(\};)",
            self.current,
            re.S | re.I,
        )
        self.assertIsNotNone(match)
        body = match.group(2)
        self.assertRegex(body, r"(?<!\d)10(?!\d)")
        body = re.sub(r"(?<!\d)10(?!\d)", "11", body, count=1)
        mutant = self.current[: match.start(2)] + body + self.current[match.end(2) :]
        self.assert_rejected(mutant, "trace schedule")

    def test_rejects_removed_platform_gates(self) -> None:
        gate_mutants = {
            "module arm": self.replace_once("if (!arm_digital_handoff)", "if (false)"),
            "DT": self.current.replace(
                '"econet,allow-en7570-los-trace"',
                '"econet,allow-en7570-los-trace-removed"',
                1,
            ),
            "compatible": self.current.replace(
                '"econet,en751221-en7570-los-trace-experimental"',
                '"econet,en751221-en7570-los-trace-removed"',
                1,
            ),
            "root": self.current.replace(
                '"tplink,archer-xr500v"', '"tplink,not-the-xr500v"', 1
            ),
            "factory hash": self.current.replace("0x40,", "0x41,", 1),
        }
        expected = {
            "module arm": "module arm parameter",
            "DT": "phase-27 DT opt-in",
            "compatible": "phase-27 experimental compatible",
            "root": "root-machine compatible",
            "factory hash": "factory SHA-256 changed",
        }
        for name, mutant in gate_mutants.items():
            with self.subTest(gate=name):
                self.assertNotEqual(mutant, self.current)
                self.assert_rejected(mutant, expected[name])

    def test_rejects_cold_preflight_allowing_phyready(self) -> None:
        old = "static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, false);"
        self.assertGreaterEqual(self.current.count(old), 3)
        # Replace the final occurrence, which is the platform-probe preflight.
        head, tail = self.current.rsplit(old, 1)
        mutant = head + "static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, true);" + tail
        self.assert_rejected(mutant, "platform probe lost its exact full-word cold preflight")

    def test_rejects_baseline_mutation(self) -> None:
        mutant = self.sub_once(
            r"^(#define\s+COLD_PHYSET2\s+)0x00003c00$",
            r"\g<1>0x00003c01",
        )
        self.assert_rejected(mutant, "COLD_PHYSET2 changed")


if __name__ == "__main__":
    unittest.main()

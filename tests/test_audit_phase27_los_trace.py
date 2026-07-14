#!/usr/bin/env python3
"""Mutation tests for the phase-27 static safety auditor."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDITOR = ROOT / "scripts/audit_phase27_los_trace.py"
SOURCE = (
    ROOT
    / "package/kernel/xr500v-en7570-los-trace-observer/src"
    / "xr500v-en7570-los-trace-observer.c"
)


class Phase27AuditMutationTests(unittest.TestCase):
    def run_audit(self, source: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "observer.c"
            path.write_text(source, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(AUDITOR), str(path)],
                text=True,
                capture_output=True,
                check=False,
            )

    def test_current_source_passes(self) -> None:
        result = self.run_audit(SOURCE.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fast_gate_requires_both_cached_identity_bytes(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        original = (
            "if (observer->silicon_id != EN7570_EXPECTED_ID ||\n"
            "\t    observer->silicon_variant != EN7570_EXPECTED_VARIANT)"
        )
        mutant = "if (observer->silicon_variant != EN7570_EXPECTED_VARIANT)"
        self.assertEqual(source.count(original), 1)
        result = self.run_audit(source.replace(original, mutant, 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exact cached EN7570 ID/variant", result.stdout + result.stderr)

        variant_mutant = "if (observer->silicon_id != EN7570_EXPECTED_ID)"
        result = self.run_audit(source.replace(original, variant_mutant, 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exact cached EN7570 ID/variant", result.stdout + result.stderr)

    def test_probe_requires_exact_silicon_id(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        original = "if (ret || observer->silicon_id != EN7570_EXPECTED_ID)"
        self.assertEqual(source.count(original), 1)
        result = self.run_audit(source.replace(original, "if (ret)", 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exact EN7570 silicon ID", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

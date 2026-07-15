#!/usr/bin/env python3
"""Mutation tests for the guarded GPON MAC lab-probe auditors."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

PROBES = {
    "preflight": ("xr500v-gpon-mac-preflight", "audit_gpon_mac_preflight.py"),
    "mux": ("xr500v-gpon-wan-mux-cycle", "audit_gpon_wan_mux_cycle.py"),
    "single": ("xr500v-gpon-mac-single-read", "audit_gpon_mac_single_read.py"),
    "snapshot": ("xr500v-gpon-mac-snapshot", "audit_gpon_mac_snapshot.py"),
}


class GponMacLabAuditorTests(unittest.TestCase):
    def paths(self, name: str) -> tuple[Path, Path, Path]:
        package, auditor = PROBES[name]
        return (
            ROOT / "scripts" / auditor,
            ROOT / f"package/kernel/{package}/src/{package}.c",
            ROOT / f"package/kernel/{package}/Makefile",
        )

    def run_audit(
        self, name: str, source: str, makefile: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        auditor, _, package_makefile = self.paths(name)
        with tempfile.TemporaryDirectory() as temporary:
            source_path = Path(temporary) / "probe.c"
            makefile_path = Path(temporary) / "Makefile"
            source_path.write_text(source, encoding="utf-8")
            makefile_path.write_text(
                package_makefile.read_text(encoding="utf-8")
                if makefile is None
                else makefile,
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    sys.executable,
                    str(auditor),
                    str(source_path),
                    str(makefile_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

    def source(self, name: str) -> str:
        return self.paths(name)[1].read_text(encoding="utf-8")

    def assert_rejected(self, name: str, source: str, expected: str) -> None:
        result = self.run_audit(name, source)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(expected, output)

    def test_current_sources_pass(self) -> None:
        for name in PROBES:
            with self.subTest(name=name):
                result = self.run_audit(name, self.source(name))
                self.assertEqual(
                    result.returncode, 0, result.stdout + result.stderr
                )

    def test_preflight_rejects_direct_gpon_read(self) -> None:
        source = self.source("preflight") + "\nstatic u32 bad(void *p) { return ioread32(p); }\n"
        self.assert_rejected("preflight", source, "forbidden call")

    def test_mux_rejects_direct_gpon_read(self) -> None:
        source = self.source("mux") + "\nstatic u32 bad(void *p) { return ioread32(p); }\n"
        self.assert_rejected("mux", source, "forbidden call")

    def test_single_read_rejects_a_second_access(self) -> None:
        source = self.source("single")
        needle = "result.onu_id = ioread32(ctx->base);"
        self.assertEqual(source.count(needle), 1)
        source = source.replace(needle, needle + "\n\t" + needle, 1)
        self.assert_rejected("single", source, "exactly one direct")

    def test_single_read_requires_global_exclusion(self) -> None:
        source = self.source("single").replace("stop_machine(", "bad_machine(", 1)
        self.assert_rejected("single", source, "stop_machine")

    def test_snapshot_rejects_non_allowlisted_offset(self) -> None:
        source = self.source("snapshot").replace(
            "#define GPON_G_ACTIVATION_ST\t\t0x0bc",
            "#define GPON_G_ACTIVATION_ST\t\t0x0c0",
            1,
        )
        self.assert_rejected("snapshot", source, "allowlist mismatch")

    def test_snapshot_rejects_fifo_data_register(self) -> None:
        source = self.source("snapshot") + "\n#define GPON_G_PLOAMD_RDATA 0x05c\n"
        self.assert_rejected("snapshot", source, "FIFO data register")

    def test_snapshot_rejects_any_gpon_write(self) -> None:
        source = self.source("snapshot") + "\nstatic void bad(void *p) { iowrite32(0, p); }\n"
        self.assert_rejected("snapshot", source, "forbidden call")


if __name__ == "__main__":
    unittest.main()

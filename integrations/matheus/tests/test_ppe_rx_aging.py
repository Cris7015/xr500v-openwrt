#!/usr/bin/env python3
"""Compile/run actual GEN1 policy functions with stubbed hardware, 32/64-bit.

Generated translation units live only in TemporaryDirectory. This does not
load code on the router and is not hardware performance validation.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
if len(sys.argv) != 2:
    raise SystemExit("usage: test_ppe_rx_aging.py PATH_TO_LOCAL_PREPARED_airoha_ppe.c")
source = Path(sys.argv[1])
text = source.read_text()


def function(name):
    found = re.search(r"static [^;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\n\{", text)
    assert found, name
    end = text.index("\n}", found.end()) + 2
    return text[found.start():end]


rx = function("econet_ppe_rx_check")
start = rx.index("\tif (skb_headlen(skb) >= ETH_HLEN &&")
end = rx.index("\n\t}", start) + 3
gate = rx[start:end]
assert end < rx.index("if (!econet_ppe_parse_ipv4_tuple") < rx.index("spin_lock_bh")
assert "dev_kfree" not in gate and "3105" not in gate
assert "flow->lastused = jiffies;" in rx
assert "flow->lastused = jiffies;" in function("econet_flow_offload_replace")
assert "econet_flow_offload_stats(dev, cls)" in function("econet_flow_offload_cmd")
extracted = function("econet_ppe_flow_lastused") + "\n" + function("econet_flow_offload_stats")
extracted += "\nstatic void test_multicast_gate(struct econet_ppe *ppe, struct sk_buff *skb)\n{\n"
extracted += gate + "\n++scans;\n}\n"

with tempfile.TemporaryDirectory(prefix="xr500v-ppe-aging-test-") as tmp:
    out = Path(tmp)
    (out / "ppe-aging-extracted.inc").write_text(extracted)
    for bits in (32, 64):
        binary = out / f"test{bits}"
        subprocess.run(["gcc", f"-m{bits}", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-ffreestanding", "-fno-builtin",
                        "-fno-stack-protector", "-fno-pie", "-no-pie", "-nostdlib", "-static",
                        "-I", tmp, str(ROOT / "ppe-aging-harness.c"), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)])
        if result.returncode:
            raise SystemExit(f"FAIL {bits}-bit harness: C line/exit {result.returncode}")
        print(f"PASS {bits}-bit: extracted C multicast/aging/stats, clock/jiffies wrap, ownership and expiry")

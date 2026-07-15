#!/usr/bin/env python3
"""Fail-closed source audit for the XR500v digital xPON handoff probe.

The probe is deliberately narrower than a normal driver: two reversible PHY
bit writes, a finite status trace and no asynchronous or optical-front-end
machinery.  This auditor therefore treats the source as an immutable lab
recipe instead of trying to approve a family of equivalent drivers.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = (
    ROOT
    / "package/kernel/xr500v-xpon-digital-handoff-probe/src"
    / "xr500v-xpon-digital-handoff-probe.c"
)

EXPECTED_SCHEDULE_MS = [0, 10, 100, 500, 2000, 5000]
EXPECTED_FACTORY_SHA256 = bytes.fromhex(
    "401dfdaee77c84649bda100fd5dd85be"
    "01c7ea126d0a5cc2116b141c1a07a5e4"
)

# Frozen cold-boot values from this XR500v.  Requiring the symbol as well as
# its value makes the source review show exactly which retained word is being
# relied upon; none of these are masks or family-wide defaults.
EXPECTED_DEFINES = {
    "PHYSET2": 0x0104,
    "PHYSET2_FW_READY": 0,
    "PHYSET2_PHY_READY": 2,
    "PHYSET3": 0x0108,
    "PHYSET3_ESD_PRO": 2,
    "PHYSET3_TXEN": 5,
    "PHYSET10": 0x0124,
    "PHYSTA1": 0x0130,
    "XPON_SETTING": 0x0138,
    "MISC": 0x01FC,
    "MISC_ROGUE_TX_TEST": 28,
    "PHYRX_STATUS": 0x021C,
    "BISTCTL_PRBS_TX_EN": 0x04A4,
    "TEST_FRAME_EN": 0x0510,
    "XPON_STA": 0x05E0,
    "XPON_INT_EN": 0x05F0,
    "XR500V_XPON_BASE": 0x1FAF0000,
    "XR500V_XPON_SIZE": 0x1000,
    "XR500V_FACTORY_LENGTH": 0x190,
    "XR500V_TX_DISABLE_GPIO": 16,
    "COLD_PHYSET2": 0x00003C00,
    "COLD_PHYSET3": 0x4581E114,
    "COLD_PHYSET10": 0xFF000000,
    "COLD_XPON_SETTING": 0x0000014F,
    "HANDOFF_SAMPLE_COUNT": 6,
    "PHY_READY_SETTLE_MS": 500,
}

EXPECTED_BASELINE_SYMBOLS = (
    "COLD_PHYSET2",
    "COLD_PHYSET3",
    "COLD_PHYSET10",
    "COLD_XPON_SETTING",
)

WRITE_PRIMITIVES = ("update_esd_pro_bit", "update_fw_ready_bit")
WRITE_WRAPPERS = (
    "apply_esd_disable",
    "apply_fw_ready",
    "rollback_fw_ready_disable",
    "rollback_esd_restore",
)

EXPECTED_DEBUG_LABELS = (
    "mode: manually armed finite ESD_PRO/FWRDY probe",
    "sequence_result:",
    "rollback_write_result:",
    "rollback_result:",
    "rollback_complete:",
    "physical_powercut_required:",
    "factory_hash_matched:",
    "mmio_writes:",
    "i2c_en7570_apd_laser_writes: 0",
    "reset_pll_counter_irq_mac_scu_writes: 0",
    "PHYSET2:",
    "rollback_PHYSET2:",
    "PHYSET2.PHYRDY:",
    "PHYSET3:",
    "final_guard: PHYSET10=",
    "final_guard: PRBS_TX=",
    "final_status: PHYSTA1=",
    "final_status: XPON_STA=",
    "final_gpio_direction/value/raw:",
    "powercut_reason_fwrdy_set:",
    "powercut_reason_esd_not_restored:",
    "powercut_reason_gpio_tx_barrier_unsafe:",
    "powercut_reason_digital_tx_barrier_unsafe:",
    "powercut_reason_writable_baseline_mismatch:",
    "samples_taken:",
    "target_ms=",
    "PHYSET2.PHYRDY=",
    "PHYRX_STATUS=",
    "XPON_INT_STA=",
    "gpio_direction/value/raw=",
)

FORBIDDEN_PATTERNS = {
    "I2C access": r"\b(?:__)?i2c_[a-z0-9_]+\b|\bI2C_[A-Z0-9_]+\b|\bstruct\s+i2c_",
    "GPIO mutation": r"\bgpiod_(?:set|direction_(?:input|output))[a-z0-9_]*\s*\(",
    "IRQ access": (
        r"\b(?:devm_)?request_(?:threaded_)?irq\s*\(|"
        r"\b(?:enable|disable|free|synchronize)_irq\s*\(|"
        r"\bplatform_get_irq[a-z0-9_]*\s*\("
    ),
    "worker, poller or timer": (
        r"\b(?:schedule|queue|cancel)_(?:delayed_)?work\s*\(|"
        r"\bINIT_(?:DELAYED_)?WORK\s*\(|\bstruct\s+(?:delayed_)?work_struct\b|"
        r"\b(?:timer_setup|mod_timer|add_timer|del_timer|hrtimer_[a-z0-9_]+)\s*\(|"
        r"\b(?:kthread|tasklet)_[a-z0-9_]+\s*\(|\b(?:poll|poller)_[a-z0-9_]+\s*\("
    ),
    "reset access": r"\breset_control_[a-z0-9_]+\s*\(|\bdevm_reset_control_[a-z0-9_]+\s*\(",
    "clock access": r"\b(?:devm_)?clk_[a-z0-9_]+\s*\(",
    "SCU or regmap access": r"\b(?:syscon|regmap)_[a-z0-9_]+\s*\(|\bstruct\s+regmap\b",
    "GPON MAC access": (
        r"\b(?:G_ONU_ID|G_GBL_CFG|G_INT_ENABLE|G_PLOAM[UD]_|G_ACTIVATION_ST|WAN_CONF)\b|"
        r"\bGPON_MAC_[A-Z0-9_]+\b|0x1fb5[0-9a-fA-F]{4}|0x1fb6[0-9a-fA-F]{4}"
    ),
    "PLL mutation": r"\b(?:PHYSET3_PLL_EN|PLLRST|PLL_RST|PMA_CTRL|TDCSET)\b",
    "counter mutation": (
        r"\b(?:XP_ERRCNT_EN|XP_ERRCNT_CTL|ERR_BYTE_CNT|ERR_CODE_CNT|"
        r"NOSOL_CODE_CNT|RX_CODE_CNT|FEC_SECONDS|BIP_CNT|FRAME_CNT_[LH]|LOF_CNT)\b"
    ),
    "alternate MMIO write": (
        r"\b(?:write[blqw]|__raw_write[blqw]|memcpy_toio|memset_io|"
        r"iorep_write[blqw]|ioport_map)\s*\("
    ),
}


@dataclass(frozen=True)
class Function:
    name: str
    body: str
    start: int


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def code_without_comments_or_strings(source: str) -> str:
    """Blank comments/strings while retaining offsets for function parsing."""

    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.S,
    )
    return pattern.sub(lambda match: " " * len(match.group(0)), source)


def integer_define(source: str, name: str) -> int:
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+"
        r"(?:(?:BIT)\(\s*(\d+)\s*\)|(0x[0-9a-fA-F]+|\d+))\s*$",
        source,
        re.M,
    )
    require(match is not None, f"missing frozen define {name}")
    return int(match.group(1) or match.group(2), 0)


def functions(source: str) -> list[Function]:
    clean = code_without_comments_or_strings(source)
    signature = re.compile(
        r"(?m)^\s*static\s+(?:(?:noinline|__always_inline)\s+)?"
        r"(?:[A-Za-z_]\w*[\s*]+)+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{"
    )
    parsed: list[Function] = []
    for match in signature.finditer(clean):
        brace = clean.find("{", match.start(), match.end())
        depth = 1
        cursor = brace + 1
        while cursor < len(clean) and depth:
            if clean[cursor] == "{":
                depth += 1
            elif clean[cursor] == "}":
                depth -= 1
            cursor += 1
        require(depth == 0, f"unterminated function {match.group(1)}")
        parsed.append(Function(match.group(1), source[brace + 1 : cursor - 1], match.start()))
    return parsed


def calls_in(body: str, names: set[str]) -> list[str]:
    pattern = r"\b(" + "|".join(re.escape(name) for name in sorted(names)) + r")\s*\("
    return re.findall(pattern, code_without_comments_or_strings(body))


def parse_factory_hash(source: str) -> bytes:
    match = re.search(
        r"static\s+const\s+u8\s+xr500v_factory_sha256"
        r"\s*\[\s*SHA256_DIGEST_SIZE\s*\]\s*=\s*\{(.*?)\};",
        source,
        re.S,
    )
    require(match is not None, "exact per-unit factory SHA-256 array is absent")
    values = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    return values


def parse_schedule(source: str) -> tuple[str, list[int]]:
    candidates = re.finditer(
        r"static\s+const\s+(?:u32|unsigned\s+int|unsigned\s+long)\s+"
        r"([A-Za-z_]\w*(?:schedule|target)[A-Za-z_]\w*)"
        r"\s*\[[^]]+\]\s*=\s*\{(.*?)\};",
        source,
        re.S | re.I,
    )
    parsed: list[tuple[str, list[int]]] = []
    for match in candidates:
        values = [int(value, 0) for value in re.findall(r"(?<![A-Za-z_])(0x[0-9a-fA-F]+|\d+)", match.group(2))]
        parsed.append((match.group(1), values))
    exact = [candidate for candidate in parsed if candidate[1] == EXPECTED_SCHEDULE_MS]
    require(len(exact) == 1, "trace schedule is not exactly 0,10,100,500,2000,5000 ms")
    return exact[0]


def require_gate(source: str, clean: str) -> None:
    require(
        "module_param(arm_digital_handoff, bool, 0444)" in source,
        "read-only module arm parameter is absent or mutable",
    )
    require(
        re.search(r"if\s*\(\s*!arm_digital_handoff\s*\)", clean) is not None,
        "module arm parameter is not enforced",
    )
    require(
        "device_property_read_bool" in source
        and '"econet,allow-en7570-los-trace"' in source,
        "reused phase-27 DT opt-in is not enforced",
    )
    require(
        '"econet,en751221-en7570-los-trace-experimental"' in source,
        "reused phase-27 experimental compatible is absent",
    )
    require(
        source.count('"econet,en751221-en7570-los-trace-experimental"') == 1,
        "experimental compatible must occur exactly once",
    )
    require(
        re.search(
            r"if\s*\(\s*!of_machine_is_compatible\s*\(\s*"
            r"\"tplink,archer-xr500v\"\s*\)\s*\)",
            source,
        )
        is not None,
        "exact root-machine compatible gate is absent",
    )
    require(
        'devm_nvmem_cell_get(' in clean and '"factory-bob"' in source,
        "factory-bob NVMEM gate is absent",
    )
    require("crypto_shash_tfm_digest(" in clean, "factory SHA-256 calculation is absent")
    require(
        re.search(
            r"memcmp\s*\([^;]*xr500v_factory_sha256[^;]*\)", clean, re.S
        )
        is not None,
        "factory SHA-256 is not compared with the per-unit oracle",
    )


def require_tx_guards(source: str, parsed_functions: list[Function]) -> tuple[str, str]:
    by_name = {function.name: function for function in parsed_functions}
    require("tx_disable_guard" in by_name, "retained physical TX guard is absent")
    require("static_guard" in by_name, "exact digital cold-state guard is absent")
    physical = by_name["tx_disable_guard"]
    digital = by_name["static_guard"]
    body = code_without_comments_or_strings(physical.body)
    for token in (
        "gpiod_is_active_low",
        "gpiod_get_direction",
        "gpiod_get_value_cansleep",
        "gpiod_get_raw_value_cansleep",
    ):
        require(token in body, f"physical TX guard lost {token}")
    require(
        "direction == 0 && logical == 1 && raw == 1" in body,
        "TX_DISABLE output-direction guard changed",
    )
    require(
        source.count("GPIOD_ASIS") == 1 and '"tx-disable"' in source,
        "TX_DISABLE must be acquired once without changing retained direction/value",
    )
    require("gpiod_is_active_low" in clean_source(source), "active-high TX_DISABLE proof is absent")
    require('"tc3162-gpio"' in source, "exact TX_DISABLE GPIO controller proof is absent")

    digital_body = code_without_comments_or_strings(digital.body)
    require(
        calls_in(digital_body, {physical.name}) == [physical.name],
        "digital guard does not begin with the complete physical TX guard",
    )
    for required in (
        "PHYSET2",
        "PHYSET3",
        "COLD_PHYSET10",
        "COLD_XPON_SETTING",
        "PHYSET3_TXEN",
        "MISC_ROGUE_TX_TEST",
        "BISTCTL_PRBS_TX_EN",
        "TEST_FRAME_EN",
        "XPON_INT_EN",
    ):
        require(required in digital_body, f"digital TX guard lost {required}")
    require(
        re.search(r"xpon_read\s*\([^,]+,\s*MISC\s*\)\s*&\s*MISC_ROGUE_TX_TEST", digital_body)
        is not None,
        "rogue-ONU TX mode guard changed",
    )
    for zero_register in ("BISTCTL_PRBS_TX_EN", "TEST_FRAME_EN", "XPON_INT_EN"):
        require(
            re.search(rf"xpon_read\s*\([^,]+,\s*{zero_register}\s*\)", digital_body)
            is not None,
            f"zero TX guard for {zero_register} is absent",
        )
    return physical.name, digital.name


def clean_source(source: str) -> str:
    return code_without_comments_or_strings(source)


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE
    source = path.read_text(encoding="utf-8")
    clean = clean_source(source)

    for name, expected in EXPECTED_DEFINES.items():
        require(integer_define(source, name) == expected, f"frozen define {name} changed")
    require(parse_factory_hash(source) == EXPECTED_FACTORY_SHA256, "per-unit factory SHA-256 changed")
    schedule_name, schedule = parse_schedule(source)

    for label, pattern in FORBIDDEN_PATTERNS.items():
        require(re.search(pattern, clean, re.I) is None, f"forbidden {label} present")

    io_write_apis = re.findall(
        r"\b(iowrite(?:8|16|32|64)(?:be|_be|_relaxed)?)\s*\(", clean, re.I
    )
    require(
        io_write_apis == ["iowrite32", "iowrite32"],
        "MMIO write API set is not exactly two iowrite32 sites",
    )
    require(clean.count("devm_gpiod_get(") == 1, "TX_DISABLE GPIO acquisition count changed")
    require(clean.count("devm_ioremap_resource(") == 1, "xPON mapping count changed")
    require(clean.count("platform_get_resource(") == 1, "xPON resource lookup count changed")
    require(
        not re.search(
            r"\b(?:(?:devm_)?ioremap(?!_resource)|of_iomap|memremap|pci_iomap)\w*\s*\(",
            clean,
        ),
        "alternate MMIO mapping path is present",
    )

    require_gate(source, clean)
    parsed_functions = functions(source)
    require(parsed_functions, "no static functions were parsed")
    by_name = {function.name: function for function in parsed_functions}
    require(
        len(by_name) == len(parsed_functions),
        "duplicate static function names prevent a closed call-graph audit",
    )
    for name in (
        *WRITE_PRIMITIVES,
        *WRITE_WRAPPERS,
        "static_guard",
        "capture_sample",
        "verify_sample",
        "capture_final_state",
        "verify_final_state",
        "rollback_handoff",
        "run_handoff",
        "wait_until_target",
        "handoff_status_show",
        "xr500v_digital_handoff_probe",
    ):
        require(name in by_name, f"required frozen function {name} is absent")

    physical_guard_name, digital_guard_name = require_tx_guards(source, parsed_functions)
    static_body = clean_source(by_name[digital_guard_name].body)
    require(
        re.search(
            r"\(\s*!allow_phy_ready\s*&&\s*physet2\s*!=\s*expected_physet2\s*\)"
            r"\s*\|\|\s*\(\s*allow_phy_ready\s*&&\s*"
            r"\(\s*physet2\s*&\s*~PHYSET2_PHY_READY\s*\)\s*!=\s*"
            r"expected_physet2\s*\)",
            static_body,
        )
        is not None,
        "static guard no longer permits only PHYSET2.PHYRDY variability",
    )
    require(
        static_body.count("PHYSET2_PHY_READY") == 1,
        "static guard has an additional PHYRDY exception",
    )

    require(
        re.search(
            r"^\s*#define\s+ACTIVE_PHYSET2\s+"
            r"\(COLD_PHYSET2\s*\|\s*PHYSET2_FW_READY\)\s*$",
            source,
            re.M,
        )
        is not None,
        "active PHYSET2 word is not the exact cold word plus FWRDY",
    )
    require(
        re.search(
            r"^\s*#define\s+ACTIVE_PHYSET3\s+"
            r"\(COLD_PHYSET3\s*&\s*~PHYSET3_ESD_PRO\)\s*$",
            source,
            re.M,
        )
        is not None,
        "active PHYSET3 word is not the exact cold word minus ESD_PRO",
    )
    for symbol in EXPECTED_BASELINE_SYMBOLS:
        require(clean.count(symbol) >= 2, f"exact baseline {symbol} is defined but not enforced")

    # The two raw primitives are the only MMIO writers.  They deliberately do
    # not contain advancement guards: guarded apply wrappers and unconditional
    # protective rollback wrappers share these exact single-bit operations.
    write_functions = [
        function
        for function in parsed_functions
        if "iowrite32(" in clean_source(function.body)
    ]
    require(clean.count("iowrite32(") == 2, "expected exactly two iowrite32 call sites")
    require(
        [function.name for function in write_functions] == list(WRITE_PRIMITIVES),
        "MMIO writes escaped the two frozen raw primitives",
    )
    esd_body = clean_source(by_name["update_esd_pro_bit"].body)
    fw_body = clean_source(by_name["update_fw_ready_bit"].body)

    for body, offset, mask, other_mask, label in (
        (esd_body, "PHYSET3", "PHYSET3_ESD_PRO", "PHYSET2_FW_READY", "ESD"),
        (fw_body, "PHYSET2", "PHYSET2_FW_READY", "PHYSET3_ESD_PRO", "FWRDY"),
    ):
        require(body.count("iowrite32(") == 1, f"{label} primitive write count changed")
        require(
            re.search(
                rf"iowrite32\s*\(\s*new\s*,\s*probe->base\s*\+\s*{offset}\s*\)",
                body,
            )
            is not None,
            f"{label} primitive destination or proved value changed",
        )
        require(
            re.search(
                rf"new\s*=\s*set\s*\?\s*old\s*\|\s*{mask}\s*:\s*"
                rf"old\s*&\s*~{mask}\s*;",
                body,
            )
            is not None,
            f"{label} primitive reversible single-bit assignment changed",
        )
        require(
            re.search(
                rf"if\s*\(\s*\(old\s*\^\s*new\)\s*&\s*~{mask}\s*\)",
                body,
            )
            is not None,
            f"{label} primitive single-bit change proof is absent",
        )
        require(
            len(re.findall(r"\bnew\s*(?:\|=|&=|\^=|\+=|-=|=)", body)) == 1,
            f"{label} primitive mutates the write value more than once",
        )
        require(other_mask not in body, f"{label} primitive references the other control bit")
        require(
            not calls_in(body, {physical_guard_name, digital_guard_name}),
            f"{label} raw primitive acquired an apply-only guard",
        )
        require(
            re.search(r"\b(?:for|while|do)\b", body) is None,
            f"{label} raw primitive contains a loop",
        )

    require(
        re.search(
            r"if\s*\(\s*old\s*==\s*new\s*\)\s*"
            r"return\s+set\s*\?\s*0\s*:\s*-EALREADY\s*;",
            esd_body,
        )
        is not None,
        "ESD no-op semantics no longer accept restore and reject stale apply",
    )
    require(
        re.search(
            r"if\s*\(\s*old\s*==\s*new\s*\)\s*"
            r"return\s+set\s*\?\s*-EALREADY\s*:\s*0\s*;",
            fw_body,
        )
        is not None,
        "FWRDY no-op semantics no longer reject stale apply and accept clear",
    )
    require(
        re.search(
            r"return\s+xpon_read\s*\(\s*probe\s*,\s*PHYSET3\s*\)\s*"
            r"==\s*new\s*\?\s*0\s*:\s*-EIO\s*;",
            esd_body,
        )
        is not None,
        "ESD primitive exact readback proof changed",
    )
    require(
        re.search(
            r"if\s*\(\s*!\!\s*\(\s*readback\s*&\s*PHYSET2_FW_READY\s*\)\s*"
            r"!=\s*set\s*\)",
            fw_body,
        )
        is not None
        and re.search(
            r"if\s*\(\s*\(\s*readback\s*\^\s*new\s*\)\s*&\s*"
            r"~PHYSET2_PHY_READY\s*\)",
            fw_body,
        )
        is not None,
        "FWRDY readback no longer permits only PHYRDY variability",
    )
    require(
        fw_body.count("PHYSET2_PHY_READY") == 1,
        "FWRDY primitive has an additional PHYRDY exception",
    )
    require(
        len(re.findall(r"handoff_start_ns\s*=", clean)) == 1
        and re.search(
            r"if\s*\(\s*set\s*\)\s*"
            r"probe->handoff_start_ns\s*=\s*ktime_get_ns\s*\(\s*\)\s*;\s*"
            r"iowrite32\s*\(\s*new\s*,\s*probe->base\s*\+\s*PHYSET2\s*\)",
            fw_body,
        )
        is not None,
        "handoff timestamp is not immediately before the asserted FWRDY write",
    )

    # Freeze the complete four-wrapper graph.  Apply wrappers are guarded and
    # latch rollback before touching a primitive; rollback wrappers are exact,
    # unconditional, strictly decreasing one-line calls to the same primitives.
    primitive_names = set(WRITE_PRIMITIVES)
    primitive_callers = {
        primitive: [
            function.name
            for function in parsed_functions
            if calls_in(function.body, {primitive})
        ]
        for primitive in WRITE_PRIMITIVES
    }
    require(
        primitive_callers["update_esd_pro_bit"]
        == ["apply_esd_disable", "rollback_esd_restore"],
        "ESD primitive call graph changed",
    )
    require(
        primitive_callers["update_fw_ready_bit"]
        == ["apply_fw_ready", "rollback_fw_ready_disable"],
        "FWRDY primitive call graph changed",
    )

    apply_esd = clean_source(by_name["apply_esd_disable"].body)
    apply_fw = clean_source(by_name["apply_fw_ready"].body)
    rollback_fw = clean_source(by_name["rollback_fw_ready_disable"].body)
    rollback_esd = clean_source(by_name["rollback_esd_restore"].body)
    require(
        calls_in(apply_esd, primitive_names) == ["update_esd_pro_bit"]
        and re.search(
            r"static_guard\s*\(\s*probe\s*,\s*COLD_PHYSET2\s*,\s*"
            r"COLD_PHYSET3\s*,\s*false\s*\)",
            apply_esd,
        )
        is not None,
        "ESD apply wrapper lost its exact cold guard",
    )
    require(
        calls_in(apply_fw, primitive_names) == ["update_fw_ready_bit"]
        and re.search(
            r"static_guard\s*\(\s*probe\s*,\s*COLD_PHYSET2\s*,\s*"
            r"ACTIVE_PHYSET3\s*,\s*false\s*\)",
            apply_fw,
        )
        is not None,
        "FWRDY apply wrapper lost its exact preflight guard",
    )
    for body, latch, primitive, label in (
        (apply_esd, "esd_rollback_needed", "update_esd_pro_bit", "ESD"),
        (apply_fw, "fw_ready_rollback_needed", "update_fw_ready_bit", "FWRDY"),
    ):
        latch_match = re.search(rf"probe->{latch}\s*=\s*true\s*;", body)
        call_match = re.search(rf"\b{primitive}\s*\(", body)
        require(
            latch_match is not None
            and call_match is not None
            and latch_match.start() < call_match.start(),
            f"{label} rollback-needed latch is not set before raw apply",
        )
        require(
            clean.count(f"probe->{latch} = true;") == 1,
            f"{label} rollback-needed latch assignment changed",
        )

    require(
        re.fullmatch(
            r"\s*return\s+update_fw_ready_bit\s*\(\s*probe\s*,\s*false\s*\)\s*;\s*",
            rollback_fw,
        )
        is not None,
        "FWRDY rollback wrapper is not unconditional and strictly decreasing",
    )
    require(
        re.fullmatch(
            r"\s*return\s+update_esd_pro_bit\s*\(\s*probe\s*,\s*true\s*\)\s*;\s*",
            rollback_esd,
        )
        is not None,
        "ESD rollback wrapper is not unconditional and protective",
    )

    rollback_body = clean_source(by_name["rollback_handoff"].body)
    rollback_calls = calls_in(rollback_body, set(WRITE_WRAPPERS))
    require(
        rollback_calls == ["rollback_fw_ready_disable", "rollback_esd_restore"],
        "rollback order is not exactly FWRDY-clear then ESD-restore",
    )
    require(
        re.search(
            r"if\s*\(\s*probe->fw_ready_rollback_needed\s*\)\s*\{.*?"
            r"rollback_fw_ready_disable\s*\(.*?\}\s*"
            r"if\s*\(\s*probe->esd_rollback_needed\s*\)\s*\{.*?"
            r"rollback_esd_restore\s*\(",
            rollback_body,
            re.S,
        )
        is not None,
        "both reverse rollback operations are not independently attempted",
    )
    require(
        len(re.findall(r"\breturn\b", rollback_body)) == 1,
        "rollback path can return before both protective writes are attempted",
    )
    require(
        rollback_body.count("if (err && !write_ret)") == 2
        and rollback_body.count("write_ret = err;") == 2,
        "rollback no longer records the first write error while attempting both writes",
    )

    rollback_positions = [
        rollback_body.find("rollback_fw_ready_disable("),
        rollback_body.find("rollback_esd_restore("),
        rollback_body.find("rollback_write_result = write_ret"),
        rollback_body.find("rollback_immediate_physet2 = xpon_read"),
        rollback_body.find("msleep(PHY_READY_SETTLE_MS)"),
        rollback_body.find("capture_final_state("),
        rollback_body.find("rollback_result = verify_final_state("),
        rollback_body.find("physical_powercut_required ="),
    ]
    require(
        all(position >= 0 for position in rollback_positions)
        and rollback_positions == sorted(rollback_positions),
        "rollback write, settle, final-capture or verdict order changed",
    )
    require(
        re.search(
            r"if\s*\(\s*!\(\s*probe->rollback_immediate_physet2\s*&\s*"
            r"PHYSET2_FW_READY\s*\)\s*&&\s*"
            r"probe->rollback_immediate_physet2\s*&\s*PHYSET2_PHY_READY\s*\)\s*"
            r"msleep\s*\(\s*PHY_READY_SETTLE_MS\s*\)",
            rollback_body,
        )
        is not None,
        "500ms PHYRDY coarse settle is not after both rollback writes",
    )

    # Samples and final verification allow exactly the hardware-owned PHYRDY
    # bit to vary.  Every writable baseline and every physical/digital TX
    # barrier remains part of both the trace and the final protected verdict.
    capture_body = clean_source(by_name["capture_sample"].body)
    verify_body = clean_source(by_name["verify_sample"].body)
    for token in (
        "gpiod_get_direction",
        "gpiod_get_value_cansleep",
        "gpiod_get_raw_value_cansleep",
        "PHYSET2",
        "PHYSET3",
        "PHYSET10",
        "PHYSTA1",
        "XPON_SETTING",
        "ANASTA1",
        "MISC",
        "PHYRX_STATUS",
        "BISTCTL_PRBS_TX_EN",
        "TEST_FRAME_EN",
        "XPON_STA",
        "XPON_INT_EN",
        "XPON_INT_STA",
    ):
        require(token in capture_body, f"sample capture lost field {token}")
    require(
        re.search(
            r"\(\s*sample->physet2\s*&\s*~PHYSET2_PHY_READY\s*\)\s*"
            r"!=\s*ACTIVE_PHYSET2",
            verify_body,
        )
        is not None
        and verify_body.count("PHYSET2_PHY_READY") == 1,
        "sample verifier no longer permits only PHYRDY variability",
    )
    for token in (
        "gpio_direction != 0",
        "gpio_value != 1",
        "gpio_raw_value != 1",
        "physet3 != ACTIVE_PHYSET3",
        "physet10 != COLD_PHYSET10",
        "xpon_setting != COLD_XPON_SETTING",
        "physet3 & PHYSET3_TXEN",
        "misc & MISC_ROGUE_TX_TEST",
        "prbs_tx",
        "test_frame",
        "xpon_int_en",
    ):
        require(token in verify_body, f"per-sample protected-state guard lost {token}")

    final_capture_body = clean_source(by_name["capture_final_state"].body)
    for field in (
        "final_gpio_direction",
        "final_gpio_value",
        "final_gpio_raw_value",
        "final_physet2",
        "final_physet3",
        "final_physet10",
        "final_physta1",
        "final_xpon_setting",
        "final_anasta1",
        "final_misc",
        "final_phyrx_status",
        "final_prbs_tx",
        "final_test_frame",
        "final_xpon_sta",
        "final_xpon_int_en",
        "final_xpon_int_sta",
    ):
        require(
            final_capture_body.count(f"probe->{field} =") == 1,
            f"final protected-state capture lost {field}",
        )

    final_verify_body = clean_source(by_name["verify_final_state"].body)
    require(
        re.search(
            r"\(\s*probe->final_physet2\s*&\s*~PHYSET2_PHY_READY\s*\)\s*"
            r"!=\s*COLD_PHYSET2",
            final_verify_body,
        )
        is not None
        and final_verify_body.count("PHYSET2_PHY_READY") == 1,
        "final verifier no longer ignores only the hardware PHYRDY bit",
    )
    for token in (
        "final_gpio_direction != 0",
        "final_gpio_value != 1",
        "final_gpio_raw_value != 1",
        "final_physet3 != COLD_PHYSET3",
        "final_physet10 != COLD_PHYSET10",
        "final_xpon_setting != COLD_XPON_SETTING",
        "final_physet3 & PHYSET3_TXEN",
        "final_misc & MISC_ROGUE_TX_TEST",
        "final_prbs_tx",
        "final_test_frame",
        "final_xpon_int_en",
    ):
        require(token in final_verify_body, f"final protected-state verdict lost {token}")
    require(
        "phy_ready_settle_result" not in final_verify_body,
        "PHYRDY observation leaked into the protected rollback verdict",
    )
    require(
        clean.count("physical_powercut_required =") == 1
        and re.search(
            r"probe->rollback_result\s*=\s*verify_final_state\s*\(\s*probe\s*\)\s*;\s*"
            r"probe->rollback_complete\s*=\s*!probe->rollback_result\s*;\s*"
            r"probe->physical_powercut_required\s*=\s*"
            r"probe->rollback_result\s*!=\s*0\s*;",
            rollback_body,
        )
        is not None,
        "power-cut verdict is not derived only from the captured final protected state",
    )

    # The immutable trace is synchronous.  Its only sleep sites are the
    # bounded target waiter; the sole additional sleep is the 500ms post-write
    # PHYRDY observation in rollback_handoff.
    schedule_users = [
        function
        for function in parsed_functions
        if re.search(rf"\b{re.escape(schedule_name)}\s*\[", clean_source(function.body))
    ]
    require(len(schedule_users) == 1, "immutable trace schedule must have exactly one consumer")
    require(schedule_users[0].name == "run_handoff", "trace schedule escaped the finite runner")
    run_body = clean_source(by_name["run_handoff"].body)
    require(
        "capture_sample(" in run_body
        and "verify_result" in run_body
        and "wait_until_target(" in run_body,
        "finite trace does not wait, capture and verify every sample",
    )
    require(
        calls_in(run_body, set(WRITE_WRAPPERS))
        == ["apply_esd_disable", "apply_fw_ready"],
        "apply order is not exactly guarded ESD-disable then FWRDY-assert",
    )
    require(
        calls_in(run_body, {"rollback_handoff"}) == ["rollback_handoff"],
        "finite runner does not converge on the single rollback path",
    )
    exact_run_guards = (
        r"static_guard\s*\(\s*probe\s*,\s*COLD_PHYSET2\s*,\s*COLD_PHYSET3\s*,\s*false\s*\)",
        r"static_guard\s*\(\s*probe\s*,\s*COLD_PHYSET2\s*,\s*ACTIVE_PHYSET3\s*,\s*false\s*\)",
        r"static_guard\s*\(\s*probe\s*,\s*ACTIVE_PHYSET2\s*,\s*ACTIVE_PHYSET3\s*,\s*true\s*\)",
    )
    require(
        calls_in(run_body, {"static_guard"}) == ["static_guard"] * 3
        and all(re.search(pattern, run_body) is not None for pattern in exact_run_guards),
        "finite runner lost an exact cold/intermediate/active guard",
    )
    run_positions = [
        run_body.find("before_physet2 = xpon_read"),
        run_body.find("before_physet3 = xpon_read"),
        run_body.find("static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, false)"),
        run_body.find("apply_esd_disable("),
        run_body.find("static_guard(probe, COLD_PHYSET2, ACTIVE_PHYSET3, false)"),
        run_body.find("apply_fw_ready("),
        run_body.find("static_guard(probe, ACTIVE_PHYSET2, ACTIVE_PHYSET3, true)"),
        run_body.find("rollback_result = rollback_handoff("),
    ]
    require(
        all(position >= 0 for position in run_positions)
        and run_positions == sorted(run_positions),
        "finite runner preflight/apply/rollback order changed",
    )

    wait_body = clean_source(by_name["wait_until_target"].body)
    require(wait_body.count("msleep(") == 1, "target wait must have one coarse sleep site")
    require(wait_body.count("usleep_range(") == 1, "target wait must have one fine sleep site")
    sleep_sites = {
        function.name: len(
            re.findall(r"\b(?:msleep|usleep_range|fsleep)\s*\(", clean_source(function.body))
        )
        for function in parsed_functions
        if re.search(r"\b(?:msleep|usleep_range|fsleep)\s*\(", clean_source(function.body))
    }
    require(
        sleep_sites == {"wait_until_target": 2, "rollback_handoff": 1},
        "sleep sites escaped the finite trace or post-rollback PHYRDY settle",
    )

    probe_body = clean_source(by_name["xr500v_digital_handoff_probe"].body)
    require(
        re.search(
            r"static_guard\s*\(\s*probe\s*,\s*COLD_PHYSET2\s*,\s*"
            r"COLD_PHYSET3\s*,\s*false\s*\)",
            probe_body,
        )
        is not None,
        "platform probe lost its exact full-word cold preflight",
    )
    run_call = probe_body.find("run_handoff(")
    ordered_gates = [
        probe_body.find("!arm_digital_handoff"),
        probe_body.find("device_property_read_bool"),
        probe_body.find("of_machine_is_compatible"),
        probe_body.find("tx_disable_guard("),
        probe_body.find("verify_factory_hash("),
        probe_body.find("static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, false)"),
        probe_body.find("debugfs_create_file("),
        run_call,
    ]
    require(
        ordered_gates == sorted(ordered_gates) and all(position >= 0 for position in ordered_gates),
        "arm/DT/root/GPIO/factory/cold/debugfs gates do not all precede the handoff",
    )

    status_body = by_name["handoff_status_show"].body
    for label in EXPECTED_DEBUG_LABELS:
        require(label in status_body, f"debugfs report lost field {label}")
    for field in (
        "rollback_write_result",
        "rollback_result",
        "rollback_complete",
        "physical_powercut_required",
        "rollback_immediate_physet2",
        "final_physet2",
        "phy_ready_settle_result",
        "final_physet3",
        "final_physet10",
        "final_xpon_setting",
        "final_misc",
        "final_prbs_tx",
        "final_test_frame",
        "final_xpon_int_en",
        "final_physta1",
        "final_anasta1",
        "final_phyrx_status",
        "final_xpon_sta",
        "final_xpon_int_sta",
        "final_gpio_direction",
        "final_gpio_value",
        "final_gpio_raw_value",
    ):
        require(f"probe->{field}" in status_body, f"debugfs report does not publish {field}")

    print(f"PASS: {path}")
    print("writes: two raw single-bit primitives; four-wrapper closed call graph")
    print("apply: guarded ESD clear then timestamped FWRDY set; rollback latched first")
    print("rollback: unconditional FWRDY clear then ESD restore; both attempted")
    print("PHYRDY: sole variable bit; 500ms post-write observation is non-fatal")
    print("schedule_ms:", ",".join(str(value) for value in schedule))
    print("powercut: captured final protected-state verdict only; full debug report")
    print("I2C/IRQ/async/reset/clock/SCU/GPON-MAC/PLL/counter write paths absent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

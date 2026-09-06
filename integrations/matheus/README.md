# XR500v: Airoha/EcoNet integration review set

Public source snapshot, 2026-09-06. This is **not a complete firmware overlay,
not a ready-to-flash release, and not a claim that unmodified Matheus HEAD
works on the XR500v**. Do not copy this directory wholesale over a build.
The original repository-root overlay and its older build instructions remain
separate. This directory preserves the newer work without conflating them.

## What works in our local integration

Hardware: TP-Link Archer XR500v v1, EN751221 (big-endian MIPS), EN7570 optical
front end, cascaded MT7530 switches, MT7603/MT7662 radios, dual-channel Le9642.

- Independent cold boot from NAND; persistent configuration and sysupgrade.
- GPON O5, OMCI and PPPoE without inheriting a warm stock MAC state.
- Four LAN ports, both radios, USB storage, buttons and PON/LOS panel behavior.
- About 715/700 Mbit/s wired and 421/285 Mbit/s over 5 GHz in local tests.
  These are observations, not guaranteed performance. An intervening reboot
  prevents attributing the entire WiFi improvement to the PPE fixes alone.
- Two FXS ports, local SIP calls with two-way audio, ring/answer/hang-up,
  echo cancellation and mild denoise. Pre-call keypad 1#/2# now reaches the
  other port after correcting the PCM-release/hook ordering.
- Native LuCI status/audio/accounts pages, English/gettext, selected existing
  IPv4 interface, write-only SIP password, explicit confirmation,
  backups and idle checks for changes. Provider voice and emergencies unvalidated.

The tested local firmware used Linux 6.18.41 and an accumulated local patch
series. This **selected review set does not reproduce that complete build**.
A stress-time mt76 headroom warning remains under investigation;
an intermittent ringing/silent-call report also needs reproducible traces.

## Source map / suggested integration order

| Area | Sources | Review boundary |
| --- | --- | --- |
| Board | DTS, board scripts, `board-image.mk.reference` | Port against current bindings; no operator profile |
| NAND | BMT source, 930-194/931 patches, preinit/platform/header tool | Flash geometry/readback/rollback must be validated together |
| GPON | GPIO27 hog, reset/sleep-counter and LED patches | Local driver baseline; not directly applicable to reorganized xPON APIs |
| WiFi/PPE | WHNAT source, 999999998/999999999 and mt76 hooks | Uses local `econet_*` GEN1 paths; adapt to current `airoha_ppe_v1_*` |
| PCM/FXS | `overlay/package/kernel/econet-pcm` | Reconstructed experimental driver; see provenance and unsafe debug nodes |
| SIP/LuCI | `overlay/package/xr500v-voip`, `luci-app-xr500v-voip` | Separate userspace; does not configure VLANs/routes/firewall |

GPIO27 is the XR500v's active-low BOSA TX supply control. It is not a universal
EN751221 pin. With identical software DBRu configuration, toggling it changed
upstream approximately 19 -> 294 -> 21 Mbit/s locally. The later cold-boot
combination uses hardware DBRu and the OEM-style sleep counter before O2.

WHNAT is the PPE plus CPU wireless handoff path, **not WED**. The upstream
discussion now favors WHNAT on EN751221; do not enable WED merely because an
experimental SoC node exists. Existing packet ownership, pre-GRO ordering,
descriptor reserve and backpressure rules matter.

## Board defaults

The reference DTS disables xPON. WAN VLAN and protocol settings are
configured for the target deployment. Factory MAC addresses and optical
calibration are read from the board's nvmem cells.

## Tests

From this directory (host, no router access):

```sh
python3 tests/test_whnat_port.py
python3 tests/test_image_header.py
gcc -O2 tests/test_voip_dtmf.c -lm -o /tmp/xr500v-dtmf-test
/tmp/xr500v-dtmf-test
gcc -O2 tests/test_voip_keypad.c -lm -o /tmp/xr500v-keypad-test
/tmp/xr500v-keypad-test
python3 tests/test_luci_voip_backend.py | ucode -
python3 tests/test_luci_voip_config.py | ucode -
python3 tests/test_luci_voip_accounts.py | ucode -
python3 tests/test_voip_profile.py | ucode -
```

The ucode fixtures mock fs/UCI/ubus.
`tests/test_ppe_rx_aging.py PATH_TO_PREPARED_airoha_ppe.c` extracts the **local
baseline's** actual C policy functions and runs 32/64-bit freestanding tests.
It requires the matching prepared kernel source; it is not a test of current
upstream merely because names can be changed. Tests of models do not replace
hardware/long-duration validation. See `VALIDATION.md` for recorded scope.

## Current upstream references

Reviewed OpenWrt branch `airoha_en7523` at
`95466b533a77283d76fc299072e060664a9b0389` and kernel `airoha_en7523_all` at
`2e2cf91fe84467d77649efebd99a28284f2124b3`. Neither current board list included
the XR500v. These are **review targets**, not asserted tested base revisions.

- [GPON / integration discussion](https://sirherobrine23.com.br/airoha/kernel/issues/5)
- [PCM discussion](https://sirherobrine23.com.br/airoha/kernel/issues/1)
- [Matheus' WHNAT/PCM response](https://sirherobrine23.com.br/airoha/kernel/issues/5#issuecomment-5131)
- [Merbanan's PCM review offer](https://sirherobrine23.com.br/airoha/kernel/issues/5#issuecomment-5132)
- [Updated WED assessment](https://sirherobrine23.com.br/airoha/kernel/issues/5#issuecomment-5134)

The next step is a small board-support series with its prerequisites, followed
by independently reviewable PPE/WHNAT and PCM work. This export intentionally
does not turn on flashable support in another maintainer's build.

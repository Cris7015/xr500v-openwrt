# TP-Link Archer XR500v — OpenWrt community port

Experimental OpenWrt work for the **Archer XR500v v1**, built around the
**EcoNet EN7526G / EN751221 family**: GPON Internet, Wi-Fi acceleration and
two analog telephone ports in one router.

[OpenWrt wiki and photos](https://openwrt.org/inbox/toh/tp-link/archer_xr500v_v1) ·
[Current source and tests](integrations/matheus/) ·
[Focused contributions](integrations/matheus/focused/) ·
[Release notes](https://github.com/Cris7015/xr500v-openwrt/releases) ·
[Historical documentation](docs/Home.md)

**Development status — 8 September 2026:** this is a community project, not
official XR500v support in OpenWrt release images. The newer GPON integration
has been tested locally; its public source export is a review set, **not a
complete reproducible build or a ready-to-flash firmware release**.

## Results in the local integration

These results describe the local Matheus-based integration, not every image
available in Releases.

| Area | Exercised locally |
| --- | --- |
| Boot and persistence | Cold boot from NAND, persistent configuration and board-specific sysupgrade |
| GPON | O5, OMCI and PPPoE without a warm handoff from stock |
| Ethernet | Four Gigabit LAN ports through cascaded MT7530 switches |
| Wi-Fi | 2.4 GHz and 5 GHz APs, with the WHNAT CPU/PPE handoff |
| USB and panel | USB storage, buttons and front-panel LEDs, including PON/LOS |
| Telephony | Two-way local SIP calls on both FXS ports, ring/hook control, echo cancellation and mild denoise |
| LuCI | Native status, audio configuration and SIP account pages |

Selected local runs measured approximately **715/700 Mbit/s wired** and
**421/285 Mbit/s over 5 GHz** (download/upload). They are observations, not
guaranteed performance or a long-duration qualification. See the
[validation scope and limitations](integrations/matheus/VALIDATION.md).

Still open: the stress-time mt76 headroom warning, longer stability testing,
and the remaining ports to current upstream APIs. Operator VoIP, in-call IVR
and emergency calling are not validated. The
[ALSA/ASoC and SLIC refactor](integrations/matheus/focused/ASOC-PLAN.md) is planned,
not implemented.

## Start here

- **Review the current work:** [integration overview](integrations/matheus/)
  and [focused submissions](integrations/matheus/focused/). The public
  reference DTS leaves optical service disabled.
- **Try an existing image:** read the exact
  [release notes](https://github.com/Cris7015/xr500v-openwrt/releases) and match
  your board, bootloader and BMT variant first. A newer tag does not mean an
  image is compatible with every XR500v.
- **Build or study the older port:** use the
  [archived README and build instructions](docs/README-history-before-2026-09-08.md)
  and [historical subsystem documentation](docs/Home.md). Those describe the
  separate cjdelisle-based overlay, not the complete newer GPON integration.

**Important:** the
[August Bootbase 2019 / BMT81 release](https://github.com/Cris7015/xr500v-openwrt/releases/tag/v2026.08.28-bootbase2019-bmt81)
is revision-specific and **does not support GPON**. It is not an image of the
newer local integration. Confirm your recovery procedure before writing flash;
do not mix images, modules or instructions from different variants.

## Hardware

- **Tested SoC:** EcoNet EN7526G, in the EN751221 software family; MIPS 34Kc, big-endian.
  Identified using the manufacturer's package-code rule, not a visible chip marking.
- **Memory:** 256 MiB RAM and 128 MiB SPI-NAND; bootloader/BMT variants exist.
- **Networking:** four Gigabit LAN ports, cascaded MT7530 switches, GPON WAN.
- **Radios:** MT7603 (2.4 GHz) and MT7662 (5 GHz).
- **Peripherals:** USB 2.0 and two FXS ports on a dual-channel Le9642 SLIC.

A matching SoC does not make another router compatible with XR500v firmware.
SLIC power profiles are board-specific; see
[provenance and electrical limits](integrations/matheus/PROVENANCE.md).

## Help move it forward

Useful contributions include tests on a matching XR500v revision, reproducible
bug reports and small driver or documentation patches. Report the board/build
revision, steps and relevant log excerpts in
[Issues](https://github.com/Cris7015/xr500v-openwrt/issues).

The [focused contribution index](integrations/matheus/focused/) separates board
basics and the PPE multicast fix from the remaining NAND, WHNAT, aging and
PCM/SLIC work. Other EcoNet devices may benefit from individual driver changes;
the firmware images and electrical settings are not interchangeable.

## Credits

This work builds on [Matheus / Sirherobrine23's Airoha trees](https://sirherobrine23.com.br/airoha),
[merbanan's contributions](https://github.com/merbanan),
[Caleb DeLisle's OpenWrt work](https://github.com/cjdelisle/openwrt), and the
OpenWrt/Linux community. See the [provenance notes](integrations/matheus/PROVENANCE.md)
and the notices in each source file.

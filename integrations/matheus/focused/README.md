# Focused submissions after review

The original mixed reference submission is superseded by smaller changes.
Its source archive remains in the parent directory for provenance and future
porting; it is not the patch set being proposed for wholesale integration.

## Submitted separately

- [OpenWrt PR64: board basics](https://sirherobrine23.com.br/airoha/openwrt/pulls/64)
  adds one DTS file: 256 MiB memory, UART, panel LEDs/buttons, USB2 and physical
  LAN labels. It does not register a firmware image or include the old
  NAND/GPON/PCM/PPE prerequisites. Flash/optics are disabled, TX_DISABLE is
  held asserted, and Ethernet/PCIe remain disabled in the base dtsi. The
  pin mapping was exercised in the local integration; the reduced file is
  CPP/dtc-checked but has not had a fresh hardware boot.
- [Kernel PR50: V1 multicast lookup guard](https://sirherobrine23.com.br/airoha/kernel/pulls/50)
  is seven lines directly against `airoha_ppe_v1_rx_check()` at kernel base
  `2e2cf91fe84467d77649efebd99a28284f2124b3`. It bypasses a bind lookup, not
  packet delivery. The actual modified function passes synthetic tests and
  the full translation unit compiles for MIPS using the current Airoha
  headers and the prepared Linux6.18.41 environment. No linked kernel build,
  router installation or new hardware benchmark is claimed.

The kernel commit is exported in `patches/0001-airoha-v1-skip-multicast.patch`.
For a checkout of that kernel branch:

```sh
python3 tests/test_v1_multicast.py /path/to/kernel/drivers/net/ethernet/airoha/airoha_ppe.c
```

The test uses host stubs and never performs network or hardware IO.

## Separate work, not implemented by these PRs

- **PPE timestamp aging:** port the last-used behavior with correct flow
  lifetime/ownership, locking and hardware timestamp handling in the current
  shared V1 implementation. Renaming old `econet_*` functions is not enough.
- **WHNAT:** adapt the radio hooks, CPU handoff, descriptor ownership and
  backpressure to current kernel/mt76 APIs as its own series. Not bundled
  into the multicast guard and not replaced by experimental WED.
- **NAND / complete board support:** review BMT, geometry, nvmem, image
  validation and readback/rollback before enabling flashable images. PCIe,
  radio calibration and GPON wiring/power sequencing follow separately.
- **PCM / SLIC:** see [ASOC-PLAN.md](ASOC-PLAN.md). The SoC ASoC/PCM component,
  ZSI/ISI transport and SLIC control API need distinct ownership. The old
  working character-device implementation was not relabeled as an ALSA port.

No force-push is needed to preserve the old reference branch/discussion:
[original PR63](https://sirherobrine23.com.br/airoha/openwrt/pulls/63).

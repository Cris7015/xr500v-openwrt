# Provenance and review limits

This publication preserves existing notices; it is not a legal certification
that every experimental driver is ready for upstream licensing acceptance.

- Airoha/EcoNet kernel/OpenWrt work is based on Matheus/Sirherobrine23's trees
  and the EcoNet community work. The root historical overlay uses the separate
  cjdelisle baseline; do not treat the histories as interchangeable.
- WHNAT is a forward-port of the older XR500v PPE/mt76 handoff work. The
  `airoha_whnat` files and integration patches retain this lineage. The PPE
  multicast/aging change targets the local GEN1 implementation; its hardware
  timestamp layout is not the newer MediaTek GEN2 layout.
- PCM and SLIC code has GPL notices, but is an experimental reconstruction
  using available OEM GPL source, module disassembly/Ghidra observations,
  register traces and hardware tests. Source comments identify dependencies
  such as `spi.ko`, `pcm1.ko`, `slic3_main.c` and the Le9642 profile family.
  Profile tables and inferred programming sequences need maintainer review;
  working on one board is not evidence of a generic supported SLIC API.
  In particular, the SLIC power-converter/profile configuration is specific
  to this board. BB, IB and TB supply arrangements are not interchangeable;
  selecting the wrong one can physically damage hardware. Do not load these
  fixed-address/profile drivers on another model just because it has the
  same SoC or SLIC name, or experimentally switch power modes to test them.
- The Le9642 is a dual-channel chip, not two independent Le9642 packages.
  Legacy single-channel debugfs tests remain in the source and can reset,
  reconfigure or drive the hardware. They are **not** safe status interfaces
  and are deliberately not exposed through LuCI. Review/removal is a separate
  prerequisite for general-purpose upstream inclusion.
- `xr500v-voip` contains the Baresip audio module, control manager, generated
  runtime profiles and pre-call DTMF collector. It links SpeexDSP; the copied
  `speexdsp.COPYING` notice accompanies it. Shared SLIC/PCM ownership, dual-line
  isolation and hook transition ordering are documented in source and tests.
- The native LuCI application is GPL-2.0-only, with scoped API permissions
  and validated configuration actions.

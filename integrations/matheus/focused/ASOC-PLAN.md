# PCM/SLIC refactor: separate follow-up, not implemented

This responds to merbanan's review of the original mixed reference PR:
https://sirherobrine23.com.br/airoha/openwrt/pulls/63#issuecomment-5159

This is an ASoC design proposal; the port is not yet implemented.
The existing dual-FXS implementation provides the behavioral reference.

## Proposed ownership

1. **SoC PCM/TDM driver:** ASoC CPU DAI and a PCM component for the shared DMA
   engine. Own its MMIO/IRQ, clocks, DMA rings, stream state and slot mapping.
   The current descriptor engine is custom: using generic DMAengine PCM would
   first require a real DMAengine provider, not just changing registration.
   Both FXS streams must share the running engine without resetting each other.
2. **ZSI/ISI control transport:** determine with maintainers whether this
   belongs in an ASoC extension or a separate control-bus/regmap abstraction.
   Register transactions need the PCM control clock even when no audio stream
   is open. Do not invent a public transport API before reviewing existing
   implementations and the hardware framing constraints.
3. **Le9642 SLIC driver:** separate chip driver consuming that transport.
   Identify the device, apply the correct board power profile and manage
   hook/ring/line-feed state. ALSA audio controls alone do not define a complete
   telephony API. Review earlier SLIC/telephony approaches with maintainers
   before exposing a new userspace ABI for these states/events.
4. **Board description / sound card:** describe clocks, TDM wiring, the two
   ports and the actual electrical supplies/profile. GPIOs and BB/IB/TB
   converter assumptions must not be inferred from the SoC model alone.
5. **Userspace:** Baresip/SIP and LuCI stay separate. An ALSA backend can replace
   the lab character audio devices only once duplex audio/control work. Keep
   the tested AEC/denoise reference for comparisons; do not tie the new CPU
   DAI to one SIP stack or to operator provisioning.

## Keep out of the first driver submission

- Fixed-address register sweeping and destructive legacy debugfs probes.
- SIP, dialing policy and operator network configuration belong in userspace.
- A supposedly generic SLIC API implemented solely for one board without
  maintainer agreement.
- Unreviewed redistribution assumptions for reconstructed profile tables.

## Validation gates

Start with source/provenance and the clock/slot model; then test ALSA PCM
capture/playback and two simultaneous streams on the target hardware.
Verify that open/close, underrun recovery and the second stream never reset
the first, and that the control clock remains available in idle. Test hook,
ring, cancel, remote hangup and prolonged duplex audio after that.

The old application had a false hook sample when PCM release changed the
line-feed state. The new ownership model needs an explicit transition rule,
not just a copy of the character-device workaround. Power-mode changes and
cross-board tests need verified electrical documentation before execution.

The first focused submissions cover board basics and a small PPE RX guard.
This architecture migration is a separate follow-up.

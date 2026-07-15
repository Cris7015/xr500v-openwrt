# EN757x repository cross-check and PADO boundary

Date: 2026-07-14

## Public reference

The public repository examined here is
[`Sirherobrine23/airoha_xpon_en757x`](https://github.com/Sirherobrine23/airoha_xpon_en757x),
an archived collection of EN757x PHY material, the older EcoNet xPON stack and
a first Linux driver attempt.

Repository state checked on 2026-07-14:

```text
default branch: main
HEAD:           950199a8de6b75e76906a7c1b39b7a9a3e2913f9
commits:        8
archived:       yes
repository license: no global/GitHub-detected license; mixed file-level notices
```

## What is directly reusable

- The EN757x identity rule corroborates this XR500v: I2C register `0x0170`
  reports hardware ID `0x03`, while `0x015c` values below `0x03` identify
  EN7570 and later variants identify EN7571.
- The BoB layout and versioned magic at offset `0x94` are useful independent
  checks when locating per-unit calibration data.  Published samples have
  different lengths, and this XR500v's OEM blob is 400 bytes; none is
  permission to use another board's analogue values.
- `v1/xpon_phy/src/mt7570.c` and `mt7570_reg.h` describe the same EN7570 family
  already present in the XR500v GPL tree.
- `v1/xpon/src/gpon/` is useful for understanding the order of PLOAM O1-O5,
  EqD/ranging, Alloc-ID/T-CONT and GEM setup.
- The vendor PWAN/QDMA path identifies where packet counters and descriptors
  must eventually be instrumented after optical activation and OMCI exist.

## Direct comparison with the XR500v GPL tree

The new collection does not add a second independent EN7570 register map:

```text
mt7570_reg.h, both copies:
  7f26080f0d5e9ba3808fde398d89b754817cd0c22bd978847de9af5a35eb334c
```

After ignoring whitespace, the meaningful `mt7570.c` changes are small:

- version `107` to `108`;
- `kernel_read()` / `kernel_write()` in place of the removed `set_fs()` file
  access pattern;
- a bound check for the environmental-temperature calibration input.

It therefore corroborates the existing analysis but does not provide a new
EN7570 LOS/APD initialization recipe.  The GPON PLOAM copy has a handful of
vendor-branch deltas, including retained upstream-overhead fields, optional
EqD tracing and a different EN7521 recovery guard.  Those are behavioral
clues, not code to transplant.

## Why this first Linux driver cannot receive PADO

The patch's netdev transmit callback logs the skb and immediately frees it.
It does not submit a QDMA descriptor.  There is no corresponding functional
RX path, registered/functional IRQ path, GEM/T-CONT data path, PLOAM state
machine or OMCI integration in that driver attempt.  Matching stock optical
parameters cannot compensate for those missing layers.

For this XR500v's normal stock Movistar HGU service, the expected chain before
PADO can be meaningful is:

```text
downstream optical sync
  -> PLOAM O1/O2/O3/O4/O5 and EqD
  -> Alloc-ID / T-CONT
  -> OMCI normally provisions the service
  -> service GEM and VLAN mapping
  -> WAN-QDMA TX of PADI
  -> WAN-QDMA/GEM RX of PADO
```

If a future test reaches O5 but not PADO, it must be split into measurable
boundaries: did PADI enter QDMA, was its GEM/T-CONT valid, had the normal OMCI
service configuration opened traffic, did the service VLAN match, and did any
GEM/QDMA RX counter advance?  OMCI is not a protocol-level prerequisite when
equivalent provisioning is supplied out of band; it is the stock service path
observed here.

## XR500v implications

No EN7571 analogue constants, APD values, BoB blob, IRQ number or descriptor
bitfield should be copied into this EN751221/EN7570 board.  In particular, the
XR500v kernel is big-endian MIPS; future QDMA and PON descriptors must use
explicit masks and endian conversions rather than vendor C bitfields.

The current XR500v work is still below the PADO boundary.  Phase 27 later ran
once with TX physically disabled: its terminal message proves all 15 guarded
writes and 12 samples completed, but a local capture error lost the detailed
LOS transition series.  The mandatory physical cut restored the passive cold
state.  This one-shot is consumed and must not be repeated.  The safe stop is:

- passive EN7570 and xPON PHY diagnostics only;
- the phase-27 writer now requires exact `0x0170/0x015c = 0x03/0x01` before
  the cold map and before its first possible write, so an EN7571 cannot pass;
- phase-27 module and tmpfs artifact absent after verified cold recovery;
- no speculative PON-MAC status reads until read-to-clear behavior and clock
  dependencies have been audited from the exact EN751221 source;
- no active laser, APD or QDMA operation.

The detailed phase-27 optical boundary is not resolved and cannot be inferred
from its terminal summary.  The clean implementation path remains modular:
passive PON-MAC counters, minimum PLOAM through O5, a raw OMCI channel, one
verified GEM/T-CONT, then a big-endian WAN-QDMA data path.  See
`2026-07-15-gpon-phase27-live-run-partial-evidence.md` for the retained
evidence boundary.

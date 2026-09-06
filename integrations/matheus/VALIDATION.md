# Validation, 2026-09-06

## Source checks

- Synthetic image header: geometry/payload preservation and rejection of
  truncated, invalid-LZMA and invalid-SquashFS inputs: PASS. No flash writes.
- WHNAT byte/tag rollback, reserved descriptor and ownership/result model:
  PASS. This is an algorithm model, not a substitute for kernel packet tests.
- DTMF: all 16 tones, phase/DC variation, held-key debounce and negative
  dial-tone/single-tone/noise fixtures: PASS.
- Real keypad collector with socket-backed PCM: pre-release hook check,
  real on-hook cancellation, worker/reap, URI/self-call guard and JSON bounds:
  PASS. This fixes the source-level transition mistake found in the first
  physical keypad trial; it does not prove all DECT behavior.
- Status, audio config, account RPC and runtime renderer fixtures: PASS.
  The ucode interpreter ran on the test device with **all** fs/UCI/ubus access
  stubbed. No real line/control/config operation is performed by those tests.
- PPE policy extraction from the matching locally prepared Linux6.18.41:
  32-bit and 64-bit PASS, including hardware timestamp/jiffies wrap, owner
  validity and aging after hardware-only activity. This checks the local
  `econet_*` implementation, not a completed forward-port to Matheus HEAD.
- Exported shell scripts and native LuCI JavaScript: syntax PASS.
- Selected patch files: parse as unified diffs. Applicability to current
  upstream is **not** claimed.
- Board DTS: CPP+dtc PASS using the OpenWrt EN751221 dtsi from
  `95466b533a77283d76fc299072e060664a9b0389` and the local Linux6.18.41 headers.
  Existing dtsi warnings remain for i2cclock unit-address and unnecessary
  switch address/size cells. This is structural compilation, not a new boot,
  full dt-schema validation, NAND compatibility or electrical certification.

## Remaining validation

- A complete build or boot of Matheus' current branch with these changes.
- Safe NAND upgrade across all bootbase/flash revisions. The reference image
  recipe is intentionally not hooked into `TARGET_DEVICES`.
- Operator SIP/emergency calling, universal OLT compatibility, in-call analog
  DTMF-to-RFC4733 conversion, or resolution of the stress-time mt76 warning.
- Long-term performance/aging qualification or all remote-hangup/reject cases.

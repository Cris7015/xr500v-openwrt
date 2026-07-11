# GPON no-OLT bring-up — phase 4 RX-only init prototype (compile-only)

Date: 2026-07-11

## Result

A first write-capable xPON PHY prototype now compiles against the XR500v
OpenWrt 6.12.80 kernel, but it was deliberately not included in an image,
described in DT, transferred to the router or loaded.

The prototype is:

```text
package/kernel/xr500v-xpon-rx-init/
```

Its first stage implements only the one common RX-side operation found in both
the OEM `phy_dev_init()` and Merbanan's `pon_phy_esd_deglitch_clear()`:

```text
PHYSET3.ESD_PRO: 1 -> 0
```

There is no generic MMIO write helper.  The only PHY write function can modify
only `ESD_PRO`, refuses to operate if `TXEN` is already high, and always writes
`TXEN` low.  The original `ESD_PRO` state is retained for failure rollback and,
by default, module-remove rollback.

## Why the OEM/full experimental init was split

The OEM `pon_phy_init()` and Merbanan driver combine RX, TX, monitoring and
protocol setup.  Their writes were classified as follows:

| Operation | Classification for first RX-only test | Decision |
|---|---|---|
| EN7521 top-level xPON PHY reset | resets the entire PHY | defer |
| Clear `PHYSET3.ESD_PRO` | signal-detect deglitch, common OEM/Merbanan step | stage 1 only |
| Enable PBUS access | already functional because the live CSR window reads correctly | omit |
| Change legacy LED/GPIO mux | unrelated and board-specific | omit |
| Enable/latch/clear counters | changes observation state | omit |
| Program GPON delimiter/guard | upstream TX formatting | omit |
| Enable/clear PHY IRQs | asynchronous state mutation | omit |
| Program `XPON_SETTING` polarity | includes burst/TX-fault/TX-SD controls | defer |
| Select GPON mode | already GPON; rewriting it is unnecessary | omit |
| Pulse PLL/software/count reset | disturbs both RX and TX state | defer |
| Set firmware-ready | MAC coordination; OEM call is commented out | omit |
| EN7570 init/calibration | resets LDDLA and changes ADC/APD/TX/LOS state | forbidden |
| Start EN7570 worker/DDMI | continuous analogue/control writes | forbidden |
| MAC, PLOAM or QDMA setup | outside PHY RX experiment | forbidden |

The live baseline already has RX/TX PLL lock and completed impedance/VCO
calibration, so resetting those blocks before testing the isolated deglitch
bit would add risk without answering the first question.

## Fail-closed gates

The prototype cannot bind in the shipping firmware.  A future test image must
replace the passive diagnostic node with compatible
`econet,en751221-xpon-rx-init-experimental` and explicitly add
`econet,allow-rx-only-init`.  Loading also requires `arm_rx_init=1`.

Before mapping/writing the PHY, it requests the active-high GPIO16 TX-disable
line as `GPIOD_OUT_HIGH`.  It then aborts unless all of these are true:

- GPIO16 `TX_DISABLE` is an output and logically asserted;
- `PHYSET3.TXEN` is clear;
- `PHYSET10` already reports GPON mode;
- rogue-ONU TX test mode is clear;
- PRBS TX is zero;
- test-frame TX is zero;
- xPON interrupts are disabled.

The same preflight runs again after clearing `ESD_PRO`; a failure attempts to
restore its original state.  The prototype contains no EN7570, I2C, MAC, QDMA,
counter, interrupt, PLL, reset or mode-write path.

## Build and binary audit

The package was selected as `=m` only in the local OpenWrt build configuration
and built explicitly.  It remains absent from `DEVICE_PACKAGES`.

```text
module:       xr500v-xpon-rx-init.ko
kernel:       6.12.80 SMP preempt, MIPS32_R2, 32BIT
SHA-256:      21a28805b40e613b70811c0524f163a7f0cdba6f646669900dfc7725898b6b7c
MMIO writes:  one iowrite32 relocation / one source write site
I2C symbols:  none
reset symbols:none
IRQ/timer:    none
clock/regmap: none
```

The phase-3 shipping image was rescanned after this build:

```text
prototype in SquashFS: no
prototype autoload:    no
experimental DT node: no
DT allow property:     no
```

The router was not rebooted or modified during phase 4.  Its persistent passive
probe remained at FSM `0x3`, no RX sync, `TXEN=0`, GPIO16 TX-disable asserted,
all TX generators off and EN7570 Tx-fault pending.

## Criteria for the first execution

The first active test still requires no fibre.  It should use a dedicated test
image that replaces the passive node, captures the complete phase-3 snapshot,
loads the prototype armed, reads the snapshot again, unloads it to exercise
rollback, and finally rechecks PPPoE plus the physical TX-disable line.

Success means only:

1. `ESD_PRO` clears and reads back;
2. GPIO16 remains asserted and every TX generator remains off;
3. no unexpected register outside `PHYSET3.ESD_PRO` changes;
4. unload restores the original bit;
5. Ethernet/PPPoE remains unaffected.

It is not expected to reach RX sync or PHY-ready without downstream optical
signal.  PLL/reset, polarity, LOS calibration and EN7570 setup remain later,
separately reviewed stages.

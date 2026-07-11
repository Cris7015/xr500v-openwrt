# GPON no-OLT bring-up — phase 3 persistent xPON safety baseline

Date: 2026-07-10

## Result

The temporary phase-0 MMIO reader is now a persistent device-tree platform
driver.  It owns the xPON PHY resource at `0x1faf0000`, autoloads from the
XR500v image and exposes `status` and raw `regs` under:

```text
/sys/kernel/debug/xr500v-gpon/
```

The driver contains no MMIO write helper, requests no IRQ, starts no poller and
does not latch or clear counters.  It also reserves the board's physical
TX-disable line with `GPIOD_ASIS`, which reads the existing direction and level
without changing either.

A clean boot reported:

```text
mode:                    GPON
phy_fsm_state:           0x3
rx_sync:                 no
tx_enable:               no
tx_disable_direction:    output
tx_disable_raw:          high
tx_disable_asserted:     yes
rogue_onu_test_mode:     no
prbs_tx_enable_raw:      0x00000000
test_frame_enable:       0x00000000
interrupt_enable:        0x00
irq_tx_fault:            pending
mmio_writes_performed:   0
counter_latch_or_clear:  no
reset_or_mode_change:    no
polling_or_irq_handler:  no
```

The EN7570 independently continued to report Tx-fault set, with no reset,
initialisation, ADC, APD or laser-control operation.

## Physical TX-disable provenance

The exact OEM rootfs used by this XR500v maps logical LED/control entry 42 as:

```text
LED_PHY_TX_POWER_DISABLE = 42
42        16       1       0        1
```

That is GPIO16 in on/off mode with the configured polarity.  The OEM
`phy_tx_ctl(PHY_DISABLE)` calls `ledTurnOn(LED_PHY_TX_POWER_DISABLE)`; with the
entry's `onoff=1`, the LED/GPIO implementation drives the physical line high.
Live register reads before integration found GPIO16 configured as output,
open-drain and high.  After requesting it `GPIOD_ASIS`, gpiolib showed:

```text
gpio-528 (... |tx-disable) out hi
```

The active-high DTS description therefore matches both the OEM control path
and the observed hardware state.

## Audit of Merbanan's current PON-PHY driver

The `econet-eth-mainline` `airoha-en7512-pon-phy` driver is useful reference
code but is not read-only on probe.  Before returning from `probe()` it:

1. clears the PHY signal-detect deglitch bit;
2. enables and clears counters;
3. writes the GPON delimiter/guard word;
4. enables PHY interrupts;
5. programs board/transceiver polarity bits;
6. selects GPON or EPON mode and pulses PLL/software reset;
7. clears pending W1C interrupt status;
8. starts a polling timer.

Its counter debugfs view also writes latch bits before reading.  Importing it
unchanged would therefore destroy the passive baseline and, for EPON mode,
eventually set `PHYSET3.TXEN`.  Phase 3 deliberately imports none of that
behaviour.

## Safety contract for the first active RX-only experiment

Any active xPON PHY prototype must replace, rather than coexist with, the
diagnostic platform driver and must fail closed unless all of these preconditions
hold:

- the physical GPIO16 `TX_DISABLE` line is owned as active-high output and
  reads asserted;
- `PHYSET3.TXEN` is clear;
- rogue-ONU test, PRBS TX and test-frame generators are disabled;
- the requested mode is GPON; EPON continuous-TX mode is forbidden;
- EN7570 register writes remain disabled;
- no MAC/PLOAM or QDMA data path is registered.

The first write-enabled prototype should then split RX/SerDes setup from
Merbanan's combined init and remain compile-only until every write has an OEM
provenance and rollback value.  In particular, counter and delimiter writes
are not prerequisites for merely testing RX PLL/FSM progression.

## Persistent image validation

```text
image SHA-256:         b5d513e7ef47259321a53f2e07432424240d5e7bdcb1142611b784f374ef4792
compressed kernel:     2,947,841 bytes
safe payload limit:    0x2ffe00
kernel headroom:       197,375 bytes
SquashFS file offset:  0x300200
required 512-byte gap: present
TrendChip header:      valid
installed xPON kmod:   fa4608cd35677f627f4dbde979091a742c89d8d01b0610e6c13d695178526fd4
```

After the final sysupgrade, both diagnostic modules autoloaded and PPPoE
returned without interface, QDMA, xPON, I2C or GPIO errors.  Repeated HTTP
connectivity checks completed 10/10.  ICMP to `1.1.1.1` completed 9/10 in the
longer sample (the PPPoE peer itself filters ICMP); the session stayed up and
the Ethernet/PPP interfaces recorded no TX/RX errors.  This is recorded rather
than treating ICMP variability as either a pass or a PON-induced regression.

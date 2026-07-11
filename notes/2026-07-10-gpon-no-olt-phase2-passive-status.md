# GPON no-OLT bring-up — phase 2 passive EN7570 status

Date: 2026-07-10

## Result

The read-only EN7570 diagnostic now reports the four status bits used by the
OEM `show_BoB_information()` function.  A live test on the XR500v, with no
EN7570 reset or optical initialisation, returned:

```text
silicon_id:           0x03 (EN7570)
variant:              0x01
los_status:           0
los_debug_raw:        03 02 00 10
rogue_onu_status:     0
tx_sd_status:         0
rogue_tx_raw:         34 02
tx_fault_status:      1
safe_protect_raw:     ff 8f
status_interpretation: raw/uninitialized optical block
register_data_writes: 0
reset_or_init:        no
laser_or_apd_control: no
adc_or_ddmi_control:  no
```

Three consecutive reads left the decoded bits unchanged.  The low byte of
`LOS_DBG_RG` advanced from `0x03` to `0x05` to `0x07`, while its OEM LOS bit
remained zero.  PPPoE uptime did not reset and an external IPv4 ping completed
4/4 after replacing the module in RAM.

## Register provenance

The selected bits come directly from the OEM information-only routine:

| Meaning | EN7570 register | OEM extraction |
|---|---:|---:|
| LOS | `LOS_DBG_RG` (`0x0130`) | byte 3 bit 0 / register bit 24 |
| Rogue ONU | `ROGUE_ONU_DET_CTRL` (`0x0168`) | byte 1 bit 2 / register bit 10 |
| Tx signal detect | `ROGUE_ONU_DET_CTRL` (`0x0168`) | byte 1 bit 3 / register bit 11 |
| Tx fault | `SAFE_PROTECT` (`0x0100`) | byte 1 bit 7 / register bit 15 |

The OEM clear operations are separate writes: rogue-ONU clear writes byte 1
bit 0 at `0x0169`, and safe-circuit reset writes byte 1 bit 6 at `0x0101`.
The diagnostic has neither operation.  It also deliberately excludes
`ADC_PROBE_STATUS`: meaningful temperature, RSSI, optical-power and current
measurements require writes to ADC mux and latch controls.

## Interpretation limit

These are raw silicon states, not a valid optical-link report.  In particular,
`los_status=0` must not be translated to “fibre present” or “optical signal
received”: the EN7570 has not received its calibration matrix or LOS setup,
and the xPON PHY has no RX sync.  `tx_fault_status=1` is consistent with the
separately observed xPON-PHY pending Tx-fault state, but this phase intentionally
does not clear either latch.

This is the useful no-OLT boundary: identity and passive status are observable.
DDMI requires controlled analogue initialisation, while ranging, PLOAM O5,
GEM traffic and OMCI require an OLT.

## Persistent image and post-boot validation

The phase-2 image was built, validated and installed with `sysupgrade`:

```text
image SHA-256:         54dce0bc26c703ecb617d4649f693c637b85d0f9c50d5ddcbff85dff90d580bc
compressed kernel:     2,947,688 bytes
safe payload limit:    0x2ffe00
kernel headroom:       197,528 bytes
SquashFS file offset:  0x300200
required 512-byte gap: present
TrendChip header:      valid
installed kmod SHA-256:f135dbc192150d8f7fa26f10f395a213eb948da3e1eee57fdb2de934749ebdb1
```

After a clean boot, the EN7570 module autoloaded from `/lib/modules/6.12.80`,
reported the same decoded states and generated no I2C errors.  PPPoE came up
and five external IPv4 pings completed with zero loss.

A separately loaded read-only xPON-PHY probe confirmed that the sysupgrade and
new EN7570 reads had not enabled the transmitter or altered the phase-0 state:

```text
mode:              GPON
phy_fsm_state:     0x3
rx_sync:           no
tx_enable:         no
pll_enable:        no
interrupt_enable:  0x00
irq_tx_fault:      pending
writes_performed:  0
```

The temporary xPON probe was unloaded and deleted after the comparison.

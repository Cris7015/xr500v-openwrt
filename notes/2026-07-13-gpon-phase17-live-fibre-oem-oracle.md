# GPON bring-up — phase 17 live-fibre failure and OEM oracle

Date: 2026-07-13

## Result

The phase-16 `reset -> isolated LOS` sequence was repeated once with downstream
fibre connected and every transmit barrier asserted.  The EN7570 did not
distinguish connected fibre from disconnected fibre: its raw LOS bit remained
set in every sample and the xPON PHY never reached receive sync.

Booting the same unit into its OEM image with the same fibre immediately
produced a working optical link, GPON traffic and OMCI activity.  This closes
the external-hardware question: the fibre, OLT, BOSA and EN7570 receive path are
functional.  OpenWrt is missing an EN7570/OEM PHY initialisation prerequisite.

No GPON serial number, password, key or other operator credential was read or
recorded.

## Live OpenWrt execution

The exact audited phase-16 artifacts were reused:

```text
temporary image SHA-256: 6e686de1e73847b06e7f5345087ba225087b8c07b350c798b87fcf3361cf0f22
module SHA-256:          3eb48c5a75011b969c30bd771d2d7fd95fccdf2db4278793e1ae0938cd52085d
temporary DTB SHA-256:   08f6dd9d057408220a3313edf2c114bb57963bb911762ac135446c680aa8b1ca
```

The pre-execution EN7570 state was the exact clean phase-12 baseline even with
fibre present.  The one-shot completed without error:

```text
I2C writes:              6 (one reset plus five LOS)
reset / snapshot / TX:   0 / 0 / 0
LOS / snapshot / TX:     0 / 0 / 0
module pinned:           yes
GPIO16 TX_DISABLE:       output-high/asserted
xPON TXEN:               clear
APD / bias / modulation: disabled / zero / zero
```

The final programmed bytes again matched phase 16:

```text
SVADC_PD  @0x0024: 00 00 41 04
LOS_CTRL1 @0x011c: 06 1f 1c 10
LOS_CTRL2 @0x0120: 05 1f 22 00
LOS timeout @0x012c: 3e 00 00 00
LOS status:              1
```

Twenty spaced samples with fibre connected all reported LOS.  Fibre was then
removed without rebooting and sampled 30 times at intervals plus 100 times
rapidly.  It was reconnected on the same boot and sampled another 100 times.
The two rapid populations were indistinguishable:

| `LOS_DBG_RG` byte | Fibre disconnected | Fibre connected |
|---|---:|---:|
| byte 0 mean / standard deviation | 28.720 / 18.919 | 31.820 / 17.938 |
| byte 1 | always `0x00` | always `0x00` |
| byte 2 mean / standard deviation | 60.320 / 36.730 | 62.280 / 35.827 |
| byte 3 | always `0x89` | always `0x89` |
| decoded LOS | 100/100 | 100/100 |

The variable bytes were autonomous scan/noise state, not received-light
evidence.  No TX, APD or safety anomaly occurred during any sample.

## OEM stock oracle

Stock was selected with `bflag set 0`.  A useful UART shortcut was discovered:
`jump 81fb8a80` restarts Bootbase and boots the selected slot without removing
power.  This is not a cold reset of the external EN7570, but stock subsequently
executes its complete EN7570 reset and initialisation sequence.

With fibre connected, stock reported:

```text
PHY Status:              plug
PHY_RD:                  0x6
RX_SYNC:                 0xa
PHYSTA1:                 0x001b1919
XPON_STA:                0x00000000
PHYRX_STATUS:            0x6f2b844a
/proc/tc3162/los_status: 1 (the OEM proc ABI defines 1 as not-LOS)
/proc/pon_phy/RSSICurrent: 0x900 (the proc node prints hexadecimal)
```

The OEM `pon`/`omci` interfaces and receive counters were active, and PPPoE had
established service.  This proves considerably more than a raw light reading:
the device was synchronized and exchanging GPON/OMCI traffic with the OLT.

The read-only OEM debug command `mt7570_register_dump c1` captured all 193
four-byte EN7570 groups from `0x000` through `0x300`; `flash_dump` captured the
40-word calibration matrix.  The local capture is
`/home/cristuu/tools/xr500v/en7570_stock_oem_logs.txt`, 8244 bytes, with SHA-256:

```text
73ff98ad170b681278ac833deb73821b262e09ce5f5fc94060a0339c4a0aef64
```

The calibration matrix independently confirms GPON magic `0x07050700`, LOS
thresholds `0x1c/0x10`, initial bias/modulation codes `0x237/0x4eb`, and no
per-unit TIA-gain override (`flash_TIAGAIN = 0xffffffff`).

## Register comparison

OEM printk renders each four-byte I2C value as a little-endian integer.  The
table below presents the actual I2C byte order:

| Register | OpenWrt after isolated LOS | OEM stock with live link | Finding |
|---|---|---|---|
| `LA_PWD 0x014` | `00 24 00 00` | `00 24 05 00` | stable receiver setting absent from OpenWrt |
| `SVADC_PD 0x024` | `00 00 41 04` | `00 00 41 04` | exact match |
| `LOS_CTRL1 0x11c` | `06 1f 1c 10` | `06 1f 1c 10` | exact match |
| `LOS_CTRL2 0x120` | `05 1f 22 00` | `05 1f 22 00` | exact match |
| timeout `0x12c` | `3e 00 00 00` | `00 00 00 00` | OpenWrt stayed timed out; stock link cleared it |
| LOS debug `0x130` | `var 00 var 89` | `2e 18 65 88` | bit 0 is LOS in OpenWrt and clear in stock |
| ERC filter `0x16c` | `3f 2f 0f 00` | `ff a7 58 00` | OEM TX feedback/filter setup, not the first RX hypothesis |

The decisive correction is that the five LOS writes themselves were already
right.  OEM source order is reset, TIA/ERC, MPD calibration, ADC calibration,
RSSI calibration, mode setup, `mt7570_RSSI_gain_init()`, then LOS setup.  ADC
and RSSI calibration restore the visible mux/control bytes and retain their
results in software variables.  The sole stable receiver-oriented delta
immediately preceding LOS is the RSSI-gain RMW:

```text
LA_PWD byte 2 = (old & 0xf8) | 0x05
```

This is the next minimal hypothesis.  If it does not make LOS respond to light,
the following boundary is the transient ADC/RSSI calibration sequence rather
than another retry of reset or LOS alone.

# GPON bring-up — phase 23 stock APD, O5 and service oracle

Date: 2026-07-13

## Result

A physical cold boot of the stock slot with the authorised Movistar fibre
connected produced the first time-correlated oracle for the XR500v APD path.
It also closed a larger uncertainty: the stock system reached GPON activation
state O5, completed OMCI activity and established its PPPoE service over
`nas1_0`.

The EN7570 reads were directed and read-only.  The first reachable APD sample
was `b1 09 20 00`; a subsequent 90-sample series was entirely
`b2 09 20 00`.  All 91 corresponding reads of the undocumented OVP latch at
`0x0164` were zero.  The two codes agree exactly with the cached IC
temperature and this unit's factory APD calibration.

This is a strong stock software oracle, not a measurement of the physical APD
rail or proof of its electrical limit.  No OpenWrt APD write was made in this
phase.

## Cold-boot and access procedure

The external EN7570 retains state across a warm Linux reboot, so this run did
not use `boot`, `go`, `decomp`, `reboot` or the bootloader shortcut
`jump 81fb8a80`.

1. Intercept the bootloader and select stock with `bflag set 0`.
2. Remove physical power for at least 30 seconds.
3. Power on without intercepting autoboot.
4. Keep the single UART reader in `picocom`; do not race it with a second
   `/dev/ttyUSB0` reader.
5. After stock starts, expose the pre-existing lab shell at
   `192.168.68.99:2323` and use it only for passive status and directed I2C
   reads.

The UART boot log explicitly reported:

```text
phy_power_ctl ctrlFlag(1, 0-limit 1-open). set pon tx power(0, 0-ON 1-OFF).
```

This stock run therefore had its normal transmitter enabled.  That is
acceptable only on the authorised fibre with the unit's factory identity; it
is not a procedure for an OpenWrt RX-only experiment.

## Read-only trace method

The stock `sifm` `xr` operation calls the SIF read path.  It was used
sequentially, never in parallel with itself:

```sh
/usr/bin/sifm xr 0 0xc7 0x70 2 0x30 4
/usr/bin/sifm xr 0 0xc7 0x70 2 0x164 4
```

The first command reads four APD bytes.  The second reads four OVP bytes.  The
tool prints through `printk`, so the output was captured both from UART and by
a temporary `/proc/kmsg` reader.  The reader and sampling shell exited at the
end of the run.  There was no `sifm xw`, `/proc/i2c` write, APD control proc,
ADC/DDMI proc read, latch clear, module operation or NAND access.

The periodic run performed one APD/OVP pair approximately every 1.09 seconds
for 90 samples.  Cached status was recorded every five samples.  `tcapi` reads
the already cached DDMI value; it does not initiate another ADC conversion.

## Evidence

### Earliest reachable state

At stock uptime `185.86` seconds:

```text
PHY Status: plug
RX_SYNC:    0xa
los_status: 1
```

The OEM proc ABI defines `los_status=1` as **not LOS** at
`xpon_phy/src/phy.c:2567-2583`.

The first directed pair at uptime `261.94` seconds returned:

```text
0x0030: b1 09 20 00
0x0164: 00 00 00 00
```

At uptime `327.85` seconds, the independent stock status was:

```text
Info_PonPhy Temperature: 11231
G_ACTIVATION:            0x5
PHY Status:              plug, RX_SYNC=0xa
los_status:              1
```

`G_ACTIVATION_ST.act_st` is the low three-bit activation state.  Value `5` is
the operational O5 state used throughout the OEM GPON driver.

### 90-sample series

The series covered stock uptime `416.85` through `514.02` seconds:

| Check | Result |
|---|---:|
| APD reads | 90/90 `b2 09 20 00` |
| OVP reads | 90/90 `00 00 00 00` |
| SIF read verifications | 180/180 successful |
| matched I2C/atomic/timeout failures | 0 |
| activation snapshots | 18/18 `G_ACTIVATION=0x5` |
| PHY snapshots | 18/18 `plug`, `RX_SYNC=0xa` |
| no-LOS ABI snapshots | 18/18 value `1` |

The cached temperature was `11456` through sample 55 and `11680` from sample
60 onward.  O5 and no-LOS remained stable throughout.

The user-provided UART capture is local-only:

```text
/home/cristuu/tools/xr500v/apd_hex_dump_mt7570.txt
SHA-256: 18eecc58345a5f2ad19821dcd7b22e2ca4182097eb88b5aa54d9b94f83e2e7be
```

It contains 182 successful read messages, one `b1` APD record, 90 `b2`
records, 91 zero OVP records and no matching read failure.  It is deliberately
not copied into this repository.

### Stock end-to-end service

A separate UART excerpt captured the later stock service startup:

- board TX control opened and PON TX was enabled;
- the OMCI process exchanged MIB reset and MIB set messages;
- the OLT mode was normal;
- PPPoE bound to the PON-backed `nas1_0`, authenticated and created `ppp1`.

That proves more than physical sync: on the stock stack this fibre, ONU
identity, OMCI path and WAN datapath are functional together.  The raw file is
also local-only because it contains the ONU serial, ISP-facing addresses and
other service identifiers:

```text
/home/cristuu/tools/xr500v/gpon_stockoem_booting.txt
SHA-256: 363c5d144df38e21a65e36f8b4bed2aa7677c588fae0306667de8411655569b3
```

None of those private values are reproduced here.

At uptime `992.23` seconds a final passive check still showed
`G_ACTIVATION=0x5`, `plug`, `RX_SYNC=0xa`, no-LOS ABI value `1`, and a present
`ppp1` interface with nonzero RX and TX byte counters.  O5 therefore remained
stable well beyond the 90-sample APD series.

## Exact APD temperature decode

The factory words established in phase 22 are:

```text
hot slope:        0.08 V/degC
25-degC target:  47.42 V
legacy zero:     30.00 V
legacy step:      0.103 V/code
BOSA offset:       8 degC below IC temperature
update period:    20 seconds
```

Stock publishes IC temperature as signed SFF-8472 Q8.8.  For `11231`:

```text
IC   = 11231 / 256                         = 43.8711 degC
BOSA = 43.8711 - 8                         = 35.8711 degC
VAPD = 47.42 + 0.08 * (35.8711 - 25)       = 48.2897 V
code = trunc((48.2897 - 30.00) / 0.103)    = 177 = 0xb1
```

For `11456`, the same calculation gives IC `44.75 degC`, BOSA `36.75 degC`,
`48.36 V` and code `0xb2`.  For `11680`, it still truncates to `0xb2`.

The relevant code ranges are:

| Code | IC temperature | BOSA temperature |
|---:|---:|---:|
| `0xb1` | `43.1375..44.425 degC` | `35.1375..36.425 degC` |
| `0xb2` | `44.425..45.7125 degC` | `36.425..37.7125 degC` |
| `0xb3` | `45.7125..47.000 degC` | `37.7125..39.000 degC` |

The earlier phase-22 `b3` stock dump was therefore a warmer observation, not
a different configuration.  It also strengthens the phase-22 finding that a
proposal which swaps the factory hot/cold slopes or uses the wrong legacy step
cannot reproduce this board.

OEM references are:

- temperature acquisition and BOSA offset: `mt7570.c:2192-2226`;
- Q8.8 publication: `mt7570.c:2288-2303`;
- APD calculation and legacy mapping: `mt7570.c:1776-1938`;
- 1 Hz worker: `phy_init.c:174-184`;
- temperature/APD schedules: `mt7570.c:3131-3150`.

The cached temperature may advance before the next DAC update: temperature is
refreshed at `cnt % 10 == 4`, while APD is rewritten at `cnt % 20 == 19`.

## What this proves and what it does not

Proved:

1. Stock enables APD control and soft-start as `09 20` and operates with
   temperature-derived DAC codes `b1`, `b2` and previously `b3`.
2. The live codes match the exact per-unit factory matrix.
3. OVP remained zero over 91 observed pairs.
4. The optical path reaches RX sync and GPON O5.
5. Stock OMCI and the PON-backed PPPoE datapath provide working service.

Not proved:

1. The APD rail's physical voltage or OVP threshold was not measured.
2. A zero latch does not establish safe behaviour for an arbitrary DAC code.
3. Stock O5 does not imply that OpenWrt has a PLOAM, OMCI or network datapath.
4. The immediate kernel-side transition
   `00 08 00 00 -> 00 08 20 00 -> 00 09 20 00 -> a2 09 20 00` occurs before
   userspace access and was not time-resolved.  Capturing it would require an
   external I2C analyser or an instrumented stock kernel.

## Next boundary

The stock transition oracle is now strong enough to design, compile and audit
a separate APD-only OpenWrt observer.  It must not be added to the existing
reset/RSSI/LOS module, whose isolation and live hashes must remain intact.

Before any live OpenWrt APD attempt, the new observer must remain absent from
the shipping image, autoload and normal DT; require an exact cold baseline,
factory-block hash and independent opt-ins; assert physical TX-disable and all
digital TX gates; perform only the exact OEM-width APD sequence; read APD and
OVP after every transfer; issue no retry or rollback; retain GPIO16 high; and
require physical power removal afterward.  The first such run must be done
with fibre disconnected and the operator ready to remove power.

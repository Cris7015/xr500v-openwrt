# EN751221 GPON MAC first live read: system-bus stall

Date: 2026-07-15

## Result

The first direct OpenWrt read of the EN751221 GPON MAC window did **not**
return.  It stalled the running router strongly enough that both SSH and ICMP
stopped responding.  Recovery requires a physical power removal of at least
35 seconds.  Fibre was disconnected and the xPON diagnostic still showed
`TXEN=0`, TX_DISABLE asserted and zero diagnostic writes before the attempt.

This is useful negative evidence: releasing the reset array currently owned by
`econet-eth` is not, by itself, sufficient proof that `0x1fb64000` is safe to
read.  No second direct GPON MAC read may be attempted until the SCU WAN mux,
both reset banks and any hidden bus/clock prerequisite are characterized.

## Preflight state

The live target was OpenWrt Linux `6.12.80`, boot ID
`33cecd0a-f8ab-414d-9fbd-1e49f78bcecb`.  Before the experiment:

```text
fibre:                  disconnected
xPON PHY TX enable:     no
TX_DISABLE GPIO16:      asserted
xPON diagnostic writes: 0
GPON MAC /proc/iomem:   unclaimed
```

The experimental module was not autoloaded.  Its local and remote SHA-256
matched:

```text
3dda586c611f46d4b4d997603b798cacc31cf52ab47065c7aef0ec1387dc0b2d
```

The module first proved its manual arm gate.  Loading without the parameter
failed, registered neither a module nor debugfs, and logged:

```text
disarmed_rc=255
module_loaded=no
debugfs=absent
xr500v-gpon-mac-passive: refused: allow_snapshot=1 is required
```

## Armed attempt

The armed command was:

```text
insmod /tmp/xr500v-gpon-mac-passive.ko allow_snapshot=1
```

The SSH command returned no buffered output and timed out after 30 seconds.
Three ICMP probes then received no replies and a new SSH connection could not
be established.  Because output never returned, it is not possible to identify
which of the six ordered reads stalled from live output alone.  The first
allowlisted access was `G_ONU_ID` at physical `0x1fb64000`, so it is the leading
hypothesis, not a proven exact program counter.

The source and linked-symbol audit proved the attempted module contained:

- one `ioread32()` call site over a frozen six-register allowlist;
- no MMIO write primitive;
- no IRQ, poller, reset, clock, GPIO or I2C operation;
- no FIFO data read (`PLOAMd_RDATA`) and no upstream FIFO access;
- no laser, EN7570 or APD path.

Therefore the loss of reachability was caused by a read-side bus-access
precondition, not by an optical transmission or a GPON register write.

## OEM and modern-tree interpretation

The OEM map remains unambiguous: it maps `0x1fb60000`, reserves its first
`0x4000` bytes and places `G_ONU_ID` at physical `0x1fb64000`.  OEM code selects
GPON through `SCU_WAN_CONF[2:0]=0` and toggles the GPON MAC reset at
`SCU_RESET_CTRL1[31]`.  Current `econet-eth` asserts/deasserts an array that
contains the xPON MAC and PHY resets, but it does not explicitly select the
GPON WAN mux.

The cross-check also exposed two integration hazards:

1. The merbanan draft DT gives its driver a resource starting at `0x1fb60000`,
   while that draft's GPON offsets start at zero.  It therefore does not model
   the OEM `+0x4000` register displacement and remains compile-tested code.
2. The newer Matheus driver correctly accepts either the encompassing
   `0x1fb60000` region or the direct `0x1fb64000` window.  However, its current
   `gpon_prepare_hardware()` debug line reads four GPON MAC registers before it
   selects GPON in the SCU WAN mux.  That ordering is unsafe to copy to this
   EN751221 after the live stall.

## Replacement probe

The direct-read module was removed from the working tree rather than committed
as a loadable landmine.  It was replaced with
`package/kernel/xr500v-gpon-mac-preflight`, which reads only these already-live
SCU syscon registers through `regmap_read()`:

```text
0x070  SCU_WAN_CONF
0x830  SCU_RESET_CTRL2
0x834  SCU_RESET_CTRL1
```

The replacement explicitly performs zero GPON MAC accesses.  Its source audit
forbids direct MMIO, writes, IRQs, reset APIs, clocks, GPIO, I2C and workers.  It
passes strict checkpatch and cross-compiles as an ELF32 big-endian MIPS32r2
module for the target's exact `6.12.80` vermagic.  Artifact:

```text
path    /tmp/xr500v-gpon-mac-preflight-build/xr500v-gpon-mac-preflight.ko
sha256  9971117600511b8272d013abfbb14149804f92e6562f83df9d2781f62b8149a0
```

## Next boundary

1. Cold-boot OpenWrt without fibre after at least 35 seconds with power removed.
2. Load only the SCU preflight and capture its three values.
3. Do not reload SHA-256 `3dda586c...` or perform any direct GPON MAC read.
4. Compare the cold OpenWrt SCU snapshot against stock before designing a
   strictly ordered `WAN mux -> reset status -> GPON read` experiment.
5. Keep TX_DISABLE asserted; none of these bus-precondition tests needs fibre.

## Cold-recovery SCU result

After the physical cut, OpenWrt returned with boot ID
`19791d27-098b-4a62-bb07-0911cd397abd`; UBI mounted normally and the stale tmpfs
module was absent.  The exact replacement preflight artifact above loaded,
captured its snapshot, unloaded and was deleted without changing the boot ID:

```text
WAN_CONF[0x070]:    0x00000003
RESET_CTRL2[0x830]: 0x00000000
RESET_CTRL1[0x834]: 0x00000000

wan_mode:       3 (not GPON)
xpon_phy_reset: released
xpon_mac_reset: released
fe_reset:       released
qdma1_reset:    released
qdma2_reset:    released
```

The raw captured status has SHA-256
`ef21152c4fb2d850f3f30a194d91b08e2be8f5b2e6e3c4f8bfbcfa9452095759`.
It asserted zero GPON MAC reads, zero writes and no IRQ/poller/reset/clock/GPIO/
I2C/optical path.

This isolates the first concrete prerequisite failure: the GPON and xPON
resets are already released, but the WAN mux is mode `3`.  The exact EN751221
OEM QDMA source identifies mode `3` as `MAC_TYPE_ATM`; mode `2` is PTM, mode
`1` is EPON and mode `0` is GPON.  Reading the GPON MAC while the shared WAN
complex was selected for ATM is therefore the leading explanation for the bus
stall.

## Reversible WAN-mux cycle

A second standalone package, `xr500v-gpon-wan-mux-cycle`, was then built to
test only the missing selector prerequisite.  It has a manual arm parameter,
requires the exact XR500v/EN751221 compatibles, revalidates frozen mode `3` and
all five released resets inside a preemption- and local-IRQ-disabled section,
and performs exactly this sequence:

```text
SCU WAN mode 3 -> readback 0 -> restore mode 3 -> exact full-register readback
```

The syscon is a non-sleeping MMIO regmap.  No delay, log, allocation or GPON
MAC access occurs between the two mux writes.  A failed first update or GPON
readback still takes the common restore path.  The source audit and strict
checkpatch passed; the exact target artifact was:

```text
sha256  7734a1df02669fa1cad6d4aac3002fc6d540b9ef70b343eedfbb67e8f17b779b
vermagic 6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

The unarmed load first refused and left no module/debugfs state.  The armed
cycle then succeeded:

```text
wan_before: 0x00000003 (mode 3)
wan_gpon:   0x00000000 (mode 0)
wan_after:  0x00000003 (mode 3)
measured_cycle_ns: 8925
original_mode_restored: yes
non_mode_bits_preserved: yes
```

The boot ID remained unchanged, SSH stayed live, IRQ22 remained at zero and
IRQ21 showed no storm.  The read-only xPON guard still reported TX disabled,
TX_DISABLE asserted and zero writes.  The module was unloaded and deleted;
the captured status has SHA-256
`6a307cddc3f7230351456a16637bed4f95e30a455c219b5cd33fa1e7b2721d45`.

This proves that the SCU mux accepts and cleanly restores GPON mode.  It does
not by itself prove that a GPON MAC register read is safe; that must remain a
separate one-read experiment with physical recovery available.

## Two-VPE hardening

The first mux-only artifact disabled preemption and local IRQs, which protects
only the executing VPE on this SMP target.  Before touching the GPON MAC, both
the mux-cycle and single-read probes were changed to use `stop_machine()`.
The callback therefore runs once while both online VPEs are quiesced and hard
IRQs are disabled.  This closes the second-CPU QDMA/IRQ race, but it cannot
provide rollback if a synchronous MMIO read itself never returns.

The hardened mux-only artifact had SHA-256
`671ebdd10f31a9309f5440b9893901db3cef6a46ba6d10afa2c2013084f67475`.
It passed the same source audit, strict checkpatch and exact-kernel compile,
then completed this live sequence:

```text
wan_before: 0x00000003 (mode 3)
wan_gpon:   0x00000000 (mode 0)
wan_after:  0x00000003 (mode 3)
measured_cycle_ns: 8505
```

The boot ID and optical guard remained unchanged, IRQ22 remained zero and
IRQ21 showed no storm.  The raw capture has SHA-256
`bc47d86b2d6ad1041779e7e3c16699b62e97ce10739e2c002e61b063653ef1f2`.

## Mux-guarded `G_ONU_ID` result

The next package, `xr500v-gpon-mac-single-read`, claims and maps exactly four
bytes at physical `0x1fb64000`.  Inside the same global stop-machine window it
rechecks mode `3` and all released resets, selects mode `0`, verifies that
readback, executes exactly one OEM-style `ioread32()`, restores the original
WAN word and requires an exact final readback.  It has no GPON write, IRQ,
poller, FIFO-data, reset, clock, GPIO, I2C or optical path.

The exact artifact had SHA-256
`9d16c999a9f55892e490bc892900d25b54b70a329471a5b7c173526aa1b89332`.
Its unarmed load first refused cleanly.  The single armed attempt then
returned successfully:

```text
wan_before: 0x00000003 (mode 3)
wan_gpon:   0x00000000 (mode 0)
G_ONU_ID:   0x000000ff
onu_id_valid: no
onu_id:     0xff
wan_after:  0x00000003 (mode 3)
measured_cycle_ns: 9030
```

The value is a plausible uninitialized/unassigned state; it is not evidence
of GPON registration.  The important result is that the corrected ordering
makes the GPON MAC window readable on EN751221.  The boot ID remained
`19791d27-098b-4a62-bb07-0911cd397abd`, IRQ22 remained zero, TX remained
disabled and no GPON write occurred.  The module was unloaded and its target
copy removed.  The raw live capture has SHA-256
`46cf5e06714dd4eaad1bd4f5ac68ab629d68e9a79b3a2d363e55ad04f15bc243`.

This converts the earlier broad negative result into a precise sequencing
rule: on this OpenWrt port, GPON MAC access must be ordered after selecting
`SCU_WAN_CONF[2:0] = 0`; released xPON resets alone are not sufficient.

## Six-register passive snapshot

After the one-register boundary succeeded, a final diagnostic package,
`xr500v-gpon-mac-snapshot`, extended the same guarded sequence to the six
ordinary state/configuration registers that the OEM register-dump path reads:

```text
+0x00  G_ONU_ID
+0x04  G_GBL_CFG
+0x0c  G_INT_ENABLE
+0x50  G_PLOAMu_FIFO_STS
+0x58  G_PLOAMd_FIFO_STS
+0xbc  G_ACTIVATION_ST
```

The mapping is exactly `0x1fb64000..0x1fb640bf`.  The allowlist deliberately
excludes W1C `G_INT_STATUS`, upstream `PLOAMu_WDATA`, destructive/pop-on-read
`PLOAMd_RDATA`, AES keys, serial/vendor identity and every indirect table.
The module does not register an IRQ or poller and still performs zero GPON
writes.  Mutation tests verify that its source auditor rejects an extra read,
FIFO-data token, write primitive, changed offset or missing global exclusion.

The exact target artifact had SHA-256
`b6c7ac31b4888a201851553e7a9d628ab0104e9af2da1a25f91a7e573fc1f75a`.
Its unarmed load refused and the single armed run completed successfully:

```text
WAN_CONF:                0x00000003 -> 0x00000000 -> 0x00000003
G_ONU_ID:                0x000000ff  (valid=no, ONU-ID=0xff)
G_GBL_CFG:               0x00000034  (US FEC off, block size 52)
G_INT_ENABLE:            0x00000000  (all GPON MAC IRQ sources masked)
G_PLOAMu_FIFO_STS:       0x00800080  (128 available/min, no underrun)
G_PLOAMd_FIFO_STS:       0x00000000  (empty, no overrun)
G_ACTIVATION_ST:         0x00000001  (O1)
measured_cycle_ns:       9555
```

The boot ID remained unchanged, IRQ22 stayed at zero, IRQ21 showed no storm,
the optical guard still reported TX disabled/TX_DISABLE asserted/zero writes,
and the module and target artifact were removed.  The raw live capture has
SHA-256
`b578e0187876f64767e60b207851581c119ff4bef96d8ad5299b9e32666100d9`.

This is the expected uninitialized GPON condition: no assigned ONU-ID, no
enabled MAC interrupts, empty downstream PLOAM FIFO and activation state O1.
It proves a stable passive data path to the MAC register block; it does not
prove downstream synchronization, PLOAM exchange, O5 or an OMCI/network data
path.

## Next implementation boundary

Do not advance by enabling every GPON interrupt or replaying the OEM init as a
single opaque sequence.  The next phase should be a separately reviewed core
that preserves the proven order and adds, one boundary at a time:

1. permanent SCU mux/reset ownership coordinated with the Ethernet driver;
2. downstream PHY synchronization and read-only activation observation;
3. GPON interrupt demultiplexing inside WAN-QDMA bit 16, not a standalone DTS
   IRQ;
4. minimal serial-number/PLOAM handling to move beyond O1;
5. only after O5, OMCI and the Ethernet/network data path.

Fibre is not needed for the completed passive snapshot.  It will be required
for downstream synchronization and any O1-to-O5 activation work.

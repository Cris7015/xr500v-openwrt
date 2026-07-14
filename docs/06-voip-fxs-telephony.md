## Summary

The Archer XR500v's two RJ11 telephone jacks are driven by a **Microsemi/Microchip Le9642** dual-channel SLIC (Subscriber Line Interface Circuit), a member of the VE886/VP886 "miSLIC" family. The OEM firmware speaks to it through the proprietary VoicePath API-II (VP-API-II) stack (`slic3.ko` + `pcm1.ko` + DSP blobs) on its 2.6.36/3.18 kernel — none of which is mainline, and none of which loads on the OpenWrt 6.12 target. The entire FXS/VoIP path on this wiki's OpenWrt port was therefore rebuilt from scratch: a ZSI control transport, the SLIC line bring-up (profiles, switcher, feed, ring, hook), a G.711 audio chain over the SoC PCM/TDM DMA engine, a full-duplex `/dev/xr500v-voice` character device, and a SIP integration (baresip + a small call-manager daemon) that makes a cordless DECT handset behave like a real telephone. Point-to-point SIP calls between a PC softphone and the handset work bidirectionally with clean audio, with working ring / answer / hang-up. Both FXS channels have been validated end-to-end one line at a time; the upstream EcoNet OpenWrt project explicitly excludes VoIP, so as far as is known this is the first working FXS port for this SoC family. See [02-hardware-chip-inventory.md](02-hardware-chip-inventory.md) for the device map and [03-boot-partitions-flashing.md](03-boot-partitions-flashing.md) for the flash/boot rules referenced below.

## Hardware topology

```
  RJ11 jack "phone2" (case label) ── Le9642 channel 1 (EC_1 = 0x01) ┐
  RJ11 jack "phone1" (case label) ── Le9642 channel 2 (EC_2 = 0x02) ┤
                                                                     │
                          Le9642 SLIC  ── analog line drive / DC feed / ring gen
                              │  ▲
                       ZSI control     PCM/TDM audio (PCLK/FSYNC + 8 kHz frames)
                              │  │
            ZSI wrapper @ 0x1fbd1000 ──┐  ┌── PCM engine @ 0x1fbd0000
                                       │  │
                               EN751221 SoC (MIPS 24Kc BE)
```

Two important wiring notes:

- **The case labels are inverted relative to the silicon**, exactly like the Ethernet ports (see [04-ethernet-dsa.md](04-ethernet-dsa.md)). The jack silk-screened **"phone2"** is SLIC **channel 1 / EC_1**, and **"phone1"** is **channel 2 / EC_2**. All of the initial bring-up — and all of the VoIP development — happened on EC_1, the line behind the "phone2" jack.
- The SLIC sits near the UART pads, not the WiFi section. On the board it is a single 2-channel chip (marked `Le9642 ZZ 2113TTQ` = year 2021, week 13), not two single-channel devices.

The OEM `rcS` confirms the configuration: `insmod slic3.ko type=ZSI`, `insmod fxs3.ko slic1=le9642 slic2=le9642`, country code BRA (Brazil), `FXS_NUM=2 FXO_NUM=0`.

## The ZSI control transport

The key insight was that **the SLIC is controlled over ZSI (Zarlink Serial Interface), not plain SPI.** Early attempts to drive it as an SPI device (replaying `New_SPI_bytes_*_zarlink` from `spi.ko`) failed for nine debugging cycles — the chip stayed electrically mute (MISO idle-high `0xff`), which looked like a board-level power/clock problem and nearly sent the investigation to an oscilloscope. The OEM `rcS` line `type=ZSI` resolved it: ZSI **multiplexes the SLIC control channel over the PCM bus**, so the SLIC only answers when the PCM engine is clocking PCLK/FSYNC, and the control "MISO" pin is shared with the PCM block.

ZSI control goes through a wrapper at **`0x1fbd1000`** (+`id*0x2000` for the second instance). The register map (reverse-engineered from `spi.ko`'s `ZSI_bytes_read/write` and verified live by register-poking before any driver was written):

| Offset | Name | Meaning |
|--------|------|---------|
| `0x000` | `ZSI_CFG` | config; OEM-stock value (voip running) = `0x0f030313` |
| `0x004` | `ZSI_TXRX` | write a command/data byte |
| `0x008` | `ZSI_CTL` | bit0 = read-req, bit1 = tx-ack (W1C), bit2 = rx-ready (W1C) |
| `0x00c` | `ZSI_RX` | received byte (low 8 bits) |
| `0x010` | `ZSI_EN` | block enable = `0x19` |

ZSI mode is selected by a chain of SoC setup writes (from `slic3_main.c`, in `zsi_hw_init()`):

```
SYS 0x1fb00094 [3:0] = 0x5            # interface route = ZSI
SCU 0x1fa20104 = (old & ~0x7d00) | 0x2000   # IOMUX: GPIO_ZSI_ISI (vs 0x7400 SPI)
SCU 0x1fa20148 |= 0x1c                # PCM clock source = ZSI
SCU 0x1fa200d8 = 0x00a00301           # PCM clock output enable (OEM value)
# + PCM master config (see below), then:
ZSI 0x1fbd1000 = 0x0f030313           # wrapper config
ZSI 0x1fbd1010 |= 0x19                # wrapper enable
```

A SLIC reset toggles **`0x1fb00834` bit0 then bit17** (NOT bit25 — that is the SPI reset). The GPIO reset does *not* cut power to the SLIC; only a physical cold boot does (see the "cold boot" note below).

### Byte-level protocol and the mandatory 5 ms gap

Each ZSI byte transfer is: write to `ZSI_TXRX`, poll for `tx-ack` (bit1), W1C it; or for a read, set `read-req` (bit0), poll for `rx-ready` (bit2), read `ZSI_RX`, W1C it. The wrapper ack only means *the wrapper transmitted*; the SLIC then needs time to clock the byte over the 8 kHz PCM bus. **Without an inter-byte gap of ~5 ms the SLIC replies garbage (`0xf1`).** The driver uses `usleep_range(5000, 6000)` after every byte:

```c
static int zsi_write_byte(u8 v)
{
    writel(v, sd.zsi + ZSI_TXRX);
    if (zsi_poll_set(ZSI_CTL_TX_ACK))   return -ETIMEDOUT;
    writel(ZSI_CTL_TX_ACK, sd.zsi + ZSI_CTL);   /* W1C the ack */
    usleep_range(5000, 6000);                   /* SLIC clocks over 8 kHz PCM bus */
    return 0;
}
```

> **Note (busybox `sleep`).** When prototyping these gaps by poking registers from a shell over SSH, note that the device's busybox `sleep` **rejects fractional seconds** (`sleep 0.04` → `invalid number`), and the failure is silent (no `set -e`). A poke script that relies on `sleep 0.04` for the ZSI gap therefore waits *zero* time and corrupts every write — in one switcher test this hung the kernel and required a physical power-cycle. Use `usleep 40000`, host-side Node scripts with `setTimeout`, or do the register change inside the `.ko`. The gap lives inside the driver here precisely so it cannot be skipped.

A register read needs a NOOP sub-command after the register byte to align the framing: `zsi_read()` writes the register opcode, then `0x06` (= the VP886 `NOOP_WRT`), then reads `len` bytes. Without the `0x06` the first byte returned is a framing prefix (`0x03`) and the real data is shifted by one. The chip-detect sequence is therefore: `EC_REG_WRT(0x4a) → EC_1(0x01)` then `read(0x73)` → returns `08 75` = Le9642 confirmed.

Importantly, **the ZSI control read does NOT need the PCM DMA running** — the wrapper self-clocks once the PCM is configured as clock-master. Arming the DMA actually *prevented* the SLIC from answering during bring-up. So `zsi_hw_init()` configures the PCM as master (`INTFACE_CTRL=0xf5071306`, OEM timeslots, clock output) but does **not** start the rings for control transactions.

## SLIC line bring-up: profiles, switcher, feed, ring, hook

With ZSI working, the line is brought up by streaming the **Raw MPI sections** of the OEM profiles for this exact chip (extracted from `ZLR964124_Le9641_BB_profiles.c` in the OEM 2.6.36 source tree). Each MPI command is opcode + data bytes streamed straight over ZSI; the SLIC self-delimits the stream. Four profiles are loaded (`slic_load_profiles()`), in order:

| Profile | Opcode | Purpose |
|---------|--------|---------|
| `DEV_PROFILE_100V_BB_124_ZSI` | `0x46 0x44 0x5e 0xf6 0xe4 0xe6` | PCLK, PCM slot, device mode, buck-boost switcher config |
| `DC_FXS_miSLIC_BB_DEF` | `0xc6 0x92 0x27` | DC feed |
| `AC_FXS_RF14_600R_DEF_LE9641` | `0xa4` + 73 coeff bytes | AC impedance / hybrid (600R) |
| `RING_ZL880_BB90V_DEF` | `0xc0` + 11 bytes | ringing generator (~24.9 Hz, ~70 Vpk) |

> **AC profile framing (worth flagging).** The AC profile in the source carries a 6-byte `VpProfile` header (`a4 00 f4 4c 01 49 …`). Streamed *with* the header, all the filter coefficients are misaligned → garbage → a loud self-sustaining hybrid oscillation. The fix is to stream from offset 6 (`slic_write_mpi("ac", ac_mpi + 6, …)`). Profile header layout: `[0]TYPE_MSB [1]TYPE_LSB [2]INDEX [3]LENGTH [4]VERSION [5]MPI_LEN [6…]DATA`.

After profiles, the line is activated. Two hard-won rules:

1. **Never send the MPI `HWRESET` (0x04).** It drops the SLIC out of ZSI mode into an unrecoverable state (chip-id then reads `00 00`); only a physical power-cycle restores it. The OEM uses it from a different starting state; here the clean cold-boot state is the starting point and the profile is written *directly*.
2. **The switcher is the feed gate, not calibration.** The device profile writes `SWCTRL(0xe6)=0x60` = switchers OFF. The simple, reliable way to enable the feed is a *direct* `SWCTRL=0x6f` (HP/HP) write — the "robust" `CALCTRL 0x50` disconnect sequence from the VP-API actually breaks the `SWCTRL` write and leaves the switcher off. The full `VpCalLine` calibration state machine (`vp886_cal*.c`) was investigated and found **not required for feed/ring/audio** — it only refines supervisory offsets/gains and does not touch the PCM audio path.

The line-state register (`STATE_WRT 0x56` / `0x57 rd`) encodes `CODEC(0x20) | POL(0x10) | SS_state(bits0-3)`, with `SS`: SHUTDOWN=`0x0f`, ACTIVE=`0x03`, IDLE=`0x04`, BAL_RING=`0x07`, DISCONNECT=`0x00`. So:

- **Feed active:** `SWCTRL=0x6f` → `STATE=0x03`
- **Ringing:** `STATE=0x07` (readback `0xa7`, bit 0x80 = ring active) — *the physical bell rings*
- **Active + codec (audio):** `STATE=0x23`

**Off-hook detection** reads SIGREG. **SIGREG (`0x4d` no-update-latch / `0x4f` update-latch) is a device-level 4-byte register**, with each channel's HOOK bit (`0x01`) in **its own byte**: byte[0] = channel 1, byte[1] = channel 2. The hook index is therefore `slic_ec - 1`, not a fixed byte[0] — a subtle bug that only surfaced on the 2nd FXS channel (off-hook on ch2 reads `4d = 00 01 00 00`). With this, the router both rings the phone and knows when you answer.

Key VP886/Le9642 MPI opcodes (read = wrt+1):

| Reg | Opcode | Use |
|-----|--------|-----|
| RCNPCN_RD | `0x73` | chip-id (08/75) |
| NOOP | `0x06` | clears MPI buffer / aligns read framing |
| STATE | `0x56`/`0x57` | line state (see above) |
| SIGREG | `0x4d`/`0x4f` | hook/signaling, device-level 4 bytes |
| TXSLOT / RXSLOT | `0x40` / `0x42` | PCM bus timeslot (mic→bus / bus→ear) |
| OPFUNC | `0x60`/`0x61` | codec mode (u-law 0x40, A-law 0x00) + filter enables (0x3f) |
| OPCOND | `0x70`/`0x71` | CUT_TX 0x80 / CUT_RX 0x40 / TSA_LB 0x04 / HIGHPASS_DIS 0x20 |
| SWCTRL | `0xe6`/`0xe7` | switcher mode (Y bits0-1, Z bits2-3; on = 0x6f) |
| DCFEED | `0xc6`/`0xc7` | DC feed; byte1 ILA (loop current limit) bits0-4 |
| GR / GX | `0x82` / `0x80` | receive (earpiece) / transmit (mic) gain |
| EC selector | `0x4a` (`CSLAC_EC_REG_WRT`) | EC_1=0x01, EC_2=0x02, EC_GLOBAL=0x03 |

## PCM / TDM engine and timeslots

The audio moves over the SoC's on-die PCM/TDM controller at **`0x1fbd0000`** (`compatible = "econet,en751221-pcm"`, DTS node `pcm0`, interrupt `<12>`). The register map and DMA descriptor structures were reconstructed from the OEM `pcmdriver.h` headers plus disassembly of the unstripped `pcm1.ko`. SoC detection matches the EN751221 by `(read32(0x1fb00064) & 0xffff0000) == 0x00070000`.

Ground-truth register values (read from the OEM stock device with VoIP running):

| Offset | Name | OEM value |
|--------|------|-----------|
| `0x00` | `INTFACE_CTRL` | `0xf5071306` (master, bits30-28 enable, bit25 loopback clear) |
| `0x04`–`0x10` | TX timeslot cfg 0..3 | `0x10301020, 0x10501040, 0x10701060, 0x10901080` |
| `0x14`–`0x20` | RX timeslot cfg 0..3 | same as TX |
| `0x24`/`0x28` | ISR / IMR | — |
| `0x2c`/`0x30` | TX / RX polling demand | write 1 to kick |
| `0x34`/`0x38` | TX / RX desc ring base | physical, masked `0x1fffffff` |
| `0x3c` | ring size/offset | `0x9f` |
| `0x40` | `TX_RX_DMA_CTRL` | `0x0f000003` (chValid 0x0f, TX_EN+RX_EN) |

The DMA descriptor `status` word packs `OWN (bit31) | chValid (bits16-23) | sampleSize (bits0-9)`, followed by 8 `buf_addr[]` entries (one per channel). A frame is 80 samples = 160 bytes (5 ms at 8 kHz). The descriptor ring has 15 entries.

The PCM/DMA engine itself was validated independently before any SLIC audio: a back-to-back internal loopback (`INTFACE_CTRL` bit25 set) does a full TX→RX→IRQ→data round-trip. **The piece that originally blocked RX was the ring-size-offset register: the default `0xc0` had to be the OEM `0x9f`** — with the wrong value the RX descriptor's OWN bit never cleared and RX timed out.

> The PCM interrupt (hwirq 12) does not map cleanly — the INTC reports it as a "shadow interrupt" (`can't map hwirq 12 … rc=-22`), an EN751221 INTC quirk. Rather than chase the INTC/DT fix, the voice path is driven by a **polling kthread**, which works reliably.

### Timeslot ↔ DMA channel mapping

The SLIC's `TXSLOT`/`RXSLOT` is a PCM **bus** timeslot; the SoC RX DMA lands that audio on a **DMA channel** that is offset from the slot. The empirically determined mapping is **DMA channel = bus slot − 4**:

- channel 1 (FXS) uses bus slot **4** → DMA **ch0**
- a second simultaneous line would use bus slot **6** → DMA **ch2**

Two further subtleties that cost many iterations: the mic's DMA channel also **shifts with the descriptor `chNum`** (it was ch2 when `chNum=8`, ch0 when `chNum=4`), and the buffer stride uses an **8-channel layout** (`VOICE_CHANS=8`, index `d*8+ch`) even though the DMA channel-valid mask is `0x0f` (4 channels). Using a 4-channel stride read the wrong slab for descriptors past the first. The final voice config: `chValid = 0x0f`, `VOICE_RX_CH = 0` (8-channel stride). `chValid = 0xff` is wrong — with it the TX descriptors never advance.

## The audio chain: u-law, DC feed, OPFUNC ordering, tx_msb, FIFO latency

Getting *clean* audio out of this chip required four independent root-cause fixes, all baked into the driver:

**1. DC feed under-driven (the noise/hum root cause).** The miSLIC BB DC profile sets `DCFEED(0xc6)` byte1 ILA = `0x07` (≈ 25 mA), tuned for a 600R desk phone. The cordless DECT base draws more loop current; at the low ILA the mic modulation barely reached the supervisory/voice ADC, so capture stayed at idle "`00 XX`" unless the DAC stirred the line. A four-condition test (mic captured at full scale, ~15 dB SNR, no hum) pinned it to the feed: `slic_audio_setup()` raises ILA via the `feed_ila` parameter (`mA = 18 + field`, default `0x14` = 38 mA). This alone eliminated the noise and hum — no comfort tone, no AEC, none of the earlier workarounds were needed. A long chain of discarded hypotheses (ADC dither-cancel `INDCAL`, switcher `SWTIMING`/`SWPARAM`, frame-boundary glitches, GX boost, LMS AEC, calibration) is recorded in the project notes so they are not re-investigated; the feed current was the answer.

**2. OPFUNC must be written before TXSLOT/RXSLOT.** Matching the OEM `le9641.c` order, the codec (`OPFUNC`) is programmed first, then the timeslots. Out of order, the capture is degraded.

**3. The Le9642 drives only 8 bits per timeslot → use u-law (the choppy-audio root cause).** Even with `OPFUNC` set to "linear" (0x80), the chip drives only 8 bits per timeslot — the other byte of each 16-bit slot is `0x00`. So 16-bit linear gives 8-bit effective resolution, and quiet speech between words quantizes to zero → "choppy, cuts out on every word." The fix is to run the codec in **u-law** (`OPFUNC = 0x7f` = `CODEC_ULAW 0x40 | ALL_FILTERS 0x3f`) and (de)compand in the voice thread: RX decodes the 8-bit u-law code → 16-bit linear; TX encodes 16-bit linear → u-law. u-law's logarithmic companding preserves the quiet passages. The char device stays 16-bit linear so baresip is unchanged (and baresip already negotiates PCMU). The G.711 tables are standard:

```c
static inline s16 ulaw_decode(u8 byte) {
    int u = (~byte) & 0xff, sign = u & 0x80, exp = (u>>4)&7, man = u & 0x0f;
    int s = (((man << 3) + 0x84) << exp) - 0x84;
    return sign ? -s : s;
}
```

**4. Asymmetric byte position (`tx_msb`).** The SLIC uses different clock-slot offsets for TX-capture (CLKSLOTS TCS) versus RX-playback (RCS), so the 8-bit code lands in a *different byte* for capture versus playback. RX always reads the low byte (`src[i] & 0xff`); playback selects the byte via the `tx_msb` parameter (default 1 = MSB):

```c
f[i] = tx_msb ? (s16)((u16)code << 8) : code;   /* place u-law code in MSB or LSB */
```

**FIFO latency.** The capture/playback path uses two lock-free SPSC kfifos drained/filled by `read()`/`write()` and serviced by the kthread. The kfifo size (`voice_fifo_sz`, default **4096** bytes ≈ 0.26 s) is the dominant audio-latency knob; before the clean u-law capture it had to be 12288–16384 to mask the choppy capture, but with u-law the knee drops to 4096 (2048 chops). All of these are live module parameters (`feed_ila`, `icr4_mode`, `tx_msb`, `voice_gain`, `voice_rx_ch`, `voice_fifo_sz`), tunable without a rebuild.

The resulting validated defaults: `econet_slic: feed_ila=0x14, icr4_mode=2`; `pcm_en751221: tx_msb=1, voice_gain=1, voice_rx_ch=0, voice_fifo_sz=4096`. With these, a real SIP call is clean in both directions.

### Mic-capture analog noise floor

The mic-capture noise floor on this design has been characterized and is **isolated to the analog / ADC front-end**, not the driver or the streaming path. Measured in true silence (no speaker present), the captured idle floor is a uniform broadband level (~RMS 3600, ≈ −19 dBFS) spread evenly across the frame, with no elevation at frame boundaries. The following candidate causes were each tested and ruled out: TX interference during full-duplex capture, byte-order, gain/clipping, kthread streaming glitches/ordering, ADC dither-cancel (`INDCAL`), and switcher `SWTIMING`/`SWPARAM`. With the `feed_ila` fix above the voice-to-floor SNR is roughly 14–15 dB, so **calls are clean in practice** — the floor is a known analog limitation, not a defect in the audio path. The one remaining lead for lowering it further is the OEM `VpCalLine` calibration state machine (`vp886_cal*.c`), which the OEM runs at init and which is not yet ported; it stabilizes the analog front-end (it does not apply offset to the PCM audio itself).

## The character device and SIP call flow

`/dev/xr500v-voice` is a misc device (`econet-slic.c`) exposing full-duplex 16-bit-linear 8 kHz PCM: **read = mic, write = earpiece.** Open lines the SLIC up (`slic_audio_setup`) and starts the continuous DMA + kthread; release stops streaming. Because baresip's audio source and audio player run as two **threads** sharing one fd, the device uses `stream_open()` so the VFS does not take the per-file `f_pos` lock — otherwise a writer blocked in `voice_write()` holds `f_pos_lock` and starves the reader (capture throttles to ~3%, TX→0).

The SIP side is **baresip 3.16.0**, cross-compiled for the `econet` (mips_24kc) target, with `g711` + `l16` codecs and a custom audio module **`xr500v`** (an `ausrc`+`auplay` pair that opens `/dev/xr500v-voice`, sharing a ref-counted fd, native big-endian = the device format, no byte-swap). A point-to-point call (no registrar) works against a PC softphone using a "local account" (`sip:xr500v@<ip>`).

The "real telephone" behavior is the **`xr500v-callmgr`** daemon — a small musl C binary (the device has only busybox, so a script was not viable). It ties the SLIC hook to baresip's `ctrl_tcp` (127.0.0.1:4444, netstring-framed JSON `<len>:<json>,`):

| Event / action | Result |
|----------------|--------|
| baresip `CALL_INCOMING` | ring the SLIC line (debugfs `ring`) |
| lift handset while ringing | `accept` the call (off-hook detected) |
| baresip `CALL_ESTABLISHED` | stop ringing |
| hang up during a call | `hangup`, line back to idle |
| lift handset while idle | speed-dial the URI in `/root/voip-speeddial` |

> **The ring vs. hook race.** The 70–90 V ring voltage (`STATE 0x07`) corrupts the SIGREG hook bit → a false off-hook → the call would "answer and drop" on its own. The fix is to ring with a **cadence** (`RING_ON_TICKS=5` ≈ 0.75 s on / `RING_PERIOD=12` ≈ 1.05 s off, on 150 ms ticks) and sample the hook **only in the off gaps**, with a 3-read debounce. Answer latency is therefore ~1–2 s (wait for a gap + debounce + SLIC line-up). A baresip `audio_alert` pointing at the device must be removed, or baresip opens the device on an incoming call and steps on the driver's ring.

There is a residual cordless-handset quirk worth recording for anyone testing with a DECT phone: because the DECT base controls its own RF link, the handset's "end" button drops the DECT link but does **not** put the FXS line on-hook, so a hang-up from the handset alone can leave the SLIC line off-hook. The call-manager's `CALL_CLOSED` handling and the on-hook path cover the normal cases; this is a property of cordless phones, not the driver.

## The 2nd FXS channel (channel 2 / EC_2)

The Le9642 is a 2-channel device, and the second channel (the line behind the case "phone1" jack = SLIC channel 2 = EC_2 = 0x02) has been **validated end-to-end, one line at a time**: ring, clean RX earpiece audio (same u-law + `feed_ila` as ch1), hook detection, **and TX mic** all confirmed on a single handset moved between jacks (the test setup has one phone, so the two channels were not exercised simultaneously). TX was confirmed via the SADC spread + `rx_scan` diagnostics. This path is blob-free, using the same reconstructed ZSI/PCM stack as ch1. A `slic_ec` module parameter moves the whole per-channel pipeline (ring / hook / audio setup / codec / slot) to channel 2, reusing the same timeslot, DMA, char device and baresip account. Device init and chip detect stay on EC_1 (device-level).

Two facts that the 2nd-channel work nailed down:

- **The DC/AC/ring profiles are per-channel.** Device init only programs channel 1; `slic_load_channel_profiles()` loads a matched copy of `DC_FXS_miSLIC_BB_DEF`, `AC_FXS_RF14_600R_DEF_LE9641` and `RING_ZL880_BB90V_DEF` onto the selected channel so its hybrid and ring generator are correct.
- **The SIGREG hook bit is at byte[EC-1]** (the device-level register described above) — the original code read byte[0] and worked only for ch1.

The remaining work is **simultaneous two-line operation in userspace**. The per-channel plumbing in the kernel (`slic_ec`, `slic_slot`, per-channel hook index, per-channel profile load) is in place; what is left is the userspace side plus a second PCM timeslot: channel 2 on its **own bus slot** (e.g. slot 6 → DMA ch2, per the slot−4 mapping) so the lines don't collide, a second char device `/dev/xr500v-voice1`, a second baresip account, and a 2-line call-manager.

## Panel LED integration

The front-panel **phone LEDs are SoC GPIOs** on the TC3162 GPIO block (`gpio-tc3162` driver @ physical `0x1fbf0200`), not on the SLIC: **GPIO25 = phone1, GPIO26 = phone2** (active-low; matching the DTS `led-25`/`led-26` nodes). They are exposed as `gpio-leds` in the DTS and are independent of the FXS audio path — i.e. they can be wired to call/off-hook state via standard LED triggers or a userspace hook. See [08-front-panel-leds.md](08-front-panel-leds.md) for the full panel LED map and trigger wiring; the relevant fact here is simply that the two phone-port indicators are software-controllable SoC GPIOs.

## Persistence and operational notes

- **The SLIC state only resets on a cold boot.** Live `rmmod`/`insmod` does not reset the SLIC (it's a separate chip), and `HWRESET` is forbidden, so its codec/line state can accumulate cruft across many reloads. When in doubt, power-cycle (see [03-boot-partitions-flashing.md](03-boot-partitions-flashing.md) — `boot` crashes, always cold-boot).
- **The SLIC/PCM kernel modules ship in the firmware image; only the baresip userspace lives on the overlay.** `kmod-econet-pcm` is in `DEVICE_PACKAGES`, so the two kernel modules (`pcm-en751221.ko` + `econet-slic.ko`) are built into the squashfs root. baresip and the call-manager are deployed on the persistent UBIFS overlay (`/root/bsdeploy`, `/root/xr500v-callmgr`), launched from `/root/voip-start.sh` via `rc.local` — so the userspace can be updated without reflashing (lower brick risk).
- One gotcha: the package installs `/etc/modules-boot.d/35-econet-pcm`, which loads the modules in **pre-init, before the overlay mounts**. If newer `.ko` builds are staged on the overlay, the start script *swaps* them (`rmmod` the squashfs pair, `insmod` the overlay ones) before bring-up. A cleaner alternative is to whiteout that preload symlink in the overlay.
- **Flashing rule:** if updating the in-image modules, patch the image with
  `patch_trendchip_header.py --entry 0x80020000`, then use the board-specific
  OpenWrt `sysupgrade -T` / `sysupgrade` path. Stock telnet `:2323` remains the
  recovery alternative (see [03-boot-partitions-flashing.md](03-boot-partitions-flashing.md)).

## Source layout

| Path | Contents |
|------|----------|
| `package/kernel/econet-pcm/src/pcm-en751221.{c,h}` | PCM/TDM engine: probe, DMA rings, timeslots, the voice kthread, u-law (de)companding, kfifos, the `pcm_en751221_voice_*` API |
| `package/kernel/econet-pcm/src/econet-slic.c` | Le9642 over ZSI: transport, profiles, switcher/feed/ring, hook, audio setup, `/dev/xr500v-voice`, debugfs (`slic_init`, `audio_setup`, `rx_scan`, `ring`, `hook`, `sadc_probe`, `swctrl`) |
| `package/kernel/econet-pcm/src/baresip-xr500v/xr500v.c` | baresip `ausrc`/`auplay` audio module for `/dev/xr500v-voice` |
| `package/kernel/econet-pcm/src/xr500v-callmgr.c` | call-manager daemon (SLIC hook ↔ baresip `ctrl_tcp`) |
| `target/linux/econet/dts/en751221.dtsi` | `pcm0: pcm@1fbd0000` node |
| `target/linux/econet/dts/en751221_tplink_archer-xr500v.dts` | `&pcm0 { status = "okay"; }` |

The two kernel modules build as one package (a shared cross-module symbol export: `pcm_en751221_*` is called from `econet-slic.c`), and autoload in the order `pcm-en751221 econet-slic`.

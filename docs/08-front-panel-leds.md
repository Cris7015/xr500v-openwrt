# Front-Panel LEDs

The Archer XR500v has a ten-indicator front panel: **Power, GPON, LOS, Internet, 2.4GHz, 5GHz, WPS, Phone1, Phone2, USB**. Eight of these are ordinary active-low open-drain GPIOs on the EN751221 SoC, driven by a small reverse-engineered `gpio-tc3162` platform driver against the TrendChip GPIO block at physical `0x1fbf0200`. The two WiFi indicators (2.4GHz, 5GHz) are **not** SoC GPIOs at all — they live inside the two MediaTek WiFi chips, on two different mechanisms. They were initially hypothesized to require the proprietary OEM `rtpci` blob, but that conclusion was later overturned by reverse-engineering the OEM driver and confirming with live register pokes: the 5GHz LED is the MT7662 MAC LED engine slot 2 (active-low, configured purely from device tree), and the 2.4GHz LED is MT7603 chip-internal GPIO12/13 (active-low), lit by a small mainline `mt76` package patch. All ten indicators work natively in the OpenWrt port, persist across reboot, with no userspace scripts; the two radios blink on throughput via the driver-assigned `phyNtpt` trigger. This page documents the panel layout, the GPIO register block and its driver, the pad-enable quirk, the per-LED GPIO map, the phone-LED behavior, and the WiFi-LED result with exact register sequences.

## Panel overview

| Panel label | Source | Physical line | Active level | Trigger / behavior in the OpenWrt port |
|---|---|---|---|---|
| Power | hardwired | — (no GPIO) | — | Always on when powered; not software-controllable |
| GPON | SoC GPIO | gpio 2 | active-low | `default-on` (DT) — lit while the port is up |
| LOS / Alarm | SoC GPIO | gpio 24 | active-low | red:alarm — loss-of-signal / fault indicator |
| Internet | SoC GPIO | gpio 28 | active-low | `netdev` trigger on `eth0` (link/tx/rx) |
| 2.4GHz | MT7603 chip GPIO12+GPIO13 | inside WiFi chip | active-low | `phy0tpt` throughput trigger (mt76) |
| 5GHz | MT7662 MAC LED slot 2 | inside WiFi chip | active-low | `phy1tpt` throughput trigger (mt76) |
| WPS | SoC GPIO | gpio 7 | active-low | static (no default trigger) |
| Phone1 | SoC GPIO | gpio 25 | active-low | green:phone1 (FXS-0) |
| Phone2 | SoC GPIO | gpio 26 | active-low | green:phone2 (FXS-1) |
| USB | SoC GPIO | gpio 11 | active-low | `usbport` trigger (usb1/usb2 port1/port2) |

All eight SoC LEDs are **active-low, open-drain**: driving the GPIO **LOW lights the LED**. The Power LED is wired straight to a rail and cannot be addressed in software.

> Naming note: the panel silkscreen carries a discrete **LOS** (Loss-Of-Signal) indicator typical of GPON CPEs. In the DTS this is exposed as the device's single red indicator, `red:alarm` on gpio 24; the GPON-up indicator is the separate green `green:gpon` on gpio 2. The driver's reverse-engineered map labels gpio 24 as `ALAM` (alarm). On a GPON HGW this red/alarm LED is the LOS/fault state.

## The SoC GPIO block and the `gpio-tc3162` driver

The eight panel GPIOs are managed by a purpose-built driver, `package/kernel/gpio-tc3162`, which was reverse-engineered from the OEM GPL `tp_gpio` driver (`tp_gpio_led.c`, macros `LED_OEN` / `DO_LED_ON` / `DO_LED_OFF`). It is a standard Linux `gpio_chip` exposing 64 lines (`ngpio = 64`), so the LEDs are wired up through the generic `gpio-leds` framework and ordinary LED triggers.

The register block is at physical `0x1fbf0200` (OEM virtual `CR_GPIO_BASE = 0xbfbf0200`), spanning `0x80`. The device-tree node lives in `en751221.dtsi`:

```dts
gpio: gpio@1fbf0200 {
        compatible = "tplink,tc3162-gpio";
        reg = <0x1fbf0200 0x80>;
        gpio-controller;
        #gpio-cells = <2>;
        ngpios = <64>;
};
```

### Register map (offsets from `0x1fbf0200`)

| Offset | Name | Width | Meaning |
|---|---|---|---|
| `+0x00` | `TC_CTRL0` | gpio 0–15 | direction, **2 bits per gpio**, output-enable = bit `local*2` |
| `+0x04` | `TC_DATA0` | gpio 0–31 | data / level (1 bit per gpio) |
| `+0x14` | `TC_ODRAIN0` | gpio 0–31 | open-drain enable (1 bit per gpio) |
| `+0x20` | `TC_CTRL1` | gpio 16–31 | direction (2 bits per gpio) |
| `+0x60` | `TC_CTRL2` | gpio 32–47 | direction |
| `+0x64` | `TC_CTRL3` | gpio 48–63 | direction |
| `+0x70` | `TC_DATA1` | gpio 32–63 | data |
| `+0x78` | `TC_ODRAIN1` | gpio 32–63 | open-drain |

Direction uses **two bits per GPIO**: setting the output-enable bit (`local*2`) in the relevant `CTRLn` register makes the line an output. To turn an LED into an active-low open-drain output, the driver sets the output-enable bit in `CTRLn`, sets the matching bit in `ODRAINn`, then writes the level into `DATAn`. The bit-banking math (which `CTRLn`/`DATAn`/`ODRAINn` register a given line lands in) is in the driver's `ctrl_reg()`, `data_reg()`, and `odrain_reg()` helpers. Access is serialized by a spinlock and the chip is marked `can_sleep = false` (MMIO, no bus latency), so LED triggers can drive it from atomic context.

### The pad-enable quirk (IOMUX_CONTROL1 = 0xa0ad)

There is one non-obvious step without which the WPS LED (gpio 7) stays dark even though the GPIO is driven correctly: the SoC pin-share / IOMUX register must route the LED pads to the GPIO function. The bootloader leaves `IOMUX_CONTROL1` (`0x1fa20104`) at `0xf8`; the OEM firmware raises it to `0xa0ad`, which additionally enables the WPS LED pad. The driver replicates this at probe time:

```c
/* IOMUX_CONTROL1 = 0x1fa20104. Bootloader leaves 0xf8; OEM uses 0xa0ad,
 * which also enables the WPS LED pad (gpio7). */
void __iomem *mux = ioremap(0x1fa20104, 4);
iowrite32(0xa0ad, mux);
iounmap(mux);
```

This single write is what took the panel from 7/8 to 8/8 working SoC LEDs.

### `tc3162_poke` debug interface

The driver also exposes a debugfs poke node used throughout the LED (and VoIP) reverse-engineering work. It ioremaps any physical address — so it also reaches PCIe BARs — which made it the primary tool for probing the WiFi-chip LED registers. It is intentionally left in the shipped driver (read-only-ish, useful for RE):

```sh
# read a 32-bit physical register (result printed to dmesg)
echo "0x1fbf0204" > /sys/kernel/debug/tc3162_poke
# write a 32-bit value then read it back (readback in dmesg)
echo "0x1fa20104 0xa0ad" > /sys/kernel/debug/tc3162_poke
```

Implementation: it `ioremap`s `addr & ~0xfff` for `0x1000`, applies the optional write, then reads back and logs `tc3162-poke: [0x<addr>] = 0x<val>`. On the OpenWrt port (built without `CONFIG_DEVMEM`) this debugfs poke is the standard way to read/write MMIO and PCIe-mapped registers; on the stock OEM firmware the equivalent was a static MIPS binary doing its own `mknod /tmp/.mem` + `mmap` (see cross-reference below).

## Device-tree LED definitions

The eight SoC LEDs are declared as a `gpio-leds` node in `en751221_tplink_archer-xr500v.dts`:

```dts
leds {
        compatible = "gpio-leds";

        led-2  { label = "green:gpon";     gpios = <&gpio 2  GPIO_ACTIVE_LOW>; linux,default-trigger = "default-on"; };
        led-7  { label = "green:wps";      gpios = <&gpio 7  GPIO_ACTIVE_LOW>; };
        led-11 { label = "green:usb";      gpios = <&gpio 11 GPIO_ACTIVE_LOW>; };
        /* WiFi 2.4/5GHz LEDs are inside the MT7603/MT7662 chips (driven by
         * the mt76 cdevs mt76-phy0/mt76-phy1), NOT these SoC GPIOs. */
        led-24 { label = "red:alarm";      gpios = <&gpio 24 GPIO_ACTIVE_LOW>; };
        led-25 { label = "green:phone1";   gpios = <&gpio 25 GPIO_ACTIVE_LOW>; };
        led-26 { label = "green:phone2";   gpios = <&gpio 26 GPIO_ACTIVE_LOW>; };
        led-28 { label = "green:internet"; gpios = <&gpio 28 GPIO_ACTIVE_LOW>; };
};
```

The full reverse-engineered GPIO map (from the driver header) is: **XPON/GPON=2, WPS=7, USB=11, WL2G=12, WL5G=13, ALAM=24, FXS1/Phone1=25, FXS2/Phone2=26, INET/Internet=28**, with Power hardwired. Note that the OEM `tp_led` table *lists* the WiFi LEDs as `WL2G=gpio12` and `WL5G=gpio13`, but on this board those SoC GPIOs do **not** drive the panel WiFi LEDs (see the WiFi section): in stock firmware gpio 12/13 read as HIGH/open-drain — the active-low "off" state — while the panel WiFi LEDs are physically lit, so they are not the driving lines. The `gpio-leds` node deliberately omits gpio 12/13.

### Triggers (board.d/01_leds)

The dynamic triggers for the Internet and USB LEDs are assigned per-board in `target/linux/econet/base-files/etc/board.d/01_leds`:

```sh
tplink,archer-xr500v)
        ucidef_set_led_netdev "internet" "internet" "green:internet" "eth0" "link tx rx"
        ucidef_set_led_usbport "usb" "usb" "green:usb" "usb1-port1" "usb1-port2" "usb2-port1" "usb2-port2"
        ;;
```

- **Internet** (gpio 28): netdev trigger on `eth0`, lighting on link and flickering on tx/rx.
- **USB** (gpio 11): `usbport` trigger covering both USB2 and USB3 root-hub ports; plug→on / unplug→off is confirmed working. This pulls in `kmod-usb-ledtrig-usbport`, which is listed in `DEVICE_PACKAGES` for the device.
- **GPON** (gpio 2): static `default-on` from the DTS.

> Note on persistence: on a unit that already has a populated UBI overlay, a stale `/etc/config/system` can shadow `board.d` defaults, in which case triggers were applied directly via `uci`. A fresh flash applies them from `01_leds` automatically.

## Phone (FXS) LED behavior

The two telephone-port LEDs are plain SoC GPIOs — **Phone1 = gpio 25 (FXS-0)** and **Phone2 = gpio 26 (FXS-1)** — labeled `green:phone1` / `green:phone2`. They are not wired to any WiFi or PCIe device; they are driven through the normal `gpio-leds` framework like the other SoC LEDs.

Call-state indication is driven from the VoIP call manager rather than a kernel netdev trigger, because the meaningful states (registered, ringing, off-hook / in-call) are SIP/SLIC events, not link events. The reconstructed VoIP stack exposes the SLIC hook and ring state via debugfs in the `econet-pcm` package (`/sys/kernel/debug/econet-slic/{ring,hook,...}`), and the userspace `xr500v-callmgr` daemon bridges the Microsemi **Le9642** SLIC hook/ring events to baresip's `ctrl_tcp` interface (CALL_INCOMING→ring, CALL_ESTABLISHED, CALL_CLOSED→idle). The call manager owns those events and sets the phone-LED brightness directly through `/sys/class/leds/green:phone1` (and `phone2`): off-hook / in-call drives the LED solid on, ringing blinks it, and idle clears it. The gpio-leds sysfs class is the same interface used by every other panel LED.

> Uncertainty: the phone LEDs are exposed and individually controllable through the standard `gpio-leds` sysfs class, and the call manager drives them from the SLIC hook/ring events. The hardware path and the call-state event source both exist; the glue that ties ring/off-hook to LED brightness lives in the VoIP call-flow work (the `xr500v-callmgr` daemon) rather than in a static `ucidef_set_led_*` line in `01_leds`. See the [VoIP / FXS](06-voip-fxs-telephony.md) page for the SLIC ring/hook mechanism.

## WiFi LEDs

The 2.4GHz and 5GHz indicators were the hard part. They are **not** SoC GPIOs: driving the OEM-listed gpio 12/13 in every mode (high/low/push-pull/open-drain) lit nothing, and a byte-for-byte diff of the entire SCU pinmux/clock block (`0x1fa20000`–`+0x140`) and GPIO block between stock and OpenWrt showed them identical while the LEDs stayed dark. Both indicators live **inside the WiFi chips**, on different mechanisms. The key insight was that each is reached at a different layer of its chip; once those were identified, both light blob-free.

### 5GHz — MT7662 MAC LED, slot 2, active-low

The MT7662 (MT76x2 family, PCIe) has the standard mt76 MAC LED block (`MT_LED_CTRL = BAR+0x770`, `MT_LED_S0/S1` at `+0x77c`/`+0x780`, `MT_LED_CFG = +0x102c`). mt76 already kicks `MT_LED_CTRL`, but it defaults to **LED slot 0 active-high**, whereas this board's 5GHz panel LED is wired to **slot 2 active-low**. The earlier attempt matched slot 0 and so never lit anything; the same pattern (boards needing `led-sources=<2>`) appears upstream on the ZBT-WG1602 and Asus RP-AC56 (mt76 issue #55).

The fix is entirely in device tree — no driver patch. In `&slot1` (the MT7662 card):

```dts
&slot1 {
        wifi@0,0 {
                compatible = "mediatek,mt76";
                reg = <0x0000 0 0 0 0>;
                nvmem-cells = <&eeprom_misc_e0000>, <&macaddr_misc_f100>;
                nvmem-cell-names = "eeprom", "mac-address";

                /* Panel 5GHz LED = MT7662 MAC LED slot 2, active-low. */
                led {
                        led-sources = <2>;
                        led-active-low;
                };
        };
};
```

mt76 auto-assigns the `phy1tpt` throughput trigger via `cdev.default_trigger`, so no board.d/uci entry is needed; the LED lights and blinks on traffic. (The same can be reached at runtime, no reflash, through the mt76 debugfs knobs: `echo 2 > /sys/kernel/debug/ieee80211/phy1/mt76/led_pin; echo 1 > .../led_active_low; echo phy1tpt > /sys/class/leds/mt76-phy1/trigger`.)

For reference, the MT7662 BAR0 is `0x28000000` under OpenWrt (mainline assignment) versus `0x20100000` under the stock OEM driver; register offsets are identical, so the LED registers are at `0x28000770` etc. on the running port.

### 2.4GHz — MT7603 chip-internal GPIO12/13, active-low

The MT7603 (2.4GHz, PCIe) is different: its MAC LED block (`0x80024000`) is **not routed to this board's LED pad**, so that block lights nothing regardless of polarity. The panel 2.4GHz LED is instead wired to the **MT7603's chip-internal GPIO12 and GPIO13** (both drive the same LED, active-low). These chip GPIOs are reached through the PCIe **remap-2** window via `mt7603_reg_map()`; the relevant registers are in the chip's GPIO block at base `0x80023000`.

This required a small mainline `mt76` patch, `package/kernel/mt76/patches/004-mt7603-xr500v-chip-gpio-led.patch`, gated by a `mediatek,gpio-led` DT property so it only activates on this board. The register definitions added to `mt7603/regs.h`:

| Define | Address | Field / mask |
|---|---|---|
| `MT_GPIO_PINMUX_SEL2` | `0x80023084` | G12 = bits `[18:16]`, G13 = bits `[22:20]`; select GPIO function = `0x2` |
| `MT_GPIO_OE1_SET` | `0x80023034` | output-enable set |
| `MT_GPIO_DOUT1_SET` | `0x80023024` | data-out set (drive HIGH = LED off) |
| `MT_GPIO_DOUT1_RESET` | `0x80023028` | data-out reset (drive LOW = LED on) |
| `MT_GPIO_LED_MASK` | — | `BIT(12) | BIT(13)` = `0x3000` |

The init sequence selects the GPIO function on both pads, enables them as outputs, and the set/reset callbacks toggle the active-low level:

```c
/* init: select GPIO function (0x2) on pads 12 and 13, then enable as outputs */
addr = mt7603_reg_map(dev, MT_GPIO_PINMUX_SEL2);
val  = mt76_rr(dev, addr);
val &= ~(MT_GPIO_PINMUX_SEL2_G12 | MT_GPIO_PINMUX_SEL2_G13);
val |= FIELD_PREP(MT_GPIO_PINMUX_SEL2_G12, 0x2) |
       FIELD_PREP(MT_GPIO_PINMUX_SEL2_G13, 0x2);   /* -> sets 0x00220000 */
mt76_wr(dev, addr, val);

addr = mt7603_reg_map(dev, MT_GPIO_OE1_SET);
mt76_wr(dev, addr, MT_GPIO_LED_MASK);               /* OE = 0x3000 */

/* set: active-low -> drive LOW for on, HIGH for off */
addr = on ? mt7603_reg_map(dev, MT_GPIO_DOUT1_RESET)
          : mt7603_reg_map(dev, MT_GPIO_DOUT1_SET);
mt76_wr(dev, addr, MT_GPIO_LED_MASK);
```

Two details make it behave like a real activity LED:

1. The LED set callback is hooked from `mt7603_led_set_config()` — `mt7603_gpio_led_set(dev, delay_on != 0)` — so the brightness/activity state reaches the chip GPIO.
2. The hardware-blink callback is overridden to **return `-EINVAL`** (`mt7603_gpio_led_set_blink`). This forces the LED core to fall through to *software* blink (`led_set_software_blink`), whose timer toggles `brightness_set` at ~1 Hz, producing a real on-activity blink on GPIO12/13 — because the MAC LED block's hardware blink does not reach this pad.

The DT side, in `&slot0` (the MT7603), enables the patch and (separately) supplies the synthetic EEPROM the 2.4GHz radio needs:

```dts
&slot0 {
        wifi@0,0 {
                compatible = "mediatek,mt76";
                reg = <0x0000 0 0 0 0>;
                nvmem-cells = <&macaddr_misc_f100>;
                nvmem-cell-names = "mac-address";
                mac-address-increment = <1>;

                /* Panel 2.4GHz LED = MT7603 chip-internal GPIO12+GPIO13 */
                mediatek,gpio-led;

                mediatek,eeprom-data = /bits/ 8 < ... >;   /* synthetic EEPROM */
        };
};
```

mt76 auto-assigns the `phy0tpt` trigger, so like the 5GHz LED no board.d/uci entry is needed.

Because the `DOUT`, `PINMUX`, and `OE` registers are sticky (the driver does not otherwise touch them), a one-shot poke leaves a solid-on LED — useful for diagnostics. The full register-poke sequence for a manual solid-on (via `tc3162_poke` against the PCIe-mapped window) is `PINMUX_SEL2 = 0x00220000`, `OE1_SET = 0x3000`, then `DOUT1_RESET = 0x3000` (on) / `DOUT1_SET = 0x3000` (off). The MT7603 BAR0 is `0x20000000`; with the remap-2 window pointed at `0x80000000`, the GPIO block at `0x80023000` falls at BAR offset `0x200a3000`.

> OEM mechanism, for completeness: the stock driver does **not** poke these GPIO registers directly. It lights the 2.4GHz LED through an in-band MCU command — `AndesLedOP()` → `EXT_CID(0xED)` + `EXT_CMD_LED_CTRL(0x17)` — with the firmware owning the LED pad mux. The mainline driver has no such MCU LED hook, which is why the direct chip-GPIO approach was used here. Both reach the same physical pad; the chip-GPIO route avoids depending on firmware behavior. The genuine efuse has the EEPROM LED-config bytes (`0x3a`/`0x3c`/`0x3e`/`0x40`) blank (`0xFF`), so the OEM hardcodes the LED mode rather than reading it from EEPROM — meaning the synthetic-EEPROM zeros there are harmless and polarity defaults to active-low.

### History: the earlier dead-end hypothesis, later overturned

The 5GHz LED was at first thought to require the proprietary `rtpci` driver. mt76's `MT76x2` code marks the `MT_WLAN_FUN_CTRL_GPIO_*` LED-routing bits as `/* MT76x0 */` (i.e., not implemented for MT76x2), and a byte-for-byte diff of the MT7662 LED block (`MT_LED_CTRL=0x770`, `MT_LED_CFG=0x102c`; stock `CFG=0x7f031e46` vs mt76 `0x00031e46`) between stock and OpenWrt left the LED dark either way — leading to a (later overturned) verdict that the pad-enable was done only inside the OEM blob. The actual problem was matching the **wrong LED slot** (slot 0 instead of slot 2). Once `led-sources=<2>` + `led-active-low` were applied, the 5GHz LED lit and blinked with no blob. The 2.4GHz LED — the harder of the two — was solved in parallel by reverse-engineering the OEM MT7603 path and finding it on the chip's internal GPIO rather than the MAC LED pad. Neither indicator needs the proprietary blob.

## Build / image notes

The XR500v device recipe (`target/linux/econet/image/en751221.mk`) lists `kmod-mt76x2`, `kmod-mt7603`, and `kmod-usb-ledtrig-usbport` in `DEVICE_PACKAGES`, which is what makes the WiFi and USB LEDs work out of the box. A build trap to be aware of: the `mt76` that actually compiles for the image is the **mt76 package** (e.g. `mt76-2026.03.21~...`), not an in-kernel-tree copy, so the `004-mt7603-...` patch must land in the package patch directory (`package/kernel/mt76/patches/`) to take effect. For a clean upstream submission, the `mediatek,gpio-led` property would need to be added to the mt76 binding YAML (otherwise `unevaluatedProperties: false` would reject it).

All ten panel indicators were confirmed lit and persistent across reboot in the native (flashed) build, with both WiFi LEDs blinking on throughput and the USB LED responding to plug/unplug — with no userspace scripts.

## Cross-references

- [SoC and platform](02-hardware-chip-inventory.md) — the EN751221 SoC, SCU/pinmux block at `0x1fa20000`.
- [WiFi (mt76)](05-wifi-mt7603-mt7662.md) — MT7603 / MT7662 bring-up, EEPROM, PCIe BARs; the radios behind the 2.4GHz/5GHz LEDs.
- [VoIP / FXS (Le9642 SLIC)](06-voip-fxs-telephony.md) — SLIC ring/hook events that drive the phone LEDs; `econet-pcm` debugfs.
- [Reading physical memory](08-front-panel-leds.md) — the `tc3162_poke` debugfs interface and the stock-OEM `mknod`+`mmap` reader used during LED RE.
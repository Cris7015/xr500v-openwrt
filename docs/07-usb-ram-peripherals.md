## Summary

Beyond the radios, switch and GPON datapath, the Archer XR500v carries a small set of "everything-else" peripherals that the OpenWrt port brings up: a MediaTek xHCI USB host, 256 MB of DDR3 system RAM, the TC3162-style GPIO block that drives the front-panel LEDs (covered in detail on the [panel-LEDs page](08-front-panel-leds.md)), and the physical reset/WPS buttons on the chassis. None of these are exotic, but two of them needed real reverse-engineering to work on this board: the USB host has a USB3 port whose T-PHY is not wired/powered, which deadlocks the standard `xhci-mtk` probe until U3 is masked off; and the SoC actually has twice the RAM the early DTS declared, which only became usable after a kernel "diet" small enough to fit the fixed 3 MB `kernel1` flash slot. This page documents both fixes, the GPIO/USB package set, and the state of the buttons. All register addresses, mask values and partition sizes below are taken from the working port (`target/linux/econet/dts/en751221.dtsi`, `target/linux/econet/dts/en751221_tplink_archer-xr500v.dts`, `target/linux/econet/image/en751221.mk`, and `config.seed`) and from on-device verification.

---

## USB host (MediaTek xHCI)

### Hardware

The EN751221 integrates a MediaTek xHCI USB host controller, the same IP family used on `mediatek,mt8173-xhci` SoCs. On the XR500v it backs the single external USB port on the chassis. The controller is described by two register windows:

| Region | Physical base | Size | Role |
|---|---|---|---|
| `mac`  | `0x1fb90000` | `0x4000` | xHCI MMIO (operational/runtime registers) |
| `ippc` | `0x1fa80700` | `0x100`  | IP Port Control (MediaTek wrapper: port enable, PHY reset/status) |

Interrupt is INTC line `17`. The node is in the SoC `.dtsi`:

```dts
usb: usb@1fb90000 {
        compatible = "mediatek,mt8173-xhci", "mediatek,mtk-xhci";
        reg = <0x1fb90000 0x4000>,
              <0x1fa80700 0x100>;
        reg-names = "mac", "ippc";

        #address-cells = <1>;
        #size-cells = <0>;

        interrupt-parent = <&intc>;
        interrupts = <17>;
        usb3-lpm-capable;

        mediatek,u3p-dis-msk = <0x1>;   /* see below */
};
```

Note there is no `phys`/`phy-names` and no explicit `clocks` in the node. On this board the `xhci-mtk` driver does **not** call `of_phy_get` — the USB PHY is driven entirely through the IPPC registers that `xhci_mtk_host_enable()` pokes, and the core USB clocks (SYSPLL / REF / SYS125 / XHCI) are already stable out of the bootloader. So no `phy-mtk-tphy` driver and no DTS clock plumbing were required.

### The `u3p-dis-msk` fix (the load-bearing line)

The controller exposes both a USB2 and a USB3 port internally, but on the XR500v the **USB3 (SuperSpeed) port has no T-PHY cabled / powered**. The consequence is a hard probe failure:

- `xhci_mtk_host_enable()` waits for each enabled port's MAC to come out of reset. For the U3 port it polls `STS1_U3_MAC_RST` (bit 16 of the IPPC status register).
- With no live U3 T-PHY, that bit never clears.
- The driver times out with `clocks are not stable (0x2003d0f)` and aborts the whole controller with `can't setup: -145` — taking the working USB2 port down with it.

The fix is to tell the driver the U3 port does not exist:

```dts
mediatek,u3p-dis-msk = <0x1>;   /* disable USB3 port 0 (bit 0) */
```

Inside the driver, this makes `num_u3_ports > u3_ports_disabled` evaluate false, so it never waits on `STS1_U3_MAC_RST`. `host_enable` then completes, and the **USB2 port enumerates normally**. The trade-off is explicit and acceptable: the external port runs at USB 2.0 (480 Mbit/s) speed. There is no high-speed-only silicon limit being hidden here — the SuperSpeed lane simply has no PHY brought up on this board.

> Forward note: the OEM kernel's clock/reset code does define USB reset lines for this SoC (`USB_PHY_P0/P1`, `USB_HOST_P0`-style resets, exposed as `EN751221_USB_PHY_P0/P1_RST` and `EN751221_USB_HOST_P0_RST` in the airoha clock patch). They are not used by the current port — wiring a real U3 T-PHY (clocks + `phy-mtk-tphy` + those resets) is the path to SuperSpeed, but it has not been attempted and there is no evidence the XR500v board routes the U3 lanes at all.

### Enumeration and storage

With the mask in place, a USB2 mass-storage device enumerates as a standard SCSI disk:

```text
usb-storage 1-1:1.0: USB Mass Storage device detected
scsi 0:0:0:0: Direct-Access ...
sd 0:0:0:0: [sda] ... 
 sda: sda1
```

i.e. the stick appears as `/dev/sda` with partition `/dev/sda1`, mountable through the normal block/filesystem stack. Verified on-device with a USB2 flash drive.

### Package set

USB is built as **loadable modules**, deliberately: keeping them out of the built-in kernel matters because the compressed kernel must stay under the 3 MB `kernel1` slot (see the RAM section). The relevant selections in `config.seed`:

```text
CONFIG_PACKAGE_kmod-usb-common=y
CONFIG_PACKAGE_kmod-usb-core=y
CONFIG_PACKAGE_kmod-usb-xhci-hcd=y
CONFIG_PACKAGE_kmod-usb-xhci-mtk=y     # the MediaTek glue; HIDDEN, pulled in by usb3
CONFIG_PACKAGE_kmod-usb3=y             # selecting usb3 is what drags in usb-xhci-mtk
CONFIG_PACKAGE_kmod-usb-storage=y
CONFIG_PACKAGE_kmod-fs-vfat=y          # kmod-fs-exfat is also selected
CONFIG_PACKAGE_kmod-fs-exfat=y
```

`kmod-usb-xhci-mtk` is marked `HIDDEN` in OpenWrt, so it cannot be ticked directly — selecting `kmod-usb3` is what pulls it in. The board recipe (`Device/tplink_archer-xr500v` in `target/linux/econet/image/en751221.mk`) ships `kmod-usb-ledtrig-usbport` in `DEVICE_PACKAGES` so the front-panel USB LED follows the port state; the storage/filesystem kmods come from the saved `config.seed`. Plug → LED on / unplug → LED off is confirmed (see [panel LEDs](08-front-panel-leds.md)).

> Build note (historical): enabling USB once appeared to "break" Ethernet (`econet_eth ...: insufficient register space, probe failed -22`). The actual cause was an unrelated duplicate `econet-eth` package directory compiling a stale driver in parallel — not the USB change. Keep no backup copies of packages under `package/`.

---

## 256 MB RAM

### The hardware vs. the DTS

The XR500v is populated with **256 MB of DDR3-1066**, confirmed by the bootloader log (`DRAM size=256MB` / `Memory size 256MB`). The early DTS, however, declared only 128 MB, so half the RAM was simply unused. The fix in the board DTS is one line:

```dts
memory@0 {
        device_type = "memory";
        reg = <0x00000000 0x10000000>;   /* 0x10000000 = 256 MB */
};
```

After the full fix the device reports `MemTotal ≈ 249980 kB` (~244 MB usable), up from ~117 MB before.

### Why the one-liner wasn't enough — the 3 MB kernel slot

Bumping the memory node by itself produced a kernel that **would not boot**. The cause is the flash layout, not the RAM: the OpenWrt slot-B kernel lives in a NAND partition `kernel1` that is **exactly 3 MB (`0x300000`)** and cannot be grown without re-partitioning (the partition map mirrors the stock OEM layout — see the [boot / flash-layout page](03-boot-partitions-flashing.md)). Two interacting limits had to be satisfied at once:

1. **`kernel1` = 3 MB fixed.** The sysupgrade recipe builds the kernel image as `append-kernel | lzma | pad-to $(KERNEL_SIZE)`, with `KERNEL_SIZE := 3072k`. The kernel is flashed into the 3 MB `kernel1` slot, so the LZMA-compressed kernel must fit within 3072k; anything larger overflows the slot. There is no headroom.
2. **Bootloader decompression window (~7 MB raw).** The bootloader decompresses into a scratch region; the *decompressed* image must not overrun `free_mem_ptr` (boot log showed `Decompress to 80020000 free_mem_ptr=80750000`, i.e. ~7.18 MB of headroom) or LZMA corrupts and the boot aborts with a decompression error.

So the goal was a kernel that is **< 3 MB compressed AND < ~7 MB raw**.

### The fixes that made it boot

```text
# 1. DTS: declare the real RAM (board dts)
memory@0 { reg = <0x0 0x10000000>; };

# 2. Don't embed a rootfs into the kernel
CONFIG_TARGET_ROOTFS_INITRAMFS  → not set
CONFIG_INITRAMFS_SOURCE         → ""

# 3. Kernel "diet" so the compressed image fits 3072k
# CONFIG_KERNEL_KALLSYMS is not set      # drops the symbol table (~150 KB+ from .rodata)
# CONFIG_KERNEL_DEBUG_INFO is not set
```

- **INITRAMFS off** matters because with `CONFIG_TARGET_ROOTFS_INITRAMFS=y` the kernel embedded the full root filesystem via `CONFIG_INITRAMFS_SOURCE`, ballooning it far past the slot. Turning it off gives a plain kernel that boots its real squashfs rootfs from `rootfs1`.
- **KALLSYMS + DEBUG_INFO off** are what actually pulled the compressed kernel back under 3072k; both are present in `config.seed` (`# CONFIG_KERNEL_KALLSYMS is not set`, `# CONFIG_KERNEL_DEBUG_INFO is not set`). Better LZMA tuning was *not* the answer — the compressor was already near its limit (`lzma -lc1 -lp2 -pb2`, 8 MB dict; larger dictionaries saved only ~600 B). The diet, not the compressor, closed the gap.

A subtle build-cache trap surfaced here: incremental builds that only rebuilt the driver (`make package/.../clean`) kept reusing an older, smaller cached kernel and "worked" by accident; a real `make target/linux/clean` rebuilt the kernel with the current (fat) config and overflowed the slot. Always verify the produced sizes: the compressed kernel offset in the `sysupgrade.bin` (`hsqs` squashfs magic should land ~3 MB in) and `size vmlinux` (text+data < ~7 MB).

> Keeping big subsystems modular (`=m` → squashfs, which has 16 MB and uses ~5 MB) is the general lever for staying under the slot. WiFi (`mt76`), USB, netfilter, crypto and debug are the usual heavyweights; this is exactly why the USB stack ships as modules rather than built-in.

The alternative — re-partitioning NAND to grow `kernel1` — was avoided as riskier than the kernel diet.

---

## GPIO block (TC3162)

The SoC exposes a TrendChip/TC3162-style GPIO controller at physical **`0x1fbf0200`** (OEM `CR_GPIO_BASE = 0xbfbf0200`). The OpenWrt port drives it with an out-of-tree driver, `package/kernel/gpio-tc3162`, bound to compatible `tplink,tc3162-gpio`:

```dts
gpio: gpio@1fbf0200 {
        compatible = "tplink,tc3162-gpio";
        reg = <0x1fbf0200 0x80>;
        gpio-controller;
        #gpio-cells = <2>;
        ngpios = <64>;
};
```

### Register map

The block is 64 GPIOs wide, split across direction/data/open-drain registers (offsets relative to `0x1fbf0200`):

| Offset | Name | Covers | Notes |
|---|---|---|---|
| `0x00` | `TC_CTRL0`   | gpio 0–15  | direction, 2 bits/gpio (output-enable = bit `local*2`) |
| `0x04` | `TC_DATA0`   | gpio 0–31  | data/level |
| `0x14` | `TC_ODRAIN0` | gpio 0–31  | open-drain enable |
| `0x20` | `TC_CTRL1`   | gpio 16–31 | direction |
| `0x60` | `TC_CTRL2`   | gpio 32–47 | direction |
| `0x64` | `TC_CTRL3`   | gpio 48–63 | direction |
| `0x70` | `TC_DATA1`   | gpio 32–63 | data/level |
| `0x78` | `TC_ODRAIN1` | gpio 32–63 | open-drain enable |

Direction is 2 bits per GPIO (one register quadrant per 16 lines); data and open-drain are 1 bit per GPIO across two 32-bit registers. The panel LEDs hang off this controller as `gpio-leds` and are active-low.

### Pad-enable quirk (IOMUX_CONTROL1)

One detail the driver handles at probe time: the bootloader leaves `IOMUX_CONTROL1` (`0x1fa20104`) at `0xf8`, which does **not** enable the WPS-LED pad (gpio 7). The OEM firmware raises it to `0xa0ad`. The driver replicates this by `ioremap`-ing `0x1fa20104` and writing `0xa0ad` on probe, so the WPS LED pad is routed under OpenWrt:

```c
/* probe(): replicate OEM IOMUX_CONTROL1 = 0xa0ad (bootloader only sets 0xf8) */
void __iomem *mux = ioremap(0x1fa20104, 4);
iowrite32(0xa0ad, mux);
```

### Debug poke interface

For register-level RE the driver exposes a debugfs poke at `/sys/kernel/debug/tc3162_poke` (it `ioremap`s any physical page and reads/writes a 32-bit word, result in `dmesg`):

```sh
echo "0x1fa20104"        > /sys/kernel/debug/tc3162_poke   # read  -> dmesg
echo "0x1fa20104 0xa0ad" > /sys/kernel/debug/tc3162_poke   # write -> readback in dmesg
```

It is left enabled in the build deliberately — useful for SoC register exploration and harmless on a development device. (See [physical-memory access](08-front-panel-leds.md) and the [panel-LEDs page](08-front-panel-leds.md) for how this was used to map the WiFi-LED chip GPIOs.)

The full LED GPIO map (WPS=7, USB=11, GPON=2, ALARM=24, phone1=25, phone2=26, internet=28) and the WiFi-LED detail (those LEDs live inside the MT7603/MT7662 chips, not on SoC GPIOs) are documented on the [panel-LEDs page](08-front-panel-leds.md).

---

## Buttons (reset / WPS)

The chassis has the usual physical controls — a recessed reset button and a WPS button (plus the power switch, which is a hard power control, not a GPIO input). The OEM firmware maps these to SoC GPIO inputs via its `tp_button`/`tp_gpio` configuration.

**Current OpenWrt status (working).** All three buttons are wired up as a `gpio-keys-polled` node in the device tree — the TC3162 GPIO controller exposes no per-line interrupts, so the keys are polled and events are delivered by `kmod-gpio-button-hotplug`. The GPIO lines were read from the OEM `tp_btn_def` table and confirmed on the device, all active-low:

| Button | GPIO | `linux,code` |
|---|---|---|
| Reset | 0 | `KEY_RESTART` |
| Wi-Fi | 4 | `KEY_RFKILL` (toggles both radios) |
| WPS | 9 | `KEY_WPS_BUTTON` |

Wi-Fi and WPS are verified working (WPS needs a full `wpad`/`hostapd` with WPS support, e.g. `wpad-mbedtls`, and `option wps_pushbutton '1'` on the AP). **The Reset button's factory-reset path is confirmed working (2026-06-23):** holding Reset ≥ 5 s runs the standard handler (`jffs2reset -y && reboot`), which wipes the config back to defaults. Importantly, on this device's **UBI overlay** the reset re-initializes cleanly on the next boot — `mount_root` re-creates the UBIFS overlay (it does *not* fall back to tmpfs) — and the DSA LAN comes straight back up on the `board.d` default config, so no manual switch reconfiguration is needed after a reset.

---

## Cross-references

- [Hardware overview](02-hardware-chip-inventory.md) — chip inventory, RAM/flash part numbers, board photos.
- [Boot & flash layout](03-boot-partitions-flashing.md) — the NAND partition map, the 3 MB `kernel1` slot, dual-boot slots A/B, the trendchip header, flashing flow.
- [Panel LEDs](08-front-panel-leds.md) — the full GPIO/LED map, the IOMUX pad-enable, and the MT7603/MT7662 chip-internal WiFi LEDs.
- [Physical-memory access](08-front-panel-leds.md) — using the `tc3162_poke` debugfs and the OEM `/dev/mem` method for register RE.
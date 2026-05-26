# 256 MB RAM unlock (2026-05-26)

The XR500v physically has **256 MB DDR3-1066** (the stock bootloader prints `DRAM size=256MB`), but
the OpenWrt port was using only **128 MB**. After this change OpenWrt sees **MemTotal 249980 kB
(~244 MB usable)** — RAM roughly doubled. Image iter142 boots fine with LAN + WiFi + BQL + 256 MB.

## What blocked it
Just bumping the DTS to 256 MB made the device fail to boot with `decompression error`. Two
constraints, both hit at once:

1. **`kernel1` NAND partition is exactly 3 MB** (confirmed: the mtd6_kernel1 backup is 3.0 MB;
   rootfs1 is 16 MB). The flash path writes the (LZMA) kernel into this 3 MB partition.
2. **The image recipe rounds the kernel up to a multiple of 3072k.**
   `IMAGE/sysupgrade.bin := append-kernel | lzma | dd bs=3072k conv=sync | append-rootfs | mktplinkfw2`.
   The `dd conv=sync` pads the compressed kernel up to the next 3072k boundary. The kernel was only
   **~7.6 KB over 3072k**, so `dd` rounded it to **6144k (6 MB)** → it no longer fit the 3 MB
   partition → the bootloader read a truncated LZMA stream → decompression error.

The earlier working images (iter133–139) booted on a small **cached** kernel (< 3 MB) from an early
build; the first full kernel rebuild exposed the size problem.

## The fix (recipe)
1. **DTS** — `target/linux/econet/dts/en751221_tplink_archer-xr500v.dts`, `memory@0`:
   `reg = <0x00000000 0x10000000>;`  (256 MB; was `0x8000000` = 128 MB).
2. **Disable `CONFIG_TARGET_ROOTFS_INITRAMFS`** — with it on, `CONFIG_INITRAMFS_SOURCE` pointed at
   `root-econet` and the kernel embedded the whole rootfs as an initramfs → a ~6 MB compressed
   kernel. Turning it off gives a plain kernel.
3. **Kernel diet** so the compressed kernel fits under 3072k:
   `CONFIG_KERNEL_KALLSYMS=n` (drops the kernel symbol-name table from `.rodata`, ~150 KB+) and
   `CONFIG_KERNEL_DEBUG_INFO=n`. Result: LZMA kernel + appended DTB < 3,145,728 bytes → `dd` keeps
   it at exactly 3 MB → fits the partition.

## What did NOT help (don't bother)
- **Bigger LZMA dictionary** (`-d24/25/26`): saves ~600 B. **`-fb273`**: slightly worse.
  Compression with `-lc1 -lp2 -pb2` was already near-optimal — the diet was required, not better
  compression.
- **Bumping `KERNEL_SIZE`** in the recipe: useless — the NAND `kernel1` partition is physically 3 MB.

## Notes
- The bootloader's LZMA window handled the ~10 MB raw kernel fine; the earlier error was the 3 MB
  truncation, not a decompress-window overflow.
- `CONFIG_KERNEL_KALLSYMS=n` only costs symbol names in oops backtraces / empty `/proc/kallsyms`;
  module loading is unaffected (it uses `__ksymtab`). Fine for a router.
- The WiFi config (channel 100 / 23 dBm DFS) lives in the UBI overlay and survives a kernel/rootfs
  reflash. BQL `min_limit=262144` (patch 330) is in the image. SQM tools are bundled but inactive.

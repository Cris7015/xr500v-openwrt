#
# Sysupgrade support for EcoNet EN751221 boards.
#
# TP-Link Archer XR500v boots a raw squashfs from slot B: kernel1 (TP-Link v2
# header + lzma kernel, 3 MiB) and rootfs1 (squashfs). Configuration lives in
# the separate "openwrt_ubi" overlay partition, which is NOT part of the image
# and is left untouched, so it survives the upgrade. The sysupgrade.bin layout
# (see the image recipe: append-kernel | lzma | pad-to 3072k | append-rootfs |
# tplink-v2-header) is:
#
#   [ kernel1 = header + lzma kernel | 0x300000 ][ 0x200 gap ][ squashfs @ 0x300200 ]
#
# i.e. exactly the two slices flash-from-wsl.sh writes from stock telnet, only
# here we write them with the running system's `mtd` after sysupgrade pivots to
# its ramdisk. (The old "mtd write from running OpenWrt corrupts the NAND"
# warning predates the en75_bmt driver managing the OEM BMT; the UBI overlay
# already writes this NAND on every commit, so a BMT-aware mtd write is safe.)

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	tplink,archer-xr500v)
		# The squashfs must sit at 0x300200 (4-byte word index 786560).
		if [ "$(dd if="$1" bs=4 skip=786560 count=1 2>/dev/null)" != "hsqs" ]; then
			echo "Invalid image: no squashfs at 0x300200 — not an Archer XR500v sysupgrade.bin"
			return 1
		fi
		# kernel1 must carry the TrendChip header (0x4c3d2e1f at 0x60), or the
		# bootloader cannot find the rootfs (-> kernel_rootfs_ptr FFFFFFFF, an
		# Undefined Exception). This requires the trendchip-patched image
		# (the *-patched.bin), not the raw sysupgrade.bin.
		if [ "$(dd if="$1" bs=1 skip=96 count=4 2>/dev/null)" != "$(printf '\114\075\056\037')" ]; then
			echo "Image is missing the TrendChip header — flash the -patched.bin, not the raw sysupgrade.bin"
			return 1
		fi
		return 0
		;;
	esac

	return 0
}

platform_do_upgrade() {
	local board=$(board_name)

	case "$board" in
	tplink,archer-xr500v)
		# kernel1 = first 3 MiB (TP-Link v2 header + lzma kernel)
		dd if="$1" bs=131072 count=24 2>/dev/null | mtd write - kernel1
		# rootfs1 = squashfs from 0x300200 (512-byte block 6145)
		dd if="$1" bs=512 skip=6145 2>/dev/null | mtd write - rootfs1

		# Config lives in the untouched openwrt_ubi overlay, so it survives.
		# Only with -n (SAVE_CONFIG explicitly 0) do we wipe that overlay so the
		# first boot auto-provisions a clean one (see 79_ubi_attach).
		if [ "$SAVE_CONFIG" = "0" ]; then
			local um=$(grep '"openwrt_ubi"' /proc/mtd | cut -d: -f1 | tr -d 'mtd:')
			[ -n "$um" ] && { ubidetach -m "$um" 2>/dev/null; mtd erase "/dev/mtd$um" 2>/dev/null; }
		fi

		sync
		return 0
		;;
	esac

	default_do_upgrade "$1"
}

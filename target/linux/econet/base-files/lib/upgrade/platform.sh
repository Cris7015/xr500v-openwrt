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
	local image="$1"
	local image_size rootfs_payload gap_nonzero

	image_hex() {
		dd if="$image" bs=1 skip="$1" count="$2" 2>/dev/null |
			hexdump -v -e '1/1 "%02x"'
	}

	case "$board" in
	tplink,archer-xr500v)
		image_size=$(wc -c < "$image") || return 1
		rootfs_payload=$((image_size - 0x300200))
		if [ "$rootfs_payload" -lt 96 ] || [ "$rootfs_payload" -gt $((0x1000000)) ]; then
			echo "Invalid image: rootfs payload does not fit the 16 MiB rootfs1 partition"
			return 1
		fi
		# The squashfs must sit at 0x300200 (4-byte word index 786560).
		if [ "$(image_hex $((0x300200)) 4)" != "68737173" ]; then
			echo "Invalid image: no squashfs at 0x300200 — not an Archer XR500v sysupgrade.bin"
			return 1
		fi
		gap_nonzero=$(dd if="$image" bs=1 skip=$((0x300000)) count=$((0x200)) 2>/dev/null |
			tr -d '\000' | wc -c)
		if [ "$gap_nonzero" -ne 0 ]; then
			echo "Invalid image: required 512-byte gap at 0x300000 is not all zero"
			return 1
		fi
		# Validate the complete immutable TrendChip wrapper contract. The entry
		# is the LZMA wrapper load address, never the inner ELF entry.
		if [ "$(image_hex $((0x60)) 8)" != "4c3d2e1faa55aa55" ] ||
		   [ "$(image_hex $((0x68)) 4)" != "80020000" ] ||
		   [ "$(image_hex $((0x6c)) 4)" != "80020000" ] ||
		   [ "$(image_hex $((0x70)) 4)" != "01300000" ] ||
		   [ "$(image_hex $((0x74)) 4)" != "00000200" ] ||
		   [ "$(image_hex $((0x7c)) 4)" != "00300000" ] ||
		   [ "$(image_hex $((0x80)) 4)" != "01000000" ] ||
		   [ "$(image_hex $((0x88)) 4)" != "00000000" ] ||
		   [ "$(image_hex $((0x8c)) 4)" != "55aa0101" ]; then
			echo "Invalid image: incomplete/unsafe TrendChip header — use the validated -patched.bin"
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

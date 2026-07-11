TRX_ENDIAN := be

define Device/en751221_generic
  DEVICE_VENDOR := EN751221 Family
  DEVICE_MODEL := Initramfs Image
  DEVICE_TITLE := EN751221 Initramfs Image
  DEVICE_DESCRIPTION := In-memory build for testing and recovery of EN751221 SoCs
  DEVICE_DTS := en751221_generic
endef
TARGET_DEVICES += en751221_generic

define Device/nokia_g240g-e
  DEVICE_VENDOR := Nokia
  DEVICE_MODEL := G-240G-E
  DEVICE_DTS := en751221_nokia_g240g-e
  IMAGES := tclinux.trx
  IMAGE/tclinux.trx := append-kernel | lzma | tclinux-trx
  DEVICE_PACKAGES := kmod-usb3
endef
TARGET_DEVICES += nokia_g240g-e

define Device/smartfiber_xp8421-b
  DEVICE_VENDOR := SmartFiber
  DEVICE_MODEL := XP8421-B
  DEVICE_DTS := en751221_smartfiber_xp8421-b
  IMAGES := tclinux.trx
  IMAGE/tclinux.trx := append-kernel | lzma | tclinux-trx
  DEVICE_PACKAGES := kmod-usb3 kmod-mt7603 kmod-mt76x2
endef
TARGET_DEVICES += smartfiber_xp8421-b

# NOTE: This will not work for upgrading from factory because it requires a cryptographic signature
#       however, it it can be flashed, then it will boot correctly.
define Device/tplink_archer-vr1200v-v2
  DEVICE_VENDOR := TP-Link
  DEVICE_MODEL := Archer vr1200v
  DEVICE_VARIANT := v2
  TPLINK_FLASHLAYOUT := 16Mmtk
  TPLINK_HWID := 0x0b473502
  TPLINK_HWREV := 0x0006007c
  TPLINK_HWREVADD := 0x0
  TPLINK_HVERSION := 3
  DEVICE_DTS := en751221_tplink_archer-vr1200v-v2
  DEVICE_PACKAGES := kmod-mt76x2 wpad-mbedtls luci
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | lzma | pad-to 4193792 | append-rootfs | \
    tplink-v2-header -R 0x400000
endef
TARGET_DEVICES += tplink_archer-vr1200v-v2

# NOTE: XR500v is an OEM TCLinux (EcoNet EN751221) rebadged as TP-Link.
#       HWID is unknown (stock firmware is encrypted/unsigned OEM); dummy HWID used.
#       tplink-v2-header produces "03 00 00 00 ver. 2.0" magic accepted by the bldr.
#       tclinux-trx was dropped: it produces "2RDH" magic which the bldr rejects.
#       Flash via UART/U-Boot tftpboot for PoC Phase 1.
define Device/tplink_archer-xr500v
  DEVICE_VENDOR := TP-Link
  DEVICE_MODEL := Archer XR500v
  DEVICE_VARIANT := v1
  TPLINK_FLASHLAYOUT := 16Mmtk
  TPLINK_HWID := 0x0ec60001
  TPLINK_HWREV := 0x00000001
  TPLINK_HWREVADD := 0x0
  TPLINK_HVERSION := 3
  DEVICE_DTS := en751221_tplink_archer-xr500v
  KERNEL_SIZE := 3072k
  IMAGE_SIZE := 16384k
  BLOCKSIZE := 128k
  DEVICE_PACKAGES := kmod-mt76x2 kmod-mt7603 kmod-usb-ledtrig-usbport wpad-mbedtls luci kmod-econet-pcm kmod-gpio-button-hotplug kmod-leds-gpio \
	kmod-i2c-mt7621-econet kmod-xr500v-en7570-diag \
	baresip baresip-mod-g711 baresip-mod-l16 baresip-mod-ctrl_tcp xr500v-voip
  IMAGES := sysupgrade.bin
  # raw squashfs in slot B; mtd-split creates rootfs_data; UBI is provisioned
  # from running OpenWrt (see chosen.bootargs ubi.mtd=rootfs_data).
  IMAGE/sysupgrade.bin := append-kernel | lzma | pad-to $$$$(KERNEL_SIZE) | append-rootfs | \
    tplink-v2-header -R 0x400000
endef
TARGET_DEVICES += tplink_archer-xr500v

define Device/zyxel_pmg5617ga
  DEVICE_VENDOR := Zyxel
  DEVICE_MODEL := PMG5617GA
  DEVICE_DTS := en751221_zyxel_pmg5617ga
  IMAGES := tclinux.trx
  IMAGE/tclinux.trx := append-kernel | lzma | tclinux-trx
  DEVICE_PACKAGES := kmod-usb3 kmod-mt7603 kmod-mt76x2
endef
TARGET_DEVICES += zyxel_pmg5617ga

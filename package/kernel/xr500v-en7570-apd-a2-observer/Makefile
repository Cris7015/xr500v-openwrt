include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-en7570-apd-a2-observer
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-en7570-apd-a2-observer
  SUBMENU:=Other modules
  TITLE:=XR500v guarded EN7570 APD A2 observer
  DEPENDS:=@TARGET_econet +kmod-gpio-tc3162 +kmod-i2c-mt7621-econet +kmod-crypto-sha256
  FILES:=$(PKG_BUILD_DIR)/xr500v-en7570-apd-a2-observer.ko
endef

define KernelPackage/xr500v-en7570-apd-a2-observer/description
  Non-shipping, non-autoload EN7570 APD-only observer for the XR500v. It has
  an experimental compatible and requires immutable module and DT opt-ins,
  the exact per-unit 0x190-byte factory-block SHA-256, asserted GPIO16
  TX_DISABLE, inactive xPON TX gates, and exact cold EN7570 safety state. It
  permits at most the three fixed OEM-width APD writes ending at code 0xa2,
  records read-only pre/post results in debugfs, never retries or rolls back,
  and remains pinned with TX_DISABLE asserted until physical power removal.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-en7570-apd-a2-observer))

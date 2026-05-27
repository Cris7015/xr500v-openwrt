include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=gpio-tc3162
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/gpio-tc3162
  SUBMENU:=Other modules
  TITLE:=TC3162/EN751221 GPIO driver
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/gpio-tc3162.ko
  AUTOLOAD:=$(call AutoLoad,30,gpio-tc3162,1)
endef

define KernelPackage/gpio-tc3162/description
  GPIO controller for the EcoNet/TrendChip TC3162 (EN751221) SoC register
  block at 0x1fbf0200. Drives the TP-Link Archer XR500v panel LEDs.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,gpio-tc3162))

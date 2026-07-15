include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-mac-preflight
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-mac-preflight
  SUBMENU:=Other modules
  TITLE:=XR500v read-only GPON MAC bus preflight
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-mac-preflight.ko
endef

define KernelPackage/xr500v-gpon-mac-preflight/description
  Manual one-shot snapshot of the EN751221 SCU WAN mode and xPON reset state.
  It deliberately performs zero accesses to the GPON MAC window after live
  testing proved that an unprepared 0x1fb64000 read stalls the system bus.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-mac-preflight))

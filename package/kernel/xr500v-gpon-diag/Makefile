include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-diag
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-diag
  SUBMENU:=Other modules
  TITLE:=XR500v read-only xPON PHY diagnostics
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-diag.ko
endef

define KernelPackage/xr500v-gpon-diag/description
  Read-only diagnostics for the EN751221 xPON PHY register block used by
  the TP-Link Archer XR500v. The module never writes PHY, MAC, reset, laser,
  APD, or interrupt registers.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-diag))

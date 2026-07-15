include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-mac-snapshot
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-mac-snapshot
  SUBMENU:=Other modules
  TITLE:=XR500v guarded six-register GPON MAC snapshot
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-mac-snapshot.ko
endef

define KernelPackage/xr500v-gpon-mac-snapshot/description
  Manually armed EN751221 lab probe that globally quiesces both VPEs, selects
  GPON in the SCU WAN mux, reads six non-destructive GPON MAC status registers
  and restores the exact original ATM mux value. It has no GPON write or IRQ.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-mac-snapshot))

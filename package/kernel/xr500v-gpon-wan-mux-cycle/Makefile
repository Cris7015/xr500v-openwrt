include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-wan-mux-cycle
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-wan-mux-cycle
  SUBMENU:=Other modules
  TITLE:=XR500v reversible GPON WAN-mux cycle audit
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-wan-mux-cycle.ko
endef

define KernelPackage/xr500v-gpon-wan-mux-cycle/description
  Manually armed EN751221 lab audit that briefly selects GPON in the SCU WAN
  mux, verifies the readback and restores the original ATM selection. It makes
  no access to the GPON MAC, PHY, transceiver, IRQ, reset or packet datapath.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-wan-mux-cycle))

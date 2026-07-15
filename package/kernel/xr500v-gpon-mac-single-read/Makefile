include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-mac-single-read
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-mac-single-read
  SUBMENU:=Other modules
  TITLE:=XR500v one-shot GPON MAC ONU-ID read
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-mac-single-read.ko
endef

define KernelPackage/xr500v-gpon-mac-single-read/description
  Manually armed EN751221 lab probe that selects GPON in the SCU WAN mux,
  performs exactly one read of G_ONU_ID, restores the exact original ATM mux
  value and removes the four-byte mapping. It has no GPON write or IRQ path.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-mac-single-read))

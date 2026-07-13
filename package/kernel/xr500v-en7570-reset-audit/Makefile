include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-en7570-reset-audit
PKG_RELEASE:=2

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-en7570-reset-audit
  SUBMENU:=Other modules
  TITLE:=XR500v guarded EN7570 reset audit prototype
  DEPENDS:=@TARGET_econet +kmod-i2c-core
  FILES:=$(PKG_BUILD_DIR)/xr500v-en7570-reset-audit.ko
endef

define KernelPackage/xr500v-en7570-reset-audit/description
  Non-shipping, non-autoload EN7570 whole-device reset observation prototype.
  It requires independent module and DT opt-ins, an exact per-register phase-12
  baseline, asserted physical TX_DISABLE and inactive xPON TX/test/IRQ state.
  It supports either the proven reset-only observation or a reset-then-LOS
  receiver observation. Both are non-transactional, use only fixed-register
  four-byte writes, perform no TX/APD/DDMI initialization or software rollback,
  and keep TX_DISABLE asserted afterward.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-en7570-reset-audit))

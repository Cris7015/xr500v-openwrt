include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-xpon-digital-handoff-probe
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-xpon-digital-handoff-probe
  SUBMENU:=Other modules
  TITLE:=XR500v guarded xPON digital handoff probe
  DEPENDS:=@TARGET_econet +kmod-gpio-tc3162 +kmod-crypto-sha256
  FILES:=$(PKG_BUILD_DIR)/xr500v-xpon-digital-handoff-probe.ko
endef

define KernelPackage/xr500v-xpon-digital-handoff-probe/description
  Lab-only, non-shipping and non-autoload XR500v receiver probe. It
  temporarily clears PHYSET3.ESD_PRO, asserts PHYSET2.FWRDY, takes six finite
  read-only samples, then restores both writable controls in reverse order.
  PHYSET2.PHYRDY is observed as hardware status and is excluded only from the
  final writable-baseline proof. It has no EN7570, APD, laser, reset, PLL,
  counter, IRQ, GPON MAC or SCU path.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-xpon-digital-handoff-probe))

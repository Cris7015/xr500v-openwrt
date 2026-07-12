include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-xpon-rx-init
PKG_RELEASE:=3

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-xpon-rx-init
  SUBMENU:=Other modules
  TITLE:=XR500v guarded xPON RX init prototype
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-xpon-rx-init.ko
endef

define KernelPackage/xr500v-xpon-rx-init/description
  Staged RX/SerDes prototype for the Archer XR500v. It is
  deliberately absent from DEVICE_PACKAGES, has no autoload entry and has no
  matching node in the shipping DTS. If all explicit safety gates are
  supplied, one mutually exclusive stage clears the signal-detect deglitch
  bit, applies the OEM EN7570 RX LOS/SD polarity, or enables only the passive
  RX counters without latching or clearing them. It always requires TXEN low
  and preserves all unrelated register bits. It never accesses EN7570, MAC,
  QDMA, interrupts, mode selection, PLL/reset, PRBS or test-frame controls.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-xpon-rx-init))

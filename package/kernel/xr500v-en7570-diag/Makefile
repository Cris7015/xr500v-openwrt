include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-en7570-diag
PKG_RELEASE:=2

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-en7570-diag
  SUBMENU:=Other modules
  TITLE:=XR500v read-only EN7570 identification/status
  DEPENDS:=@TARGET_econet +kmod-i2c-mt7621-econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-en7570-diag.ko
  AUTOLOAD:=$(call AutoLoad,53,xr500v-en7570-diag,1)
endef

define KernelPackage/xr500v-en7570-diag/description
  Passive identification/status driver for the Archer XR500v EN7570 LDDLA at
  I2C address 0x70. It issues register-pointer reads for the silicon ID,
  variant, and the LOS, rogue-ONU, Tx-SD, and Tx-fault status bits used by the
  OEM information function. It never writes register data, resets the chip,
  enables the laser, changes APD state, loads calibration, or starts ADC/DDMI
  conversions.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-en7570-diag))

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-en7570-los-trace-observer
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-en7570-los-trace-observer
  SUBMENU:=Other modules
  TITLE:=XR500v guarded EN7570 terminal LOS trace observer
  DEPENDS:=@TARGET_econet +kmod-gpio-tc3162 +kmod-i2c-mt7621-econet +kmod-crypto-sha256
  FILES:=$(PKG_BUILD_DIR)/xr500v-en7570-los-trace-observer.ko
endef

define KernelPackage/xr500v-en7570-los-trace-observer/description
  Compile-only, non-shipping and non-autoload XR500v terminal LOS observer.
  It permits exactly the 15 fixed reset, same-unit RSSI one-LSB, gain and LOS
  I2C writes, then takes a finite timestamped read-only trace with dense early
  LOS probes and critical/full safety checkpoints. It has no xPON MMIO write,
  APD write, worker, retry, rollback, arbitrary payload, current, laser or
  TGEN path; it self-pins and requires physical power removal.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-en7570-los-trace-observer))

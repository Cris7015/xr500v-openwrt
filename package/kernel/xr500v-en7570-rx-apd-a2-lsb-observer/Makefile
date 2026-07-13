include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-en7570-rx-apd-a2-lsb-observer
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-en7570-rx-apd-a2-lsb-observer
  SUBMENU:=Other modules
  TITLE:=XR500v guarded EN7570 RX/APD A2 one-LSB observer
  DEPENDS:=@TARGET_econet +kmod-gpio-tc3162 +kmod-i2c-mt7621-econet +kmod-crypto-sha256
  FILES:=$(PKG_BUILD_DIR)/xr500v-en7570-rx-apd-a2-lsb-observer.ko
endef

define KernelPackage/xr500v-en7570-rx-apd-a2-lsb-observer/description
  Compile-only, non-shipping and non-autoload XR500v receiver observer. It
  permits exactly 18 fixed EN7570 I2C writes: reset, the strict same-unit RSSI
  one-LSB oracle, RSSI gain, LOS setup and the initial APD code 0xa2. It
  excludes ESD and every TX/current/worker path, performs one fixed xPON
  polarity write, samples OVP first at sample zero and 20 fixed 100 ms
  intervals, self-pins, never retries or rolls back, and requires physical
  power removal.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-en7570-rx-apd-a2-lsb-observer))

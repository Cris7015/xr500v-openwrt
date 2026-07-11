include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=i2c-mt7621-econet
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/i2c-mt7621-econet
  SUBMENU:=I2C support
  TITLE:=MT7621-compatible I2C controller for EcoNet
  DEPENDS:=@TARGET_econet +kmod-i2c-core
  KCONFIG:=CONFIG_I2C_MT7621
  FILES:=$(LINUX_DIR)/drivers/i2c/busses/i2c-mt7621.ko
  AUTOLOAD:=$(call AutoLoad,52,i2c-mt7621,1)
endef

define KernelPackage/i2c-mt7621-econet/description
  Linux I2C adapter driver for the MT7621-compatible controller in EN751221.
  The Econet kernel patches make its reset line optional.
endef

# The module is built as part of the target kernel. This package only collects
# the resulting in-tree .ko; do not run the generic userspace build recipe.
define Build/Compile
endef

$(eval $(call KernelPackage,i2c-mt7621-econet))

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=econet-pcm
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/econet-pcm
  SUBMENU:=Other modules
  TITLE:=EN751221 PCM/TDM controller driver
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/pcm-en751221.ko
  AUTOLOAD:=$(call AutoLoad,35,pcm-en751221,1)
endef

define KernelPackage/econet-pcm/description
  PCM/TDM controller driver for the EcoNet/TrendChip EN751221 SoC register
  block at 0x1fbd0000. Foundation for VoIP/FXS support (Le9641 SLIC).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,econet-pcm))

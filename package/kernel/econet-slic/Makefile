include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=econet-slic
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/econet-slic
  SUBMENU:=Other modules
  TITLE:=EN751221 Le9641 SLIC SPI driver
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/econet-slic.ko
  AUTOLOAD:=$(call AutoLoad,36,econet-slic,1)
endef

define KernelPackage/econet-slic/description
  Le9641 SLIC SPI/MPI interface driver for the EcoNet/TrendChip EN751221 SoC.
  Phase 3: chip detect over the "new" Zarlink SLIC SPI block at 0x1fbd4000.
  The detect runs on demand via debugfs, not at module load.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,econet-slic))

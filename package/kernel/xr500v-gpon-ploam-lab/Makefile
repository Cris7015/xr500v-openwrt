include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=xr500v-gpon-ploam-lab
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/xr500v-gpon-ploam-lab
  SUBMENU:=Other modules
  TITLE:=XR500v software-only GPON PLOAM integration lab
  DEPENDS:=@TARGET_econet
  FILES:=$(PKG_BUILD_DIR)/xr500v-gpon-ploam-lab.ko
endef

define KernelPackage/xr500v-gpon-ploam-lab/description
  Hardware-independent GPON PLOAM O1-O7 state machine plus a software-only
  activation-vector self-test. The module has no DT match, autoload, MMIO,
  IRQ, I2C, GPIO, laser, APD, PHY, netdev or packet data-path access. It is an
  integration checkpoint for backporting the EN7523 work to Linux 6.12 before
  an EN751221-specific hardware adapter is written.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,xr500v-gpon-ploam-lab))

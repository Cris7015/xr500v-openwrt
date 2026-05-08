include $(TOPDIR)/rules.mk

PKG_NAME:=econet-eth
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/cjdelisle/econet_eth.git
PKG_MIRROR_HASH:=skip
PKG_SOURCE_DATE:=2026-02-13
PKG_SOURCE_VERSION:=c2f855cf5bdcb2dfc536d865d21bc793a346ce44

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

# iter57: this commit ships 5 .ko files compiled from one source tree:
#   econet-eth.ko          - main eth (GDM/QDMA)
#   econet-gsw.ko          - DSA switch core (mt7530 fork w/ EN751221 support)
#   econet-gsw-mdio.ko     - MDIO probe variant of switch driver
#   econet-gsw-mmio.ko     - MMIO probe variant (used by EN751221 switch@1fb58000)
#   mtk-tag.ko             - DSA tag protocol "MTK" (taggers/tag_mtk.c equivalent)
# Upstream CONFIG_NET_DSA_MT7530 must be DISABLED in kernel config to avoid
# symbol collision (these forks define the same exported symbols).

define KernelPackage/econet-eth
  SUBMENU:=Network Devices
  TITLE:=EcoNet EN751221 Ethernet Driver (eth+DSA bundle)
  DEPENDS:=@TARGET_econet +kmod-libphy +kmod-of-mdio +kmod-mdio-devres \
           +kmod-dsa +kmod-pcs-mtk-lynxi
  FILES:=$(PKG_BUILD_DIR)/econet-eth.ko \
         $(PKG_BUILD_DIR)/econet-gsw.ko \
         $(PKG_BUILD_DIR)/econet-gsw-mmio.ko \
         $(PKG_BUILD_DIR)/mtk-tag.ko
  AUTOLOAD:=$(call AutoLoad,90,econet-eth econet-gsw econet-gsw-mmio mtk-tag)
endef

define KernelPackage/econet-eth/description
  Out-of-tree EcoNet EN751221 ethernet + DSA switch driver bundle.
  Replaces upstream NET_DSA_MT7530 for this chip.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,econet-eth))

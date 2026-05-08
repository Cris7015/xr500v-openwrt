#!/usr/bin/env python3
"""
iter52: Extiende el sysfs del driver econet-eth para exponer TODO el bloque
MMIO 0x1FB50000-0x1FB60000 (struct en751221_regs completo), no solo switch_regs.

Agrega 2 sysfs entries nuevos:
  /sys/devices/platform/1fb50000.ethernet/raw_offset  (escribir offset 0x0..0xFFFC, 4-byte aligned)
  /sys/devices/platform/1fb50000.ethernet/raw_value   (read/write u32 a esa offset)

Mapping de offsets dentro del MMIO (de en75_eth.c struct en751221_regs):
  0x0000-0x03FF  fe[]            (frame engine)
  0x0400-0x0BFF  port0 (GDM1)    ← donde llegan los frames del switch
  0x0C00-0x0FFF  ppe[]           (Packet Processing Engine)
  0x1000-0x13FF  unknown_deadbeef
  0x1400-0x1BFF  port1 (GDM2)
  0x1C00-0x1FFF  unknown_deadbeef2
  0x2000-0x23FF  ppe_accounting
  0x2400-0x3FFF  ppe_unused
  0x4000-0x5FFF  qdma_regs[0..1]
  0x6000-0x7FFF  unknown_zeroed
  0x8000-0xFFFF  switch_regs[]   (igual que sw_offset actual de iter51)

Uso:
  ./iter52_apply_raw_sysfs.py [openwrt_dir]
    openwrt_dir defaults to ~/openwrt

Antes de correr este script:
  cd ~/openwrt
  make package/kernel/econet-eth/clean V=s
  make package/kernel/econet-eth/prepare V=s   # extrae fuente fresca

Después:
  cd ~/openwrt
  make package/kernel/econet-eth/compile V=s
  make target/linux/install V=s   # rebuild kernel image

Idempotente: detecta si ya está aplicado y no hace nada.
"""
import os
import sys
import glob

OPENWRT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/openwrt")
SENTINEL = "ITER52_RAW_SYSFS"


def find_eth_c(openwrt_dir):
    """Locate econet_eth.c inside build_dir."""
    pattern = os.path.join(
        openwrt_dir,
        "build_dir/target-mips_24kc_musl/linux-econet_en751221/econet-eth-*/econet_eth.c",
    )
    matches = glob.glob(pattern)
    if not matches:
        sys.exit(f"ERROR: no se encontró econet_eth.c bajo {pattern}\n"
                 f"Corré primero: cd {openwrt_dir} && make package/kernel/econet-eth/prepare V=s")
    return matches[0]


def patch(fn):
    src = open(fn).read()

    if SENTINEL in src:
        print(f"[i] {fn} ya tiene iter52 aplicado, skipping")
        return

    # Localizar dónde insertar las funciones nuevas y los DEVICE_ATTR.
    # Asumimos que iter51 ya agregó sw_offset/sw_value y device_create_file en probe.
    # Insertamos las nuevas funciones justo antes de en75_probe.
    needle = "static int en75_probe(struct platform_device *pdev)\n"
    if needle not in src:
        sys.exit("ERROR: en75_probe no encontrado — driver muy distinto al esperado")

    new_code = r'''
/* === ITER52_RAW_SYSFS: full MMIO debug access ===
 * Expose any 4-byte-aligned offset in 0x0..0xFFFC of the en751221_regs MMIO
 * range. Lets us read GDM/PPE/QDMA registers (not just switch_regs).
 */
static u32 g_iter52_raw_offset;

static ssize_t raw_offset_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x%x\n", g_iter52_raw_offset);
}

static ssize_t raw_offset_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	u32 v;
	if (kstrtou32(buf, 0, &v) < 0)
		return -EINVAL;
	if (v >= 0x10000)
		return -EINVAL;
	g_iter52_raw_offset = v & ~3u;
	return count;
}
static DEVICE_ATTR_RW(raw_offset);

static ssize_t raw_value_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct en75_eth_pvt *eth = platform_get_drvdata(pdev);
	u32 __iomem *base;
	u32 val;

	if (!eth || !eth->regs)
		return -ENODEV;
	base = (u32 __iomem *) eth->regs;
	val = en75_rreg(&base[g_iter52_raw_offset / 4]);
	return sysfs_emit(buf, "0x%08x\n", val);
}

static ssize_t raw_value_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct en75_eth_pvt *eth = platform_get_drvdata(pdev);
	u32 __iomem *base;
	u32 v;

	if (!eth || !eth->regs)
		return -ENODEV;
	if (kstrtou32(buf, 0, &v) < 0)
		return -EINVAL;
	base = (u32 __iomem *) eth->regs;
	en75_wreg(v, &base[g_iter52_raw_offset / 4]);
	return count;
}
static DEVICE_ATTR_RW(raw_value);
/* === END ITER52_RAW_SYSFS === */

'''
    src = src.replace(needle, new_code + needle, 1)

    # Insertar device_create_file en probe.
    # Buscamos el lugar donde iter51 ya hizo device_create_file(&pdev->dev, &dev_attr_sw_value);
    # y agregamos los nuestros justo después.
    sw_value_create = "device_create_file(&pdev->dev, &dev_attr_sw_value);"
    if sw_value_create in src:
        addition = (sw_value_create
                    + "\n\tdevice_create_file(&pdev->dev, &dev_attr_raw_offset);"
                    + "\n\tdevice_create_file(&pdev->dev, &dev_attr_raw_value);")
        src = src.replace(sw_value_create, addition, 1)
        print("[+] Inserted device_create_file calls after sw_value")
    else:
        # Fallback: agregarlas al final de probe — buscamos return 0 cerca del final
        print("[!] sw_value device_create_file no encontrado — iter51 sysfs no instalado?")
        print("    Intentando fallback: agregar al final del probe via comentario manual")
        # Marker simple: inserción directa antes del return final del probe
        # No forzar — mejor que el user lo agregue a mano
        sys.exit("ERROR: agregá manualmente en en75_probe:\n"
                 "    device_create_file(&pdev->dev, &dev_attr_raw_offset);\n"
                 "    device_create_file(&pdev->dev, &dev_attr_raw_value);")

    open(fn, "w").write(src)
    print(f"[+] {fn} parchado con iter52 raw sysfs")


if __name__ == "__main__":
    fn = find_eth_c(OPENWRT)
    print(f"[*] econet_eth.c at {fn}")
    patch(fn)
    print()
    print("Listo. Ahora rebuilds:")
    print(f"  cd {OPENWRT} && make package/kernel/econet-eth/compile V=s")
    print(f"  cd {OPENWRT} && make target/linux/install V=s")
    print(f"  cd {OPENWRT} && make -j4 V=s   # full rebuild si querés .bin nuevo")

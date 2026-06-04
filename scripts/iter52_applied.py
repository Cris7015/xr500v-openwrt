#!/usr/bin/env python3
"""iter52: adds raw_offset/raw_value sysfs to econet_eth.c (full MMIO 0x0..0xFFFC)."""
import sys

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/econet-eth-2026.01.27~1db74f83/econet_eth.c"

src = open(FN).read()

if "en75_sysfs_raw_offset" in src:
    print("[i] iter52 already applied")
    sys.exit(0)

needle = 'static DEVICE_ATTR(sw_value, 0644, en75_sysfs_value_show, en75_sysfs_value_store);\n'
if needle not in src:
    sys.exit("ERROR: sw_value anchor not found")

addition = '''
/* iter52: raw MMIO sysfs - read any reg in en751221_regs (0x0..0xFFFC) */
static u32 en75_sysfs_raw_offset;

static ssize_t en75_sysfs_raw_offset_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\\n", en75_sysfs_raw_offset);
}

static ssize_t en75_sysfs_raw_offset_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	u32 v;
	if (kstrtou32(buf, 0, &v))
		return -EINVAL;
	if (v >= 0x10000 || (v & 3))
		return -EINVAL;
	en75_sysfs_raw_offset = v;
	return count;
}

static ssize_t en75_sysfs_raw_value_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	u32 v;
	u32 __iomem *base;
	if (!en75_sysfs_pvt || !en75_sysfs_pvt->regs)
		return -ENODEV;
	base = (u32 __iomem *) en75_sysfs_pvt->regs;
	v = en75_rreg(&base[en75_sysfs_raw_offset / 4]);
	return sprintf(buf, "0x%08x\\n", v);
}

static ssize_t en75_sysfs_raw_value_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	u32 v;
	u32 __iomem *base;
	if (!en75_sysfs_pvt || !en75_sysfs_pvt->regs)
		return -ENODEV;
	if (kstrtou32(buf, 0, &v))
		return -EINVAL;
	base = (u32 __iomem *) en75_sysfs_pvt->regs;
	en75_wreg(v, &base[en75_sysfs_raw_offset / 4]);
	return count;
}

static DEVICE_ATTR(raw_offset, 0644, en75_sysfs_raw_offset_show, en75_sysfs_raw_offset_store);
static DEVICE_ATTR(raw_value, 0644, en75_sysfs_raw_value_show, en75_sysfs_raw_value_store);

'''

src = src.replace(needle, needle + addition, 1)

sw_create = '\tdevice_create_file(&pdev->dev, &dev_attr_sw_value);\n'
if sw_create not in src:
    sys.exit("ERROR: device_create_file sw_value anchor not found")

create_addition = '\tdevice_create_file(&pdev->dev, &dev_attr_raw_offset);\n\tdevice_create_file(&pdev->dev, &dev_attr_raw_value);\n'
src = src.replace(sw_create, sw_create + create_addition, 1)

open(FN, 'w').write(src)
print("[+] iter52 applied to econet_eth.c")

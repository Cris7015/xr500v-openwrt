// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO driver for EcoNet / TrendChip TC3162 (EN751221) SoC.
 *
 * Register block at physical 0x1fbf0200 (OEM CR_GPIO_BASE = 0xbfbf0200).
 * Direction is set via CR_GPIO_CTRLn (2 bits per gpio, output-enable = bit
 * local*2) plus open-drain via CR_GPIO_ODRAINn; the level is in CR_GPIO_DATAn.
 * Reverse-engineered from the OEM GPL tp_gpio driver (tp_gpio_led.c macros
 * LED_OEN / DO_LED_ON / DO_LED_OFF).
 *
 * The panel LEDs on the TP-Link Archer XR500v are active-low open-drain GPIOs
 * (drive LOW = lit): XPON=2 WPS=7 USB1=11 WL2G=12 WL5G=13 ALAM=24 FXS1=25
 * FXS2=26 INET=28 (power LED is hardwired).
 */
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

#define TC_CTRL0	0x00	/* gpio 0-15  direction (2 bits/gpio)  */
#define TC_DATA0	0x04	/* gpio 0-31  data                     */
#define TC_ODRAIN0	0x14	/* gpio 0-31  open-drain               */
#define TC_CTRL1	0x20	/* gpio 16-31 direction                */
#define TC_CTRL2	0x60	/* gpio 32-47 direction                */
#define TC_CTRL3	0x64	/* gpio 48-63 direction                */
#define TC_DATA1	0x70	/* gpio 32-63 data                     */
#define TC_ODRAIN1	0x78	/* gpio 32-63 open-drain               */

struct tc3162_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	spinlock_t lock;
};

static u32 tcg_rd(struct tc3162_gpio *p, unsigned int reg)
{
	return ioread32(p->base + reg);
}

static void tcg_wr(struct tc3162_gpio *p, unsigned int reg, u32 v)
{
	iowrite32(v, p->base + reg);
}

/* direction register + output-enable bit for a gpio (2 bits per gpio) */
static unsigned int ctrl_reg(unsigned int off, unsigned int *bit)
{
	if (off < 16)		{ *bit = off * 2;		return TC_CTRL0; }
	else if (off < 32)	{ *bit = (off - 16) * 2;	return TC_CTRL1; }
	else if (off < 48)	{ *bit = (off - 32) * 2;	return TC_CTRL2; }
	*bit = (off - 48) * 2;	return TC_CTRL3;
}

static unsigned int data_reg(unsigned int off, unsigned int *bit)
{
	if (off < 32) { *bit = off; return TC_DATA0; }
	*bit = off - 32; return TC_DATA1;
}

static unsigned int odrain_reg(unsigned int off, unsigned int *bit)
{
	if (off < 32) { *bit = off; return TC_ODRAIN0; }
	*bit = off - 32; return TC_ODRAIN1;
}

static int tcg_get(struct gpio_chip *gc, unsigned int off)
{
	struct tc3162_gpio *p = gpiochip_get_data(gc);
	unsigned int bit, reg = data_reg(off, &bit);

	return !!(tcg_rd(p, reg) & BIT(bit));
}

static void tcg_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct tc3162_gpio *p = gpiochip_get_data(gc);
	unsigned int bit, reg = data_reg(off, &bit);
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&p->lock, flags);
	v = tcg_rd(p, reg);
	if (val)
		v |= BIT(bit);
	else
		v &= ~BIT(bit);
	tcg_wr(p, reg, v);
	spin_unlock_irqrestore(&p->lock, flags);
}

static int tcg_dir_out(struct gpio_chip *gc, unsigned int off, int val)
{
	struct tc3162_gpio *p = gpiochip_get_data(gc);
	unsigned int cbit, creg = ctrl_reg(off, &cbit);
	unsigned int obit, oreg = odrain_reg(off, &obit);
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&p->lock, flags);
	v = tcg_rd(p, creg); v |= BIT(cbit);  tcg_wr(p, creg, v);	/* output */
	v = tcg_rd(p, oreg); v |= BIT(obit);  tcg_wr(p, oreg, v);	/* o-drain */
	spin_unlock_irqrestore(&p->lock, flags);

	tcg_set(gc, off, val);
	return 0;
}

static int tcg_dir_in(struct gpio_chip *gc, unsigned int off)
{
	struct tc3162_gpio *p = gpiochip_get_data(gc);
	unsigned int cbit, creg = ctrl_reg(off, &cbit);
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&p->lock, flags);
	v = tcg_rd(p, creg); v &= ~BIT(cbit); tcg_wr(p, creg, v);
	spin_unlock_irqrestore(&p->lock, flags);
	return 0;
}

static int tcg_get_dir(struct gpio_chip *gc, unsigned int off)
{
	struct tc3162_gpio *p = gpiochip_get_data(gc);
	unsigned int cbit, creg = ctrl_reg(off, &cbit);

	return (tcg_rd(p, creg) & BIT(cbit)) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

/* --- debug: poke arbitrary physical registers (iomux/pinmux experiments) ---
 * echo "0xADDR"       > /sys/kernel/debug/tc3162_poke   (read,  result in dmesg)
 * echo "0xADDR 0xVAL" > /sys/kernel/debug/tc3162_poke   (write, readback in dmesg)
 */
static ssize_t tc3162_poke_write(struct file *f, const char __user *ub,
				 size_t n, loff_t *o)
{
	char buf[64];
	unsigned long addr, val;
	void __iomem *m;
	u32 cur;
	int have_val;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, n))
		return -EFAULT;
	buf[n] = '\0';
	have_val = (sscanf(buf, "%lx %lx", &addr, &val) == 2);
	if (!have_val && sscanf(buf, "%lx", &addr) != 1)
		return -EINVAL;
	m = ioremap(addr & ~0xfffUL, 0x1000);
	if (!m)
		return -ENOMEM;
	if (have_val)
		iowrite32((u32)val, m + (addr & 0xfff));
	cur = ioread32(m + (addr & 0xfff));
	iounmap(m);
	pr_info("tc3162-poke: [0x%lx] = 0x%08x%s\n",
		addr, cur, have_val ? " (after write)" : "");
	return n;
}

static const struct file_operations tc3162_poke_fops = {
	.owner = THIS_MODULE,
	.write = tc3162_poke_write,
};

static int tc3162_gpio_probe(struct platform_device *pdev)
{
	struct tc3162_gpio *p;
	int ret;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	spin_lock_init(&p->lock);
	p->gc.label = "tc3162-gpio";
	p->gc.parent = &pdev->dev;
	p->gc.owner = THIS_MODULE;
	p->gc.base = -1;
	p->gc.ngpio = 64;
	p->gc.get = tcg_get;
	p->gc.set = tcg_set;
	p->gc.direction_output = tcg_dir_out;
	p->gc.direction_input = tcg_dir_in;
	p->gc.get_direction = tcg_get_dir;
	p->gc.can_sleep = false;

	ret = devm_gpiochip_add_data(&pdev->dev, &p->gc, p);
	if (ret)
		return ret;

	/* Enable LED pad functions in the SCU pin-share register
	 * (IOMUX_CONTROL1 = 0x1fa20104). The bootloader only sets 0xf8;
	 * the OEM value 0xa0ad also enables the WPS LED pad (gpio7). */
	{
		void __iomem *mux = ioremap(0x1fa20104, 4);
		if (mux) {
			iowrite32(0xa0ad, mux);
			iounmap(mux);
		}
	}

	debugfs_create_file("tc3162_poke", 0200, NULL, NULL, &tc3162_poke_fops);
	return 0;
}

static const struct of_device_id tc3162_gpio_of[] = {
	{ .compatible = "tplink,tc3162-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, tc3162_gpio_of);

static struct platform_driver tc3162_gpio_driver = {
	.probe = tc3162_gpio_probe,
	.driver = {
		.name = "gpio-tc3162",
		.of_match_table = tc3162_gpio_of,
	},
};
module_platform_driver(tc3162_gpio_driver);

MODULE_DESCRIPTION("EcoNet/TrendChip TC3162 (EN751221) GPIO driver");
MODULE_LICENSE("GPL");

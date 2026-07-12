// SPDX-License-Identifier: GPL-2.0-only
/*
 * Passive EN7570 identification and status for the TP-Link Archer XR500v.
 *
 * The EN7570 register pointer is a 16-bit big-endian value. Reads therefore
 * require a pointer-only I2C write followed by a repeated-start read. This
 * module never sends register payload data and has no write/reset/calibration,
 * laser, APD, ADC, DDMI, or control-worker path.
 */

#include <linux/debugfs.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#define EN7570_SAFE_PROTECT	0x0100
#define EN7570_SVADC_PD		0x0024
#define EN7570_LOS_CTRL1	0x011c
#define EN7570_LOS_CTRL2	0x0120
#define EN7570_LOS_CAL_TIMER	0x0124
#define EN7570_LOS_CAL_TIMEOUT_CNT 0x0128
#define EN7570_LOS_CAL_TIMEOUT	0x012c
#define EN7570_LOS_DBG_RG	0x0130
#define EN7570_ADC_PROBE_STATUS	0x0154
#define EN7570_PROBE_CONTROL	0x0158
#define EN7570_DUMMY		0x015c
#define EN7570_ROGUE_ONU_DET_CTRL 0x0168
#define EN7570_FT_ADC_CLK_CLR	0x0170
#define EN7570_EXPECTED_ID	0x03

struct xr500v_en7570_diag {
	struct i2c_client *client;
	struct dentry *debugfs_dir;
	u8 silicon_id;
	u8 variant;
};

static int en7570_read(struct i2c_client *client, u16 reg, void *value,
			int length)
{
	u8 pointer[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg messages[2] = {
		{
			.addr = client->addr,
			.len = sizeof(pointer),
			.buf = pointer,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = value,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(messages) ? 0 : -EIO;
}

static int en7570_read8(struct i2c_client *client, u16 reg, u8 *value)
{
	return en7570_read(client, reg, value, 1);
}

static void en7570_seq_raw(struct seq_file *s, struct i2c_client *client,
			    const char *name, u16 reg, int length)
{
	u8 data[4];
	int ret;

	ret = en7570_read(client, reg, data, length);
	if (ret)
		seq_printf(s, "%-23s read error %d\n", name, ret);
	else
		seq_printf(s, "%-23s %*ph\n", name, length, data);
}

static int status_show(struct seq_file *s, void *unused)
{
	struct xr500v_en7570_diag *diag = s->private;
	u8 safe_protect[2];
	u8 los_debug[4];
	u8 rogue_tx[2];
	u8 silicon_id;
	u8 variant;
	int safe_ret;
	int los_ret;
	int rogue_ret;
	int id_ret;
	int variant_ret;

	safe_ret = en7570_read(diag->client, EN7570_SAFE_PROTECT,
				safe_protect, sizeof(safe_protect));
	los_ret = en7570_read(diag->client, EN7570_LOS_DBG_RG,
			       los_debug, sizeof(los_debug));
	rogue_ret = en7570_read(diag->client, EN7570_ROGUE_ONU_DET_CTRL,
				 rogue_tx, sizeof(rogue_tx));
	id_ret = en7570_read8(diag->client, EN7570_FT_ADC_CLK_CLR,
			       &silicon_id);
	variant_ret = en7570_read8(diag->client, EN7570_DUMMY, &variant);

	seq_printf(s, "i2c_bus:              %d\n", diag->client->adapter->nr);
	seq_printf(s, "i2c_address:          0x%02x\n", diag->client->addr);
	if (id_ret)
		seq_printf(s, "silicon_id:           read error %d\n", id_ret);
	else
		seq_printf(s, "silicon_id:           0x%02x%s\n", silicon_id,
			   silicon_id == EN7570_EXPECTED_ID ? " (EN7570)" :
			   " (unexpected)");
	if (variant_ret)
		seq_printf(s, "variant:              read error %d\n", variant_ret);
	else
		seq_printf(s, "variant:              0x%02x\n", variant);
	if (los_ret) {
		seq_printf(s, "los_status:           read error %d\n", los_ret);
	} else {
		seq_printf(s, "los_status:           %u\n",
			   !!(los_debug[3] & BIT(0)));
		seq_printf(s, "los_debug_raw:        %*ph\n",
			   (int)sizeof(los_debug), los_debug);
	}
	if (rogue_ret) {
		seq_printf(s, "rogue_onu_status:     read error %d\n", rogue_ret);
		seq_printf(s, "tx_sd_status:         read error %d\n", rogue_ret);
	} else {
		seq_printf(s, "rogue_onu_status:     %u\n",
			   !!(rogue_tx[1] & BIT(2)));
		seq_printf(s, "tx_sd_status:         %u\n",
			   !!(rogue_tx[1] & BIT(3)));
		seq_printf(s, "rogue_tx_raw:         %*ph\n",
			   (int)sizeof(rogue_tx), rogue_tx);
	}
	if (safe_ret) {
		seq_printf(s, "tx_fault_status:      read error %d\n", safe_ret);
	} else {
		seq_printf(s, "tx_fault_status:      %u\n",
			   !!(safe_protect[1] & BIT(7)));
		seq_printf(s, "safe_protect_raw:     %*ph\n",
			   (int)sizeof(safe_protect), safe_protect);
	}
	en7570_seq_raw(s, diag->client, "los_ctrl1_raw:",
			EN7570_LOS_CTRL1, 4);
	en7570_seq_raw(s, diag->client, "svadc_pd_raw:",
			EN7570_SVADC_PD, 4);
	en7570_seq_raw(s, diag->client, "los_ctrl2_raw:",
			EN7570_LOS_CTRL2, 4);
	en7570_seq_raw(s, diag->client, "los_cal_timer_raw:",
			EN7570_LOS_CAL_TIMER, 4);
	en7570_seq_raw(s, diag->client, "los_timeout_cnt_raw:",
			EN7570_LOS_CAL_TIMEOUT_CNT, 4);
	en7570_seq_raw(s, diag->client, "los_timeout_raw:",
			EN7570_LOS_CAL_TIMEOUT, 4);
	en7570_seq_raw(s, diag->client, "adc_probe_raw_unlatched:",
			EN7570_ADC_PROBE_STATUS, 4);
	en7570_seq_raw(s, diag->client, "probe_control_raw:",
			EN7570_PROBE_CONTROL, 4);
	seq_puts(s, "status_interpretation: raw/uninitialized optical block\n");
	seq_puts(s, "register_data_writes: 0\n");
	seq_puts(s, "reset_or_init:        no\n");
	seq_puts(s, "laser_or_apd_control: no\n");
	seq_puts(s, "adc_or_ddmi_control:  no\n");

	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, inode->i_private);
}

static const struct file_operations status_fops = {
	.owner = THIS_MODULE,
	.open = status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int xr500v_en7570_diag_probe(struct i2c_client *client)
{
	struct xr500v_en7570_diag *diag;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	diag = devm_kzalloc(&client->dev, sizeof(*diag), GFP_KERNEL);
	if (!diag)
		return -ENOMEM;

	diag->client = client;
	i2c_set_clientdata(client, diag);

	ret = en7570_read8(client, EN7570_FT_ADC_CLK_CLR, &diag->silicon_id);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "cannot read EN7570 silicon ID\n");

	ret = en7570_read8(client, EN7570_DUMMY, &diag->variant);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "cannot read EN7570 variant\n");

	if (diag->silicon_id != EN7570_EXPECTED_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unexpected silicon ID 0x%02x\n",
				     diag->silicon_id);

	diag->debugfs_dir = debugfs_create_dir("xr500v-en7570", NULL);
	if (IS_ERR(diag->debugfs_dir))
		return PTR_ERR(diag->debugfs_dir);

	debugfs_create_file("status", 0444, diag->debugfs_dir, diag,
			    &status_fops);

	dev_info(&client->dev,
		 "EN7570 identified: silicon ID 0x%02x, variant 0x%02x; passive mode\n",
		 diag->silicon_id, diag->variant);
	return 0;
}

static void xr500v_en7570_diag_remove(struct i2c_client *client)
{
	struct xr500v_en7570_diag *diag = i2c_get_clientdata(client);

	debugfs_remove_recursive(diag->debugfs_dir);
}

static const struct of_device_id xr500v_en7570_diag_of_match[] = {
	{ .compatible = "airoha,en7570-diag" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_en7570_diag_of_match);

static struct i2c_driver xr500v_en7570_diag_driver = {
	.driver = {
		.name = "xr500v-en7570-diag",
		.of_match_table = xr500v_en7570_diag_of_match,
	},
	.probe = xr500v_en7570_diag_probe,
	.remove = xr500v_en7570_diag_remove,
};
module_i2c_driver(xr500v_en7570_diag_driver);

MODULE_DESCRIPTION("Passive EN7570 identification/status for TP-Link Archer XR500v");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");

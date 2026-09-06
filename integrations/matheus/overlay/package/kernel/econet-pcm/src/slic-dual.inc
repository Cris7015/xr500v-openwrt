/* SPDX-License-Identifier: GPL-2.0-only */
#include "pcm-dual.h"
/* Opt-in dual FXS: shared reset/setup once; subsequent controls are per line. */
static bool dual_fxs;
module_param(dual_fxs, bool, 0444);

struct slic_dual_line {
	struct miscdevice misc;
	unsigned int index;
	u8 ec, slot;
	bool ringing;
	int hook_cache;
};
static struct slic_dual_line dl[2];

/* Selected-channel register reads only: never invoke legacy reset/probe nodes. */
static int slic_dual_status_show(struct seq_file *s, void *unused)
{
	struct slic_dual_line *l = s->private;
	const u8 opcodes[] = { 0x57, 0x41, 0x43, 0x61, 0x71 };
	const char * const names[] = { "state", "tx_slot", "rx_slot", "opfunc", "opcond" };
	u8 v, icr4[4];
	int i, ret;

	mutex_lock(&sd.lock);
	seq_printf(s, "phone=%u ec=%u ring_requested=%u hook_cache=%d\n",
		l->index + 1, l->ec, l->ringing, l->hook_cache);
	for (i = 0; i < ARRAY_SIZE(opcodes); i++) {
		ret = zsi_mpi_read(l->ec, opcodes[i], &v, 1);
		if (ret) seq_printf(s, "%s error=%d\n", names[i], ret);
		else seq_printf(s, "%s=0x%02x\n", names[i], v);
	}
	ret = zsi_mpi_read(l->ec, 0xf5, icr4, sizeof(icr4));
	if (!ret)
		seq_printf(s, "icr4=%02x %02x %02x %02x\n", icr4[0], icr4[1], icr4[2], icr4[3]);
	mutex_unlock(&sd.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_dual_status);

/* sd.lock serializes both managers' complete ZSI operations. */
static int slic_dual_state(struct slic_dual_line *l, u8 state)
{
	u8 cmd[] = { 0x56, state };
	int ret = zsi_write(CSLAC_EC_REG_WRT, &l->ec, 1);

	return ret ? ret : slic_write_mpi("dual-state", cmd, sizeof(cmd));
}

static int slic_dual_open(struct inode *inode, struct file *file)
{
	struct slic_dual_line *l = container_of(file->private_data,
						struct slic_dual_line, misc);
	int ret = pcm_en751221_dual_open(l->index);

	if (ret)
		return ret;
	mutex_lock(&sd.lock);
	ret = slic_dual_state(l, 0x23);
	mutex_unlock(&sd.lock);
	if (ret) {
		pcm_en751221_dual_close(l->index);
		return ret;
	}
	stream_open(inode, file);
	file->private_data = l;
	pr_info("econet-slic: phone%u stream opened\n", l->index + 1);
	return 0;
}

static int slic_dual_release(struct inode *inode, struct file *file)
{
	struct slic_dual_line *l = file->private_data;

	pcm_en751221_dual_close(l->index);
	mutex_lock(&sd.lock);
	slic_dual_state(l, 0x0c);
	mutex_unlock(&sd.lock);
	pr_info("econet-slic: phone%u stream closed (shared PCM retained)\n", l->index + 1);
	return 0;
}

static ssize_t slic_dual_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;

	return pcm_en751221_dual_io(l->index, buf, len, true);
}

static ssize_t slic_dual_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;

	return pcm_en751221_dual_io(l->index, (char __user *)buf, len, false);
}

static const struct file_operations slic_dual_fops = {
	.owner = THIS_MODULE, .open = slic_dual_open, .release = slic_dual_release,
	.read = slic_dual_read, .write = slic_dual_write,
};

static ssize_t slic_dual_control_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;
	u8 sig[4];
	char value[32];
	int ret = 0, n;

	if (*off)
		return 0;
	mutex_lock(&sd.lock);
	if (!strcmp(f->f_path.dentry->d_name.name, "hook")) {
		/* Ring voltage corrupts hook samples; freeze them while either
		 * ring waveform is energized, without resetting either channel. */
		if (!dl[0].ringing && !dl[1].ringing) {
			ret = zsi_mpi_read(l->ec, 0x4d, sig, sizeof(sig));
			if (!ret)
				l->hook_cache = !!(sig[l->ec - 1] & 1);
		}
		n = scnprintf(value, sizeof(value), "%d\n", l->hook_cache);
	} else {
		ret = slic_dual_state(l, 0x23);
		n = scnprintf(value, sizeof(value), "active=%d\n", !ret);
	}
	mutex_unlock(&sd.lock);
	return ret ? ret : simple_read_from_buffer(buf, len, off, value, n);
}

static ssize_t slic_dual_control_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	struct slic_dual_line *l = f->private_data;
	bool ringing = !strcmp(f->f_path.dentry->d_name.name, "ring");
	u8 value;
	int ret = kstrtou8_from_user(buf, len, 10, &value);

	if (ret)
		return ret;
	mutex_lock(&sd.lock);
	if (ringing) {
		slic_ring_state(l->ec, value != 0);
		l->ringing = value != 0;
		ret = 0;
	} else {
		/* Keep the common switcher in HP; never drop another call's rail. */
		ret = slic_dual_state(l, value ? 0x0c : 0x23);
	}
	mutex_unlock(&sd.lock);
	return ret ? ret : len;
}

static const struct file_operations slic_dual_control_fops = {
	.owner = THIS_MODULE, .open = simple_open, .read = slic_dual_control_read,
	.write = slic_dual_control_write, .llseek = default_llseek,
};

static int slic_dual_init(void)
{
	int i, ret, registered = 0;
	int old_ec = slic_ec, old_slot = slic_slot;

	pcm_en751221_voice_reset();
	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	slic_load_profiles();
	for (i = 0; i < 2; i++) {
		dl[i].index = i;
		dl[i].ec = i ? VP886_EC_1 : VP886_EC_2;
		dl[i].slot = 4 + 2 * i;
		slic_ec = dl[i].ec;
		slic_slot = dl[i].slot;
		ret = slic_audio_setup();
		if (ret)
			break;
		slic_lpm_arm_locked();
		ret = slic_dual_state(&dl[i], 0x0c);
		if (ret)
			break;
	}
	slic_ec = old_ec;
	slic_slot = old_slot;
	mutex_unlock(&sd.lock);
	if (ret)
		return ret;
	ret = pcm_en751221_dual_start();
	if (ret)
		return ret;
	sd.dbg = debugfs_create_dir(DRV_NAME, NULL);
	for (i = 0; i < 2; i++) {
		struct dentry *dir;

		dl[i].misc.minor = MISC_DYNAMIC_MINOR;
		dl[i].misc.name = i ? "xr500v-voice1" : "xr500v-voice";
		dl[i].misc.fops = &slic_dual_fops;
		ret = misc_register(&dl[i].misc);
		if (ret)
			goto fail;
		registered++;
		dir = debugfs_create_dir(i ? "phone2" : "phone1", sd.dbg);
		debugfs_create_file("status", 0400, dir, &dl[i], &slic_dual_status_fops);
		debugfs_create_file("hook", 0400, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("audio_setup", 0400, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("ring", 0200, dir, &dl[i], &slic_dual_control_fops);
		debugfs_create_file("lpm", 0200, dir, &dl[i], &slic_dual_control_fops);
	}
	return 0;
fail:
	while (registered--)
		misc_deregister(&dl[registered].misc);
	debugfs_remove_recursive(sd.dbg);
	pcm_en751221_dual_stop();
	return ret;
}

static void slic_dual_exit(void)
{
	int i;

	pcm_en751221_dual_stop();
	for (i = 0; i < 2; i++) {
		misc_deregister(&dl[i].misc);
		mutex_lock(&sd.lock);
		slic_dual_state(&dl[i], 0x0c);
		mutex_unlock(&sd.lock);
	}
	debugfs_remove_recursive(sd.dbg);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * J713 experiment: READ-ONLY, one-register-at-a-time AP-side read of the PMC block (default PA 0x3_82800000,
 * the window the DCP firmware hard-codes; macOS ApplePMC/AppleT8132PMC use enable regs at +0xac000.. and state
 * regs at +0xaa000..). The fault that kills the dcpext firmware is a read of +0xaa140.
 * RISK: if the register aborts on the fabric, this is an SError on the AP (kernel panic/reboot, as with any
 * unclocked MMIO on Apple SoCs). The test script logs + syncs BEFORE each read so the culprit is known.
 *   insmod j713_pmcpeek.ko [base=0x382800000]; echo 0xac028 > /sys/kernel/debug/j713_pmcpeek/peek;
 *   cat /sys/kernel/debug/j713_pmcpeek/peek      -> "off value" of the last read
 * No write path exists in this module.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

static ulong base = 0x382800000UL;
module_param(base, ulong, 0444);
static ulong size = 0xb8000;
module_param(size, ulong, 0444);

static void __iomem *map;
static struct dentry *dir;
static u32 last_off = ~0u, last_val;

static ssize_t peek_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	char buf[32];
	unsigned long off;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (kstrtoul(strim(buf), 0, &off) || (off & 3) || off + 4 > size)
		return -EINVAL;
	pr_info("j713_pmcpeek: reading PA 0x%lx (base+0x%lx)\n", base + off, off);
	last_val = readl(map + off);
	last_off = off;
	pr_info("j713_pmcpeek: PA 0x%lx = 0x%08x\n", base + off, last_val);
	return len;
}

static int peek_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "0x%x 0x%08x\n", last_off, last_val);
	return 0;
}

static int peek_open(struct inode *i, struct file *f)
{
	return single_open(f, peek_show, NULL);
}

static const struct file_operations peek_fops = {
	.owner = THIS_MODULE, .open = peek_open, .read = seq_read, .llseek = seq_lseek,
	.release = single_release, .write = peek_write,
};

static int __init pmcpeek_init(void)
{
	map = ioremap(base, size);
	if (!map)
		return -ENOMEM;
	dir = debugfs_create_dir("j713_pmcpeek", NULL);
	debugfs_create_file("peek", 0600, dir, NULL, &peek_fops);
	return 0;
}

static void __exit pmcpeek_exit(void)
{
	debugfs_remove_recursive(dir);
	iounmap(map);
}

module_init(pmcpeek_init);
module_exit(pmcpeek_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 read-only PMC block peek");

// SPDX-License-Identifier: GPL-2.0
/*
 * J713 experiment: READ-ONLY dump of PMGR power-state (PS) registers for every non-virtual device of the
 * ADT pmgr node whose PS register lies inside a window the Linux apple-pmgr-pwrstate driver already maps
 * (0x3_80700000/0x14000, 0x3_88280000/0x4000, 0x3_88284000/0x4000). Only the addresses listed in
 * pmgr_table.h (generated from j713.adt) are read, one 32-bit word each; nothing is written.
 *   insmod j713_pmgrdump.ko; cat /sys/kernel/debug/j713_pmgrdump/dump
 * Output is generated on every read.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/bitfield.h>

#include "pmgr_table.h"

static const struct { u64 pa; size_t len; } wins[] = {
	{ 0x380700000ULL, 0x14000 },
	{ 0x388280000ULL, 0x4000 },
	{ 0x388284000ULL, 0x4000 },
};
static void __iomem *map[ARRAY_SIZE(wins)];
static struct dentry *dir;

static int dump_show(struct seq_file *m, void *unused)
{
	unsigned int i, w;

	seq_puts(m, "# id idx adtflags name pa value | target actual auto clkgated(was) pwrgated(was) parent_off dev_disable reset\n");
	for (i = 0; i < ARRAY_SIZE(devtab); i++) {
		void __iomem *p = NULL;
		u32 v;

		for (w = 0; w < ARRAY_SIZE(wins); w++)
			if (devtab[i].pa >= wins[w].pa && devtab[i].pa < wins[w].pa + wins[w].len)
				p = map[w] + (devtab[i].pa - wins[w].pa);
		if (!p)
			continue;
		v = readl(p);
		seq_printf(m, "0x%03x %3u 0x%02x %-16s 0x%010llx 0x%08x | t=%x a=%x auto=%u clkg=%u pwrg=%u poff=%u dis=%u rst=%u\n",
			   devtab[i].id, devtab[i].idx, devtab[i].flags, devtab[i].name,
			   devtab[i].pa, v, v & 0xf, (v >> 4) & 0xf, (v >> 28) & 1, (v >> 9) & 1,
			   (v >> 8) & 1, (v >> 11) & 1, (v >> 10) & 1, (v >> 31) & 1);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dump);

static int __init pmgrdump_init(void)
{
	unsigned int w;

	for (w = 0; w < ARRAY_SIZE(wins); w++) {
		map[w] = ioremap(wins[w].pa, wins[w].len);
		if (!map[w]) {
			while (w--)
				iounmap(map[w]);
			return -ENOMEM;
		}
	}
	dir = debugfs_create_dir("j713_pmgrdump", NULL);
	debugfs_create_file("dump", 0400, dir, NULL, &dump_fops);
	return 0;
}

static void __exit pmgrdump_exit(void)
{
	unsigned int w;

	debugfs_remove_recursive(dir);
	for (w = 0; w < ARRAY_SIZE(wins); w++)
		iounmap(map[w]);
}

module_init(pmgrdump_init);
module_exit(pmgrdump_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 read-only PMGR PS register dump");

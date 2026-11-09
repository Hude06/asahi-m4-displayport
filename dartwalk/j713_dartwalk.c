// SPDX-License-Identifier: GPL-2.0
/*
 * J713 experiment: READ-ONLY dump of a T8110-style DART (default dart-dcpext0 @ 0x2_86340000):
 * global regs, per-stream TCR/TTBR, and a coalesced page-table walk (iova -> pa runs + flags).
 * Never writes DART registers or tables. Page tables are read through Normal-NC/WC mappings
 * (iBoot tables; cacheable aliases are avoided where possible).
 *   insmod j713_dartwalk.ko [base=0x286340000]; cat /sys/kernel/debug/j713_dartwalk/dump
 * Output is generated on every read of 'dump'.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/bitfield.h>

static ulong base = 0x286340000UL;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "DART MMIO physical base (default dart-dcpext0)");
static ulong size = 0x20000;
module_param(size, ulong, 0444);

#define PARAMS3 0x08
#define PARAMS4 0x0c
#define ERRORR  0x100
#define ERR_LO  0x170
#define ERR_HI  0x174
#define PROTECT 0x200
#define TCR(s)  (0x1000 + 4 * (s))
#define TTBR(s) (0x1400 + 4 * (s))
#define TCR_4LEVEL BIT(3)
#define DPTE_VALID BIT(0)
#define PADDR_MASK GENMASK_ULL(37, 10)
#define PMC_LO 0x382000000ULL
#define PMC_HI 0x383000000ULL
#define GRAN_SHIFT 14
#define LVL_BITS 11
#define MAX_TTBRS 8
#define OUTSZ (512 * 1024)

static struct dentry *dir;

struct ctx {
	char *buf;
	size_t len;
	bool trunc;
	unsigned int nlevels;
	/* current coalesced run */
	bool have;
	u64 iova, pa, bytes, flags;
	unsigned long nruns, ntables, pmc_runs;
};

static void out(struct ctx *c, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (c->trunc)
		return;
	va_start(ap, fmt);
	n = vscnprintf(c->buf + c->len, OUTSZ - c->len, fmt, ap);
	va_end(ap);
	c->len += n;
	if (c->len >= OUTSZ - 256) {
		c->trunc = true;
		c->len += scnprintf(c->buf + c->len, OUTSZ - c->len, "... TRUNCATED\n");
	}
}

static void flush_run(struct ctx *c)
{
	bool pmc;

	if (!c->have)
		return;
	pmc = c->pa < PMC_HI && c->pa + c->bytes > PMC_LO;
	c->nruns++;
	c->pmc_runs += pmc;
	out(c, "  iova %#014llx..%#014llx -> pa %#012llx..%#012llx  %#llx bytes  flags %#llx (NO_CACHE=%d NO_W=%d NO_R=%d)%s\n",
	    c->iova, c->iova + c->bytes - 1, c->pa, c->pa + c->bytes - 1, c->bytes, c->flags,
	    !!(c->flags & BIT(1)), !!(c->flags & BIT(2)), !!(c->flags & BIT(3)),
	    pmc ? "   <== PMC-WINDOW 0x3_82000000-0x3_83000000" : "");
	c->have = false;
}

static void leaf(struct ctx *c, u64 iova, u64 pte)
{
	u64 pa = (pte & PADDR_MASK) << 4;
	u64 fl = pte & (GENMASK_ULL(63, 40) | GENMASK_ULL(3, 1));
	u64 pg = 1ULL << GRAN_SHIFT;

	if (c->have && iova == c->iova + c->bytes && pa == c->pa + c->bytes && fl == c->flags) {
		c->bytes += pg;
		return;
	}
	flush_run(c);
	c->have = true;
	c->iova = iova;
	c->pa = pa;
	c->flags = fl;
	c->bytes = pg;
}

static void *map_tbl(u64 pa)
{
	void *p = memremap(pa, 1UL << GRAN_SHIFT, MEMREMAP_WC);

	if (!p)
		p = memremap(pa, 1UL << GRAN_SHIFT, MEMREMAP_WB); /* read-only use */
	return p;
}

static void walk(struct ctx *c, u64 tbl_pa, unsigned int level, u64 iova_base)
{
	unsigned int shift = GRAN_SHIFT + LVL_BITS * (c->nlevels - 1 - level);
	u64 *t = map_tbl(tbl_pa);
	int i;

	if (!t) {
		out(c, "  !! cannot map table pa %#llx (level %u)\n", tbl_pa, level);
		return;
	}
	c->ntables++;
	for (i = 0; i < (1 << LVL_BITS) && !c->trunc; i++) {
		u64 pte = READ_ONCE(t[i]);
		u64 iova = iova_base | ((u64)i << shift);

		if (!(pte & DPTE_VALID))
			continue;
		if (level == c->nlevels - 1)
			leaf(c, iova, pte);
		else
			walk(c, (pte & PADDR_MASK) << 4, level + 1, iova);
	}
	memunmap(t);
}

static void raw_top(struct ctx *c, u64 tbl_pa)
{
	u64 *t = map_tbl(tbl_pa);
	int i, n = 0;

	if (!t) {
		out(c, "  !! cannot map top table pa %#llx\n", tbl_pa);
		return;
	}
	for (i = 0; i < (1 << LVL_BITS); i++) {
		if (t[i]) {
			if (n++ < 16)
				out(c, "  top[%4d] = %#018llx\n", i, t[i]);
		}
	}
	out(c, "  top table: %d non-zero entries\n", n);
	memunmap(t);
}

static int dump_show(struct seq_file *m, void *v)
{
	struct ctx *c = kzalloc(sizeof(*c), GFP_KERNEL);
	void __iomem *r;
	u32 p3, p4, tcr, ttbr, seen_n = 0;
	u64 seen[MAX_TTBRS];
	int sid, k;

	if (!c)
		return -ENOMEM;
	c->buf = kvzalloc(OUTSZ, GFP_KERNEL);
	if (!c->buf) {
		kfree(c);
		return -ENOMEM;
	}
	r = ioremap(base, size);
	if (!r) {
		seq_puts(m, "ioremap failed\n");
		goto done;
	}
	p3 = readl(r + PARAMS3);
	p4 = readl(r + PARAMS4);
	out(c, "DART @ %#lx  PARAMS3 %#x (VA width %u, PA width %u, ver %u.%u)  PARAMS4 %#x (clients %u, sids %u)\n",
	    base, p3, FIELD_GET(GENMASK(21, 16), p3), FIELD_GET(GENMASK(29, 24), p3),
	    FIELD_GET(GENMASK(15, 8), p3), FIELD_GET(GENMASK(7, 0), p3), p4,
	    FIELD_GET(GENMASK(24, 16), p4), FIELD_GET(GENMASK(8, 0), p4));
	out(c, "ERROR %#x  ERROR_ADDR %#x%08x  PROTECT %#x\n", readl(r + ERRORR), readl(r + ERR_HI),
	    readl(r + ERR_LO), readl(r + PROTECT));

	for (sid = 0; sid < 256; sid++) {
		bool dup = false;

		tcr = readl(r + TCR(sid));
		ttbr = readl(r + TTBR(sid));
		if (!tcr && !ttbr)
			continue;
		out(c, "sid %#x: TCR %#x%s%s%s%s  TTBR %#x%s\n", sid, tcr,
		    (tcr & BIT(0)) ? " TRANSLATE" : "", (tcr & BIT(1)) ? " BYPASS_DART" : "",
		    (tcr & BIT(2)) ? " BYPASS_DAPF" : "", (tcr & TCR_4LEVEL) ? " 4LEVEL" : "", ttbr,
		    (ttbr & 1) ? " VALID" : "");
		if (!(ttbr & 1) || (tcr & BIT(1)))
			continue;
		{
			u64 tpa = ((u64)(ttbr >> 2)) << GRAN_SHIFT;

			for (k = 0; k < seen_n; k++)
				if (seen[k] == tpa)
					dup = true;
			if (dup) {
				out(c, "  (same page tables as an earlier sid)\n");
				continue;
			}
			if (seen_n >= MAX_TTBRS)
				continue;
			seen[seen_n++] = tpa;
			c->nlevels = (tcr & TCR_4LEVEL) ? 3 : 2;
			c->ntables = c->nruns = c->pmc_runs = 0;
			out(c, "  walk: top table pa %#llx, %u table levels, 16K granule\n", tpa, c->nlevels);
			raw_top(c, tpa);
			walk(c, tpa, 0, 0);
			flush_run(c);
			out(c, "  sid %#x: %lu runs, %lu tables, %lu runs touch the PMC window\n", sid, c->nruns,
			    c->ntables, c->pmc_runs);
		}
	}
	iounmap(r);
done:
	seq_write(m, c->buf, c->len);
	kvfree(c->buf);
	kfree(c);
	return 0;
}

static int dump_open(struct inode *i, struct file *f)
{
	return single_open_size(f, dump_show, NULL, OUTSZ);
}

static const struct file_operations fops = {
	.owner = THIS_MODULE, .open = dump_open, .read = seq_read, .llseek = seq_lseek, .release = single_release,
};

static int __init init(void)
{
	dir = debugfs_create_dir("j713_dartwalk", NULL);
	debugfs_create_file("dump", 0400, dir, NULL, &fops);
	pr_info("j713_dartwalk: ready (base %#lx), read-only; cat /sys/kernel/debug/j713_dartwalk/dump\n", base);
	return 0;
}

static void __exit fini(void)
{
	debugfs_remove_recursive(dir);
}

module_init(init);
module_exit(fini);
MODULE_DESCRIPTION("J713 read-only DART dump");
MODULE_LICENSE("GPL");

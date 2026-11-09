// SPDX-License-Identifier: GPL-2.0
/*
 * J713 experiment: put the PMGR device "PMC" (ADT pmgr device id 0x14, PS register PA 0x3_80700138, Linux DT
 * power-controller@138 label "pmc") into a given power state. Linux's genpd powers it OFF at boot because the
 * domain has no consumer; the DCP firmware's PMC library (RTKPMGR_pmc_boost_disp_perf_state) reads the PMC block at
 * PA 0x3_82800000 and takes an SError if the device is off. Mimics apple-pmgr-pwrstate: update AUTO_ENABLE|TARGET,
 * poll ACTUAL. genpd still thinks the domain is off; fine for an experiment (nobody requests it again).
 *   insmod j713_pmcon.ko [target=0xf]      (target=0 powers it back down)
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/bitfield.h>

#define PS_PA        0x380700138ULL
#define PS_TARGET    GENMASK(3, 0)
#define PS_ACTUAL    GENMASK(7, 4)
#define PS_AUTO_EN   BIT(28)

static uint target = 0xf;
module_param(target, uint, 0444);

static int __init pmcon_init(void)
{
	void __iomem *p = ioremap(PS_PA, 4);
	u32 v, v2;
	int ret;

	if (!p)
		return -ENOMEM;
	v = readl(p);
	pr_info("j713_pmcon: PMC PS before 0x%08x (target %lu actual %lu)\n", v, FIELD_GET(PS_TARGET, v), FIELD_GET(PS_ACTUAL, v));
	v2 = (v & ~(PS_TARGET | PS_AUTO_EN)) | FIELD_PREP(PS_TARGET, target);
	writel(v2, p);
	ret = readl_poll_timeout(p, v2, FIELD_GET(PS_ACTUAL, v2) == target, 10, 100000);
	pr_info("j713_pmcon: PMC PS after 0x%08x (target %lu actual %lu) -> %s\n", v2, FIELD_GET(PS_TARGET, v2), FIELD_GET(PS_ACTUAL, v2),
		ret ? "TIMEOUT" : "ok");
	iounmap(p);
	return ret ? -ETIMEDOUT : 0;
}

static void __exit pmcon_exit(void) { }

module_init(pmcon_init);
module_exit(pmcon_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 set PMGR device PMC power state");

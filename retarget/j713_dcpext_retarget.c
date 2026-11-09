// SPDX-License-Identifier: GPL-2.0
/*
 * J713 experiment: point dcpext0's DP PHY and crossbar at another ATC port
 * by rewriting its live devicetree properties (CONFIG_OF_DYNAMIC), before
 * j713_dcpext_probe2 runs "connect". Load-and-stay; properties are not restored.
 *   insmod j713_dcpext_retarget.ko atc=0
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

static int atc;
module_param(atc, int, 0444);
MODULE_PARM_DESC(atc, "ATC port for dcpext0 DP (0 = phy@403000000, 1 = phy@40b000000)");

static const char *const phy_path[] = { "/soc/phy@403000000", "/soc/phy@40b000000" };
static const char *const mux_path[] = { "/soc/mux@40304c000", "/soc/mux@40b04c000" };

static struct property *mkprop(const char *name, u32 a, u32 b)
{
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);
	__be32 *v = kmalloc(8, GFP_KERNEL);

	if (!p || !v) {
		kfree(p);
		kfree(v);
		return NULL;
	}
	v[0] = cpu_to_be32(a);
	v[1] = cpu_to_be32(b);
	p->name = kstrdup(name, GFP_KERNEL);
	p->value = v;
	p->length = 8;
	return p;
}

static u32 free_phandle(void)
{
	u32 n;

	for (n = 0x1000; n < 0x2000; n++) {
		struct device_node *np = of_find_node_by_phandle(n);

		if (!np)
			return n;
		of_node_put(np);
	}
	return 0;
}

static struct of_changeset cs; /* applied for the rest of this boot */

static int __init retarget_init(void)
{
	struct device_node *dcp, *phy, *mux;
	struct property *pphys, *pmux;
	int ret = -ENODEV;

	if (atc < 0 || atc > 1)
		return -EINVAL;
	dcp = of_find_node_by_path("/soc/dcpext@286e00000");
	phy = of_find_node_by_path(phy_path[atc]);
	mux = of_find_node_by_path(mux_path[atc]);
	if (!dcp || !phy || !mux || !phy->phandle)
		goto out;

	if (!mux->phandle) {
		struct property *ph = kzalloc(sizeof(*ph), GFP_KERNEL);
		__be32 *v = kmalloc(4, GFP_KERNEL);
		u32 n = free_phandle();

		if (!ph || !v || !n) {
			ret = -ENOMEM;
			goto out;
		}
		*v = cpu_to_be32(n);
		ph->name = kstrdup("phandle", GFP_KERNEL);
		ph->value = v;
		ph->length = 4;
		of_add_property(mux, ph);
		mux->phandle = n;
		pr_info("j713_dcpext_retarget: %pOF gets phandle 0x%x\n", mux, n);
	}

	pphys = mkprop("phys", phy->phandle, 6 /* PHY_TYPE_DP */);
	pmux = mkprop("mux-controls", mux->phandle, 0);
	if (!pphys || !pmux) {
		ret = -ENOMEM;
		goto out;
	}
	of_changeset_init(&cs);
	ret = of_changeset_update_property(&cs, dcp, pphys);
	if (!ret)
		ret = of_changeset_update_property(&cs, dcp, pmux);
	if (!ret)
		ret = of_changeset_apply(&cs);
	pr_info("j713_dcpext_retarget: %pOF -> phys=<0x%x 6> (%pOF) mux-controls=<0x%x 0> (%pOF): %d\n",
		dcp, phy->phandle, phy, mux->phandle, mux, ret);
out:
	of_node_put(dcp);
	of_node_put(phy);
	of_node_put(mux);
	return ret;
}
module_init(retarget_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 dcpext0 DP port retarget (experiment)");

// SPDX-License-Identifier: GPL-2.0
/*
 * J713 external display, scanout buffer: allocate one write-combined buffer and make it visible to the display pipe.
 *
 *  1. Applies a runtime DT overlay (dispfb.dts) adding dart-dispext0 (ADT /arm-io/dart-dispext0, locked by iBoot with a valid empty
 *     L1 for sid 0) plus a client node bound to sid 0. Linux apple-dart (locked-DART path) binds it, exactly like dart-dcpext0.
 *  2. The client driver (this module) allocates the buffer with dma_alloc_attrs(WRITE_COMBINE) on the dcpext device = IOVA with bit 40
 *     in dart-dcpext0 and a WC kernel mapping (no cache maintenance needed), then mirrors the SAME pages at the SAME IOVA into
 *     dart-dispext0 with iommu_map_sgtable (m1n1 display_map_fb does the same on both DARTs).
 *  3. j713_dispfb_get() hands the buffer to j713_extdrm.ko. debugfs j713_dispfb/info + pattern remain for debugging.
 *
 * v2: buffer is width x height = 3840 x 2160 by default (the largest mode the DRM driver offers); v1 was a fixed 2560x1440
 * cacheable buffer with explicit dma_sync.
 * NO exit function on purpose: apple-dart zeroes iBoot's L1 on detach, so this module must never be unloaded.
 * Touches no display PMGR register. Run on a fresh boot BEFORE the dcpext connect (a failure here then costs no connect).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/iommu.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/scatterlist.h>
#include <linux/debugfs.h>
#include <linux/completion.h>
#include <linux/seq_file.h>
#include <linux/pfn.h>
#include <linux/mm.h>

#include "dispfb_dtbo.h"
#include "j713_ext.h"

static uint width = 3840;
module_param(width, uint, 0444);
MODULE_PARM_DESC(width, "scanout buffer width in pixels (maximum mode width)");
static uint height = 2160;
module_param(height, uint, 0444);
MODULE_PARM_DESC(height, "scanout buffer height in pixels (maximum mode height)");

static struct {
	struct device *dev, *dcp;
	u32 *va;
	dma_addr_t iova;
	u32 stride;
	size_t size;
	bool ready;
	int ovcs_id;
	int pattern;
	struct dentry *dir;
} F;

static DECLARE_COMPLETION(done);
static int probe_result = -ETIMEDOUT;

static inline u32 px(u32 r, u32 g, u32 b)	/* m1n1 rgb2pixel_30: 8-bit channels in the top of each 10-bit field */
{
	return (b << 2) | (g << 12) | (r << 22);
}

/* debug test pattern, drawn in the top-left 2560x1440 (or the whole buffer if smaller) with the buffer's own stride */
static void draw(int pattern)
{
	static const u8 bars[8][3] = {
		{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
	};
	u32 x, y, w = min(width, 2560u), h = min(height, 1440u), stride_px = F.stride / 4;

	for (y = 0; y < h; y++) {
		u32 *row = F.va + (size_t)y * stride_px;

		for (x = 0; x < w; x++) {
			u32 r, g, b, v;

			switch (pattern) {
			case 1: r = g = b = 255; break;			/* solid white */
			case 2: r = 255; g = b = 0; break;		/* solid red */
			case 3: r = g = 0; b = 255; break;		/* solid blue */
			case 4: r = g = b = 0; break;			/* black */
			default:
				r = g = b = 24;
				if (y < h * 6 / 10) {			/* colour bars */
					const u8 *c = bars[x * 8 / w];

					r = c[0]; g = c[1]; b = c[2];
				} else if (x < w / 2) {			/* grey ramp */
					r = g = b = x * 255 / (w / 2 - 1);
				} else {				/* R / G / B ramps */
					v = (x - w / 2) * 255 / (w / 2 - 1);
					if (y < h * 6 / 10 + (h * 4 / 10) / 3) { r = v; g = b = 0; }
					else if (y < h * 6 / 10 + 2 * (h * 4 / 10) / 3) { g = v; r = b = 0; }
					else { b = v; r = g = 0; }
				}
				if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4)	/* white frame */
					r = g = b = 255;
				if ((x * h / w == y || (w - 1 - x) * h / w == y) )	/* diagonals */
					r = g = b = 255 - (r + g + b) / 3 > 127 ? 255 : 0;
				if (y < 12 && x % 100 == 0)		/* ticks every 100 px */
					r = g = b = 0;
				break;
			}
			row[x] = px(r, g, b);
		}
	}
	wmb();	/* WC stores: drain the write buffers before the display DMA reads them */
	F.pattern = pattern;
}

static int build_fb(struct platform_device *pdev)
{
	struct device_node *np;
	struct platform_device *dcpdev;
	struct iommu_domain *dom_disp, *dom_dcp;
	struct sg_table sgt;
	ssize_t mapped;
	unsigned int i;
	int ret = 0;

	F.dev = &pdev->dev;
	dom_disp = iommu_get_domain_for_dev(&pdev->dev);
	if (!dom_disp) {
		dev_err(F.dev, "no IOMMU domain: dart-dispext0 not bound\n");
		return -ENODEV;
	}
	dev_info(F.dev, "dart-dispext0 domain type %#x, aperture %#llx..%#llx\n", dom_disp->type,
		 (u64)dom_disp->geometry.aperture_start, (u64)dom_disp->geometry.aperture_end);

	np = of_find_node_by_path("/soc/dcpext@286e00000");
	dcpdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!dcpdev) {
		dev_err(F.dev, "dcpext platform device not found\n");
		return -ENODEV;
	}
	F.dcp = &dcpdev->dev;
	dom_dcp = iommu_get_domain_for_dev(F.dcp);
	if (!dom_dcp) {
		dev_err(F.dev, "dcpext has no IOMMU domain\n");
		return -ENODEV;
	}

	F.stride = width * 4;
	F.size = PAGE_ALIGN((size_t)F.stride * height);
	dma_set_max_seg_size(F.dcp, UINT_MAX);
	F.va = dma_alloc_attrs(F.dcp, F.size, &F.iova, GFP_KERNEL, DMA_ATTR_WRITE_COMBINE);
	if (!F.va) {
		dev_err(F.dev, "dma_alloc_attrs(%zu KiB, WC) on dcpext failed\n", F.size / 1024);
		return -ENOMEM;
	}
	if (!(F.iova & BIT_ULL(40))) {
		/* went through dma-direct, not dart-dcpext0: the dcpext device has no driver bound (DMA ops not set up) */
		dev_err(F.dev, "dcpext DMA address %#llx has no bit 40: not behind its IOMMU. Load j713_dcpext_probe2.ko first\n",
			(u64)F.iova);
		goto err_free;
	}
	dev_info(F.dev, "dart-dcpext0: buffer %zu KiB (%ux%u) allocated WC at IOVA %#llx (dcpext dma-coherent: %d)\n",
		 F.size / 1024, width, height, (u64)F.iova, dev_is_dma_coherent(F.dcp));
	if (dev_is_dma_coherent(F.dcp))
		dev_warn(F.dev, "dcpext is DMA-coherent: the buffer mapping is cacheable, not write-combined; flush needed?\n");

	/* mirror the same pages at the same IOVA into dart-dispext0 */
	ret = dma_get_sgtable_attrs(F.dcp, &sgt, F.va, F.iova, F.size, DMA_ATTR_WRITE_COMBINE);
	if (ret) {
		dev_err(F.dev, "dma_get_sgtable_attrs: %d\n", ret);
		goto err_free;
	}
	mapped = iommu_map_sgtable(dom_disp, F.iova, &sgt, IOMMU_READ | IOMMU_WRITE);
	sg_free_table(&sgt);
	if (mapped != (ssize_t)F.size) {
		dev_err(F.dev, "iommu_map_sgtable on dart-dispext0: %zd (want %zu)\n", mapped, F.size);
		ret = mapped < 0 ? mapped : -EIO;
		/* a partial mapping stays: unmapping a locked DART's entries is not worth the risk before the connect */
		goto err_free_keep;
	}
	/* verify both DARTs translate first, middle and last page to the same (valid RAM) physical page */
	for (i = 0; i < 3; i++) {
		size_t pg = i == 0 ? 0 : i == 1 ? F.size / PAGE_SIZE / 2 : F.size / PAGE_SIZE - 1;
		dma_addr_t io = F.iova + pg * PAGE_SIZE;
		phys_addr_t got_disp = iommu_iova_to_phys(dom_disp, io), got_dcp = iommu_iova_to_phys(dom_dcp, io);

		dev_info(F.dev, "verify page %zu: iova %#llx -> dispext0 %pa, dcpext0 %pa\n", pg, (u64)io, &got_disp, &got_dcp);
		if (!got_dcp || got_disp != got_dcp || !pfn_valid(PHYS_PFN(got_dcp))) {
			ret = -EIO;
			goto err_free_keep;
		}
	}
	memset(F.va, 0, F.size);
	wmb();
	F.ready = true;
	return 0;

err_free:
	dma_free_attrs(F.dcp, F.size, F.va, F.iova, DMA_ATTR_WRITE_COMBINE);
	F.va = NULL;
	return ret ?: -ENODEV;
err_free_keep:
	/* partially mirrored into dart-dispext0: keep the pages (never freed), mark unusable */
	F.va = NULL;
	return ret;
}

int j713_dispfb_get(struct j713_dispfb_buf *b)
{
	if (!READ_ONCE(F.ready))
		return -ENODEV;
	b->va = F.va;
	b->iova = F.iova;
	b->size = F.size;
	b->max_w = width;
	b->max_h = height;
	return 0;
}
EXPORT_SYMBOL_GPL(j713_dispfb_get);

static int info_show(struct seq_file *m, void *v)
{
	seq_printf(m, "iova=%#llx width=%u height=%u stride=%u size=%zu pattern=%d\n", (u64)F.iova, width, height, F.stride,
		   F.size, F.pattern);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(info);

static ssize_t pattern_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	int p, ret = kstrtoint_from_user(ubuf, len, 0, &p);

	if (ret)
		return ret;
	if (!F.ready)
		return -ENODEV;
	draw(p);
	return len;
}
static const struct file_operations pattern_fops = { .owner = THIS_MODULE, .open = simple_open, .write = pattern_write,
						      .llseek = noop_llseek };

static int dispfb_probe(struct platform_device *pdev)
{
	int ret;

	if (F.ready) {	/* already built (re-probe) */
		complete(&done);
		return 0;
	}
	ret = build_fb(pdev);
	probe_result = ret;
	if (ret) {
		dev_err(&pdev->dev, "framebuffer setup failed: %d\n", ret);
		complete(&done);
		return ret;
	}
	F.dir = debugfs_create_dir("j713_dispfb", NULL);
	debugfs_create_file("info", 0400, F.dir, NULL, &info_fops);
	debugfs_create_file("pattern", 0200, F.dir, NULL, &pattern_fops);
	dev_info(&pdev->dev, "ready: iova=%#llx %ux%u stride %u (x2r10g10b10, write-combined)\n", (u64)F.iova, width, height,
		 F.stride);
	complete(&done);
	return 0;
}

static const struct of_device_id dispfb_of[] = { { .compatible = "omarchy,j713-dispfb" }, {} };
static struct platform_driver dispfb_drv = {
	.probe = dispfb_probe,
	.driver = { .name = "j713-dispfb", .of_match_table = dispfb_of, .suppress_bind_attrs = true },
};

static int __init dispfb_init(void)
{
	struct device_node *n;
	int ret;

	ret = platform_driver_register(&dispfb_drv);
	if (ret)
		return ret;
	n = of_find_node_by_path("/soc/iommu@286300000");
	if (n) {
		of_node_put(n);
		pr_info("j713_dispfb: dart-dispext0 node already present, not re-applying the overlay\n");
	} else {
		ret = of_overlay_fdt_apply(dispfb_dtbo, dispfb_dtbo_len, &F.ovcs_id, NULL);
		if (ret) {
			pr_err("j713_dispfb: overlay apply failed: %d\n", ret);
			platform_driver_unregister(&dispfb_drv);	/* nothing was created or bound: safe to fail */
			return ret;
		}
		pr_info("j713_dispfb: overlay applied (id %d)\n", F.ovcs_id);
	}
	/*
	 * From here on the DART and client exist: never fail init (a freed module with a registered driver, or an
	 * unloaded module after a locked-DART attach, is worse than a loaded module with an error). The result is in
	 * the log and in debugfs j713_dispfb/info (present only on success).
	 */
	if (!wait_for_completion_timeout(&done, 10 * HZ))
		pr_err("j713_dispfb: client did not probe within 10 s (DART bind problem?); module stays loaded, see dmesg\n");
	else if (probe_result)
		pr_err("j713_dispfb: setup failed (%d); module stays loaded, see dmesg\n", probe_result);
	return 0;
}
module_init(dispfb_init);
/* no module_exit: never unload (apple-dart zeroes iBoot's locked L1 on detach) */

MODULE_DESCRIPTION("J713 external display scanout buffer (dart-dispext0 overlay + dart-dcpext0)");
MODULE_LICENSE("GPL");

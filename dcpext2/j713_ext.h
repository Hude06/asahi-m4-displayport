/* SPDX-License-Identifier: GPL-2.0 */
/*
 * In-kernel interfaces between the J713 external-display modules:
 *   j713_dcpext_probe2.ko  (exports j713_ext_*: the iBoot display service of dcpext0)
 *   j713_dispfb.ko         (exports j713_dispfb_get: scanout buffer mapped in dart-dcpext0 + dart-dispext0)
 *   j713_extdrm.ko         (the DRM/KMS driver that consumes both)
 */
#ifndef J713_EXT_H
#define J713_EXT_H

#include <linux/types.h>

struct device;
struct j713_ext;

struct j713_ext_timing {
	u32 idx;
	u32 width, height;
	u32 fps;	/* 16.16 fixed point */
};

/* NULL unless the dcpext0 firmware is running, the link is up, a sink is present and the timing list was read. */
struct j713_ext *j713_ext_get(void);
struct device *j713_ext_dev(struct j713_ext *e);
/* live GET_HPD: out = { op, len, hpd, n_timings, n_colors } */
int j713_ext_hpd(struct j713_ext *e, u32 out[5]);
u32 j713_ext_timing_count(struct j713_ext *e);
/* 0 on a valid entry, -ENODATA for an invalid one, -ENOENT past the end */
int j713_ext_timing(struct j713_ext *e, u32 idx, struct j713_ext_timing *t);
int j713_ext_power(struct j713_ext *e, bool on);
/* SET_MODE with timing index ti and the best colour mode (RGB 8 bpc) */
int j713_ext_set_mode(struct j713_ext *e, u32 ti);
/* SET_SURFACE: one layer at iova (valid in dart-dcpext0 AND dart-dispext0). fmt 9 = w30r (x2r10g10b10), 1 = BGRA */
int j713_ext_set_surface(struct j713_ext *e, u64 iova, u32 w, u32 h, u32 stride, u32 fmt);

struct j713_dispfb_buf {
	void *va;		/* write-combined kernel mapping */
	u64 iova;		/* identical in dart-dcpext0 and dart-dispext0 */
	size_t size;
	u32 max_w, max_h;	/* the buffer holds max_w x max_h x 4 bytes */
};
/* -ENODEV until j713_dispfb.ko finished its setup */
int j713_dispfb_get(struct j713_dispfb_buf *b);

#endif

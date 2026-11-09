// SPDX-License-Identifier: GPL-2.0
/*
 * J713 (Apple M4 MacBook Air) external display: DRM/KMS driver for the USB-C DisplayPort output driven by the dcpext0
 * coprocessor's iBoot display service.
 *
 * Structure (modelled on drivers/gpu/drm/tiny/cirrus-qemu.c / simpledrm):
 *   - one primary plane (XRGB8888 / XRGB2101010) with a GEM-shmem shadow buffer and damage tracking,
 *   - the plane update converts the damaged rectangles into ONE persistent scanout buffer owned by j713_dispfb.ko (write-combined,
 *     mapped at the same IOVA in dart-dcpext0 and dart-dispext0) in the display's native x2r10g10b10 (iBoot FMT_w30r),
 *   - CRTC enable sends iBoot SET_POWER / SET_MODE / SET_SURFACE through the j713_ext_* API of j713_dcpext_probe2.ko,
 *   - no hardware vblank interrupt is known: vblank is a timer (drm_crtc_vblank_helper_*_vblank_timer),
 *   - modes come from the monitor's iBoot timing list (the firmware read the EDID and keeps the list); timings are synthesised
 *     with CVT reduced blanking, only for presentation: the firmware picks the real timing by index,
 *   - only modes whose pixel clock fits HBR2 x 2 lanes (8.64 Gbit/s, 24 bpp => ~360 MHz) are offered.
 * The link itself (crossbar, ATC PHY, DPTX training) is brought up by extdisplay-up.sh before this module is loaded.
 * Unlike the probe and dispfb modules this one may be unloaded and reloaded.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/mutex.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include "j713_ext.h"

#define DRIVER_NAME	"j713-extdrm"
#define FMT_BGRA	1
#define FMT_W30R	9

static uint scan_fmt = FMT_W30R;
module_param(scan_fmt, uint, 0444);
MODULE_PARM_DESC(scan_fmt, "iBoot surface format: 9 = w30r x2r10g10b10 (proven, default), 1 = BGRA 8888 (untested, no conversion)");
static uint max_clock = 340000;
module_param(max_clock, uint, 0444);
MODULE_PARM_DESC(max_clock, "highest CVT-RB pixel clock (kHz) offered; HBR2 x2 carries ~360000 at 24 bpp");
static bool fbdev = true;
module_param(fbdev, bool, 0444);
MODULE_PARM_DESC(fbdev, "register the fbdev emulation client (initial modeset + /dev/fbN)");
static char *drm_name = DRIVER_NAME;
module_param(drm_name, charp, 0444);
MODULE_PARM_DESC(drm_name, "name reported to userspace by DRM_IOCTL_VERSION (default j713-extdrm). The patched Aquamarine only uses its "
		 "display-only GBM/software fallback for a device named exactly \"simpledrm\" with no render node, so Hyprland needs drm_name=simpledrm");
static bool power_off;
module_param(power_off, bool, 0644);
MODULE_PARM_DESC(power_off, "send iBoot SET_POWER 0 when the CRTC is disabled (default off: only blank the picture)");

struct extdrm {
	struct drm_device dev;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct j713_ext *ext;
	struct j713_dispfb_buf fb;
	struct mutex hw_lock;
	int hw_timing;		/* iBoot timing index the firmware currently runs, -1 = none */
	bool powered;
	enum drm_connector_status last_status;
};

static inline struct extdrm *to_extdrm(struct drm_device *dev)
{
	return container_of(dev, struct extdrm, dev);
}

/* pitch of the scanout buffer for a given mode width; 256-byte aligned like the 2560-wide pitch that is known to work */
static inline unsigned int scan_pitch(unsigned int width)
{
	return ALIGN(width * 4, 256);
}

static inline bool scan_fits(struct extdrm *e, unsigned int w, unsigned int h)
{
	return w && h && w <= e->fb.max_w && h <= e->fb.max_h && (u64)scan_pitch(w) * h <= e->fb.size;
}

/* ------------------------------------------------------------------ */
/* primary plane							      */

static const u32 plane_formats_w30r[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_XRGB2101010 };
static const u32 plane_formats_bgra[] = { DRM_FORMAT_XRGB8888 };
static const u64 plane_modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };

static int extdrm_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct extdrm *e = to_extdrm(plane->dev);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_plane_state->fb;
	struct drm_crtc *new_crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	int ret;

	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret)
		return ret;
	if (!new_plane_state->visible)
		return 0;
	if (!scan_fits(e, fb->width, fb->height))
		return -EINVAL;
	return 0;
}

static void extdrm_blit(struct extdrm *e, struct drm_shadow_plane_state *sps, struct drm_framebuffer *fb,
			const struct drm_rect *clip)
{
	unsigned int pitch = scan_pitch(fb->width);
	struct iosys_map dst = IOSYS_MAP_INIT_VADDR(e->fb.va);

	iosys_map_incr(&dst, drm_fb_clip_offset(pitch, fb->format, clip));
	if (scan_fmt == FMT_W30R && fb->format->format == DRM_FORMAT_XRGB8888)
		drm_fb_xrgb8888_to_xrgb2101010(&dst, &pitch, sps->data, fb, clip, &sps->fmtcnv_state);
	else
		drm_fb_memcpy(&dst, &pitch, sps->data, fb, clip);
}

static void extdrm_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct extdrm *e = to_extdrm(plane->dev);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *sps = to_drm_shadow_plane_state(plane_state);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	int idx;

	if (!fb)
		return;
	if (!drm_dev_enter(&e->dev, &idx))
		return;

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage)
		extdrm_blit(e, sps, fb, &damage);
	wmb();	/* drain the write-combining buffers before the display reads the memory */

	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs extdrm_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = extdrm_plane_atomic_check,
	.atomic_update = extdrm_plane_atomic_update,
};

static const struct drm_plane_funcs extdrm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/* ------------------------------------------------------------------ */
/* CRTC									      */

/*
 * iBoot timing index for a DRM mode: same size, refresh closest to the mode's integer rate. Ties keep the lowest index, so
 * 2560x1440@60 selects iBoot timing 12 (exactly 60.000 Hz, the one proven in test-fb) rather than 46 (59.95 Hz).
 */
static int extdrm_find_timing(struct extdrm *e, const struct drm_display_mode *m)
{
	u32 i, n = j713_ext_timing_count(e->ext);
	u64 want = (u64)drm_mode_vrefresh(m) << 16;
	int best = -1;
	u64 best_d = U64_MAX;

	for (i = 0; i < n; i++) {
		struct j713_ext_timing t;
		u64 d;

		if (j713_ext_timing(e->ext, i, &t) || t.width != m->hdisplay || t.height != m->vdisplay)
			continue;
		d = t.fps > want ? t.fps - want : want - t.fps;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

static int extdrm_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct extdrm *e = to_extdrm(crtc->dev);

	if (!crtc_state->enable)
		return 0;
	if (extdrm_find_timing(e, &crtc_state->mode) < 0)
		return -EINVAL;	/* not a mode the monitor/firmware offered */
	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

static void extdrm_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct extdrm *e = to_extdrm(crtc->dev);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *m = &crtc_state->mode;
	int idx, ret, ti;

	if (!drm_dev_enter(&e->dev, &idx))
		return;
	mutex_lock(&e->hw_lock);
	ti = extdrm_find_timing(e, m);
	if (ti < 0) {
		drm_err(&e->dev, "no iBoot timing for %dx%d\n", m->hdisplay, m->vdisplay);
		goto out;
	}
	if (!e->powered) {
		ret = j713_ext_power(e->ext, true);
		if (ret) {
			drm_err(&e->dev, "SET_POWER 1 failed: %d\n", ret);
			goto out;
		}
		e->powered = true;
	}
	if (e->hw_timing != ti) {
		drm_info(&e->dev, "SET_MODE %dx%d (iBoot timing %d)\n", m->hdisplay, m->vdisplay, ti);
		ret = j713_ext_set_mode(e->ext, ti);
		if (ret) {
			drm_err(&e->dev, "SET_MODE failed: %d\n", ret);
			goto out;
		}
		e->hw_timing = ti;
	}
	ret = j713_ext_set_surface(e->ext, e->fb.iova, m->hdisplay, m->vdisplay, scan_pitch(m->hdisplay), scan_fmt);
	if (ret)
		drm_err(&e->dev, "SET_SURFACE failed: %d\n", ret);
out:
	mutex_unlock(&e->hw_lock);
	drm_dev_exit(idx);
	drm_crtc_vblank_on(crtc);
}

static void extdrm_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct extdrm *e = to_extdrm(crtc->dev);
	int idx;

	if (drm_dev_enter(&e->dev, &idx)) {
		mutex_lock(&e->hw_lock);
		memset(e->fb.va, 0, e->fb.size);	/* black; the link and the last mode stay up unless power_off */
		wmb();
		if (power_off && e->powered) {
			if (!j713_ext_power(e->ext, false))
				e->powered = false;
			e->hw_timing = -1;
		}
		mutex_unlock(&e->hw_lock);
		drm_dev_exit(idx);
	}
	drm_crtc_vblank_atomic_disable(crtc, state);
}

static const struct drm_crtc_helper_funcs extdrm_crtc_helper_funcs = {
	.atomic_check = extdrm_crtc_atomic_check,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = extdrm_crtc_atomic_enable,
	.atomic_disable = extdrm_crtc_atomic_disable,
};

static const struct drm_crtc_funcs extdrm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

static const struct drm_encoder_funcs extdrm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/* ------------------------------------------------------------------ */
/* connector								      */

static int extdrm_connector_get_modes(struct drm_connector *connector)
{
	struct extdrm *e = to_extdrm(connector->dev);
	struct drm_display_mode *best = NULL, *best169 = NULL;
	u64 seen[64];
	unsigned int nseen = 0;
	int count = 0;
	u32 i, n = j713_ext_timing_count(e->ext);

	for (i = 0; i < n; i++) {
		struct j713_ext_timing t;
		struct drm_display_mode *m;
		unsigned int hz, k;
		u64 key;

		if (j713_ext_timing(e->ext, i, &t) || !scan_fits(e, t.width, t.height))
			continue;
		hz = (t.fps + 0x8000) >> 16;
		if (hz < 24 || hz > 144)
			continue;
		key = ((u64)t.width << 40) | ((u64)t.height << 16) | hz;
		for (k = 0; k < nseen; k++)
			if (seen[k] == key)
				break;
		if (k < nseen || nseen == ARRAY_SIZE(seen))
			continue;
		m = drm_cvt_mode(connector->dev, t.width, t.height, hz, true, false, false);
		if (!m)
			continue;
		/* CVT rounds the width to a multiple of 8; such a mode would not map back to its iBoot timing */
		if (m->clock > max_clock || m->hdisplay != t.width || m->vdisplay != t.height) {
			drm_mode_destroy(connector->dev, m);
			continue;
		}
		seen[nseen++] = key;
		m->type = DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(connector, m);
		count++;
		if (hz >= 59) {
			if (!best || m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay)
				best = m;
			if (m->hdisplay * 9 == m->vdisplay * 16 &&
			    (!best169 || m->hdisplay * m->vdisplay > best169->hdisplay * best169->vdisplay))
				best169 = m;
		}
	}
	/* no EDID here: assume a 16:9 panel (the monitor would letterbox 16:10 modes such as 2560x1600) */
	best = best169 ?: best;
	if (best)
		best->type |= DRM_MODE_TYPE_PREFERRED;
	return count;
}

static int extdrm_connector_detect_ctx(struct drm_connector *connector, struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct extdrm *e = to_extdrm(connector->dev);
	u32 hpd[5];
	int ret = j713_ext_hpd(e->ext, hpd);

	if (ret) {
		drm_dbg_kms(&e->dev, "GET_HPD failed (%d), keeping %d\n", ret, e->last_status);
		return e->last_status;
	}
	e->last_status = hpd[2] ? connector_status_connected : connector_status_disconnected;
	return e->last_status;
}

static enum drm_mode_status extdrm_connector_mode_valid(struct drm_connector *connector, const struct drm_display_mode *mode)
{
	struct extdrm *e = to_extdrm(connector->dev);

	if (!scan_fits(e, mode->hdisplay, mode->vdisplay))
		return MODE_MEM;
	if (mode->clock > max_clock)
		return MODE_CLOCK_HIGH;
	return MODE_OK;
}

static const struct drm_connector_helper_funcs extdrm_connector_helper_funcs = {
	.get_modes = extdrm_connector_get_modes,
	.detect_ctx = extdrm_connector_detect_ctx,
	.mode_valid = extdrm_connector_mode_valid,
};

static const struct drm_connector_funcs extdrm_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------------ */
/* pipeline + driver							      */

static int extdrm_pipe_init(struct extdrm *e)
{
	struct drm_device *dev = &e->dev;
	int ret;

	ret = drm_universal_plane_init(dev, &e->plane, 0, &extdrm_plane_funcs,
				       scan_fmt == FMT_W30R ? plane_formats_w30r : plane_formats_bgra,
				       scan_fmt == FMT_W30R ? ARRAY_SIZE(plane_formats_w30r) : ARRAY_SIZE(plane_formats_bgra),
				       plane_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&e->plane, &extdrm_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&e->plane);

	ret = drm_crtc_init_with_planes(dev, &e->crtc, &e->plane, NULL, &extdrm_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&e->crtc, &extdrm_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &e->encoder, &extdrm_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	e->encoder.possible_crtcs = drm_crtc_mask(&e->crtc);

	ret = drm_connector_init(dev, &e->connector, &extdrm_connector_funcs, DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;
	drm_connector_helper_add(&e->connector, &extdrm_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&e->connector, &e->encoder);
	if (ret)
		return ret;

	return drm_vblank_init(dev, 1);
}

static enum drm_mode_status extdrm_mode_config_mode_valid(struct drm_device *dev, const struct drm_display_mode *mode)
{
	struct extdrm *e = to_extdrm(dev);

	return scan_fits(e, mode->hdisplay, mode->vdisplay) ? MODE_OK : MODE_MEM;
}

static const struct drm_mode_config_funcs extdrm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.mode_valid = extdrm_mode_config_mode_valid,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(extdrm_fops);

static struct drm_driver extdrm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = DRIVER_NAME,
	.desc = "Apple M4 (J713) USB-C DisplayPort via dcpext0",
	.major = 1,
	.minor = 0,
	.fops = &extdrm_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static int extdrm_probe(struct platform_device *pdev)
{
	struct j713_ext *ext = *(struct j713_ext **)dev_get_platdata(&pdev->dev);
	struct extdrm *e;
	struct drm_device *dev;
	int ret;

	e = devm_drm_dev_alloc(&pdev->dev, &extdrm_driver, struct extdrm, dev);
	if (IS_ERR(e))
		return PTR_ERR(e);
	dev = &e->dev;
	e->ext = ext;
	e->hw_timing = -1;
	e->last_status = connector_status_connected;
	mutex_init(&e->hw_lock);

	ret = j713_dispfb_get(&e->fb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "scanout buffer not ready (load j713_dispfb.ko first)\n");
	dev_info(&pdev->dev, "scanout buffer %zu KiB at IOVA %#llx, up to %ux%u\n", e->fb.size / 1024, e->fb.iova,
		 e->fb.max_w, e->fb.max_h);

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = e->fb.max_w;
	dev->mode_config.max_height = e->fb.max_h;
	dev->mode_config.preferred_depth = 24;
	dev->mode_config.prefer_shadow = 0;
	dev->mode_config.funcs = &extdrm_mode_config_funcs;

	ret = extdrm_pipe_init(e);
	if (ret)
		return ret;
	drm_mode_config_reset(dev);
	platform_set_drvdata(pdev, dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;
	if (fbdev)
		drm_client_setup(dev, NULL);
	return 0;
}

static void extdrm_remove(struct platform_device *pdev)
{
	struct drm_device *dev = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(dev);
	drm_dev_unplug(dev);
}

static struct platform_driver extdrm_pdrv = {
	.probe = extdrm_probe,
	.remove = extdrm_remove,
	.driver = { .name = DRIVER_NAME },
};

static struct platform_device *extdrm_pdev;

static int __init extdrm_init(void)
{
	struct j713_ext *ext = j713_ext_get();
	struct j713_dispfb_buf probe_buf;
	int ret;

	if (!ext) {
		pr_err(DRIVER_NAME ": dcpext0 is not up (start/connect not done, no sink, or firmware failed)\n");
		return -ENODEV;
	}
	if (j713_dispfb_get(&probe_buf)) {
		pr_err(DRIVER_NAME ": j713_dispfb.ko has no scanout buffer\n");
		return -ENODEV;
	}
	if (!drm_name || !*drm_name)
		drm_name = DRIVER_NAME;
	extdrm_driver.name = drm_name;
	ret = platform_driver_register(&extdrm_pdrv);
	if (ret)
		return ret;
	extdrm_pdev =platform_device_alloc(DRIVER_NAME, PLATFORM_DEVID_NONE);
	if (!extdrm_pdev) {
		ret = -ENOMEM;
		goto err_drv;
	}
	extdrm_pdev->dev.parent = j713_ext_dev(ext);
	ret = dma_coerce_mask_and_coherent(&extdrm_pdev->dev, DMA_BIT_MASK(64));
	if (!ret)
		ret = platform_device_add_data(extdrm_pdev, &ext, sizeof(ext));
	if (!ret)
		ret = platform_device_add(extdrm_pdev);
	if (ret) {
		platform_device_put(extdrm_pdev);
		goto err_drv;
	}
	if (!device_is_bound(&extdrm_pdev->dev)) {
		pr_err(DRIVER_NAME ": probe failed, see the log above\n");
		platform_device_unregister(extdrm_pdev);
		ret = -ENODEV;
		goto err_drv;
	}
	return 0;
err_drv:
	platform_driver_unregister(&extdrm_pdrv);
	return ret;
}

static void __exit extdrm_exit(void)
{
	platform_device_unregister(extdrm_pdev);
	platform_driver_unregister(&extdrm_pdrv);
}

module_init(extdrm_init);
module_exit(extdrm_exit);

MODULE_DESCRIPTION("J713 USB-C external display (DRM/KMS over the dcpext0 iBoot display service)");
MODULE_LICENSE("GPL");

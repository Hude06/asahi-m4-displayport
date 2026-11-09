// SPDX-License-Identifier: GPL-2.0-only
/* J713 dcpext0 probe v2 (experimental).
 *
 *   start   : as v1 (RTKit boot, iBoot service, GET_HPD), then start the DPTX
 *             AFK endpoint (0x2a) and log every service it announces.
 *   connect : route atc1-dpxbar dpphy <- dispext0, connect DPTX remote port 0
 *             to ATC1 and answer the firmware's DPTX calls (link rate, lane
 *             count, drive settings) on atc1's DP PHY, then GET_HPD and
 *             GET_TIMING_MODES again.
 * No power/mode/surface/swap command is sent. Every inbound DPTX message is
 * logged. Firmware-call replies use a guessed native envelope (inbound
 * header, category 2); module parameter dptx_reply=0 disables replies.
 *
 * DPTX call handling follows drivers/gpu/drm/apple/dptxep.c,
 * Copyright 2022 Sven Peter <sven@svenpeter.dev>.
 */
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mux/consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include "afk.h"
#include "native-wire.h"
#include "j713_ext.h"

#define ASC_CPU_CONTROL		0x44
#define ASC_CPU_CONTROL_RUN	BIT(4)
#define IBOOT_GET_HPD		3
#define IBOOT_GET_TIMING_MODES	4
#define IBOOT_SET_SURFACE	1
#define IBOOT_SET_POWER		2
#define IBOOT_GET_COLOR_MODES	5
#define IBOOT_SET_MODE		6
#define TIMING_REPLY_SIZE	SZ_16K

static bool dptx_reply = true;
module_param(dptx_reply, bool, 0644);
MODULE_PARM_DESC(dptx_reply, "answer firmware DPTX calls (guessed native envelope)");
static int dptx_unit;
module_param(dptx_unit, int, 0644);
MODULE_PARM_DESC(dptx_unit, "DPTX remote port service index to connect (default 0)");
static int dptx_atc = 1;
module_param(dptx_atc, int, 0644);
MODULE_PARM_DESC(dptx_atc, "ATC index for the remote port connection (default 1)");
static int xbar_state;
module_param(xbar_state, int, 0644);
MODULE_PARM_DESC(xbar_state, "atc dpxbar state to select for dpphy (default 0 = dispext0)");
static bool defer_hpd;
module_param(defer_hpd, bool, 0444);
MODULE_PARM_DESC(defer_hpd, "connect and request_display, then return before set_hpd");

static int max_rate = 0x1e; /* LINK_RATE_HBR3; advertised via GET_MAX_LINK_RATE */
module_param(max_rate, int, 0444);
MODULE_PARM_DESC(max_rate, "max link rate code reported to the firmware (0x14 = HBR2, 0x1e = HBR3)");

static bool rate_via_dpaux;
module_param(rate_via_dpaux, bool, 0444);
MODULE_PARM_DESC(rate_via_dpaux,
		 "SET_LINK_RATE(HBR2) programs the PLL via j713_dpaux_hbr2() instead of phy_configure()");

static bool hbr2_programmed;

static bool hbr2_at_activate;
module_param(hbr2_at_activate, bool, 0444);
MODULE_PARM_DESC(hbr2_at_activate,
		 "program HBR2 inside the ACTIVATE apcall, after dev 0x18 power-on and before the reply");

int j713_dpaux_hbr2(void);

static char *fw_bootargs;
module_param(fw_bootargs, charp, 0444);
MODULE_PARM_DESC(fw_bootargs, "boot-args injected into the dcpext firmware _rtk_patchbay before start");

/*
 * dcp.macho (26.6.2 t8132dcp): DATA segment link VA 0x806000 is the reserved
 * segment whose first page holds the patchbay at VA 0x9b42f4:
 *   'GKTS' u32 8, u32 0xaff, u32 0 | 'grAb' u32 0x400 | char bootargs[0x400]
 * PE_parse_boot_argn (0x189ec) lazily copies it to 0xaf1168 (flag byte 0xae9c30,
 * length word 0xae9c64) on first use.
 */
#define FW_DATA_VA		0x806000
#define FW_PATCHBAY_VA		0x9b42f4
#define FW_BA_CACHE_VA		0xaf1168
#define FW_BA_FLAG_VA		0xae9c30
#define FW_BA_LEN_VA		0xae9c64
#define FW_BA_MAX		0x400

/* DPTX AP calls (dptxep.h) */
enum {
	APCALL_ACTIVATE = 0, APCALL_DEACTIVATE = 1, APCALL_GET_MAX_DRIVE = 2,
	APCALL_SET_DRIVE = 3, APCALL_GET_DRIVE = 4, APCALL_WILL_CHANGE = 5,
	APCALL_DID_CHANGE = 6, APCALL_GET_MAX_LINK_RATE = 7, APCALL_GET_LINK_RATE = 8,
	APCALL_SET_LINK_RATE = 9, APCALL_GET_MAX_LANES = 10, APCALL_GET_LANES = 11,
	APCALL_SET_LANES = 12, APCALL_SUPPORTS_DOWNSPREAD = 13, APCALL_SUPPORTS_HPD = 18,
};
#define LINK_RATE_RBR	0x06
#define LINK_RATE_HBR	0x0a
#define LINK_RATE_HBR2	0x14
#define LINK_RATE_HBR3	0x1e
#define PORT_CORE	GENMASK(3, 0)
#define PORT_ATC	GENMASK(7, 4)
#define PORT_DIE	GENMASK(11, 8)
#define PORT_CONNECTED	BIT(15)

struct epic_service_call {
	u8 _pad0[2];
	__le16 group;
	__le32 command;
	__le32 data_len;
#define EPIC_SERVICE_CALL_MAGIC 0x69706378
	__le32 magic;
	u8 _pad1[48];
} __packed;

static_assert(sizeof(struct epic_service_call) == 64);

struct dcpext_probe {
	struct device *dev;
	void __iomem *asc;
	struct apple_rtkit *rtk;
	struct afk_endpoint *ep, *dptx;
	struct mutex control;
	struct completion service_ready, linkcfg;
	struct dentry *debugfs;
	struct debugfs_blob_wrapper timing_blob;
	void *timing;
	void *color;
	bool color_valid;
	bool prepared, attempted, connected, failed, crashed;
	u32 cpu_control_before;
	u32 hpd_reply[5], hpd_after[5];
	int stage, cstage, last_error;
	/* DPTX link state */
	struct phy *phy;
	struct mux_control *xbar;
	bool xbar_selected;
	union phy_configure_opts phy_ops;
	u32 link_rate, lane_count, drive[2];
	unsigned int inbound, replied, unparsed;
	struct debugfs_blob_wrapper fwbuf[4];
	unsigned int n_fwbuf;
	/* v6: firmware __DATA segment (VA FW_DATA_VA) */
	u8 *data_va;
	size_t data_size;
	struct debugfs_blob_wrapper data_pre;
};

static void probe_failed(void *cookie, int error)
{
	struct dcpext_probe *p = cookie;

	WRITE_ONCE(p->last_error, error);
	WRITE_ONCE(p->failed, true);
}

static void probe_receive(void *cookie, u8 ep, u64 message)
{
	struct dcpext_probe *p = cookie;
	int ret = 0;

	if (ep == J713_AFK_ENDPOINT && p->ep)
		ret = afk_receive_message(p->ep, message);
	else if (ep == J713_DPTX_ENDPOINT && p->dptx)
		ret = afk_receive_message(p->dptx, message);
	else
		dev_info(p->dev, "message on endpoint %02x: %016llx\n", ep, message);
	if (ret)
		probe_failed(p, ret);
}

static void probe_crashed(void *cookie, const void *log, size_t length)
{
	struct dcpext_probe *p = cookie;

	WRITE_ONCE(p->crashed, true);
	probe_failed(p, -EIO);
	dev_err(p->dev, "dcpext0 firmware crashed (%zu byte log); reboot required\n", length);
	if (log && length)
		print_hex_dump(KERN_ERR, "dcpext0 crash: ", DUMP_PREFIX_OFFSET, 16, 1,
			       log, min_t(size_t, length, 512), true);
}

/* Cold coprocessor: buffers without a fixed IOVA are Linux-allocated through
 * the DART. v5: buffers at a firmware-chosen IOVA (oslog/crashlog style) that
 * fall inside one of the reserved dcpext0 segments are already DART-mapped by
 * the segment mapping, so accept them by memremap()ing the backing memory. */
static int probe_fixed_buffer(struct dcpext_probe *p, struct apple_rtkit_shmem *bfr)
{
	struct device_node *np = p->dev->of_node;
	int i;

	for (i = 0; ; i++) {
		struct device_node *rn = of_parse_phandle(np, "memory-region", i);
		struct resource res;
		const __be32 *ia;
		int len, ret;
		u64 iova, size;

		if (!rn)
			break;
		ret = of_address_to_resource(rn, 0, &res);
		ia = of_get_property(rn, "iommu-addresses", &len);
		of_node_put(rn);
		if (ret || !ia || len < 20)
			continue;
		iova = of_read_number(ia + 1, 2);
		size = of_read_number(ia + 3, 2);
		if (bfr->iova < iova || bfr->iova + bfr->size > iova + size ||
		    size > resource_size(&res))
			continue;
		bfr->buffer = memremap(res.start + (bfr->iova - iova), bfr->size, MEMREMAP_WC);
		if (!bfr->buffer)
			return -ENOMEM;
		bfr->private = (void *)1;	/* memremap()ed, not DMA-allocated */
		bfr->is_mapped = true;
		if (p->debugfs && p->n_fwbuf < ARRAY_SIZE(p->fwbuf)) {
			char name[32];

			snprintf(name, sizeof(name), "fwbuf_%llx", (u64)bfr->iova);
			p->fwbuf[p->n_fwbuf].data = bfr->buffer;
			p->fwbuf[p->n_fwbuf].size = bfr->size;
			debugfs_create_blob(name, 0400, p->debugfs, &p->fwbuf[p->n_fwbuf++]);
		}
		dev_info(p->dev, "RTKit buffer at fixed iova %#llx size %#zx -> segment %pR +%#llx\n",
			 (u64)bfr->iova, bfr->size, &res, (u64)(bfr->iova - iova));
		return 0;
	}
	dev_info(p->dev, "RTKit buffer request at fixed iova %#llx size %#zx: outside reserved segments\n",
		 (u64)bfr->iova, bfr->size);
	return -EINVAL;
}

static int probe_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct dcpext_probe *p = cookie;

	if (!bfr->size)
		return -EINVAL;
	if (bfr->iova)
		return probe_fixed_buffer(p, bfr);
	bfr->buffer = dma_alloc_coherent(p->dev, bfr->size, &bfr->iova, GFP_KERNEL);
	return bfr->buffer ? 0 : -ENOMEM;
}

static void probe_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct dcpext_probe *p = cookie;

	if (!bfr->buffer)
		return;
	if (bfr->private)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(p->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops probe_rtkit_ops = {
	.crashed = probe_crashed,
	.recv_message = probe_receive,
	.shmem_setup = probe_shmem_setup,
	.shmem_destroy = probe_shmem_destroy,
};

static void service_ready(void *cookie)
{
	struct dcpext_probe *p = cookie;

	complete(&p->service_ready);
}

static void dptx_announced(void *cookie)
{
}

struct iboot_request {
	__le32 op, length, reserved[2];
};

static int iboot_query(struct dcpext_probe *p, u32 op, void *reply, size_t size)
{
	struct iboot_request req = {
		.op = cpu_to_le32(op),
		.length = cpu_to_le32(sizeof(req)),
	};
	u32 retcode = 0;
	int ret = afk_send_command(p->ep, -1, &req, sizeof(req), reply, size, &retcode);

	if (!ret && retcode) {
		dev_err(p->dev, "iBoot op %u returned 0x%08x\n", op, retcode);
		ret = -EIO;
	}
	return ret;
}

static int get_hpd(struct dcpext_probe *p, u32 *out)
{
	u8 reply[64] = {};
	int i, ret = iboot_query(p, IBOOT_GET_HPD, reply, sizeof(reply));

	if (ret)
		return ret;
	for (i = 0; i < 5; i++)
		out[i] = get_unaligned_le32(reply + 4 * i);
	dev_info(p->dev, "GET_HPD: op=%u len=%u hpd=%u timings=%u colors=%u\n",
		 out[0], out[1], out[2], out[3], out[4]);
	return 0;
}

/* ---- DPTX: AP -> firmware service calls ---- */
static int dptx_service_call(struct dcpext_probe *p, u16 group, u32 command,
			     const void *data, size_t len, size_t pad,
			     void *out, size_t out_len, size_t out_pad)
{
	struct epic_service_call *call;
	size_t bfr_len = max(len + pad, out_len + out_pad) + sizeof(*call);
	u8 *bfr = kzalloc(bfr_len, GFP_KERNEL), *rx = kzalloc(bfr_len, GFP_KERNEL);
	u32 retcode = 0;
	int ret;

	if (!bfr || !rx) {
		ret = -ENOMEM;
		goto out;
	}
	call = (void *)bfr;
	call->group = cpu_to_le16(group);
	call->command = cpu_to_le32(command);
	call->data_len = cpu_to_le32(len + pad);
	call->magic = cpu_to_le32(EPIC_SERVICE_CALL_MAGIC);
	if (len)
		memcpy(bfr + sizeof(*call), data, len);
	ret = afk_send_command(p->dptx, dptx_unit, bfr, bfr_len, rx, bfr_len, &retcode);
	dev_info(p->dev, "DPTX call group %u cmd %u: ret %d retcode %#x\n", group, command, ret, retcode);
	if (!ret)
		print_hex_dump(KERN_INFO, "DPTX reply: ", DUMP_PREFIX_OFFSET, 16, 1,
			       rx, min_t(size_t, bfr_len, 128), true);
	if (ret)
		goto out;
	if (retcode) {
		ret = -EIO;
		goto out;
	}
	call = (void *)rx;
	if (le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC ||
	    le16_to_cpu(call->group) != group || le32_to_cpu(call->command) != command) {
		dev_err(p->dev, "DPTX reply header mismatch\n");
		ret = -EPROTO;
		goto out;
	}
	if (out && out_len)
		memcpy(out, rx + sizeof(*call), min_t(size_t, out_len,
		       le32_to_cpu(call->data_len)));
out:
	kfree(bfr);
	kfree(rx);
	return ret;
}

struct dptx_connection_cmd {
	__le32 unk;
	__le32 target;
} __packed;

static int dptx_connect_port(struct dcpext_probe *p, u8 core, u8 atc, u8 die)
{
	struct dptx_connection_cmd cmd = {}, resp = {};
	u32 target = FIELD_PREP(PORT_CORE, core) | FIELD_PREP(PORT_ATC, atc) |
		     FIELD_PREP(PORT_DIE, die) | PORT_CONNECTED;
	int ret;

	cmd.target = cpu_to_le32(target);
	ret = dptx_service_call(p, 0, 11, &cmd, sizeof(cmd), 24, &resp, sizeof(resp), 24);
	if (ret)
		return ret;
	dev_info(p->dev, "DPTX connect: target %#x -> resp target %#x unk %#x\n",
		 target, le32_to_cpu(resp.target), le32_to_cpu(resp.unk));
	return le32_to_cpu(resp.target) == target ? 0 : -EINVAL;
}

struct dptx_hotplug_cmd {
	u8 _pad0[16];
	__le32 unk;
} __packed;

/* dptxep.c dptxport_set_hpd: group 8 cmd 8, firmware answers unk=1 */
static int dptx_set_hpd(struct dcpext_probe *p, bool hpd)
{
	struct dptx_hotplug_cmd cmd = {}, resp = {};
	int ret;

	cmd.unk = cpu_to_le32(hpd ? 1 : 0);
	ret = dptx_service_call(p, 8, 8, &cmd, sizeof(cmd), 12, &resp, sizeof(resp), 12);
	dev_info(p->dev, "DPTX set_hpd(%d): ret %d resp.unk %#x\n", hpd, ret,
		 le32_to_cpu(resp.unk));
	return ret;
}

static void poll_hpd(struct dcpext_probe *p, int tries)
{
	while (tries-- > 0) {
		if (!get_hpd(p, p->hpd_after) && p->hpd_after[2])
			return;
		msleep(500);
	}
}

/* ---- DPTX: firmware -> AP calls ---- */
static int apcall(struct dcpext_probe *p, u32 idx, const u8 *in, size_t in_len,
		  u8 *out, size_t out_len)
{
	u32 rate;
	int ret;

	/*
	 * ACTIVATE is 16 bytes. The gate below skips calls that are too
	 * short to contain a drive or rate field. Programming HBR2 has to
	 * happen on this call, before the reply, so it cannot use that gate.
	 */
	if (idx == APCALL_ACTIVATE) {
		if (hbr2_at_activate) {
			int herr = j713_dpaux_hbr2();

			dev_info(p->dev, "HBR2 at ACTIVATE, before reply: %d\n", herr);
		}
		goto echo;
	}
	if (out_len < 32 || in_len < 32)
		goto echo;
	switch (idx) {
	case APCALL_GET_MAX_DRIVE:
		put_unaligned_le32(0, out);
		put_unaligned_le32(3, out + 16);
		put_unaligned_le32(3, out + 20);
		return 0;
	case APCALL_GET_DRIVE:
		memcpy(out, in, min(in_len, out_len));
		put_unaligned_le32(p->lane_count, out);
		put_unaligned_le32(p->drive[0], out + 32);
		put_unaligned_le32(0, out + 36);
		if (out_len >= 44)
			put_unaligned_le32(p->drive[1], out + 40);
		return 0;
	case APCALL_SET_DRIVE:
		memcpy(out, in, min(in_len, out_len));
		put_unaligned_le32(0, out);
		p->drive[0] = get_unaligned_le32(in + 32);
		if (in_len >= 44)
			p->drive[1] = get_unaligned_le32(in + 40);
		dev_info(p->dev, "DPTX set_drive_settings %u/%u\n", p->drive[0], p->drive[1]);
		return 0;
	case APCALL_WILL_CHANGE:
		p->phy_ops.dp.set_lanes = 0;
		p->phy_ops.dp.set_rate = 0;
		p->phy_ops.dp.set_voltages = 0;
		goto echo;
	case APCALL_DID_CHANGE:
		mdelay(10);
		goto echo;
	case APCALL_GET_MAX_LINK_RATE:
		put_unaligned_le32(0, out);
		put_unaligned_le32(max_rate, out + 16);
		return 0;
	case APCALL_GET_LINK_RATE:
		put_unaligned_le32(0, out);
		put_unaligned_le32(p->link_rate, out + 16);
		return 0;
	case APCALL_SET_LINK_RATE:
		rate = get_unaligned_le32(in + 16);
		switch (rate) {
		case LINK_RATE_RBR: p->phy_ops.dp.link_rate = 1620; break;
		case LINK_RATE_HBR: p->phy_ops.dp.link_rate = 2700; break;
		case LINK_RATE_HBR2: p->phy_ops.dp.link_rate = 5400; break;
		case LINK_RATE_HBR3: p->phy_ops.dp.link_rate = 8100; break;
		case 0: p->phy_ops.dp.link_rate = 0; break;
		default:
			dev_err(p->dev, "DPTX unsupported link rate %#x\n", rate);
			rate = 0;
			goto rate_reply;
		}
		p->phy_ops.dp.set_rate = 1;
		if (rate_via_dpaux && rate) {
			if (rate != LINK_RATE_HBR2) {
				dev_err(p->dev, "DPTX set_link_rate %#x: only HBR2 programmable via dpaux; not programmed\n",
					rate);
				ret = -EOPNOTSUPP;
			} else if (hbr2_programmed) {
				ret = 0;
			} else {
				ret = j713_dpaux_hbr2();
				if (!ret)
					hbr2_programmed = true;
			}
			dev_info(p->dev, "DPTX set_link_rate %#x (%u Mbps): dpaux hbr2 %d%s\n",
				 rate, p->phy_ops.dp.link_rate, ret,
				 hbr2_programmed ? " (programmed)" : "");
			p->link_rate = rate;
			goto rate_reply;
		}
		ret = p->phy ? phy_configure(p->phy, &p->phy_ops) : -ENODEV;
		dev_info(p->dev, "DPTX set_link_rate %#x (%u Mbps): phy_configure %d\n",
			 rate, p->phy_ops.dp.link_rate, ret);
		p->link_rate = rate;
rate_reply:
		put_unaligned_le32(0, out);
		put_unaligned_le32(rate, out + 16);
		return 0;
	case APCALL_GET_MAX_LANES: {
		union phy_configure_opts o = {};

		ret = p->phy ? phy_validate(p->phy, PHY_MODE_DP, 0, &o) : -ENODEV;
		p->lane_count = (!ret && o.dp.lanes >= 2) ? o.dp.lanes : 4;
		dev_info(p->dev, "DPTX get_max_lane_count: validate %d lanes %u -> %u\n",
			 ret, o.dp.lanes, p->lane_count);
		put_unaligned_le32(ret ? 1 : 0, out);
		put_unaligned_le64(ret ? 0 : p->lane_count, out + 16);
		return 0;
	}
	case APCALL_SET_LANES: {
		u64 lanes = get_unaligned_le64(in + 16);

		dev_info(p->dev, "DPTX set_active_lane_count %llu\n", lanes);
		put_unaligned_le32(lanes > 4 || lanes == 3 ? 1 : 0, out);
		put_unaligned_le64(lanes, out + 16);
		if (lanes > 0)
			complete(&p->linkcfg);
		return 0;
	}
	case APCALL_SUPPORTS_DOWNSPREAD:
	case APCALL_SUPPORTS_HPD:
		put_unaligned_le32(0, out);
		put_unaligned_le32(0, out + 16);
		return 0;
	default:
		dev_info(p->dev, "DPTX acking unhandled call %u\n", idx);
		break;
	}
echo:
	memcpy(out, in, min(in_len, out_len));
	put_unaligned_le32(0, out);
	return 0;
}

/*
 * Inbound firmware call (seen on macOS 26 dcpext fw):
 *   native header (24) + transaction word (8: flags, tag, pad, len) +
 *   struct epic_service_call (64: group, command, data_len, 'ipcx') + data.
 * Reply: same header with category 2, transaction word {0, tag, 0, retcode=0},
 * same epic_service_call header, then the reply data.
 */
static void dptx_inbound(void *cookie, struct afk_endpoint *ep, int svc,
			 const u8 *data, size_t size)
{
	struct dcpext_probe *p = cookie;
	const size_t off = NATIVE_HEADER_SIZE + NATIVE_TRANSACTION_SIZE;
	const struct epic_service_call *call = (const void *)(data + off);
	size_t call_len;
	u32 cmd;
	u8 *reply;
	int ret;

	p->inbound++;
	if (size < off + sizeof(*call) ||
	    le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC ||
	    off + sizeof(*call) + le32_to_cpu(call->data_len) != size) {
		p->unparsed++;
		dev_info(p->dev, "DPTX inbound (svc %d, %zu bytes): not an epic_service_call\n",
			 svc, size);
		return;
	}
	call_len = le32_to_cpu(call->data_len);
	cmd = le32_to_cpu(call->command);
	dev_info(p->dev, "DPTX apcall svc %d group %u cmd %u (%#x) len %zu tag %u\n",
		 svc, le16_to_cpu(call->group), cmd, cmd, call_len, data[off - 7]);
	print_hex_dump(KERN_INFO, "DPTX apcall in: ", DUMP_PREFIX_OFFSET, 16, 1,
		       data + off + sizeof(*call), min_t(size_t, call_len, 128), true);
	reply = kzalloc(size, GFP_KERNEL);
	if (!reply)
		return;
	ret = apcall(p, cmd, data + off + sizeof(*call), call_len,
		     reply + off + sizeof(*call), call_len);
	if (ret || !dptx_reply) {
		dev_info(p->dev, "DPTX apcall %u: not replying (ret %d)\n", cmd, ret);
		kfree(reply);
		return;
	}
	memcpy(reply, data, off + sizeof(*call));
	reply[17] = 2;				/* category: reply */
	put_unaligned_le32(0, reply + NATIVE_HEADER_SIZE + 4);	/* retcode */
	ret = afk_send_raw(ep, reply, size);
	if (!ret)
		p->replied++;
	dev_info(p->dev, "DPTX reply sent: %d\n", ret);
	print_hex_dump(KERN_INFO, "DPTX out: ", DUMP_PREFIX_OFFSET, 16, 1, reply,
		       min_t(size_t, size, 160), true);
	kfree(reply);
}

/* v6: find the private __DATA copy (the reserved segment whose page at
 * FW_PATCHBAY_VA holds the patchbay) and keep it mapped. */
static int probe_map_data(struct dcpext_probe *p)
{
	struct device_node *np = p->dev->of_node;
	u32 pb = FW_PATCHBAY_VA - FW_DATA_VA;
	int i;

	for (i = 0; ; i++) {
		struct device_node *rn = of_parse_phandle(np, "memory-region", i);
		struct resource res;
		u8 *va;
		int ret;

		if (!rn)
			break;
		ret = of_address_to_resource(rn, 0, &res);
		of_node_put(rn);
		if (ret || resource_size(&res) < FW_BA_CACHE_VA - FW_DATA_VA + FW_BA_MAX)
			continue;
		va = memremap(res.start, resource_size(&res), MEMREMAP_WC);
		if (!va)
			continue;
		if (memcmp(va + pb, "GKTS", 4) || memcmp(va + pb + 0x10, "grAb", 4)) {
			memunmap(va);
			continue;
		}
		p->data_va = va;
		p->data_size = resource_size(&res);
		dev_info(p->dev, "DATA segment %pR (%zu bytes); patchbay len word %#x\n", &res,
			 p->data_size, get_unaligned_le32(va + pb + 0x14));
		p->data_pre.data = vmalloc(p->data_size);
		if (p->data_pre.data) {
			memcpy(p->data_pre.data, va, p->data_size);
			p->data_pre.size = p->data_size;
		}
		return 0;
	}
	dev_warn(p->dev, "DATA segment (patchbay) not found in memory-region\n");
	return -ENOENT;
}

static void probe_inject_bootargs(struct dcpext_probe *p)
{
	u32 pb = FW_PATCHBAY_VA - FW_DATA_VA;
	u8 *va = p->data_va;
	size_t len;

	if (!fw_bootargs || !*fw_bootargs || !va)
		return;
	len = strnlen(fw_bootargs, FW_BA_MAX - 1);
	if (get_unaligned_le32(va + pb + 0x14) != FW_BA_MAX)
		dev_warn(p->dev, "bootargs: patchbay len word %#x != %#x; forcing\n",
			 get_unaligned_le32(va + pb + 0x14), FW_BA_MAX);
	put_unaligned_le32(FW_BA_MAX, va + pb + 0x14);
	dev_info(p->dev, "bootargs: old '%.*s' cache flag %#x len %#x old cache '%.*s'\n",
		 64, va + pb + 0x18, va[FW_BA_FLAG_VA - FW_DATA_VA],
		 get_unaligned_le32(va + FW_BA_LEN_VA - FW_DATA_VA),
		 64, va + FW_BA_CACHE_VA - FW_DATA_VA);
	memset(va + pb + 0x18, 0, FW_BA_MAX);
	memcpy(va + pb + 0x18, fw_bootargs, len);
	memset(va + FW_BA_CACHE_VA - FW_DATA_VA, 0, FW_BA_MAX);
	memcpy(va + FW_BA_CACHE_VA - FW_DATA_VA, fw_bootargs, len);
	put_unaligned_le32(len, va + FW_BA_LEN_VA - FW_DATA_VA);
	va[FW_BA_FLAG_VA - FW_DATA_VA] = 0;
	wmb();
	dev_info(p->dev, "bootargs: set '%s' (len %zu; rb flag %#x len %#x)\n", fw_bootargs, len,
		 va[FW_BA_FLAG_VA - FW_DATA_VA], get_unaligned_le32(va + FW_BA_LEN_VA - FW_DATA_VA));
}

static ssize_t data_live_read(struct file *f, char __user *ubuf, size_t n, loff_t *pos)
{
	struct dcpext_probe *p = f->private_data;

	if (!p->data_va)
		return -ENODEV;
	return simple_read_from_buffer(ubuf, n, pos, p->data_va, p->data_size);
}

static const struct file_operations data_live_fops = {
	.open = simple_open,
	.read = data_live_read,
	.llseek = default_llseek,
};

/* "VA VALUE [SIZE]" (hex VA inside DATA, size 1/2/4/8, default 8). Pre-start only
 * unless the VA is written with a leading '!'. */
static ssize_t data_poke_write(struct file *f, const char __user *ubuf, size_t n, loff_t *pos)
{
	struct dcpext_probe *p = f->private_data;
	char buf[96];
	unsigned long long vaddr, val;
	unsigned int size = 8;
	bool force = false;
	char *s = buf;
	size_t off;
	int k;

	if (!p->data_va)
		return -ENODEV;
	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	if (*s == '!') {
		force = true;
		s++;
	}
	k = sscanf(s, "%llx %llx %u", &vaddr, &val, &size);
	if (k < 2 || (size != 1 && size != 2 && size != 4 && size != 8))
		return -EINVAL;
	if (vaddr < FW_DATA_VA || vaddr + size > FW_DATA_VA + p->data_size)
		return -ERANGE;
	if (p->attempted && !force)
		return -EBUSY;
	off = vaddr - FW_DATA_VA;
	switch (size) {
	case 1: p->data_va[off] = val; break;
	case 2: put_unaligned_le16(val, p->data_va + off); break;
	case 4: put_unaligned_le32(val, p->data_va + off); break;
	case 8: put_unaligned_le64(val, p->data_va + off); break;
	}
	wmb();
	dev_info(p->dev, "data_poke %#llx = %#llx (%u)%s\n", vaddr, val, size, force ? " FORCED" : "");
	return n;
}

static const struct file_operations data_poke_fops = {
	.open = simple_open,
	.write = data_poke_write,
};

static ssize_t start_write(struct file *file, const char __user *buffer,
			   size_t size, loff_t *pos)
{
	struct dcpext_probe *p = file->private_data;
	u32 value;
	int i, ret = kstrtou32_from_user(buffer, size, 0, &value);

	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	mutex_lock(&p->control);
	if (p->attempted) {
		ret = -EALREADY;
		goto out;
	}
	if (!p->prepared || READ_ONCE(p->failed)) {
		ret = -ENODEV;
		goto out;
	}
	p->attempted = true;

	p->stage = 1;
	probe_inject_bootargs(p);
	p->cpu_control_before = readl_relaxed(p->asc + ASC_CPU_CONTROL);
	dev_info(p->dev, "CPU_CONTROL before start: %#x\n", p->cpu_control_before);
	writel_relaxed(p->cpu_control_before | ASC_CPU_CONTROL_RUN, p->asc + ASC_CPU_CONTROL);

	p->stage = 2;
	ret = apple_rtkit_wake(p->rtk);
	if (ret)
		goto failed;
	dev_info(p->dev, "RTKit booted\n");

	p->stage = 3;
	ret = afk_start(p->ep);
	if (ret)
		goto failed;

	p->stage = 4;
	if (!wait_for_completion_timeout(&p->service_ready, msecs_to_jiffies(3000))) {
		ret = -ETIMEDOUT;
		goto failed;
	}
	dev_info(p->dev, "service '%s' ready\n", afk_service_name(p->ep));

	p->stage = 5;
	ret = get_hpd(p, p->hpd_reply);
	if (ret)
		goto failed;

	p->stage = 6; /* DPTX endpoint: discovery only */
	ret = afk_start(p->dptx);
	if (ret) {
		dev_err(p->dev, "DPTX endpoint start failed: %d\n", ret);
		goto failed;
	}
	msleep(2000);
	for (i = 0; i < afk_service_count(p->dptx); i++)
		dev_info(p->dev, "DPTX service %d: channel %u '%s'\n", i,
			 afk_service_channel(p->dptx, i), afk_service_name_at(p->dptx, i));
	p->stage = 7;
	goto out;
failed:
	probe_failed(p, ret);
	dev_err(p->dev, "start stopped at stage %d: %d; no retry until reboot\n", p->stage, ret);
out:
	mutex_unlock(&p->control);
	return ret ? ret : size;
}

static ssize_t connect_write(struct file *file, const char __user *buffer,
			     size_t size, loff_t *pos)
{
	struct dcpext_probe *p = file->private_data;
	u32 value;
	int ret = kstrtou32_from_user(buffer, size, 0, &value);

	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	mutex_lock(&p->control);
	if (p->stage != 7 || p->connected || READ_ONCE(p->failed)) {
		ret = -EBUSY;
		goto out;
	}
	p->connected = true;

	p->cstage = 1; /* DP PHY of atc1 */
	p->phy = devm_phy_get(p->dev, "dp-phy");
	if (IS_ERR(p->phy)) {
		ret = PTR_ERR(p->phy);
		p->phy = NULL;
		goto failed;
	}
	p->cstage = 2; /* crossbar: dpphy <- dispext0 */
	p->xbar = devm_mux_control_get(p->dev, "dp-xbar");
	if (IS_ERR(p->xbar)) {
		ret = PTR_ERR(p->xbar);
		p->xbar = NULL;
		goto failed;
	}
	ret = mux_control_select(p->xbar, xbar_state);
	if (ret)
		goto failed;
	p->xbar_selected = true;

	p->cstage = 3; /* remote port connect + request display */
	if (afk_service_count(p->dptx) <= dptx_unit) {
		ret = -ENODEV;
		goto failed;
	}
	reinit_completion(&p->linkcfg);
	ret = dptx_connect_port(p, 0, dptx_atc, 0);
	if (ret)
		goto failed;
	ret = dptx_service_call(p, 0, 6, NULL, 0, 16, NULL, 0, 16);
	dev_info(p->dev, "DPTX request_display: %d\n", ret);

	p->cstage = 4; /* as dcp_dptx_connect: wait for link config, then set_hpd */
	if (!wait_for_completion_timeout(&p->linkcfg, msecs_to_jiffies(2000)))
		dev_warn(p->dev, "no set_active_lane_count within 2 s (inbound %u, replied %u, unparsed %u)\n",
			 p->inbound, p->replied, p->unparsed);
	usleep_range(5, 10);
	if (defer_hpd) {
		dev_info(p->dev, "defer_hpd: connected, request_display done, set_hpd not sent\n");
		goto out;
	}
	ret = dptx_set_hpd(p, true);
	if (ret)
		dev_warn(p->dev, "set_hpd failed: %d (continuing)\n", ret);
	if (!wait_for_completion_timeout(&p->linkcfg, msecs_to_jiffies(5000)))
		dev_warn(p->dev, "no set_active_lane_count within 5 s after set_hpd (inbound %u, replied %u, unparsed %u)\n",
			 p->inbound, p->replied, p->unparsed);

	p->cstage = 5;
	poll_hpd(p, 10);
	if (p->hpd_after[2] && p->hpd_after[3]) {
		ret = iboot_query(p, IBOOT_GET_TIMING_MODES, p->timing, TIMING_REPLY_SIZE);
		if (ret)
			goto failed;
		p->timing_blob.data = p->timing;
		p->timing_blob.size = TIMING_REPLY_SIZE;
		debugfs_create_blob("timing_modes", 0400, p->debugfs, &p->timing_blob);
		dev_info(p->dev, "GET_TIMING_MODES ok; raw reply in debugfs timing_modes\n");
	}
	p->cstage = 6;
	goto out;
failed:
	dev_err(p->dev, "connect stopped at stage %d: %d\n", p->cstage, ret);
out:
	mutex_unlock(&p->control);
	return ret ? ret : size;
}

static int status_show(struct seq_file *seq, void *unused)
{
	struct dcpext_probe *p = seq->private;
	int i;

	seq_printf(seq, "prepared=%u attempted=%u stage=%d cstage=%d failed=%u crashed=%u error=%d\n",
		   p->prepared, p->attempted, p->stage, p->cstage, READ_ONCE(p->failed),
		   READ_ONCE(p->crashed), READ_ONCE(p->last_error));
	seq_printf(seq, "cpu_control_before=%#x service=%s\n", p->cpu_control_before,
		   p->ep && afk_service_name(p->ep) ? afk_service_name(p->ep) : "-");
	seq_printf(seq, "hpd_reply=%u %u %u %u %u\n", p->hpd_reply[0], p->hpd_reply[1],
		   p->hpd_reply[2], p->hpd_reply[3], p->hpd_reply[4]);
	seq_printf(seq, "hpd_after=%u %u %u %u %u\n", p->hpd_after[0], p->hpd_after[1],
		   p->hpd_after[2], p->hpd_after[3], p->hpd_after[4]);
	if (p->dptx)
		for (i = 0; i < afk_service_count(p->dptx); i++)
			seq_printf(seq, "dptx service %d: ch %u '%s'\n", i,
				   afk_service_channel(p->dptx, i), afk_service_name_at(p->dptx, i));
	seq_printf(seq, "dptx inbound=%u replied=%u unparsed=%u link_rate=%#x lanes=%u\n",
		   p->inbound, p->replied, p->unparsed, p->link_rate, p->lane_count);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static const struct file_operations start_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = start_write, .llseek = noop_llseek,
};
static const struct file_operations connect_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = connect_write, .llseek = noop_llseek,
};

/* "group cmd in_pad out_pad [hex bytes...]" -> arbitrary DPTX service call */
static ssize_t call_write(struct file *file, const char __user *buffer,
			  size_t size, loff_t *pos)
{
	struct dcpext_probe *p = file->private_data;
	char *buf, *cur, *tok;
	u8 data[256], out[256] = {};
	unsigned int group, cmd, in_pad, out_pad, len = 0, v;
	int ret;

	if (size > 2048)
		return -E2BIG;
	buf = memdup_user_nul(buffer, size);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (sscanf(buf, "%u %u %u %u", &group, &cmd, &in_pad, &out_pad) != 4 ||
	    in_pad > 512 || out_pad > 512) {
		ret = -EINVAL;
		goto out;
	}
	cur = buf;
	for (v = 0; v < 4; v++)
		strsep(&cur, " \t\n");
	while (cur && (tok = strsep(&cur, " \t\n"))) {
		if (!*tok)
			continue;
		if (len >= sizeof(data) || kstrtouint(tok, 16, &v) || v > 0xff) {
			ret = -EINVAL;
			goto out;
		}
		data[len++] = v;
	}
	mutex_lock(&p->control);
	if (!p->connected || READ_ONCE(p->failed))
		ret = -EBUSY;
	else
		ret = dptx_service_call(p, group, cmd, data, len, in_pad, out,
					min_t(size_t, len + in_pad, sizeof(out)), 0);
	mutex_unlock(&p->control);
out:
	kfree(buf);
	return ret ? ret : size;
}

static ssize_t hpd_write(struct file *file, const char __user *buffer,
			 size_t size, loff_t *pos)
{
	struct dcpext_probe *p = file->private_data;
	u32 value;
	int ret = kstrtou32_from_user(buffer, size, 0, &value);

	if (ret)
		return ret;
	mutex_lock(&p->control);
	if (!p->connected || READ_ONCE(p->failed))
		ret = -EBUSY;
	else
		ret = dptx_set_hpd(p, value);
	mutex_unlock(&p->control);
	return ret ? ret : size;
}

static ssize_t gethpd_write(struct file *file, const char __user *buffer,
			    size_t size, loff_t *pos)
{
	struct dcpext_probe *p = file->private_data;
	int ret;

	mutex_lock(&p->control);
	ret = p->stage == 7 && !READ_ONCE(p->failed) ? get_hpd(p, p->hpd_after) : -EBUSY;
	mutex_unlock(&p->control);
	return ret ? ret : size;
}

/* ---- iBoot display commands with payload (phase 5, no framebuffer yet) ---- */
static int iboot_cmd(struct dcpext_probe *p, u32 op, const void *payload, size_t plen,
		     void *reply, size_t size)
{
	struct iboot_request *req;
	u32 retcode = 0;
	int ret;

	req = kzalloc(sizeof(*req) + plen, GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	req->op = cpu_to_le32(op);
	req->length = cpu_to_le32(sizeof(*req) + plen);
	if (plen)
		memcpy(req + 1, payload, plen);
	ret = afk_send_command(p->ep, -1, req, sizeof(*req) + plen, reply, size, &retcode);
	kfree(req);
	if (!ret && retcode) {
		dev_err(p->dev, "iBoot op %u returned 0x%08x\n", op, retcode);
		ret = -EIO;
	}
	return ret;
}

static int iboot_colors(struct dcpext_probe *p)
{
	u8 *r = p->color;
	u32 i, n;
	int ret = iboot_cmd(p, IBOOT_GET_COLOR_MODES, NULL, 0, r, TIMING_REPLY_SIZE);

	if (ret)
		return ret;
	n = get_unaligned_le32(r + 8);
	dev_info(p->dev, "GET_COLOR_MODES: op=%u len=%u count=%u\n",
		 get_unaligned_le32(r), get_unaligned_le32(r + 4), n);
	for (i = 0; i < n && 12 + (i + 1) * 24 <= TIMING_REPLY_SIZE; i++) {
		u8 *m = r + 12 + i * 24;

		dev_info(p->dev, "color %u: valid=%u colorimetry=%u eotf=%u encoding=%u bpp=%u pad=%08x\n", i,
			 get_unaligned_le32(m), get_unaligned_le32(m + 4), get_unaligned_le32(m + 8),
			 get_unaligned_le32(m + 12), get_unaligned_le32(m + 16), get_unaligned_le32(m + 20));
	}
	return 0;
}

static int iboot_setmode(struct dcpext_probe *p, u32 ti, u32 ci)
{
	u8 payload[48];
	u8 *t = p->timing, *c = p->color;
	int ret;

	if (ti >= get_unaligned_le32(t + 8) || ci >= get_unaligned_le32(c + 8) ||
	    12 + (ti + 1) * 24 > TIMING_REPLY_SIZE || 12 + (ci + 1) * 24 > TIMING_REPLY_SIZE)
		return -EINVAL;
	memcpy(payload, t + 12 + ti * 24, 24);
	memcpy(payload + 24, c + 12 + ci * 24, 24);
	dev_info(p->dev, "SET_MODE: timing %u (%ux%u fps16.16 %#x) color %u (bpp %u)\n", ti,
		 get_unaligned_le32(payload + 4), get_unaligned_le32(payload + 8),
		 get_unaligned_le32(payload + 12), ci, get_unaligned_le32(payload + 24 + 16));
	ret = iboot_cmd(p, IBOOT_SET_MODE, payload, sizeof(payload), NULL, 0); /* write-only: an RX buffer asserts fw AFKEPCommandContext.cpp:204 */
	dev_info(p->dev, "SET_MODE: ret %d\n", ret);
	return ret;
}

/*
 * SET_SURFACE (m1n1 dcp_ib_set_surface / display_swap): one dcp_layer_t, 164 bytes, packed little-endian:
 *   plane[0] @0: unk1 u32 @0, addr u64 @4, tile_size u32 @12, stride u32 @16, unk2[4] @20, addr_format u32 @36, unk3 u32 @40  (44 B each, 3 planes)
 *   @132 unk u32, @136 plane_cnt, @140 width, @144 height, @148 surface_fmt, @152 colorspace, @156 eotf, @160 transform u8, @161 pad[3]
 * m1n1 values: addr = IOVA (same in dart-dispext0 and dart-dcpext0), addr_format ADDR_PLANAR=1, plane_cnt 1,
 * surface_fmt FMT_w30r=9 (x2r10g10b10), colorspace 2, eotf EOTF_GAMMA_SDR=1, transform XFRM_NONE=0. Write-only: no RX buffer.
 */
static int iboot_surface(struct dcpext_probe *p, u64 iova, u32 w, u32 h, u32 stride, u32 fmt)
{
	u8 layer[164] = {};
	int ret;

	put_unaligned_le64(iova, layer + 4);
	put_unaligned_le32(stride, layer + 16);
	put_unaligned_le32(1, layer + 36);	/* ADDR_PLANAR */
	put_unaligned_le32(1, layer + 136);	/* plane_cnt */
	put_unaligned_le32(w, layer + 140);
	put_unaligned_le32(h, layer + 144);
	put_unaligned_le32(fmt, layer + 148);	/* 9 = FMT_w30r, 1 = FMT_BGRA */
	put_unaligned_le32(2, layer + 152);	/* colorspace */
	put_unaligned_le32(1, layer + 156);	/* EOTF_GAMMA_SDR */
	dev_info(p->dev, "SET_SURFACE: iova %#llx %ux%u stride %u fmt %u\n", iova, w, h, stride, fmt);
	ret = iboot_cmd(p, IBOOT_SET_SURFACE, layer, sizeof(layer), NULL, 0);
	dev_info(p->dev, "SET_SURFACE: ret %d\n", ret);
	return ret;
}

static ssize_t iboot_write(struct file *file, const char __user *buffer, size_t size, loff_t *pos)
{
	struct dcpext_probe *p = file->private_data;
	char buf[64] = {};
	u32 a = 0, b = 0;
	int ret = -EINVAL;

	if (size >= sizeof(buf) || copy_from_user(buf, buffer, size))
		return -EINVAL;
	mutex_lock(&p->control);
	if (p->stage != 7 || READ_ONCE(p->failed)) {
		ret = -EBUSY;
	} else if (sscanf(buf, "power %u", &a) == 1) {
		__le32 v = cpu_to_le32(a);

		/* m1n1: 4-byte store, but only 1 payload byte is counted */
		ret = iboot_cmd(p, IBOOT_SET_POWER, &v, 1, NULL, 0);
		dev_info(p->dev, "SET_POWER %u: ret %d\n", a, ret);
	} else if (!strncmp(buf, "colors", 6)) {
		ret = iboot_colors(p);
	} else if (sscanf(buf, "mode %u %u", &a, &b) == 2) {
		ret = iboot_setmode(p, a, b);
	} else if (!strncmp(buf, "surface ", 8)) {
		unsigned long long iova;
		u32 w, h, stride;

		if (sscanf(buf + 8, "%llx %u %u %u", &iova, &w, &h, &stride) == 4)
			ret = iboot_surface(p, iova, w, h, stride, 9);
	}
	mutex_unlock(&p->control);
	return ret ? ret : size;
}

static const struct file_operations iboot_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = iboot_write, .llseek = noop_llseek,
};

static const struct file_operations call_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = call_write, .llseek = noop_llseek,
};
static const struct file_operations hpd_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = hpd_write, .llseek = noop_llseek,
};
static const struct file_operations gethpd_fops = {
	.owner = THIS_MODULE, .open = simple_open, .write = gethpd_write, .llseek = noop_llseek,
};

/* ---- in-kernel API for j713_extdrm.ko (the DRM driver); see j713_ext.h ---- */
static struct dcpext_probe *g_probe;

struct j713_ext *j713_ext_get(void)
{
	struct dcpext_probe *p = READ_ONCE(g_probe);

	if (!p || p->stage != 7 || !p->connected || READ_ONCE(p->failed) || READ_ONCE(p->crashed))
		return NULL;
	if (!p->hpd_after[2] || !p->hpd_after[3] || !p->timing_blob.data)	/* no sink / no timing list */
		return NULL;
	return (struct j713_ext *)p;
}
EXPORT_SYMBOL_GPL(j713_ext_get);

struct device *j713_ext_dev(struct j713_ext *e)
{
	return ((struct dcpext_probe *)e)->dev;
}
EXPORT_SYMBOL_GPL(j713_ext_dev);

int j713_ext_hpd(struct j713_ext *e, u32 out[5])
{
	struct dcpext_probe *p = (struct dcpext_probe *)e;
	u8 reply[64] = {};
	int i, ret;

	mutex_lock(&p->control);
	ret = READ_ONCE(p->failed) || READ_ONCE(p->crashed) ? -EIO : iboot_query(p, IBOOT_GET_HPD, reply, sizeof(reply));
	mutex_unlock(&p->control);
	if (ret)
		return ret;
	for (i = 0; i < 5; i++)
		out[i] = get_unaligned_le32(reply + 4 * i);
	return 0;
}
EXPORT_SYMBOL_GPL(j713_ext_hpd);

u32 j713_ext_timing_count(struct j713_ext *e)
{
	struct dcpext_probe *p = (struct dcpext_probe *)e;

	return min_t(u32, get_unaligned_le32((u8 *)p->timing + 8), (TIMING_REPLY_SIZE - 12) / 24);
}
EXPORT_SYMBOL_GPL(j713_ext_timing_count);

int j713_ext_timing(struct j713_ext *e, u32 idx, struct j713_ext_timing *t)
{
	struct dcpext_probe *p = (struct dcpext_probe *)e;
	u8 *m;

	if (idx >= j713_ext_timing_count(e))
		return -ENOENT;
	m = (u8 *)p->timing + 12 + idx * 24;
	t->idx = idx;
	t->width = get_unaligned_le32(m + 4);
	t->height = get_unaligned_le32(m + 8);
	t->fps = get_unaligned_le32(m + 12);
	return get_unaligned_le32(m) ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(j713_ext_timing);

int j713_ext_power(struct j713_ext *e, bool on)
{
	struct dcpext_probe *p = (struct dcpext_probe *)e;
	__le32 v = cpu_to_le32(on);
	int ret;

	mutex_lock(&p->control);
	/* m1n1: 4-byte store, but only 1 payload byte is counted */
	ret = READ_ONCE(p->failed) ? -EIO : iboot_cmd(p, IBOOT_SET_POWER, &v, 1, NULL, 0);
	mutex_unlock(&p->control);
	dev_info(p->dev, "SET_POWER %u: ret %d\n", on, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(j713_ext_power);

/* same ordering as m1n1 display_choose_color_mode and test-fb.sh: valid, then bpp <= 32, then widest, then colorimetry/encoding/eotf */
static int pick_color(struct dcpext_probe *p)
{
	u8 *r = p->color;
	u32 i, n = min_t(u32, get_unaligned_le32(r + 8), (TIMING_REPLY_SIZE - 12) / 24);
	int best = -1;
	long best_key = 0;

	for (i = 0; i < n; i++) {
		u8 *m = r + 12 + i * 24;
		u32 valid = get_unaligned_le32(m), clr = get_unaligned_le32(m + 4), eotf = get_unaligned_le32(m + 8);
		u32 enc = get_unaligned_le32(m + 12), bpp = get_unaligned_le32(m + 16);
		long key = (valid ? 0 : 1L << 40) + ((bpp <= 32 ? 0 : 1L) << 36) + ((long)(255 - min_t(u32, bpp, 255)) << 28) +
			   ((long)clr << 20) + ((long)enc << 12) + eotf;

		if (best < 0 || key < best_key) {
			best = i;
			best_key = key;
		}
	}
	return best;
}

int j713_ext_set_mode(struct j713_ext *e, u32 ti)
{
	struct dcpext_probe *p = (struct dcpext_probe *)e;
	int ci, ret = 0;

	mutex_lock(&p->control);
	if (READ_ONCE(p->failed) || READ_ONCE(p->crashed)) {
		ret = -EIO;
		goto out;
	}
	if (!p->color_valid) {
		ret = iboot_colors(p);
		if (ret)
			goto out;
		p->color_valid = true;
	}
	ci = pick_color(p);
	ret = ci < 0 ? -ENODATA : iboot_setmode(p, ti, ci);
out:
	mutex_unlock(&p->control);
	return ret;
}
EXPORT_SYMBOL_GPL(j713_ext_set_mode);

int j713_ext_set_surface(struct j713_ext *e, u64 iova, u32 w, u32 h, u32 stride, u32 fmt)
{
	struct dcpext_probe *p = (struct dcpext_probe *)e;
	int ret;

	mutex_lock(&p->control);
	ret = READ_ONCE(p->failed) || READ_ONCE(p->crashed) ? -EIO : iboot_surface(p, iova, w, h, stride, fmt);
	mutex_unlock(&p->control);
	return ret;
}
EXPORT_SYMBOL_GPL(j713_ext_set_surface);

static void probe_remove_debugfs(void *data)
{
	struct dcpext_probe *p = data;

	debugfs_remove_recursive(p->debugfs);
}

static int validate_stream(struct device *dev)
{
	struct of_phandle_args args;
	int ret;

	if (of_count_phandle_with_args(dev->of_node, "iommus", "#iommu-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells", 0, &args);
	if (ret)
		return ret;
	ret = args.args_count == 1 && args.args[0] == 23 ? 0 : -EINVAL;
	of_node_put(args.np);
	return ret;
}

static int dcpext_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dcpext_probe *p;
	struct resource *res;
	int ret;

	if (!of_machine_is_compatible("apple,j713") ||
	    !of_property_read_bool(dev->of_node, "omarchy,dcpext-firmware-reserved") ||
	    !of_find_property(dev->of_node, "memory-region", NULL))
		return dev_err_probe(dev, -ENODEV,
				     "m1n1 did not reserve dcpext0 firmware this boot\n");
	ret = validate_stream(dev);
	if (ret)
		return dev_err_probe(dev, ret, "exactly stream 23 is required\n");
	if (!iommu_get_domain_for_dev(dev))
		return dev_err_probe(dev, -EINVAL, "no DMA domain\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	mutex_init(&p->control);
	init_completion(&p->service_ready);
	init_completion(&p->linkcfg);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	p->asc = devm_ioremap(dev, res->start, SZ_16K);
	if (!p->asc)
		return -ENOMEM;
	p->timing = devm_kzalloc(dev, TIMING_REPLY_SIZE, GFP_KERNEL);
	p->color = devm_kzalloc(dev, TIMING_REPLY_SIZE, GFP_KERNEL);
	if (!p->timing || !p->color)
		return -ENOMEM;

	p->rtk = devm_apple_rtkit_init(dev, p, "mbox", 0, &probe_rtkit_ops);
	if (IS_ERR(p->rtk))
		return dev_err_probe(dev, PTR_ERR(p->rtk), "RTKit mailbox unavailable\n");
	p->ep = afk_init(dev, p->rtk, J713_AFK_ENDPOINT, p, service_ready, probe_failed, NULL);
	if (IS_ERR(p->ep))
		return PTR_ERR(p->ep);
	p->dptx = afk_init(dev, p->rtk, J713_DPTX_ENDPOINT, p, dptx_announced, probe_failed,
			   dptx_inbound);
	if (IS_ERR(p->dptx))
		return PTR_ERR(p->dptx);

	p->debugfs = debugfs_create_dir("j713_dcpext0", NULL);
	if (IS_ERR(p->debugfs))
		return PTR_ERR(p->debugfs);
	ret = devm_add_action_or_reset(dev, probe_remove_debugfs, p);
	if (ret)
		return ret;
	debugfs_create_file("start", 0200, p->debugfs, p, &start_fops);
	debugfs_create_file("connect", 0200, p->debugfs, p, &connect_fops);
	debugfs_create_file("status", 0400, p->debugfs, p, &status_fops);
	debugfs_create_file("call", 0200, p->debugfs, p, &call_fops);
	debugfs_create_file("hpd", 0200, p->debugfs, p, &hpd_fops);
	debugfs_create_file("gethpd", 0200, p->debugfs, p, &gethpd_fops);
	debugfs_create_file("iboot", 0200, p->debugfs, p, &iboot_fops);
	if (!probe_map_data(p)) {
		if (p->data_pre.data)
			debugfs_create_blob("data_prestart", 0400, p->debugfs, &p->data_pre);
		debugfs_create_file("data_live", 0400, p->debugfs, p, &data_live_fops);
		debugfs_create_file("data_poke", 0200, p->debugfs, p, &data_poke_fops);
	}
	platform_set_drvdata(pdev, p);
	__module_get(THIS_MODULE);
	p->prepared = true;
	WRITE_ONCE(g_probe, p);
	dev_info(dev, "dcpext0 probe v9 prepared; start and connect are explicit\n");
	return 0;
}

static const struct of_device_id dcpext_match[] = {
	{ .compatible = "omarchy,j713-dcpext-probe" },
	{}
};
MODULE_DEVICE_TABLE(of, dcpext_match);

static struct platform_driver dcpext_driver = {
	.probe = dcpext_probe,
	.driver = {
		.name = "j713-dcpext-probe2",
		.of_match_table = dcpext_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(dcpext_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 dcpext0 probe v2 (RTKit + iBoot + DPTX remote port)");

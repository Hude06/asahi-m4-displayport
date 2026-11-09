// SPDX-License-Identifier: GPL-2.0+
/*
 * J713 livepatch for the built-in tipd (SN201202x/CD321x) driver.
 *
 * Replaces cd321x_update_work() of kernel 7.1.9-m4-gpu-backport-20261008
 * (kernel-controller tree, drivers/usb/typec/tipd/core.c) with an identical
 * copy except:
 *  - the partner is registered with a pointer to cd321x->cur_partner_identity,
 *    not to the on-stack status copy. typec_register_partner() keeps the
 *    pointer, so the old code left partner->identity dangling and reading
 *    /sys/class/typec/portN-partner/{type,identity/...} oopsed.
 *  - the partner descriptor is zeroed (pd_revision/attach/... were garbage).
 *  - a dev_dbg() reports status, data_status and the DP SID registers.
 * The unused m4bench_usb2_device early_param path is dropped; the module
 * refuses to load if that parameter is on the command line.
 *
 * Partners registered before this module loads keep their stale pointer
 * until the cable is replugged.
 *
 * Based on core.c: Copyright (C) 2017, Intel Corporation,
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>, and the Asahi
 * Linux cd321x changes.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/livepatch.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_tbt.h>
#include <linux/usb/role.h>
#include <linux/workqueue.h>
#include <linux/m4bench_trace.h>
#include "tps6598x.h"

static void m4_pd_status_trace(struct tps6598x *tps, unsigned int id)
{
	u32 sid;

	if (device_is_compatible(tps->dev, "apple,sn201202x") &&
	    !of_property_read_u32(tps->dev->of_node, "reg", &sid) && sid == 0xa)
		m4bench_trace(id);
}

/* Verbatim copy of the (inlined) static cd321x_typec_update_mode(). */
static void lp_cd321x_typec_update_mode(struct tps6598x *tps, struct cd321x_status *st)
{
	struct cd321x *cd321x = container_of(tps, struct cd321x, tps);

	if (!(st->data_status & TPS_DATA_STATUS_DATA_CONNECTION)) {
		if (cd321x->state.mode == TYPEC_STATE_SAFE)
			return;
		cd321x->state.alt = NULL;
		cd321x->state.mode = TYPEC_STATE_SAFE;
		cd321x->state.data = NULL;
		typec_mux_set(cd321x->mux, &cd321x->state);
	} else if (st->data_status & TPS_DATA_STATUS_DP_CONNECTION) {
		struct typec_displayport_data dp_data;
		unsigned long mode;

		switch (TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT(st->data_status)) {
		case TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_A:
			mode = TYPEC_DP_STATE_A;
			break;
		case TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_B:
			mode = TYPEC_DP_STATE_B;
			break;
		case TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_C:
			mode = TYPEC_DP_STATE_C;
			break;
		case TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_D:
			mode = TYPEC_DP_STATE_D;
			break;
		case TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_E:
			mode = TYPEC_DP_STATE_E;
			break;
		case TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_F:
			mode = TYPEC_DP_STATE_F;
			break;
		default:
			dev_err(tps->dev, "Invalid DP pin assignment\n");
			return;
		}

		if (cd321x->state.alt == cd321x->port_altmode_dp &&
		    cd321x->state.mode == mode)
			return;

		dp_data.status = le32_to_cpu(st->dp_sid_status.status_rx);
		dp_data.conf = le32_to_cpu(st->dp_sid_status.configure);
		cd321x->state.alt = cd321x->port_altmode_dp;
		cd321x->state.data = &dp_data;
		cd321x->state.mode = mode;
		typec_mux_set(cd321x->mux, &cd321x->state);
	} else if (st->data_status & TPS_DATA_STATUS_TBT_CONNECTION) {
		struct typec_thunderbolt_data tbt_data;

		if (cd321x->state.alt == cd321x->port_altmode_tbt &&
		    cd321x->state.mode == TYPEC_TBT_MODE)
			return;

		tbt_data.cable_mode = le16_to_cpu(st->intel_vid_status.cable_mode);
		tbt_data.device_mode = le16_to_cpu(st->intel_vid_status.device_mode);
		tbt_data.enter_vdo = le16_to_cpu(st->intel_vid_status.enter_vdo);
		cd321x->state.alt = cd321x->port_altmode_tbt;
		cd321x->state.mode = TYPEC_TBT_MODE;
		cd321x->state.data = &tbt_data;
		typec_mux_set(cd321x->mux, &cd321x->state);
	} else if (st->data_status & CD321X_DATA_STATUS_USB4_CONNECTION) {
		struct enter_usb_data eusb_data;

		if (cd321x->state.alt == NULL && cd321x->state.mode == TYPEC_MODE_USB4)
			return;

		eusb_data.eudo = le32_to_cpu(st->usb4_status.eudo);
		eusb_data.active_link_training =
			!!(st->data_status & TPS_DATA_STATUS_ACTIVE_LINK_TRAIN);

		cd321x->state.alt = NULL;
		cd321x->state.data = &eusb_data;
		cd321x->state.mode = TYPEC_MODE_USB4;
		typec_mux_set(cd321x->mux, &cd321x->state);
	} else {
		if (cd321x->state.alt == NULL && cd321x->state.mode == TYPEC_STATE_USB)
			return;
		cd321x->state.alt = NULL;
		cd321x->state.mode = TYPEC_STATE_USB;
		cd321x->state.data = NULL;
		typec_mux_set(cd321x->mux, &cd321x->state);
	}

	/* Clear data since it's no longer used after typec_mux_set and points to the stack */
	cd321x->state.data = NULL;
}

static void lp_cd321x_update_work(struct work_struct *work)
{
	struct cd321x *cd321x = container_of(to_delayed_work(work),
					    struct cd321x, update_work);
	struct tps6598x *tps = &cd321x->tps;
	struct cd321x_status st;

	guard(mutex)(&tps->lock);

	st = cd321x->update_status;
	cd321x->update_status.status_changed = 0;

	dev_dbg(tps->dev,
		"j713-lp: status %08x pwr %04x data %08x changed %08x dp_sid mode %02x tx %08x rx %08x conf %08x data %08x\n",
		st.status, st.pwr_status, st.data_status, st.status_changed,
		st.dp_sid_status.mode_status,
		le32_to_cpu(st.dp_sid_status.status_tx),
		le32_to_cpu(st.dp_sid_status.status_rx),
		le32_to_cpu(st.dp_sid_status.configure),
		le32_to_cpu(st.dp_sid_status.mode_data));

	bool old_connected = !!tps->partner;
	bool new_connected = st.status & TPS_STATUS_PLUG_PRESENT;
	bool was_disconnected = st.status_changed & TPS_STATUS_PLUG_PRESENT;

	bool usb_connection = st.data_status &
			      (TPS_DATA_STATUS_USB2_CONNECTION | TPS_DATA_STATUS_USB3_CONNECTION);

	m4_pd_status_trace(tps, new_connected ? 953 : 954);
	m4_pd_status_trace(tps, usb_connection ? 955 : 956);
	enum usb_role old_role = usb_role_switch_get_role(tps->role_sw);
	enum usb_role new_role = USB_ROLE_NONE;
	enum typec_pwr_opmode pwr_opmode = TYPEC_PWR_MODE_USB;
	enum typec_orientation orientation = TYPEC_ORIENTATION_NONE;

	if (usb_connection) {
		if (tps->data_status & TPS_DATA_STATUS_USB_DATA_ROLE)
			new_role = USB_ROLE_DEVICE;
		else
			new_role = USB_ROLE_HOST;
	}

	if (new_connected) {
		pwr_opmode = TPS_POWER_STATUS_PWROPMODE(st.pwr_status);
		orientation = TPS_STATUS_TO_UPSIDE_DOWN(st.status) ?
			TYPEC_ORIENTATION_REVERSE : TYPEC_ORIENTATION_NORMAL;
	}

	bool is_pd = pwr_opmode == TYPEC_PWR_MODE_PD;
	bool partner_changed = old_connected && new_connected &&
		(was_disconnected ||
		 (is_pd && memcmp(&st.partner_identity,
				  &cd321x->cur_partner_identity, sizeof(struct usb_pd_identity))));

	/* If we are switching from an active role, transition to USB_ROLE_NONE first */
	if (old_role != USB_ROLE_NONE && (new_role != old_role || was_disconnected))
		usb_role_switch_set_role(tps->role_sw, USB_ROLE_NONE);

	/* Process partner disconnection or change */
	if (!new_connected || partner_changed) {
		if (!IS_ERR(tps->partner))
			typec_unregister_partner(tps->partner);
		tps->partner = NULL;
	}

	/* If there was a disconnection, set PHY to off */
	if (!new_connected || was_disconnected) {
		cd321x->state.alt = NULL;
		cd321x->state.mode = TYPEC_STATE_SAFE;
		cd321x->state.data = NULL;
		typec_set_mode(tps->port, TYPEC_STATE_SAFE);
	}

	/* Update Type-C properties */
	typec_set_pwr_opmode(tps->port, pwr_opmode);
	typec_set_pwr_role(tps->port, TPS_STATUS_TO_TYPEC_PORTROLE(st.status));
	typec_set_vconn_role(tps->port, TPS_STATUS_TO_TYPEC_VCONN(st.status));
	typec_set_orientation(tps->port, orientation);
	typec_set_data_role(tps->port, TPS_STATUS_TO_TYPEC_DATAROLE(st.status));
	power_supply_changed(tps->psy);

	/* If the plug is disconnected, we are done */
	if (!new_connected)
		return;

	/* Set up partner if we were previously disconnected (or changed). */
	if (!tps->partner) {
		struct typec_partner_desc desc;

		memset(&desc, 0, sizeof(desc));
		desc.usb_pd = is_pd;
		desc.accessory = TYPEC_ACCESSORY_NONE; /* XXX: handle accessories */
		desc.identity = NULL;

		/* FIX: the class keeps this pointer; it must outlive this function. */
		if (desc.usb_pd) {
			cd321x->cur_partner_identity = st.partner_identity;
			desc.identity = &cd321x->cur_partner_identity;
		}

		tps->partner = typec_register_partner(tps->port, &desc);
		if (IS_ERR(tps->partner)) {
			dev_warn(tps->dev, "%s: failed to register partner\n", __func__);
			return;
		}

		if (desc.identity)
			typec_partner_set_identity(tps->partner);
	}

	/* Update the TypeC MUX/PHY state */
	m4_pd_status_trace(tps, 970);
	lp_cd321x_typec_update_mode(tps, &st);
	m4_pd_status_trace(tps, 971);

	/* Launch the USB role switch */
	m4_pd_status_trace(tps, 972);
	usb_role_switch_set_role(tps->role_sw, new_role);
	m4_pd_status_trace(tps, 973);

	power_supply_changed(tps->psy);
}

static struct klp_func funcs[] = {
	{
		.old_name = "cd321x_update_work",
		.new_func = lp_cd321x_update_work,
	}, { }
};

static struct klp_object objs[] = {
	{
		/* name being NULL means vmlinux */
		.funcs = funcs,
	}, { }
};

static struct klp_patch patch = {
	.mod = THIS_MODULE,
	.objs = objs,
};

static int __init j713_tipd_lp_init(void)
{
	if (!of_machine_is_compatible("apple,j713"))
		return -ENODEV;
	const char *bootargs = NULL;

	if (of_chosen)
		of_property_read_string(of_chosen, "bootargs", &bootargs);
	if (!bootargs || strstr(bootargs, "m4bench_usb2_device")) {
		pr_err("j713_tipd_lp: m4bench_usb2_device is set; not patching\n");
		return -EBUSY;
	}
	return klp_enable_patch(&patch);
}

static void __exit j713_tipd_lp_exit(void)
{
}

module_init(j713_tipd_lp_init);
module_exit(j713_tipd_lp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713: fix tipd partner identity lifetime (livepatch)");
MODULE_INFO(livepatch, "Y");

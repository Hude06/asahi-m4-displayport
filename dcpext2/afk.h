/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2022 Sven Peter <sven@svenpeter.dev> */
/* Modified 2026-10-18: native J713 brightness-only AFK transport.
 * Modified 2026-11-07: dcpext0 discovery probe; v2 multi-service/endpoint. */
#ifndef J713_DCPEXT_AFK_H
#define J713_DCPEXT_AFK_H

#include <linux/types.h>

struct device;
struct apple_rtkit;
struct afk_endpoint;

#define J713_AFK_ENDPOINT 0x23
#define J713_DPTX_ENDPOINT 0x2a
#define AFK_MAX_SERVICES 4

typedef void (*afk_inbound_fn)(void *cookie, struct afk_endpoint *ep, int svc,
			       const u8 *data, size_t size);

struct afk_endpoint *afk_init(struct device *dev, struct apple_rtkit *rtk, u8 endpoint,
			    void *cookie, void (*ready)(void *cookie),
			    void (*failed)(void *cookie, int error),
			    afk_inbound_fn inbound);
int afk_start(struct afk_endpoint *ep);
int afk_receive_message(struct afk_endpoint *ep, u64 message);
/* The client must serialize commands, including the initial service open. */
int afk_send_command(struct afk_endpoint *ep, int svc, const void *payload,
		     size_t payload_len, void *output, size_t output_len,
		     u32 *retcode);

const char *afk_service_name(struct afk_endpoint *ep);
int afk_service_count(struct afk_endpoint *ep);
const char *afk_service_name_at(struct afk_endpoint *ep, int i);
u32 afk_service_channel(struct afk_endpoint *ep, int i);
int afk_send_raw(struct afk_endpoint *ep, const u8 *data, size_t size);

#endif

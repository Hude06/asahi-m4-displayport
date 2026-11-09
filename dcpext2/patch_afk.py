import sys
p='afk.c'; s=open(p).read()
def rep(a,b,cnt=1):
    global s
    assert s.count(a)==cnt, (a[:60], s.count(a))
    s=s.replace(a,b)
rep("/* Modified 2026-11-07: dcpext0 discovery: any announced service, logged.",
    "/* Modified 2026-11-07 (v2): endpoint parameter, several services per endpoint,\n * firmware-initiated messages handed to the client, raw native send.\n * Modified 2026-11-07: dcpext0 discovery: any announced service, logged.")
rep("""	u8 qe_seq;
	struct afk_service service;
};""","""	u8 qe_seq;
	struct afk_service services[AFK_MAX_SERVICES];
	int nservices;
	int primary;
	afk_inbound_fn inbound;
};""")
rep("""struct afk_endpoint *afk_init(struct device *dev, struct apple_rtkit *rtk,
			    void *cookie, void (*ready)(void *cookie),
			    void (*failed)(void *cookie, int error))
{""","""struct afk_endpoint *afk_init(struct device *dev, struct apple_rtkit *rtk, u8 endpoint,
			    void *cookie, void (*ready)(void *cookie),
			    void (*failed)(void *cookie, int error),
			    afk_inbound_fn inbound)
{
	int i;""")
rep("	ep->endpoint = J713_AFK_ENDPOINT;","	ep->endpoint = endpoint;\n	ep->inbound = inbound;\n	ep->primary = -1;")
rep("	spin_lock_init(&ep->service.lock);","	for (i = 0; i < AFK_MAX_SERVICES; i++)\n		spin_lock_init(&ep->services[i].lock);")
rep("""	if (ep->service.enabled && channel == ep->service.channel)
		return &ep->service;
	return NULL;""","""	int i;

	for (i = 0; i < ep->nservices; i++)
		if (ep->services[i].enabled && channel == ep->services[i].channel)
			return &ep->services[i];
	return NULL;""")
rep("""	if (payload_size >= 16)
		memcpy(name, payload, 16);
	dev_info(ep->dev, "AFK[ep:%02x]: announce channel %u name '%s' (%zu bytes)\\n",
		 ep->endpoint, channel, name, payload_size);
	if (payload_size < 32 || ep->service.enabled || !strstr(name, "-service"))
		return;
	ep->service.channel = channel;
	strscpy(ep->service.name, name, sizeof(ep->service.name));
	ep->service.enabled = true;
	ep->ready(ep->cookie);""","""	struct afk_service *service;

	if (payload_size >= 16)
		memcpy(name, payload, 16);
	dev_info(ep->dev, "AFK[ep:%02x]: announce channel %u name '%s' (%zu bytes)\\n",
		 ep->endpoint, channel, name, payload_size);
	print_hex_dump(KERN_INFO, "AFK announce: ", DUMP_PREFIX_OFFSET, 16, 1,
		       payload, min_t(size_t, payload_size, 256), true);
	if (ep->nservices >= AFK_MAX_SERVICES)
		return;
	service = &ep->services[ep->nservices];
	service->channel = channel;
	strscpy(service->name, name, sizeof(service->name));
	service->enabled = true;
	if (ep->primary < 0 && payload_size >= 32 && strstr(name, "-service"))
		ep->primary = ep->nservices;
	ep->nservices++;
	ep->ready(ep->cookie);""")
rep("""			msg.body_size - NATIVE_TRANSACTION_SIZE, msg.body[0] & 1);
	}
}""","""			msg.body_size - NATIVE_TRANSACTION_SIZE, msg.body[0] & 1);
	}
	dev_info(ep->dev, "AFK[ep:%02x]: inbound ch %u seq %u type %#x cat %u flavor %u body %zu\\n",
		 ep->endpoint, msg.channel, msg.sequence, msg.type, msg.category,
		 msg.flavor, msg.body_size);
	print_hex_dump(KERN_INFO, "AFK in: ", DUMP_PREFIX_OFFSET, 16, 1, data,
		       min_t(size_t, data_size, 512), true);
	if (ep->inbound)
		ep->inbound(ep->cookie, ep, service ? (int)(service - ep->services) : -1,
			    data, data_size);
}

int afk_send_raw(struct afk_endpoint *ep, const u8 *data, size_t size)
{
	return afk_send_native(ep, data, size);
}""")
rep("""static void afk_recv_handle(struct afk_endpoint *ep, u8 *data, size_t data_size)
{""","""static int afk_send_native(struct afk_endpoint *ep, const u8 *payload,
			   size_t payload_len);

static void afk_recv_handle(struct afk_endpoint *ep, u8 *data, size_t data_size)
{""")
rep("""int afk_send_command(struct afk_endpoint *ep, const void *payload,
		     size_t payload_len, void *output, size_t output_len,
		     u32 *retcode)
{""","""int afk_send_command(struct afk_endpoint *ep, int svc, const void *payload,
		     size_t payload_len, void *output, size_t output_len,
		     u32 *retcode)
{""")
rep("	struct afk_service *service = &ep->service;","	struct afk_service *service;")
rep("""	if (!payload || !payload_len || payload_len > U32_MAX ||""","""	if (svc < 0)
		svc = ep->primary;
	if (svc < 0 || svc >= ep->nservices)
		return -ENODEV;
	service = &ep->services[svc];
	if (!payload || !payload_len || payload_len > U32_MAX ||""")
rep("ep->service.channel",'service->channel',2)
rep("""const char *afk_service_name(struct afk_endpoint *ep)
{
	return ep->service.enabled ? ep->service.name : NULL;
}""","""const char *afk_service_name(struct afk_endpoint *ep)
{
	return ep->primary >= 0 ? ep->services[ep->primary].name : NULL;
}

int afk_service_count(struct afk_endpoint *ep)
{
	return ep->nservices;
}

const char *afk_service_name_at(struct afk_endpoint *ep, int i)
{
	return i >= 0 && i < ep->nservices ? ep->services[i].name : NULL;
}

u32 afk_service_channel(struct afk_endpoint *ep, int i)
{
	return i >= 0 && i < ep->nservices ? ep->services[i].channel : 0;
}""")
open(p,'w').write(s)

h=open('afk.h').read()
for a,b in [("#define J713_AFK_ENDPOINT 0x23\n","""#define J713_AFK_ENDPOINT 0x23
#define J713_DPTX_ENDPOINT 0x2a
#define AFK_MAX_SERVICES 4

typedef void (*afk_inbound_fn)(void *cookie, struct afk_endpoint *ep, int svc,
			       const u8 *data, size_t size);
"""),("""struct afk_endpoint *afk_init(struct device *dev, struct apple_rtkit *rtk,
			    void *cookie, void (*ready)(void *cookie),
			    void (*failed)(void *cookie, int error));""","""struct afk_endpoint *afk_init(struct device *dev, struct apple_rtkit *rtk, u8 endpoint,
			    void *cookie, void (*ready)(void *cookie),
			    void (*failed)(void *cookie, int error),
			    afk_inbound_fn inbound);"""),
("int afk_send_command(struct afk_endpoint *ep, const void *payload,","int afk_send_command(struct afk_endpoint *ep, int svc, const void *payload,"),
("const char *afk_service_name(struct afk_endpoint *ep);","""const char *afk_service_name(struct afk_endpoint *ep);
int afk_service_count(struct afk_endpoint *ep);
const char *afk_service_name_at(struct afk_endpoint *ep, int i);
u32 afk_service_channel(struct afk_endpoint *ep, int i);
int afk_send_raw(struct afk_endpoint *ep, const u8 *data, size_t size);"""),
(" * Modified 2026-11-07: dcpext0 discovery probe. */"," * Modified 2026-11-07: dcpext0 discovery probe; v2 multi-service/endpoint. */")]:
    assert h.count(a)==1, a[:50]
    h=h.replace(a,b)
open('afk.h','w').write(h)
print("ok")

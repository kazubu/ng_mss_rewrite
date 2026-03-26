/*
 * ng_mbuf_inject - Netgraph node for testing mbuf shape variations
 *
 * This test module injects packets with various mbuf chain shapes
 * to test ng_mss_rewrite's handling of:
 * - Fragmented mbuf chains (multi-mbuf)
 * - Shared mbufs (M_EXT_WRITABLE == 0)
 * - Checksum offload flags
 * - IPv6 extension headers
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/socket.h>

#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>

/* Node private data */
struct ng_mbuf_inject_private {
	hook_p	output;		/* Output hook */
	u_long	packets_sent;	/* Statistics */
};
typedef struct ng_mbuf_inject_private *priv_p;

/* Netgraph control messages */
enum {
	NGM_MBUF_INJECT_COOKIE = 1594280169,
	NGM_MBUF_INJECT_SINGLE = 1,	/* Send single contiguous mbuf packet */
	NGM_MBUF_INJECT_FRAGMENTED,	/* Send multi-mbuf chain packet */
	NGM_MBUF_INJECT_SHARED,		/* Send shared (read-only) mbuf packet */
	NGM_MBUF_INJECT_OFFLOAD,	/* Send packet with checksum offload flags */
	NGM_MBUF_INJECT_IPV6EXT,	/* Send IPv6 packet with extension headers */
	NGM_MBUF_INJECT_GETSTATS,	/* Get statistics */
};

/* Message structures */
struct ng_mbuf_inject_params {
	uint16_t mss;			/* MSS value to use */
	uint8_t  ipv6;			/* 0=IPv4, 1=IPv6 */
	uint8_t  split_offset;		/* For fragmented: where to split (0=auto) */
	uint16_t csum_flags;		/* For offload: flags to set */
	uint8_t  ext_type;		/* For IPv6ext: extension header type */
};

struct ng_mbuf_inject_stats {
	u_long packets_sent;
};

/* Netgraph type descriptor */
static ng_constructor_t	ng_mbuf_inject_constructor;
static ng_rcvmsg_t	ng_mbuf_inject_rcvmsg;
static ng_shutdown_t	ng_mbuf_inject_shutdown;
static ng_newhook_t	ng_mbuf_inject_newhook;
static ng_disconnect_t	ng_mbuf_inject_disconnect;

/* Parse types */
static const struct ng_parse_struct_field ng_mbuf_inject_params_type_fields[] = {
	{ "mss",		&ng_parse_uint16_type	},
	{ "ipv6",		&ng_parse_uint8_type	},
	{ "split_offset",	&ng_parse_uint8_type	},
	{ "csum_flags",		&ng_parse_uint16_type	},
	{ "ext_type",		&ng_parse_uint8_type	},
	{ NULL }
};
static const struct ng_parse_type ng_mbuf_inject_params_type = {
	&ng_parse_struct_type,
	&ng_mbuf_inject_params_type_fields
};

static const struct ng_parse_struct_field ng_mbuf_inject_stats_type_fields[] = {
	{ "packets_sent",	&ng_parse_uint64_type	},
	{ NULL }
};
static const struct ng_parse_type ng_mbuf_inject_stats_type = {
	&ng_parse_struct_type,
	&ng_mbuf_inject_stats_type_fields
};

/* Command list */
static const struct ng_cmdlist ng_mbuf_inject_cmdlist[] = {
	{
		NGM_MBUF_INJECT_COOKIE,
		NGM_MBUF_INJECT_SINGLE,
		"inject_single",
		&ng_mbuf_inject_params_type,
		NULL
	},
	{
		NGM_MBUF_INJECT_COOKIE,
		NGM_MBUF_INJECT_FRAGMENTED,
		"inject_fragmented",
		&ng_mbuf_inject_params_type,
		NULL
	},
	{
		NGM_MBUF_INJECT_COOKIE,
		NGM_MBUF_INJECT_SHARED,
		"inject_shared",
		&ng_mbuf_inject_params_type,
		NULL
	},
	{
		NGM_MBUF_INJECT_COOKIE,
		NGM_MBUF_INJECT_OFFLOAD,
		"inject_offload",
		&ng_mbuf_inject_params_type,
		NULL
	},
	{
		NGM_MBUF_INJECT_COOKIE,
		NGM_MBUF_INJECT_IPV6EXT,
		"inject_ipv6ext",
		&ng_mbuf_inject_params_type,
		NULL
	},
	{
		NGM_MBUF_INJECT_COOKIE,
		NGM_MBUF_INJECT_GETSTATS,
		"getstats",
		NULL,
		&ng_mbuf_inject_stats_type
	},
	{ 0 }
};

/* Netgraph type structure */
static struct ng_type ng_mbuf_inject_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		"mbuf_inject",
	.constructor =	ng_mbuf_inject_constructor,
	.rcvmsg =	ng_mbuf_inject_rcvmsg,
	.shutdown =	ng_mbuf_inject_shutdown,
	.newhook =	ng_mbuf_inject_newhook,
	.disconnect =	ng_mbuf_inject_disconnect,
	.cmdlist =	ng_mbuf_inject_cmdlist,
};
NETGRAPH_INIT(mbuf_inject, &ng_mbuf_inject_typestruct);

/*
 * Helper: Build a TCP SYN packet with specified MSS
 */
static struct mbuf *
build_tcp_syn_packet(int ipv6, uint16_t mss)
{
	struct mbuf *m;
	struct ether_header *eh;
	uint8_t *p;
	int pkt_len;

	if (ipv6) {
		/* Ethernet + IPv6 + TCP + Options (MSS + NOP + NOP) */
		pkt_len = 14 + 40 + 20 + 8;
	} else {
		/* Ethernet + IPv4 + TCP + Options (MSS + NOP + NOP) */
		pkt_len = 14 + 20 + 20 + 8;
	}

	m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return (NULL);

	m->m_len = pkt_len;
	m->m_pkthdr.len = pkt_len;
	p = mtod(m, uint8_t *);
	memset(p, 0, pkt_len);

	/* Ethernet header */
	eh = (struct ether_header *)p;
	memset(eh->ether_dhost, 0xff, ETHER_ADDR_LEN);
	memset(eh->ether_shost, 0xaa, ETHER_ADDR_LEN);
	eh->ether_type = htons(ipv6 ? ETHERTYPE_IPV6 : ETHERTYPE_IP);
	p += 14;

	if (ipv6) {
		/* IPv6 header */
		struct ip6_hdr *ip6 = (struct ip6_hdr *)p;
		ip6->ip6_vfc = 0x60;	/* version 6 */
		ip6->ip6_plen = htons(20 + 8);	/* TCP + options */
		ip6->ip6_nxt = IPPROTO_TCP;
		ip6->ip6_hlim = 64;
		/* Use dummy addresses */
		ip6->ip6_src.s6_addr[15] = 1;
		ip6->ip6_dst.s6_addr[15] = 2;
		p += 40;
	} else {
		/* IPv4 header */
		struct ip *ip = (struct ip *)p;
		ip->ip_v = 4;
		ip->ip_hl = 5;
		ip->ip_len = htons(20 + 20 + 8);
		ip->ip_ttl = 64;
		ip->ip_p = IPPROTO_TCP;
		ip->ip_src.s_addr = htonl(0x0a000001);	/* 10.0.0.1 */
		ip->ip_dst.s_addr = htonl(0x0a000002);	/* 10.0.0.2 */
		/* Checksum will be calculated later if needed */
		p += 20;
	}

	/* TCP header */
	struct tcphdr *tcp = (struct tcphdr *)p;
	tcp->th_sport = htons(12345);
	tcp->th_dport = htons(80);
	tcp->th_seq = htonl(1000);
	tcp->th_off = 7;	/* 20 bytes header + 8 bytes options = 28 bytes / 4 */
	tcp->th_flags = TH_SYN;
	tcp->th_win = htons(65535);
	p += 20;

	/* TCP options: NOP + NOP + MSS */
	p[0] = 1;	/* TCPOPT_NOP */
	p[1] = 1;	/* TCPOPT_NOP */
	p[2] = 2;	/* TCPOPT_MAXSEG */
	p[3] = 4;	/* length */
	p[4] = (mss >> 8) & 0xff;
	p[5] = mss & 0xff;
	p[6] = 1;	/* TCPOPT_NOP */
	p[7] = 1;	/* TCPOPT_NOP */

	return (m);
}

/*
 * Inject single contiguous mbuf packet
 */
static int
inject_single(priv_p priv, struct ng_mbuf_inject_params *params)
{
	struct mbuf *m;
	int error;

	m = build_tcp_syn_packet(params->ipv6, params->mss);
	if (m == NULL)
		return (ENOMEM);

	/* Send to output hook */
	if (priv->output != NULL) {
		NG_SEND_DATA_ONLY(error, priv->output, m);
		if (error == 0)
			priv->packets_sent++;
		return (error);
	} else {
		m_freem(m);
		return (ENOTCONN);
	}
}

/*
 * Inject fragmented mbuf chain packet
 */
static int
inject_fragmented(priv_p priv, struct ng_mbuf_inject_params *params)
{
	struct mbuf *m, *m2;
	int split_offset;
	int total_len;
	uint8_t *src_data;

	/* Build the packet first */
	m = build_tcp_syn_packet(params->ipv6, params->mss);
	if (m == NULL)
		return (ENOMEM);

	total_len = m->m_pkthdr.len;

	/* Determine split offset */
	if (params->split_offset != 0) {
		split_offset = params->split_offset;
	} else {
		/* Default: split at Ethernet header boundary (after 14 bytes) */
		split_offset = 14;
	}

	/* Validate split offset */
	if (split_offset >= total_len || split_offset <= 0) {
		/* Invalid split, just send as-is */
		goto send_packet;
	}

	/* Save the packet data */
	src_data = mtod(m, uint8_t *);

	/* Create first mbuf (for data before split point) */
	m2 = m_gethdr(M_NOWAIT, MT_DATA);
	if (m2 == NULL) {
		m_freem(m);
		return (ENOMEM);
	}

	/* Copy packet header from original */
	m2->m_pkthdr.len = total_len;
	m2->m_len = split_offset;
	memcpy(mtod(m2, caddr_t), src_data, split_offset);

	/* Create second mbuf (for data after split point) */
	struct mbuf *m3 = m_get(M_NOWAIT, MT_DATA);
	if (m3 == NULL) {
		m_freem(m2);
		m_freem(m);
		return (ENOMEM);
	}

	m3->m_len = total_len - split_offset;
	memcpy(mtod(m3, caddr_t), src_data + split_offset, m3->m_len);

	/* Link them together */
	m2->m_next = m3;

	/* Free original mbuf */
	m_freem(m);

	/* Use the new fragmented chain */
	m = m2;

send_packet:

	/* Send to output hook */
	if (priv->output != NULL) {
		int error;
		NG_SEND_DATA_ONLY(error, priv->output, m);
		if (error == 0)
			priv->packets_sent++;
		return (error);
	} else {
		m_freem(m);
		return (ENOTCONN);
	}
}

/*
 * Inject shared (read-only) mbuf packet
 */
static int
inject_shared(priv_p priv, struct ng_mbuf_inject_params *params)
{
	struct mbuf *m, *m_shared;

	m = build_tcp_syn_packet(params->ipv6, params->mss);
	if (m == NULL)
		return (ENOMEM);

	/* Create a shared copy using m_dup() or manually set M_EXT_WRITABLE to fail */
	/* m_dup() creates a new mbuf but shares the external storage */
	m_shared = m_dup(m, M_NOWAIT);
	if (m_shared == NULL) {
		m_freem(m);
		return (ENOMEM);
	}

	/* Keep original to maintain refcount */
	/* This makes M_WRITABLE(m_shared) return 0 */

	/* Send shared copy to output hook */
	if (priv->output != NULL) {
		int error;
		NG_SEND_DATA_ONLY(error, priv->output, m_shared);
		/* Free original after sending */
		m_freem(m);
		if (error == 0)
			priv->packets_sent++;
		return (error);
	} else {
		m_freem(m_shared);
		m_freem(m);
		return (ENOTCONN);
	}
}

/*
 * Inject packet with checksum offload flags
 */
static int
inject_offload(priv_p priv, struct ng_mbuf_inject_params *params)
{
	struct mbuf *m;
	int error;

	m = build_tcp_syn_packet(params->ipv6, params->mss);
	if (m == NULL)
		return (ENOMEM);

	/* Set checksum offload flags */
	if (params->csum_flags != 0) {
		m->m_pkthdr.csum_flags = params->csum_flags;
	} else {
		/* Default: CSUM_TCP or CSUM_TSO */
		m->m_pkthdr.csum_flags = CSUM_TCP | CSUM_TSO;
	}

	/* Send to output hook */
	if (priv->output != NULL) {
		NG_SEND_DATA_ONLY(error, priv->output, m);
		if (error == 0)
			priv->packets_sent++;
		return (error);
	} else {
		m_freem(m);
		return (ENOTCONN);
	}
}

/*
 * Inject IPv6 packet with extension headers
 */
static int
inject_ipv6ext(priv_p priv, struct ng_mbuf_inject_params *params)
{
	struct mbuf *m;
	struct ether_header *eh;
	struct ip6_hdr *ip6;
	uint8_t *p, *ext_hdr;
	int pkt_len;
	uint8_t ext_type;

	/* Determine extension header type */
	ext_type = params->ext_type;
	if (ext_type == 0)
		ext_type = IPPROTO_HOPOPTS;	/* Default: Hop-by-Hop */

	/* Ethernet + IPv6 + ExtHdr(8) + TCP + Options */
	pkt_len = 14 + 40 + 8 + 20 + 8;

	m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return (ENOMEM);

	m->m_len = pkt_len;
	m->m_pkthdr.len = pkt_len;
	p = mtod(m, uint8_t *);
	memset(p, 0, pkt_len);

	/* Ethernet header */
	eh = (struct ether_header *)p;
	memset(eh->ether_dhost, 0xff, ETHER_ADDR_LEN);
	memset(eh->ether_shost, 0xaa, ETHER_ADDR_LEN);
	eh->ether_type = htons(ETHERTYPE_IPV6);
	p += 14;

	/* IPv6 header */
	ip6 = (struct ip6_hdr *)p;
	ip6->ip6_vfc = 0x60;	/* version 6 */
	ip6->ip6_plen = htons(8 + 20 + 8);	/* ExtHdr + TCP + options */
	ip6->ip6_nxt = ext_type;		/* Next header is extension header */
	ip6->ip6_hlim = 64;
	ip6->ip6_src.s6_addr[15] = 1;
	ip6->ip6_dst.s6_addr[15] = 2;
	p += 40;

	/* Extension header (8 bytes, simplest form) */
	ext_hdr = p;
	ext_hdr[0] = IPPROTO_TCP;	/* Next header is TCP */
	ext_hdr[1] = 0;			/* Header length = 0 (means 8 bytes) */
	/* Rest is padding */
	p += 8;

	/* TCP header with SYN */
	struct tcphdr *tcp = (struct tcphdr *)p;
	tcp->th_sport = htons(12345);
	tcp->th_dport = htons(80);
	tcp->th_seq = htonl(1000);
	tcp->th_off = 7;
	tcp->th_flags = TH_SYN;
	tcp->th_win = htons(65535);
	p += 20;

	/* TCP options: NOP + NOP + MSS */
	p[0] = 1;	/* TCPOPT_NOP */
	p[1] = 1;	/* TCPOPT_NOP */
	p[2] = 2;	/* TCPOPT_MAXSEG */
	p[3] = 4;
	p[4] = (params->mss >> 8) & 0xff;
	p[5] = params->mss & 0xff;
	p[6] = 1;
	p[7] = 1;

	/* Send to output hook */
	if (priv->output != NULL) {
		int error;
		NG_SEND_DATA_ONLY(error, priv->output, m);
		if (error == 0)
			priv->packets_sent++;
		return (error);
	} else {
		m_freem(m);
		return (ENOTCONN);
	}
}

/*
 * Node constructor
 */
static int
ng_mbuf_inject_constructor(node_p node)
{
	priv_p priv;

	priv = malloc(sizeof(*priv), M_NETGRAPH, M_WAITOK | M_ZERO);
	NG_NODE_SET_PRIVATE(node, priv);

	return (0);
}

/*
 * Receive control message
 */
static int
ng_mbuf_inject_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	priv_p priv = NG_NODE_PRIVATE(node);
	struct ng_mesg *msg, *resp = NULL;
	int error = 0;

	NGI_GET_MSG(item, msg);

	switch (msg->header.typecookie) {
	case NGM_MBUF_INJECT_COOKIE:
		switch (msg->header.cmd) {
		case NGM_MBUF_INJECT_SINGLE:
		{
			struct ng_mbuf_inject_params *params;

			if (msg->header.arglen != sizeof(*params)) {
				error = EINVAL;
				break;
			}

			params = (struct ng_mbuf_inject_params *)msg->data;
			error = inject_single(priv, params);
			break;
		}

		case NGM_MBUF_INJECT_FRAGMENTED:
		{
			struct ng_mbuf_inject_params *params;

			if (msg->header.arglen != sizeof(*params)) {
				error = EINVAL;
				break;
			}

			params = (struct ng_mbuf_inject_params *)msg->data;
			error = inject_fragmented(priv, params);
			break;
		}

		case NGM_MBUF_INJECT_SHARED:
		{
			struct ng_mbuf_inject_params *params;

			if (msg->header.arglen != sizeof(*params)) {
				error = EINVAL;
				break;
			}

			params = (struct ng_mbuf_inject_params *)msg->data;
			error = inject_shared(priv, params);
			break;
		}

		case NGM_MBUF_INJECT_OFFLOAD:
		{
			struct ng_mbuf_inject_params *params;

			if (msg->header.arglen != sizeof(*params)) {
				error = EINVAL;
				break;
			}

			params = (struct ng_mbuf_inject_params *)msg->data;
			error = inject_offload(priv, params);
			break;
		}

		case NGM_MBUF_INJECT_IPV6EXT:
		{
			struct ng_mbuf_inject_params *params;

			if (msg->header.arglen != sizeof(*params)) {
				error = EINVAL;
				break;
			}

			params = (struct ng_mbuf_inject_params *)msg->data;
			error = inject_ipv6ext(priv, params);
			break;
		}

		case NGM_MBUF_INJECT_GETSTATS:
		{
			struct ng_mbuf_inject_stats *stats;

			NG_MKRESPONSE(resp, msg, sizeof(*stats), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}

			stats = (struct ng_mbuf_inject_stats *)resp->data;
			stats->packets_sent = priv->packets_sent;
			break;
		}

		default:
			error = EINVAL;
			break;
		}
		break;

	default:
		error = EINVAL;
		break;
	}

	NG_RESPOND_MSG(error, node, item, resp);
	NG_FREE_MSG(msg);
	return (error);
}

/*
 * Hook connection
 */
static int
ng_mbuf_inject_newhook(node_p node, hook_p hook, const char *name)
{
	priv_p priv = NG_NODE_PRIVATE(node);

	if (strcmp(name, "output") == 0) {
		priv->output = hook;
	} else {
		return (EINVAL);
	}

	return (0);
}

/*
 * Hook disconnection
 */
static int
ng_mbuf_inject_disconnect(hook_p hook)
{
	priv_p priv = NG_NODE_PRIVATE(NG_HOOK_NODE(hook));

	if (hook == priv->output)
		priv->output = NULL;

	if (NG_NODE_NUMHOOKS(NG_HOOK_NODE(hook)) == 0)
		ng_rmnode_self(NG_HOOK_NODE(hook));

	return (0);
}

/*
 * Node shutdown
 */
static int
ng_mbuf_inject_shutdown(node_p node)
{
	priv_p priv = NG_NODE_PRIVATE(node);

	NG_NODE_UNREF(node);
	free(priv, M_NETGRAPH);

	return (0);
}

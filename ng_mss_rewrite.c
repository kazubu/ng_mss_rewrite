/*
 * ng_mss_rewrite.c
 *
 * Netgraph node for rewriting TCP MSS option in SYN packets.
 * Supports both IPv4 and IPv6, handles VLAN tagged frames.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/errno.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_vlan_var.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>

/* Node type name and cookie */
#define NG_MSS_REWRITE_NODE_TYPE	"mss_rewrite"
#define NGM_MSS_REWRITE_COOKIE		1234567890

/* Hook names */
#define NG_MSS_REWRITE_HOOK_LOWER	"lower"
#define NG_MSS_REWRITE_HOOK_UPPER	"upper"

/* Default MSS values */
#define DEFAULT_MSS_IPV4	1400
#define DEFAULT_MSS_IPV6	1380

/* Private node data */
struct ng_mss_rewrite_private {
	hook_p		lower;		/* Connection to physical interface */
	hook_p		upper;		/* Connection to kernel stack */
	uint16_t	mss_ipv4;	/* MSS limit for IPv4 */
	uint16_t	mss_ipv6;	/* MSS limit for IPv6 */
	uint64_t	packets_processed;
	uint64_t	packets_rewritten;
};
typedef struct ng_mss_rewrite_private *priv_p;

/* Netgraph control messages */
enum {
	NGM_MSS_REWRITE_SET_MSS = 1,
	NGM_MSS_REWRITE_GET_MSS,
	NGM_MSS_REWRITE_GET_STATS,
	NGM_MSS_REWRITE_RESET_STATS,
};

/* Control message structures */
struct ng_mss_rewrite_conf {
	uint16_t	mss_ipv4;
	uint16_t	mss_ipv6;
};

struct ng_mss_rewrite_stats {
	uint64_t	packets_processed;
	uint64_t	packets_rewritten;
};

/* Parse type for config structure */
static const struct ng_parse_struct_field ng_mss_rewrite_conf_fields[] = {
	{ "mss_ipv4",	&ng_parse_uint16_type },
	{ "mss_ipv6",	&ng_parse_uint16_type },
	{ NULL }
};
static const struct ng_parse_type ng_mss_rewrite_conf_type = {
	&ng_parse_struct_type,
	&ng_mss_rewrite_conf_fields
};

/* Parse type for stats structure */
static const struct ng_parse_struct_field ng_mss_rewrite_stats_fields[] = {
	{ "packets_processed",	&ng_parse_uint64_type },
	{ "packets_rewritten",	&ng_parse_uint64_type },
	{ NULL }
};
static const struct ng_parse_type ng_mss_rewrite_stats_type = {
	&ng_parse_struct_type,
	&ng_mss_rewrite_stats_fields
};

/* List of commands and how to convert arguments to/from ASCII */
static const struct ng_cmdlist ng_mss_rewrite_cmdlist[] = {
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_SET_MSS,
		"setmss",
		&ng_mss_rewrite_conf_type,
		NULL
	},
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_GET_MSS,
		"getmss",
		NULL,
		&ng_mss_rewrite_conf_type
	},
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_GET_STATS,
		"getstats",
		NULL,
		&ng_mss_rewrite_stats_type
	},
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_RESET_STATS,
		"resetstats",
		NULL,
		NULL
	},
	{ 0 }
};

/* Netgraph node method forward declarations */
static ng_constructor_t	ng_mss_rewrite_constructor;
static ng_rcvmsg_t	ng_mss_rewrite_rcvmsg;
static ng_shutdown_t	ng_mss_rewrite_shutdown;
static ng_newhook_t	ng_mss_rewrite_newhook;
static ng_rcvdata_t	ng_mss_rewrite_rcvdata;
static ng_disconnect_t	ng_mss_rewrite_disconnect;

/* Netgraph node type descriptor */
static struct ng_type ng_mss_rewrite_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		NG_MSS_REWRITE_NODE_TYPE,
	.constructor =	ng_mss_rewrite_constructor,
	.rcvmsg =	ng_mss_rewrite_rcvmsg,
	.shutdown =	ng_mss_rewrite_shutdown,
	.newhook =	ng_mss_rewrite_newhook,
	.rcvdata =	ng_mss_rewrite_rcvdata,
	.disconnect =	ng_mss_rewrite_disconnect,
	.cmdlist =	ng_mss_rewrite_cmdlist,
};
NETGRAPH_INIT(mss_rewrite, &ng_mss_rewrite_typestruct);

/* Helper function to calculate IP checksum */
static uint16_t
ip_checksum(uint16_t *buf, int len)
{
	uint32_t sum = 0;

	while (len > 1) {
		sum += *buf++;
		len -= 2;
	}

	if (len == 1)
		sum += *(uint8_t *)buf;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (~sum);
}

/* Helper function to calculate TCP checksum */
static uint16_t
tcp_checksum_ipv4(struct ip *ip, struct tcphdr *tcp, int tcplen)
{
	uint32_t sum = 0;
	uint16_t *buf;
	int len;

	/* Pseudo header */
	sum += (ip->ip_src.s_addr >> 16) & 0xffff;
	sum += ip->ip_src.s_addr & 0xffff;
	sum += (ip->ip_dst.s_addr >> 16) & 0xffff;
	sum += ip->ip_dst.s_addr & 0xffff;
	sum += htons(IPPROTO_TCP);
	sum += htons(tcplen);

	/* TCP header and data */
	buf = (uint16_t *)tcp;
	len = tcplen;

	while (len > 1) {
		sum += *buf++;
		len -= 2;
	}

	if (len == 1)
		sum += *(uint8_t *)buf;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (~sum);
}

/* Helper function to calculate TCP checksum for IPv6 */
static uint16_t
tcp_checksum_ipv6(struct ip6_hdr *ip6, struct tcphdr *tcp, int tcplen)
{
	uint32_t sum = 0;
	uint16_t *buf;
	int len;
	int i;

	/* Pseudo header */
	buf = (uint16_t *)&ip6->ip6_src;
	for (i = 0; i < 16; i++)
		sum += *buf++;

	sum += htons(IPPROTO_TCP);
	sum += htons(tcplen);

	/* TCP header and data */
	buf = (uint16_t *)tcp;
	len = tcplen;

	while (len > 1) {
		sum += *buf++;
		len -= 2;
	}

	if (len == 1)
		sum += *(uint8_t *)buf;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (~sum);
}

/* Process and possibly rewrite MSS in a packet */
static int
ng_mss_rewrite_process(priv_p priv, struct mbuf *m)
{
	struct ether_header *eh;
	struct ip *ip4 = NULL;
	struct ip6_hdr *ip6 = NULL;
	struct tcphdr *tcp;
	uint8_t *options;
	uint16_t ether_type;
	int ip_hlen, tcp_hlen, opt_len, i;
	int offset = 0;
	uint16_t max_mss;
	int rewritten = 0;

	/* Ensure we have enough data */
	if (m->m_pkthdr.len < sizeof(struct ether_header))
		return (0);

	/* Make sure the Ethernet header is contiguous */
	if (m->m_len < sizeof(struct ether_header)) {
		m = m_pullup(m, sizeof(struct ether_header));
		if (m == NULL)
			return (0);
	}

	eh = mtod(m, struct ether_header *);
	ether_type = ntohs(eh->ether_type);
	offset = sizeof(struct ether_header);

	/* Handle VLAN tag */
	if (ether_type == ETHERTYPE_VLAN) {
		if (m->m_pkthdr.len < offset + 4)
			return (0);

		if (m->m_len < offset + 4) {
			m = m_pullup(m, offset + 4);
			if (m == NULL)
				return (0);
		}

		ether_type = ntohs(*(uint16_t *)(mtod(m, uint8_t *) + offset + 2));
		offset += 4;
	}

	/* Check if it's IP */
	if (ether_type == ETHERTYPE_IP) {
		/* IPv4 */
		max_mss = priv->mss_ipv4;

		if (m->m_pkthdr.len < offset + sizeof(struct ip))
			return (0);

		if (m->m_len < offset + sizeof(struct ip)) {
			m = m_pullup(m, offset + sizeof(struct ip));
			if (m == NULL)
				return (0);
		}

		ip4 = (struct ip *)(mtod(m, uint8_t *) + offset);
		ip_hlen = ip4->ip_hl << 2;

		/* Check if it's TCP */
		if (ip4->ip_p != IPPROTO_TCP)
			return (0);

		offset += ip_hlen;

	} else if (ether_type == ETHERTYPE_IPV6) {
		/* IPv6 */
		max_mss = priv->mss_ipv6;

		if (m->m_pkthdr.len < offset + sizeof(struct ip6_hdr))
			return (0);

		if (m->m_len < offset + sizeof(struct ip6_hdr)) {
			m = m_pullup(m, offset + sizeof(struct ip6_hdr));
			if (m == NULL)
				return (0);
		}

		ip6 = (struct ip6_hdr *)(mtod(m, uint8_t *) + offset);

		/* Check if it's TCP (simplified, not handling extension headers) */
		if (ip6->ip6_nxt != IPPROTO_TCP)
			return (0);

		offset += sizeof(struct ip6_hdr);
		ip_hlen = sizeof(struct ip6_hdr);

	} else {
		/* Not IP */
		return (0);
	}

	/* Check TCP header */
	if (m->m_pkthdr.len < offset + sizeof(struct tcphdr))
		return (0);

	if (m->m_len < offset + sizeof(struct tcphdr)) {
		m = m_pullup(m, offset + sizeof(struct tcphdr));
		if (m == NULL)
			return (0);
	}

	tcp = (struct tcphdr *)(mtod(m, uint8_t *) + offset);
	tcp_hlen = tcp->th_off << 2;

	/* Check if it's a SYN packet */
	if (!(tcp->th_flags & TH_SYN))
		return (0);

	/* Make sure we have the full TCP header with options */
	if (m->m_pkthdr.len < offset + tcp_hlen)
		return (0);

	if (m->m_len < offset + tcp_hlen) {
		m = m_pullup(m, offset + tcp_hlen);
		if (m == NULL)
			return (0);

		/* Recalculate pointers after m_pullup */
		if (ip4)
			ip4 = (struct ip *)(mtod(m, uint8_t *) + offset - ip_hlen);
		if (ip6)
			ip6 = (struct ip6_hdr *)(mtod(m, uint8_t *) + offset - ip_hlen);
		tcp = (struct tcphdr *)(mtod(m, uint8_t *) + offset);
	}

	/* Make mbuf writable */
	if (M_WRITABLE(m) == 0) {
		struct mbuf *m_new = m_dup(m, M_NOWAIT);
		if (m_new == NULL)
			return (0);
		m_freem(m);
		m = m_new;

		/* Recalculate pointers */
		if (ip4)
			ip4 = (struct ip *)(mtod(m, uint8_t *) + offset - ip_hlen);
		if (ip6)
			ip6 = (struct ip6_hdr *)(mtod(m, uint8_t *) + offset - ip_hlen);
		tcp = (struct tcphdr *)(mtod(m, uint8_t *) + offset);
	}

	/* Search for MSS option in TCP options */
	options = (uint8_t *)(tcp + 1);
	opt_len = tcp_hlen - sizeof(struct tcphdr);

	for (i = 0; i < opt_len; ) {
		uint8_t opt_type = options[i];
		uint8_t opt_size;

		if (opt_type == TCPOPT_EOL)
			break;

		if (opt_type == TCPOPT_NOP) {
			i++;
			continue;
		}

		if (i + 1 >= opt_len)
			break;

		opt_size = options[i + 1];
		if (opt_size < 2 || i + opt_size > opt_len)
			break;

		if (opt_type == TCPOPT_MAXSEG && opt_size == 4) {
			/* Found MSS option */
			uint16_t old_mss = (options[i + 2] << 8) | options[i + 3];

			if (old_mss > max_mss) {
				/* Rewrite MSS */
				options[i + 2] = (max_mss >> 8) & 0xff;
				options[i + 3] = max_mss & 0xff;

				/* Recalculate TCP checksum */
				tcp->th_sum = 0;
				if (ip4) {
					int tcplen = ntohs(ip4->ip_len) - ip_hlen;
					tcp->th_sum = tcp_checksum_ipv4(ip4, tcp, tcplen);
				} else if (ip6) {
					int tcplen = ntohs(ip6->ip6_plen);
					tcp->th_sum = tcp_checksum_ipv6(ip6, tcp, tcplen);
				}

				rewritten = 1;
			}
			break;
		}

		i += opt_size;
	}

	priv->packets_processed++;
	if (rewritten)
		priv->packets_rewritten++;

	return (rewritten);
}

/*
 * Node constructor
 */
static int
ng_mss_rewrite_constructor(node_p node)
{
	priv_p priv;

	priv = malloc(sizeof(*priv), M_NETGRAPH, M_WAITOK | M_ZERO);
	priv->mss_ipv4 = DEFAULT_MSS_IPV4;
	priv->mss_ipv6 = DEFAULT_MSS_IPV6;

	NG_NODE_SET_PRIVATE(node, priv);

	return (0);
}

/*
 * Node destructor
 */
static int
ng_mss_rewrite_shutdown(node_p node)
{
	const priv_p priv = NG_NODE_PRIVATE(node);

	free(priv, M_NETGRAPH);
	NG_NODE_SET_PRIVATE(node, NULL);
	NG_NODE_UNREF(node);

	return (0);
}

/*
 * Hook creation
 */
static int
ng_mss_rewrite_newhook(node_p node, hook_p hook, const char *name)
{
	const priv_p priv = NG_NODE_PRIVATE(node);

	if (strcmp(name, NG_MSS_REWRITE_HOOK_LOWER) == 0) {
		priv->lower = hook;
	} else if (strcmp(name, NG_MSS_REWRITE_HOOK_UPPER) == 0) {
		priv->upper = hook;
	} else {
		return (EINVAL);
	}

	return (0);
}

/*
 * Receive a control message
 */
static int
ng_mss_rewrite_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct ng_mesg *resp = NULL;
	struct ng_mesg *msg;
	int error = 0;

	NGI_GET_MSG(item, msg);

	switch (msg->header.typecookie) {
	case NGM_MSS_REWRITE_COOKIE:
		switch (msg->header.cmd) {
		case NGM_MSS_REWRITE_SET_MSS:
		{
			struct ng_mss_rewrite_conf *conf;

			if (msg->header.arglen != sizeof(*conf)) {
				error = EINVAL;
				break;
			}

			conf = (struct ng_mss_rewrite_conf *)msg->data;
			priv->mss_ipv4 = conf->mss_ipv4;
			priv->mss_ipv6 = conf->mss_ipv6;
			break;
		}

		case NGM_MSS_REWRITE_GET_MSS:
		{
			struct ng_mss_rewrite_conf *conf;

			NG_MKRESPONSE(resp, msg, sizeof(*conf), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}

			conf = (struct ng_mss_rewrite_conf *)resp->data;
			conf->mss_ipv4 = priv->mss_ipv4;
			conf->mss_ipv6 = priv->mss_ipv6;
			break;
		}

		case NGM_MSS_REWRITE_GET_STATS:
		{
			struct ng_mss_rewrite_stats *stats;

			NG_MKRESPONSE(resp, msg, sizeof(*stats), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}

			stats = (struct ng_mss_rewrite_stats *)resp->data;
			stats->packets_processed = priv->packets_processed;
			stats->packets_rewritten = priv->packets_rewritten;
			break;
		}

		case NGM_MSS_REWRITE_RESET_STATS:
			priv->packets_processed = 0;
			priv->packets_rewritten = 0;
			break;

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
 * Receive data
 */
static int
ng_mss_rewrite_rcvdata(hook_p hook, item_p item)
{
	const node_p node = NG_HOOK_NODE(hook);
	const priv_p priv = NG_NODE_PRIVATE(node);
	struct mbuf *m;
	hook_p out_hook;
	int error = 0;

	NGI_GET_M(item, m);
	NG_FREE_ITEM(item);

	/* Determine output hook */
	if (hook == priv->lower)
		out_hook = priv->upper;
	else if (hook == priv->upper)
		out_hook = priv->lower;
	else {
		m_freem(m);
		return (EINVAL);
	}

	if (out_hook == NULL) {
		m_freem(m);
		return (ENOTCONN);
	}

	/* Process the packet */
	ng_mss_rewrite_process(priv, m);

	/* Forward the packet */
	NG_FWD_NEW_DATA(error, item, out_hook, m);

	return (error);
}

/*
 * Hook disconnection
 */
static int
ng_mss_rewrite_disconnect(hook_p hook)
{
	const priv_p priv = NG_NODE_PRIVATE(NG_HOOK_NODE(hook));

	if (hook == priv->lower)
		priv->lower = NULL;
	else if (hook == priv->upper)
		priv->upper = NULL;

	if (NG_NODE_NUMHOOKS(NG_HOOK_NODE(hook)) == 0)
		ng_rmnode_self(NG_HOOK_NODE(hook));

	return (0);
}

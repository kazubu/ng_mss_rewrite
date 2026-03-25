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
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/mutex.h>
#include <machine/atomic.h>

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

/* Maximum header size to pull up at once (optimization) */
#define MAX_HDR_LEN		(ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN + 60 + 60)

/* Statistics modes */
#define STATS_MODE_DISABLED	0	/* No statistics collection (fastest) */
#define STATS_MODE_PERCPU	1	/* Per-CPU counters (minimal overhead, default) */

/* Compile-time statistics control (set to 0 to disable all stats) */
#ifndef ENABLE_STATS
#define ENABLE_STATS		1
#endif

/* Per-CPU statistics structure */
struct ng_mss_stats_percpu {
	uint64_t	packets_processed;
	uint64_t	packets_rewritten;
} __aligned(CACHE_LINE_SIZE);

/* Private node data */
struct ng_mss_rewrite_private {
	hook_p		lower;		/* Connection to physical interface */
	hook_p		upper;		/* Connection to kernel stack */
	uint16_t	mss_ipv4;	/* MSS limit for IPv4 */
	uint16_t	mss_ipv6;	/* MSS limit for IPv6 */
	volatile u_char	stats_mode;	/* Statistics mode (can be changed at runtime) */

	/* Per-CPU statistics (allocated once, never freed until shutdown) */
	struct ng_mss_stats_percpu *stats_percpu;

	/* Baseline for resetstats (snapshot at last reset) */
	uint64_t	baseline_processed;
	uint64_t	baseline_rewritten;

	/* Mutex for getstats/resetstats mutual exclusion */
	struct mtx	stats_mtx;
};
typedef struct ng_mss_rewrite_private *priv_p;

/* Netgraph control messages */
enum {
	NGM_MSS_REWRITE_SET_MSS = 1,
	NGM_MSS_REWRITE_GET_MSS,
	NGM_MSS_REWRITE_GET_STATS,
	NGM_MSS_REWRITE_RESET_STATS,
	NGM_MSS_REWRITE_SET_STATS_MODE,
	NGM_MSS_REWRITE_GET_STATS_MODE,
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

struct ng_mss_rewrite_stats_mode {
	uint8_t		mode;		/* Statistics mode */
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

/* Parse type for stats mode structure */
static const struct ng_parse_struct_field ng_mss_rewrite_stats_mode_fields[] = {
	{ "mode",	&ng_parse_uint8_type },
	{ NULL }
};
static const struct ng_parse_type ng_mss_rewrite_stats_mode_type = {
	&ng_parse_struct_type,
	&ng_mss_rewrite_stats_mode_fields
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
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_SET_STATS_MODE,
		"setstatsmode",
		&ng_mss_rewrite_stats_mode_type,
		NULL
	},
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_GET_STATS_MODE,
		"getstatsmode",
		NULL,
		&ng_mss_rewrite_stats_mode_type
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

/*
 * Helper function to update TCP checksum incrementally (RFC 1624)
 * HC' = ~(~HC + ~m + m')
 * Where HC is old checksum, m is old data, m' is new data
 */
static uint16_t
tcp_checksum_adjust(uint16_t old_check, uint16_t old_data, uint16_t new_data)
{
	uint32_t sum;

	sum = ~old_check & 0xffff;
	sum += ~old_data & 0xffff;
	sum += new_data;
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (~sum);
}

/* Process and possibly rewrite MSS in a packet - optimized version */
static struct mbuf *
ng_mss_rewrite_process(priv_p priv, struct mbuf *m, int *rewritten)
{
	struct ether_header *eh;
	struct ip *ip4 = NULL;
	struct ip6_hdr *ip6 = NULL;
	struct tcphdr *tcp;
	uint8_t *options, *pkt;
	uint16_t ether_type;
	int ip_hlen, tcp_hlen, opt_len, i;
	int offset = 0;
	int pullup_len;
	uint16_t max_mss;
#if ENABLE_STATS
	uint8_t stats_mode;  /* Snapshot stats mode once per packet */
#endif

	*rewritten = 0;

#if ENABLE_STATS
	/* Snapshot stats mode with acquire semantics (avoid mid-packet mode changes) */
	stats_mode = priv->stats_mode;
	atomic_thread_fence_acq();
#endif

	/* Fast path: ensure minimum packet length */
	if (m->m_pkthdr.len < sizeof(struct ether_header) + sizeof(struct ip) + sizeof(struct tcphdr))
		return (m);

	/* Optimize: pull up headers in one go (most packets fit in single mbuf) */
	pullup_len = min(m->m_pkthdr.len, MAX_HDR_LEN);
	if (m->m_len < pullup_len) {
		m = m_pullup(m, pullup_len);
		if (m == NULL)
			return (NULL);
	}

	pkt = mtod(m, uint8_t *);
	eh = (struct ether_header *)pkt;
	ether_type = ntohs(eh->ether_type);
	offset = sizeof(struct ether_header);

	/* Handle VLAN tag */
	if (ether_type == ETHERTYPE_VLAN) {
		if (m->m_pkthdr.len < offset + 4)
			return (m);
		ether_type = ntohs(*(uint16_t *)(pkt + offset + 2));
		offset += 4;
	}

	/* Check IP version and protocol */
	if (ether_type == ETHERTYPE_IP) {
		if (m->m_pkthdr.len < offset + sizeof(struct ip))
			return (m);

		ip4 = (struct ip *)(pkt + offset);
		ip_hlen = ip4->ip_hl << 2;

		/* Validate IPv4 header length */
		if (ip_hlen < (int)sizeof(struct ip))
			return (m);
		if (m->m_pkthdr.len < offset + ip_hlen)
			return (m);

		/* Fast path: not TCP */
		if (ip4->ip_p != IPPROTO_TCP)
			return (m);

		/* Skip fragmented packets (only first fragment has TCP header) */
		if (ntohs(ip4->ip_off) & (IP_MF | IP_OFFMASK))
			return (m);

		/* Verify minimum packet length for TCP */
		if (ntohs(ip4->ip_len) < ip_hlen + (int)sizeof(struct tcphdr))
			return (m);

		/* Verify IP total length doesn't exceed actual packet length */
		if (ntohs(ip4->ip_len) > m->m_pkthdr.len - (offset - ip_hlen))
			return (m);

		max_mss = priv->mss_ipv4;
		offset += ip_hlen;

	} else if (ether_type == ETHERTYPE_IPV6) {
		if (m->m_pkthdr.len < offset + sizeof(struct ip6_hdr))
			return (m);

		ip6 = (struct ip6_hdr *)(pkt + offset);
		ip_hlen = sizeof(struct ip6_hdr);

		/* Fast path: not TCP (simplified, not handling extension headers) */
		if (ip6->ip6_nxt != IPPROTO_TCP)
			return (m);

		/* Verify IPv6 payload length doesn't exceed actual packet length */
		if (ntohs(ip6->ip6_plen) > m->m_pkthdr.len - offset)
			return (m);

		max_mss = priv->mss_ipv6;
		offset += ip_hlen;

	} else {
		/* Not IP */
		return (m);
	}

	/* Check TCP header */
	if (m->m_pkthdr.len < offset + sizeof(struct tcphdr))
		return (m);

	tcp = (struct tcphdr *)(pkt + offset);
	tcp_hlen = tcp->th_off << 2;

	/* Validate TCP header length */
	if (tcp_hlen < (int)sizeof(struct tcphdr))
		return (m);
	if (m->m_pkthdr.len < offset + tcp_hlen)
		return (m);

	/* Fast path: not a SYN packet */
	if (!(tcp->th_flags & TH_SYN))
		return (m);

#if ENABLE_STATS
	/* Increment SYN packets counter (if not disabled) */
	if (stats_mode == STATS_MODE_PERCPU)
		priv->stats_percpu[curcpu].packets_processed++;
#endif

	/* If we didn't pull up enough, do it now (rare case) */
	if (m->m_len < offset + tcp_hlen) {
		m = m_pullup(m, offset + tcp_hlen);
		if (m == NULL)
			return (NULL);
		/* Recalculate pointers */
		pkt = mtod(m, uint8_t *);
		if (ip4)
			ip4 = (struct ip *)(pkt + offset - ip_hlen);
		if (ip6)
			ip6 = (struct ip6_hdr *)(pkt + offset - ip_hlen);
		tcp = (struct tcphdr *)(pkt + offset);
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
				/* MSS rewrite needed - make mbuf writable now */
				if (M_WRITABLE(m) == 0) {
					struct mbuf *m_new = m_dup(m, M_NOWAIT);
					if (m_new == NULL) {
						m_freem(m);
						return (NULL);
					}
					m_freem(m);
					m = m_new;
					/* Recalculate pointers */
					pkt = mtod(m, uint8_t *);
					if (ip4)
						ip4 = (struct ip *)(pkt + offset - ip_hlen);
					if (ip6)
						ip6 = (struct ip6_hdr *)(pkt + offset - ip_hlen);
					tcp = (struct tcphdr *)(pkt + offset);
					options = (uint8_t *)(tcp + 1);
				}

				/* Update TCP checksum incrementally (RFC 1624) */
				tcp->th_sum = tcp_checksum_adjust(tcp->th_sum, old_mss, max_mss);

				/* Rewrite MSS */
				options[i + 2] = (max_mss >> 8) & 0xff;
				options[i + 3] = max_mss & 0xff;

				*rewritten = 1;

#if ENABLE_STATS
				/* Increment rewritten counter (if not disabled) */
				if (stats_mode == STATS_MODE_PERCPU)
					priv->stats_percpu[curcpu].packets_rewritten++;
#endif
			}
			break;
		}

		i += opt_size;
	}

	return (m);
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

#if ENABLE_STATS
	/* Default to disabled statistics for maximum performance */
	priv->stats_mode = STATS_MODE_DISABLED;
	/* Allocate per-CPU array immediately (never-free design) */
	priv->stats_percpu = malloc(sizeof(struct ng_mss_stats_percpu) * mp_ncpus,
	    M_NETGRAPH, M_WAITOK | M_ZERO);
	/* Initialize mutex for getstats/resetstats */
	mtx_init(&priv->stats_mtx, "ng_mss_stats", NULL, MTX_DEF);
#else
	priv->stats_mode = STATS_MODE_DISABLED;
	priv->stats_percpu = NULL;
#endif

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

#if ENABLE_STATS
	if (priv->stats_percpu != NULL) {
		mtx_destroy(&priv->stats_mtx);
		free(priv->stats_percpu, M_NETGRAPH);
	}
#endif

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
		if (priv->lower != NULL)
			return (EISCONN);
		priv->lower = hook;
	} else if (strcmp(name, NG_MSS_REWRITE_HOOK_UPPER) == 0) {
		if (priv->upper != NULL)
			return (EISCONN);
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

			/* Validate MSS values (must be non-zero) */
			if (conf->mss_ipv4 == 0 || conf->mss_ipv6 == 0) {
				error = EINVAL;
				break;
			}

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

#if ENABLE_STATS
			/* Lock to prevent race with resetstats */
			mtx_lock(&priv->stats_mtx);

			/* Aggregate per-CPU counters */
			stats->packets_processed = 0;
			stats->packets_rewritten = 0;

			if (priv->stats_percpu != NULL) {
				int cpu;
				for (cpu = 0; cpu < mp_ncpus; cpu++) {
					stats->packets_processed += priv->stats_percpu[cpu].packets_processed;
					stats->packets_rewritten += priv->stats_percpu[cpu].packets_rewritten;
				}
			}

			/* Subtract baseline (for resetstats support) */
			stats->packets_processed -= priv->baseline_processed;
			stats->packets_rewritten -= priv->baseline_rewritten;

			mtx_unlock(&priv->stats_mtx);
#else
			stats->packets_processed = 0;
			stats->packets_rewritten = 0;
#endif
			break;
		}

		case NGM_MSS_REWRITE_RESET_STATS:
#if ENABLE_STATS
		{
			/* Baseline method: snapshot current total, don't zero live counters */
			uint64_t total_processed = 0, total_rewritten = 0;

			/* Lock to prevent race with getstats */
			mtx_lock(&priv->stats_mtx);

			if (priv->stats_percpu != NULL) {
				int cpu;
				for (cpu = 0; cpu < mp_ncpus; cpu++) {
					total_processed += priv->stats_percpu[cpu].packets_processed;
					total_rewritten += priv->stats_percpu[cpu].packets_rewritten;
				}
			}

			priv->baseline_processed = total_processed;
			priv->baseline_rewritten = total_rewritten;

			mtx_unlock(&priv->stats_mtx);
		}
#endif
			break;

		case NGM_MSS_REWRITE_SET_STATS_MODE:
		{
#if ENABLE_STATS
			struct ng_mss_rewrite_stats_mode *mode_conf;

			if (msg->header.arglen != sizeof(*mode_conf)) {
				error = EINVAL;
				break;
			}

			mode_conf = (struct ng_mss_rewrite_stats_mode *)msg->data;

			/* Validate mode (only DISABLED and PERCPU are supported) */
			if (mode_conf->mode != STATS_MODE_DISABLED && mode_conf->mode != STATS_MODE_PERCPU) {
				error = EINVAL;
				break;
			}

			/* Lazy allocate per-CPU array if switching to PERCPU */
			if (mode_conf->mode == STATS_MODE_PERCPU && priv->stats_percpu == NULL) {
				priv->stats_percpu = malloc(sizeof(struct ng_mss_stats_percpu) * mp_ncpus,
				    M_NETGRAPH, M_NOWAIT | M_ZERO);
				if (priv->stats_percpu == NULL) {
					error = ENOMEM;
					break;
				}
			}

			/* Atomically update mode with release semantics (never free stats_percpu) */
			atomic_thread_fence_rel();
			priv->stats_mode = mode_conf->mode;
#else
			error = EOPNOTSUPP;
#endif
			break;
		}

		case NGM_MSS_REWRITE_GET_STATS_MODE:
		{
			struct ng_mss_rewrite_stats_mode *mode_conf;

			NG_MKRESPONSE(resp, msg, sizeof(*mode_conf), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}

			mode_conf = (struct ng_mss_rewrite_stats_mode *)resp->data;
#if ENABLE_STATS
			mode_conf->mode = priv->stats_mode;
			atomic_thread_fence_acq();
#else
			mode_conf->mode = STATS_MODE_DISABLED;
#endif
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

	/* Determine output hook */
	if (hook == priv->lower)
		out_hook = priv->upper;
	else if (hook == priv->upper)
		out_hook = priv->lower;
	else {
		m_freem(m);
		NG_FREE_ITEM(item);
		return (EINVAL);
	}

	if (out_hook == NULL) {
		m_freem(m);
		NG_FREE_ITEM(item);
		return (ENOTCONN);
	}

	/* Process the packet */
	{
		int rewritten;
		m = ng_mss_rewrite_process(priv, m, &rewritten);
		if (m == NULL) {
			/* Packet was dropped during processing */
			NG_FREE_ITEM(item);
			return (0);
		}
	}

	/* Put the (possibly modified) mbuf back into the item */
	NGI_M(item) = m;

	/* Forward the packet */
	NG_FWD_ITEM_HOOK(error, item, out_hook);

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

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

/*
 * Fast path minimum m_len threshold
 * Must accommodate VLAN-tagged packets with base headers.
 * Largest base header combination is IPv6 with VLAN:
 *   Ethernet + VLAN + IPv6 = 14 + 4 + 40 = 58 bytes
 * (TCP header requires additional check as it starts at offset 58)
 */
#define NG_MSS_FAST_PATH_MIN_LEN \
	(sizeof(struct ether_header) + ETHER_VLAN_ENCAP_LEN + sizeof(struct ip6_hdr))

/* Maximum header size to pull up at once (optimization) */
/* MAX_HDR_LEN removed - not needed with m_copydata() approach */

/* Statistics modes */
#define STATS_MODE_DISABLED	0	/* No statistics collection (fastest, default) */
#define STATS_MODE_PERCPU	1	/* Per-CPU counters (minimal overhead) */

/* Compile-time statistics control (set to 0 to disable all stats) */
#ifndef ENABLE_STATS
#define ENABLE_STATS		1
#endif

/* Compile-time debug statistics (set to 1 for detailed path tracking) */
#ifndef ENABLE_DEBUG_STATS
#define ENABLE_DEBUG_STATS	0
#endif

/* Per-CPU statistics structure */
struct ng_mss_stats_percpu {
	uint64_t	packets_processed;
	uint64_t	packets_rewritten;
#if ENABLE_DEBUG_STATS
	/* Code path dispatch tracking (entry point decision) */
	uint64_t	fast_dispatch_count;
	uint64_t	safe_dispatch_count;
	uint64_t	pullup_count;
	uint64_t	pullup_failed;
	uint64_t	unshare_count;
	uint64_t	unshare_failed;
	/* Skip reasons (only implemented counters) */
	uint64_t	skip_offload;
	uint64_t	skip_no_mss;
	uint64_t	skip_mss_ok;
#endif
} __aligned(CACHE_LINE_SIZE);

/* Private node data */
struct ng_mss_rewrite_private {
	hook_p		lower;		/* Connection to physical interface */
	hook_p		upper;		/* Connection to kernel stack */
	uint16_t	mss_ipv4;	/* MSS limit for IPv4 (atomic access) */
	uint16_t	mss_ipv6;	/* MSS limit for IPv6 (atomic access) */
	uint8_t		stats_mode;	/* Statistics mode (atomic access) */
	uint8_t		enable_lower;	/* Enable processing from lower hook (atomic access) */
	uint8_t		enable_upper;	/* Enable processing from upper hook (atomic access) */

	/* Per-CPU statistics (allocated once, never freed until shutdown) */
	struct ng_mss_stats_percpu *stats_percpu;

	/* Baseline for resetstats (snapshot at last reset) */
	uint64_t	baseline_processed;
	uint64_t	baseline_rewritten;
#if ENABLE_DEBUG_STATS
	uint64_t	baseline_fast_dispatch_count;
	uint64_t	baseline_safe_dispatch_count;
	uint64_t	baseline_pullup_count;
	uint64_t	baseline_pullup_failed;
	uint64_t	baseline_unshare_count;
	uint64_t	baseline_unshare_failed;
	uint64_t	baseline_skip_offload;
	uint64_t	baseline_skip_no_mss;
	uint64_t	baseline_skip_mss_ok;
#endif

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
	NGM_MSS_REWRITE_SET_DIRECTION,
	NGM_MSS_REWRITE_GET_DIRECTION,
};

/* Control message structures */
struct ng_mss_rewrite_conf {
	uint16_t	mss_ipv4;
	uint16_t	mss_ipv6;
};

struct ng_mss_rewrite_stats {
	uint64_t	packets_processed;
	uint64_t	packets_rewritten;
#if ENABLE_DEBUG_STATS
	/* Code path dispatch tracking (entry point decision) */
	uint64_t	fast_dispatch_count;
	uint64_t	safe_dispatch_count;
	uint64_t	pullup_count;
	uint64_t	pullup_failed;
	uint64_t	unshare_count;
	uint64_t	unshare_failed;
	/* Skip reasons (only implemented counters) */
	uint64_t	skip_offload;
	uint64_t	skip_no_mss;
	uint64_t	skip_mss_ok;
#endif
};

struct ng_mss_rewrite_stats_mode {
	uint8_t		mode;		/* Statistics mode */
};

struct ng_mss_rewrite_direction {
	uint8_t		enable_lower;	/* Process packets from lower hook */
	uint8_t		enable_upper;	/* Process packets from upper hook */
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
#if ENABLE_DEBUG_STATS
	{ "fast_dispatch_count",	&ng_parse_uint64_type },
	{ "safe_dispatch_count",	&ng_parse_uint64_type },
	{ "pullup_count",	&ng_parse_uint64_type },
	{ "pullup_failed",	&ng_parse_uint64_type },
	{ "unshare_count",	&ng_parse_uint64_type },
	{ "unshare_failed",	&ng_parse_uint64_type },
	{ "skip_offload",	&ng_parse_uint64_type },
	{ "skip_no_mss",	&ng_parse_uint64_type },
	{ "skip_mss_ok",	&ng_parse_uint64_type },
#endif
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

/* Parse type for direction structure */
static const struct ng_parse_struct_field ng_mss_rewrite_direction_fields[] = {
	{ "enable_lower",	&ng_parse_uint8_type },
	{ "enable_upper",	&ng_parse_uint8_type },
	{ NULL }
};
static const struct ng_parse_type ng_mss_rewrite_direction_type = {
	&ng_parse_struct_type,
	&ng_mss_rewrite_direction_fields
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
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_SET_DIRECTION,
		"setdirection",
		&ng_mss_rewrite_direction_type,
		NULL
	},
	{
		NGM_MSS_REWRITE_COOKIE,
		NGM_MSS_REWRITE_GET_DIRECTION,
		"getdirection",
		NULL,
		&ng_mss_rewrite_direction_type
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
 * All parameters are in network byte order, returns network byte order
 */
static __inline uint16_t
tcp_checksum_adjust(uint16_t old_check, uint16_t old_data, uint16_t new_data)
{
	uint32_t sum;

	/* Convert to host order for calculation */
	old_check = ntohs(old_check);
	old_data = ntohs(old_data);
	new_data = ntohs(new_data);

	sum = ~old_check & 0xffff;
	sum += ~old_data & 0xffff;
	sum += new_data;
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	/* Convert back to network order */
	return (htons(~sum));
}

/*
 * Forward declarations for fast and safe paths
 */
static struct mbuf *ng_mss_rewrite_process_fast(priv_p priv, struct mbuf *m, int from_upper);
static struct mbuf *ng_mss_rewrite_process_safe(priv_p priv, struct mbuf *m, int from_upper, int from_fast_fallback);

/*
 * Process and possibly rewrite MSS in a packet
 *
 * Two-tier approach for optimal performance:
 * - Fast path: Direct pointer access when mbuf is contiguous
 * - Safe path: m_copydata() for fragmented mbuf chains
 *
 * Arguments:
 *   priv - private node data
 *   m - mbuf chain to process
 *   from_upper - 1 if packet is from upper hook (kernel->interface), 0 otherwise
 */
static struct mbuf *
ng_mss_rewrite_process(priv_p priv, struct mbuf *m, int from_upper)
{
	/*
	 * Fast path entry condition:
	 * Check if packet has enough contiguous data for base headers.
	 * See NG_MSS_FAST_PATH_MIN_LEN definition for calculation.
	 * IP/TCP options are checked dynamically inside fast path.
	 */
	if (m->m_len >= NG_MSS_FAST_PATH_MIN_LEN) {
		/* Fast path: contiguous mbuf, use direct pointer access */
#if ENABLE_DEBUG_STATS
		{
			uint8_t stats_mode = atomic_load_acq_8(&priv->stats_mode);
			if (stats_mode == STATS_MODE_PERCPU)
				priv->stats_percpu[curcpu].fast_dispatch_count++;
		}
#endif
		return ng_mss_rewrite_process_fast(priv, m, from_upper);
	} else {
		/* Safe path: potentially fragmented, use m_copydata() */
#if ENABLE_DEBUG_STATS
		{
			uint8_t stats_mode = atomic_load_acq_8(&priv->stats_mode);
			if (stats_mode == STATS_MODE_PERCPU)
				priv->stats_percpu[curcpu].safe_dispatch_count++;
		}
#endif
		return ng_mss_rewrite_process_safe(priv, m, from_upper, 0);
	}
}

/*
 * Fast path: Process packet with direct pointer access
 * Assumes m->m_len >= NG_MSS_FAST_PATH_MIN_LEN
 * (sufficient for Ethernet + VLAN + IPv6 base header)
 * Checks TCP base header and IP/TCP options dynamically.
 */
static struct mbuf *
ng_mss_rewrite_process_fast(priv_p priv, struct mbuf *m, int from_upper)
{
	struct ether_header *eh;
	struct ip *ip4 = NULL;
	struct ip6_hdr *ip6 = NULL;
	struct tcphdr *tcp;
	uint8_t *options, *pkt;
	uint16_t ether_type, max_mss, old_mss, plen;
	int offset, ip_hlen, tcp_hlen, opt_len, mss_offset, i;

#if ENABLE_STATS
	uint8_t stats_mode;
	struct ng_mss_stats_percpu *st = NULL;
#endif

	/* Fast path: ensure minimum packet length */
	if (m->m_pkthdr.len < sizeof(struct ether_header) + sizeof(struct ip) + sizeof(struct tcphdr))
		return (m);

	pkt = mtod(m, uint8_t *);
	eh = (struct ether_header *)pkt;
	ether_type = ntohs(eh->ether_type);
	offset = sizeof(struct ether_header);

	/* Handle VLAN tag */
	if (ether_type == ETHERTYPE_VLAN) {
		if (m->m_pkthdr.len < offset + ETHER_VLAN_ENCAP_LEN)
			return (m);
		/*
		 * VLAN tag guaranteed accessible by NG_MSS_FAST_PATH_MIN_LEN.
		 * No m_len check needed (offset + ETHER_VLAN_ENCAP_LEN = 14 + 4 = 18).
		 */
		ether_type = ntohs(*(uint16_t *)(pkt + offset + 2));
		offset += ETHER_VLAN_ENCAP_LEN;
	}

	/* Parse IP header */
	if (ether_type == ETHERTYPE_IP) {
		/* IPv4 */
		if (m->m_pkthdr.len < offset + sizeof(struct ip))
			return (m);
		/*
		 * IPv4 base header guaranteed accessible by NG_MSS_FAST_PATH_MIN_LEN.
		 * Max offset with VLAN: sizeof(ether_header) + ETHER_VLAN_ENCAP_LEN = 18
		 * Required: 18 + sizeof(struct ip) = 18 + 20 = 38 bytes < 58
		 */

		ip4 = (struct ip *)(pkt + offset);
		ip_hlen = (ip4->ip_hl & 0x0f) << 2;

		if (ip_hlen < (int)sizeof(struct ip) || m->m_pkthdr.len < offset + ip_hlen)
			return (m);
		/* Check IP options dynamically if ip_hlen > sizeof(struct ip) */
		if (ip_hlen > (int)sizeof(struct ip) && m->m_len < offset + ip_hlen)
			return ng_mss_rewrite_process_safe(priv, m, from_upper, 1);

		if (ip4->ip_p != IPPROTO_TCP)
			return (m);
		if (ntohs(ip4->ip_off) & (IP_MF | IP_OFFMASK))
			return (m);

		plen = ntohs(ip4->ip_len);
		if (plen < ip_hlen + (int)sizeof(struct tcphdr) || plen > m->m_pkthdr.len - offset)
			return (m);

		max_mss = atomic_load_acq_16(&priv->mss_ipv4);
		offset += ip_hlen;

	} else if (ether_type == ETHERTYPE_IPV6) {
		/* IPv6 */
		if (m->m_pkthdr.len < offset + sizeof(struct ip6_hdr))
			return (m);
		/*
		 * IPv6 base header guaranteed accessible by NG_MSS_FAST_PATH_MIN_LEN.
		 * With VLAN: sizeof(ether_header) + ETHER_VLAN_ENCAP_LEN + sizeof(ip6_hdr)
		 *           = 14 + 4 + 40 = 58 bytes (exactly MIN_LEN)
		 */

		ip6 = (struct ip6_hdr *)(pkt + offset);
		ip_hlen = sizeof(struct ip6_hdr);

		if (ip6->ip6_nxt != IPPROTO_TCP)
			return (m);

		plen = ntohs(ip6->ip6_plen);
		if (plen < (int)sizeof(struct tcphdr) || plen > m->m_pkthdr.len - offset - ip_hlen)
			return (m);

		max_mss = atomic_load_acq_16(&priv->mss_ipv6);
		offset += ip_hlen;

	} else {
		return (m);
	}

	/* Parse TCP header */
	if (m->m_pkthdr.len < offset + sizeof(struct tcphdr))
		return (m);
	/*
	 * TCP base header must be checked dynamically.
	 * With IPv6+VLAN: offset = sizeof(ether_header) + ETHER_VLAN_ENCAP_LEN + sizeof(ip6_hdr)
	 *                        = 14 + 4 + 40 = 58
	 * Required: 58 + sizeof(tcphdr) = 58 + 20 = 78 bytes
	 * This exceeds NG_MSS_FAST_PATH_MIN_LEN (58), so explicit check needed.
	 */
	if (m->m_len < offset + sizeof(struct tcphdr))
		return ng_mss_rewrite_process_safe(priv, m, from_upper, 1);

	tcp = (struct tcphdr *)(pkt + offset);
	tcp_hlen = (tcp->th_off & 0xf) << 2;

	if (tcp_hlen < (int)sizeof(struct tcphdr) || m->m_pkthdr.len < offset + tcp_hlen)
		return (m);

	if (ip4) {
		if (tcp_hlen > plen - ip_hlen)
			return (m);
	} else {
		if (tcp_hlen > plen)
			return (m);
	}

	if (!(tcp->th_flags & TH_SYN))
		return (m);

#if ENABLE_STATS
	stats_mode = atomic_load_acq_8(&priv->stats_mode);
	if (stats_mode == STATS_MODE_PERCPU)
		st = &priv->stats_percpu[curcpu];
	if (st != NULL)
		st->packets_processed++;
#endif

	/* Fall back to safe path if TCP options extend beyond m_len */
	if (m->m_len < offset + tcp_hlen)
		return ng_mss_rewrite_process_safe(priv, m, from_upper, 1);

	/* Search for MSS option */
	options = (uint8_t *)(tcp + 1);
	opt_len = tcp_hlen - sizeof(struct tcphdr);
	mss_offset = -1;

	/* Fast path for common positions */
	if (opt_len >= 4 && options[0] == TCPOPT_MAXSEG && options[1] == 4) {
		mss_offset = 0;
	} else if (opt_len >= 5 && options[0] == TCPOPT_NOP &&
	           options[1] == TCPOPT_MAXSEG && options[2] == 4) {
		mss_offset = 1;
	} else if (opt_len >= 6 && options[0] == TCPOPT_NOP && options[1] == TCPOPT_NOP &&
	           options[2] == TCPOPT_MAXSEG && options[3] == 4) {
		mss_offset = 2;
	} else {
		/* Slow path: walk options */
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
				mss_offset = i;
				break;
			}
			i += opt_size;
		}
	}

	if (mss_offset < 0) {
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->skip_no_mss++;
#endif
		return (m);
	}

	old_mss = (options[mss_offset + 2] << 8) | options[mss_offset + 3];
	if (old_mss <= max_mss) {
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->skip_mss_ok++;
#endif
		return (m);
	}

	/* Checksum offload check for upper direction */
	if (from_upper && (m->m_pkthdr.csum_flags & (CSUM_TCP | CSUM_TSO))) {
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->skip_offload++;
#endif
		return (m);
	}

	/* Ensure writable */
	if (M_WRITABLE(m) == 0) {
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->unshare_count++;
#endif
		m = m_unshare(m, M_NOWAIT);
		if (m == NULL) {
#if ENABLE_DEBUG_STATS
			if (st != NULL)
				st->unshare_failed++;
#endif
			return (NULL);
		}
		/* Recalculate pointers */
		pkt = mtod(m, uint8_t *);
		tcp = (struct tcphdr *)(pkt + offset);
		options = (uint8_t *)(tcp + 1);
	}

	/* Update checksum and MSS */
	tcp->th_sum = tcp_checksum_adjust(tcp->th_sum, htons(old_mss), htons(max_mss));
	options[mss_offset + 2] = (max_mss >> 8) & 0xff;
	options[mss_offset + 3] = max_mss & 0xff;

#if ENABLE_STATS
	if (st != NULL)
		st->packets_rewritten++;
#endif

	return (m);
}

/*
 * Safe path: Process packet using m_copydata() for fragmented mbufs
 */
static struct mbuf *
ng_mss_rewrite_process_safe(priv_p priv, struct mbuf *m, int from_upper, int from_fast_fallback)
{
	/* Local copies for safe parsing via m_copydata() */
	struct ether_header eh;
	struct ip ip4;
	struct ip6_hdr ip6;
	uint8_t tcp_opts[40];
	uint8_t vlan_buf[4];

	/* Parsing state */
	uint16_t ether_type, max_mss, old_mss, plen;
	int offset, ip_hlen, tcp_hlen, opt_len, mss_offset;
	int i, is_ipv4;

#if ENABLE_STATS
	uint8_t stats_mode;
	struct ng_mss_stats_percpu *st = NULL;
#endif

	/* Fast path: ensure minimum packet length */
	if (m->m_pkthdr.len < sizeof(struct ether_header) + sizeof(struct ip) + sizeof(struct tcphdr))
		return (m);

	/* Parse Ethernet header safely */
	m_copydata(m, 0, sizeof(eh), (caddr_t)&eh);
	ether_type = ntohs(eh.ether_type);
	offset = sizeof(struct ether_header);

	/* Handle VLAN tag */
	if (ether_type == ETHERTYPE_VLAN) {
		if (m->m_pkthdr.len < offset + 4)
			return (m);
		m_copydata(m, offset, 4, (caddr_t)vlan_buf);
		ether_type = (vlan_buf[2] << 8) | vlan_buf[3];
		offset += 4;
	}

	/* Parse and validate IP header */
	if (ether_type == ETHERTYPE_IP) {
		/* IPv4 */
		uint8_t ip_vhl;

		if (m->m_pkthdr.len < offset + sizeof(struct ip))
			return (m);

		/* Read IP version/header length byte */
		m_copydata(m, offset, 1, (caddr_t)&ip_vhl);
		ip_hlen = (ip_vhl & 0x0f) << 2;  /* lower 4 bits, in 32-bit words */

		/* Validate IPv4 header length */
		if (ip_hlen < (int)sizeof(struct ip))
			return (m);
		if (m->m_pkthdr.len < offset + ip_hlen)
			return (m);

		/* Read full IPv4 header */
		m_copydata(m, offset, sizeof(struct ip), (caddr_t)&ip4);

		/* Fast path: not TCP */
		if (ip4.ip_p != IPPROTO_TCP)
			return (m);

		/* Skip fragmented packets (only first fragment has TCP header) */
		if (ntohs(ip4.ip_off) & (IP_MF | IP_OFFMASK))
			return (m);

		/* Verify minimum packet length for TCP */
		plen = ntohs(ip4.ip_len);
		if (plen < ip_hlen + (int)sizeof(struct tcphdr))
			return (m);

		/* Verify IP total length doesn't exceed actual packet length */
		if (plen > m->m_pkthdr.len - offset)
			return (m);

		max_mss = atomic_load_acq_16(&priv->mss_ipv4);
		offset += ip_hlen;
		is_ipv4 = 1;

	} else if (ether_type == ETHERTYPE_IPV6) {
		/* IPv6 */
		if (m->m_pkthdr.len < offset + sizeof(struct ip6_hdr))
			return (m);

		m_copydata(m, offset, sizeof(struct ip6_hdr), (caddr_t)&ip6);
		ip_hlen = sizeof(struct ip6_hdr);

		/* Fast path: not TCP (simplified, not handling extension headers) */
		if (ip6.ip6_nxt != IPPROTO_TCP)
			return (m);

		/* Verify payload length is valid for a TCP header */
		plen = ntohs(ip6.ip6_plen);
		if (plen < (int)sizeof(struct tcphdr) || plen > m->m_pkthdr.len - offset - ip_hlen)
			return (m);

		max_mss = atomic_load_acq_16(&priv->mss_ipv6);
		offset += ip_hlen;
		is_ipv4 = 0;

	} else {
		/* Not IP */
		return (m);
	}

	/* Parse TCP header safely */
	if (m->m_pkthdr.len < offset + sizeof(struct tcphdr))
		return (m);

	/* Read TCP data offset and flags bytes directly */
	{
		uint8_t tcp_off_x2, tcp_flags;

		m_copydata(m, offset + 12, 1, (caddr_t)&tcp_off_x2);  /* th_off and th_x2 */
		m_copydata(m, offset + 13, 1, (caddr_t)&tcp_flags);   /* th_flags */

		/* Extract th_off from upper 4 bits (endian-independent) */
		tcp_hlen = ((tcp_off_x2 >> 4) & 0x0f) << 2;

		/* Fast path: not a SYN packet */
		if (!(tcp_flags & TH_SYN))
			return (m);
	}

	/* Validate TCP header length */
	if (tcp_hlen < (int)sizeof(struct tcphdr))
		return (m);
	if (m->m_pkthdr.len < offset + tcp_hlen)
		return (m);

	/* Ensure TCP header length fits within IP payload (not Ethernet padding) */
	if (is_ipv4) {
		int l4_len = plen - ip_hlen;
		if (tcp_hlen > l4_len)
			return (m);
	} else {
		int l4_len = plen;  /* IPv6 payload length */
		if (tcp_hlen > l4_len)
			return (m);
	}

#if ENABLE_STATS
	/* Snapshot stats mode and cache pointer */
	stats_mode = atomic_load_acq_8(&priv->stats_mode);
	if (stats_mode == STATS_MODE_PERCPU)
		st = &priv->stats_percpu[curcpu];

	/*
	 * Increment SYN packets counter (if not disabled)
	 * Skip if called as fallback from fast path (already counted)
	 */
	if (st != NULL && !from_fast_fallback)
		st->packets_processed++;
#endif

	/* Parse TCP options safely */
	opt_len = tcp_hlen - sizeof(struct tcphdr);
	if (opt_len > (int)sizeof(tcp_opts))
		opt_len = sizeof(tcp_opts);  /* Truncate if too long */

	if (opt_len > 0)
		m_copydata(m, offset + sizeof(struct tcphdr), opt_len, (caddr_t)tcp_opts);

	/* Search for MSS option - fast path for common positions */
	mss_offset = -1;  /* MSS option offset within tcp_opts, or -1 if not found */

	if (opt_len >= 4) {
		/* Case 1: MSS at beginning (offset 0) */
		if (tcp_opts[0] == TCPOPT_MAXSEG && tcp_opts[1] == 4) {
			mss_offset = 0;
			goto found_mss;
		}
		/* Case 2: NOP + MSS (offset 1) */
		if (opt_len >= 5 && tcp_opts[0] == TCPOPT_NOP &&
		    tcp_opts[1] == TCPOPT_MAXSEG && tcp_opts[2] == 4) {
			mss_offset = 1;
			goto found_mss;
		}
		/* Case 3: NOP + NOP + MSS (offset 2) */
		if (opt_len >= 6 && tcp_opts[0] == TCPOPT_NOP &&
		    tcp_opts[1] == TCPOPT_NOP && tcp_opts[2] == TCPOPT_MAXSEG && tcp_opts[3] == 4) {
			mss_offset = 2;
			goto found_mss;
		}
	}

	/* Slow path: walk options generically */
	for (i = 0; i < opt_len; ) {
		uint8_t opt_type = tcp_opts[i];
		uint8_t opt_size;

		if (opt_type == TCPOPT_EOL)
			break;

		if (opt_type == TCPOPT_NOP) {
			i++;
			continue;
		}

		if (i + 1 >= opt_len)
			break;

		opt_size = tcp_opts[i + 1];
		if (opt_size < 2 || i + opt_size > opt_len)
			break;

		if (opt_type == TCPOPT_MAXSEG && opt_size == 4) {
			mss_offset = i;
			goto found_mss;
		}

		i += opt_size;
	}

	/* MSS option not found */
#if ENABLE_DEBUG_STATS
	if (st != NULL)
		st->skip_no_mss++;
#endif
	return (m);

found_mss:
	/* Extract MSS value */
	old_mss = (tcp_opts[mss_offset + 2] << 8) | tcp_opts[mss_offset + 3];

	/* Check if rewrite is needed */
	if (old_mss <= max_mss) {
		/* MSS is already within limit, no rewrite needed */
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->skip_mss_ok++;
#endif
		return (m);
	}

	/*
	 * For upper direction (kernel->interface), check for checksum offload.
	 * If TCP checksum offload is enabled, th_sum is a partial/seed value,
	 * not a complete checksum. We cannot safely adjust it incrementally.
	 */
	if (from_upper) {
		if (m->m_pkthdr.csum_flags & (CSUM_TCP | CSUM_TSO)) {
			/* Checksum offload active, skip rewrite */
#if ENABLE_DEBUG_STATS
			if (st != NULL)
				st->skip_offload++;
#endif
			return (m);
		}
	}

	/*
	 * MSS rewrite needed - now we need to make mbuf writable.
	 * Up to this point we've only read, so mbuf chain can be fragmented.
	 */

	/* Ensure we have contiguous access to headers we need to modify */
	if (m->m_len < offset + tcp_hlen) {
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->pullup_count++;
#endif
		m = m_pullup(m, offset + tcp_hlen);
		if (m == NULL) {
#if ENABLE_DEBUG_STATS
			if (st != NULL)
				st->pullup_failed++;
#endif
			return (NULL);
		}
	}

	/* Ensure mbuf is writable (not shared) */
	if (M_WRITABLE(m) == 0) {
#if ENABLE_DEBUG_STATS
		if (st != NULL)
			st->unshare_count++;
#endif
		m = m_unshare(m, M_NOWAIT);
		if (m == NULL) {
#if ENABLE_DEBUG_STATS
			if (st != NULL)
				st->unshare_failed++;
#endif
			return (NULL);
		}
	}

	/* Get pointers to actual packet data for modification */
	{
		uint8_t *pkt;
		struct tcphdr *tcp_wr;
		uint8_t *options_wr;
		uint16_t old_checksum, new_checksum;

		pkt = mtod(m, uint8_t *);
		tcp_wr = (struct tcphdr *)(pkt + offset);
		options_wr = (uint8_t *)(tcp_wr + 1);

		/* Update TCP checksum incrementally (RFC 1624) */
		old_checksum = tcp_wr->th_sum;
		new_checksum = tcp_checksum_adjust(old_checksum, htons(old_mss), htons(max_mss));
		tcp_wr->th_sum = new_checksum;

		/* Rewrite MSS */
		options_wr[mss_offset + 2] = (max_mss >> 8) & 0xff;
		options_wr[mss_offset + 3] = max_mss & 0xff;
	}

#if ENABLE_STATS
	/* Increment rewritten counter (if not disabled) */
	if (st != NULL)
		st->packets_rewritten++;
#endif

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
	atomic_store_rel_16(&priv->mss_ipv4, DEFAULT_MSS_IPV4);
	atomic_store_rel_16(&priv->mss_ipv6, DEFAULT_MSS_IPV6);
	/* Default: process only from lower hook (interface->kernel) */
	atomic_store_rel_8(&priv->enable_lower, 1);
	atomic_store_rel_8(&priv->enable_upper, 0);

#if ENABLE_STATS
	/* Default to disabled statistics for maximum performance */
	atomic_store_rel_8(&priv->stats_mode, STATS_MODE_DISABLED);
	/* Allocate per-CPU array immediately (never-free design) */
	priv->stats_percpu = malloc(sizeof(struct ng_mss_stats_percpu) * mp_ncpus,
	    M_NETGRAPH, M_WAITOK | M_ZERO);
	/* Initialize mutex for getstats/resetstats */
	mtx_init(&priv->stats_mtx, "ng_mss_stats", NULL, MTX_DEF);
#else
	atomic_store_rel_8(&priv->stats_mode, STATS_MODE_DISABLED);
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

			atomic_store_rel_16(&priv->mss_ipv4, conf->mss_ipv4);
			atomic_store_rel_16(&priv->mss_ipv6, conf->mss_ipv6);
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
			conf->mss_ipv4 = atomic_load_acq_16(&priv->mss_ipv4);
			conf->mss_ipv6 = atomic_load_acq_16(&priv->mss_ipv6);
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
#if ENABLE_DEBUG_STATS
			stats->fast_dispatch_count = 0;
			stats->safe_dispatch_count = 0;
			stats->pullup_count = 0;
			stats->pullup_failed = 0;
			stats->unshare_count = 0;
			stats->unshare_failed = 0;
			stats->skip_offload = 0;
			stats->skip_no_mss = 0;
			stats->skip_mss_ok = 0;
#endif

			if (priv->stats_percpu != NULL) {
				int cpu;
				for (cpu = 0; cpu < mp_ncpus; cpu++) {
					stats->packets_processed += priv->stats_percpu[cpu].packets_processed;
					stats->packets_rewritten += priv->stats_percpu[cpu].packets_rewritten;
#if ENABLE_DEBUG_STATS
					stats->fast_dispatch_count += priv->stats_percpu[cpu].fast_dispatch_count;
					stats->safe_dispatch_count += priv->stats_percpu[cpu].safe_dispatch_count;
					stats->pullup_count += priv->stats_percpu[cpu].pullup_count;
					stats->pullup_failed += priv->stats_percpu[cpu].pullup_failed;
					stats->unshare_count += priv->stats_percpu[cpu].unshare_count;
					stats->unshare_failed += priv->stats_percpu[cpu].unshare_failed;
					stats->skip_offload += priv->stats_percpu[cpu].skip_offload;
					stats->skip_no_mss += priv->stats_percpu[cpu].skip_no_mss;
					stats->skip_mss_ok += priv->stats_percpu[cpu].skip_mss_ok;
#endif
				}
			}

			/* Subtract baseline (for resetstats support) */
			stats->packets_processed -= priv->baseline_processed;
			stats->packets_rewritten -= priv->baseline_rewritten;
#if ENABLE_DEBUG_STATS
			stats->fast_dispatch_count -= priv->baseline_fast_dispatch_count;
			stats->safe_dispatch_count -= priv->baseline_safe_dispatch_count;
			stats->pullup_count -= priv->baseline_pullup_count;
			stats->pullup_failed -= priv->baseline_pullup_failed;
			stats->unshare_count -= priv->baseline_unshare_count;
			stats->unshare_failed -= priv->baseline_unshare_failed;
			stats->skip_offload -= priv->baseline_skip_offload;
			stats->skip_no_mss -= priv->baseline_skip_no_mss;
			stats->skip_mss_ok -= priv->baseline_skip_mss_ok;
#endif

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
#if ENABLE_DEBUG_STATS
			uint64_t total_fast_path = 0, total_safe_path = 0;
			uint64_t total_pullup = 0, total_pullup_failed = 0;
			uint64_t total_unshare = 0, total_unshare_failed = 0;
			uint64_t total_skip_offload = 0;
			uint64_t total_skip_no_mss = 0, total_skip_mss_ok = 0;
#endif

			/* Lock to prevent race with getstats */
			mtx_lock(&priv->stats_mtx);

			if (priv->stats_percpu != NULL) {
				int cpu;
				for (cpu = 0; cpu < mp_ncpus; cpu++) {
					total_processed += priv->stats_percpu[cpu].packets_processed;
					total_rewritten += priv->stats_percpu[cpu].packets_rewritten;
#if ENABLE_DEBUG_STATS
					total_fast_path += priv->stats_percpu[cpu].fast_dispatch_count;
					total_safe_path += priv->stats_percpu[cpu].safe_dispatch_count;
					total_pullup += priv->stats_percpu[cpu].pullup_count;
					total_pullup_failed += priv->stats_percpu[cpu].pullup_failed;
					total_unshare += priv->stats_percpu[cpu].unshare_count;
					total_unshare_failed += priv->stats_percpu[cpu].unshare_failed;
					total_skip_offload += priv->stats_percpu[cpu].skip_offload;
					total_skip_no_mss += priv->stats_percpu[cpu].skip_no_mss;
					total_skip_mss_ok += priv->stats_percpu[cpu].skip_mss_ok;
#endif
				}
			}

			priv->baseline_processed = total_processed;
			priv->baseline_rewritten = total_rewritten;
#if ENABLE_DEBUG_STATS
			priv->baseline_fast_dispatch_count = total_fast_path;
			priv->baseline_safe_dispatch_count = total_safe_path;
			priv->baseline_pullup_count = total_pullup;
			priv->baseline_pullup_failed = total_pullup_failed;
			priv->baseline_unshare_count = total_unshare;
			priv->baseline_unshare_failed = total_unshare_failed;
			priv->baseline_skip_offload = total_skip_offload;
			priv->baseline_skip_no_mss = total_skip_no_mss;
			priv->baseline_skip_mss_ok = total_skip_mss_ok;
#endif

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

			/* Update mode (stats_percpu already allocated at init, never freed) */
			atomic_store_rel_8(&priv->stats_mode, mode_conf->mode);
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
			mode_conf->mode = atomic_load_acq_8(&priv->stats_mode);
#else
			mode_conf->mode = STATS_MODE_DISABLED;
#endif
			break;
		}

		case NGM_MSS_REWRITE_SET_DIRECTION:
		{
			struct ng_mss_rewrite_direction *dir_conf;

			if (msg->header.arglen != sizeof(*dir_conf)) {
				error = EINVAL;
				break;
			}

			dir_conf = (struct ng_mss_rewrite_direction *)msg->data;
			atomic_store_rel_8(&priv->enable_lower, dir_conf->enable_lower ? 1 : 0);
			atomic_store_rel_8(&priv->enable_upper, dir_conf->enable_upper ? 1 : 0);
			break;
		}

		case NGM_MSS_REWRITE_GET_DIRECTION:
		{
			struct ng_mss_rewrite_direction *dir_conf;

			NG_MKRESPONSE(resp, msg, sizeof(*dir_conf), M_NOWAIT);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}

			dir_conf = (struct ng_mss_rewrite_direction *)resp->data;
			dir_conf->enable_lower = atomic_load_acq_8(&priv->enable_lower);
			dir_conf->enable_upper = atomic_load_acq_8(&priv->enable_upper);
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

	/* Determine output hook and check if processing is enabled for this direction */
	if (hook == priv->lower) {
		out_hook = priv->upper;
		/* Check if lower->upper processing is enabled (interface->kernel) */
		if (atomic_load_acq_8(&priv->enable_lower)) {
			m = ng_mss_rewrite_process(priv, m, 0);  /* from_upper=0 */
			if (m == NULL) {
				/* Packet was dropped during processing */
				NG_FREE_ITEM(item);
				return (0);
			}
		}
	} else if (hook == priv->upper) {
		out_hook = priv->lower;
		/* Check if upper->lower processing is enabled (kernel->interface) */
		if (atomic_load_acq_8(&priv->enable_upper)) {
			m = ng_mss_rewrite_process(priv, m, 1);  /* from_upper=1 */
			if (m == NULL) {
				/* Packet was dropped during processing */
				NG_FREE_ITEM(item);
				return (0);
			}
		}
	} else {
		m_freem(m);
		NG_FREE_ITEM(item);
		return (EINVAL);
	}

	if (out_hook == NULL) {
		m_freem(m);
		NG_FREE_ITEM(item);
		return (ENOTCONN);
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

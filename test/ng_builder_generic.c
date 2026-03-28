/*
 * Generic netgraph topology builder
 * Supports multiple topology types via command-line arguments
 *
 * Usage:
 *   ng_builder_generic simple        # Basic: socket -> mss_rewrite
 *   ng_builder_generic mbuf          # Mbuf test: inject -> mss -> hole
 *   ng_builder_generic wire_verify   # Wire verify: inject <-> mss (loop)
 *   ng_builder_generic test          # Test/bench: source -> mss -> hole (prefix=test)
 *   ng_builder_generic fuzz          # Fuzz test: source -> mss -> hole (prefix=fuzz)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netgraph/ng_message.h>
#include <netgraph/ng_socket.h>
#include <netgraph.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

/* Topology builder functions */
static void build_simple_topology(int cs);
static void build_mbuf_topology(int cs);
static void build_wire_verify_topology(int cs);
static void build_bench_topology(int cs, const char *prefix);

/* Helper functions */
static void ng_mkpeer_node(int cs, const char *path, const char *type,
    const char *ourhook, const char *peerhook, const char *name);
static void ng_name_node(int cs, const char *path, const char *name);
static void ng_connect_nodes(int cs, const char *path, const char *ourhook,
    const char *peerhook, const char *peerpath);
static void ng_disconnect_hook(int cs, const char *path, const char *hook);

int
main(int argc, char *argv[])
{
	int cs, ds;
	const char *topology = "simple";

	/* Parse arguments */
	if (argc > 1)
		topology = argv[1];

	/* Create control socket */
	if (NgMkSockNode(NULL, &cs, &ds) < 0)
		err(1, "NgMkSockNode");

	printf("Building '%s' topology...\n", topology);

	/* Build requested topology */
	if (strcmp(topology, "simple") == 0) {
		build_simple_topology(cs);
	} else if (strcmp(topology, "mbuf") == 0) {
		build_mbuf_topology(cs);
	} else if (strcmp(topology, "wire_verify") == 0) {
		build_wire_verify_topology(cs);
	} else if (strcmp(topology, "test") == 0) {
		build_bench_topology(cs, "test");
	} else if (strcmp(topology, "fuzz") == 0) {
		build_bench_topology(cs, "fuzz");
	} else {
		fprintf(stderr, "Unknown topology type: %s\n", topology);
		fprintf(stderr, "Valid types: simple, mbuf, wire_verify, test, fuzz\n");
		close(cs);
		close(ds);
		exit(1);
	}

	printf("\nTopology created successfully!\n");
	printf("Press Ctrl-C to exit and cleanup...\n");
	pause();

	close(cs);
	close(ds);
	return (0);
}

/*
 * Build simple topology: socket -> mss_rewrite
 */
static void
build_simple_topology(int cs)
{
	printf("Creating: socket -> mss_rewrite\n");

	/* Create mss_rewrite node */
	ng_mkpeer_node(cs, ".", "mss_rewrite", "hook", "upper", "mss_rewrite");

	printf("Final: socket:hook <-> mss_rewrite:upper\n");
}

/*
 * Build mbuf test topology: inject -> mss_rewrite -> hole
 */
static void
build_mbuf_topology(int cs)
{
	printf("Creating: mbuf_inject -> mss_rewrite -> hole\n");
	printf("Strategy: Build chain, then disconnect socket\n\n");

	/* Step 1: socket -> mss_rewrite */
	ng_mkpeer_node(cs, ".", "mss_rewrite", "hook1", "upper", "mbuf_test_mss");
	printf("Created mss_rewrite node: mbuf_test_mss\n");

	/* Step 2: mss_rewrite:lower -> mbuf_inject */
	ng_mkpeer_node(cs, "mbuf_test_mss:", "mbuf_inject", "lower", "output",
	    "mbuf_test_inject");
	printf("Created mbuf_inject node: mbuf_test_inject\n");

	/* Step 3: Disconnect socket from mss_rewrite */
	ng_disconnect_hook(cs, ".", "hook1");
	printf("Disconnected socket from mss_rewrite\n");

	/* Step 4: mss_rewrite:upper -> hole */
	ng_mkpeer_node(cs, "mbuf_test_mss:", "hole", "upper", "data",
	    "mbuf_test_hole");
	printf("Created hole node: mbuf_test_hole\n");

	printf("\nFinal: mbuf_test_inject:output <-> mbuf_test_mss:lower\n");
	printf("                                    mbuf_test_mss:upper <-> mbuf_test_hole:data\n");
}

/*
 * Build wire verification topology: inject:output -> mss:lower -> mss:upper -> inject:input
 */
static void
build_wire_verify_topology(int cs)
{
	printf("Creating: inject:output <-> mss <-> inject:input (loop)\n");
	printf("Strategy: socket -> mss -> inject, then loop back\n\n");

	/* Step 1: socket -> mss_rewrite (temporary anchor) */
	ng_mkpeer_node(cs, ".", "mss_rewrite", "hook1", "upper", "wire_verify_mss");
	printf("Created mss_rewrite node: wire_verify_mss\n");

	/* Step 2: mss_rewrite:lower -> mbuf_inject */
	ng_mkpeer_node(cs, "wire_verify_mss:", "mbuf_inject", "lower", "output",
	    "wire_verify_inject");
	printf("Created mbuf_inject node: wire_verify_inject\n");

	/* Step 3: Disconnect socket (frees mss_rewrite:upper) */
	ng_disconnect_hook(cs, ".", "hook1");
	printf("Disconnected socket from mss_rewrite\n");

	/* Step 4: Connect mss_rewrite:upper -> inject:input (creates loop) */
	ng_connect_nodes(cs, "wire_verify_mss:", "upper", "input",
	    "wire_verify_inject:");
	printf("Connected mss_rewrite:upper -> inject:input\n");

	printf("\nFinal: inject:output <-> mss:lower <-> mss:upper <-> inject:input\n");
}

/*
 * Build benchmark topology: source -> mss_rewrite -> hole
 * Used for test_cases.sh and test_fuzz.sh
 */
static void
build_bench_topology(int cs, const char *prefix)
{
	char node_name[NG_NODESIZ];

	printf("Creating: source -> mss_rewrite -> hole (prefix=%s)\n", prefix);
	printf("Strategy: socket -> mss, add source/hole, then disconnect socket\n\n");

	/* Step 1: socket -> mss_rewrite (temporary anchor) */
	snprintf(node_name, sizeof(node_name), "%s_mss", prefix);
	ng_mkpeer_node(cs, ".", "mss_rewrite", "hook1", "upper", node_name);
	printf("Created mss_rewrite node: %s\n", node_name);

	/* Step 2: mss_rewrite:lower -> source */
	snprintf(node_name, sizeof(node_name), "%s_mss:", prefix);
	char source_name[NG_NODESIZ];
	snprintf(source_name, sizeof(source_name), "%s_source", prefix);
	ng_mkpeer_node(cs, node_name, "source", "lower", "output", source_name);
	printf("Created source node: %s\n", source_name);

	/* Step 3: Disconnect socket (frees mss_rewrite:upper) */
	ng_disconnect_hook(cs, ".", "hook1");
	printf("Disconnected socket from mss_rewrite\n");

	/* Step 4: mss_rewrite:upper -> hole */
	snprintf(node_name, sizeof(node_name), "%s_mss:", prefix);
	char hole_name[NG_NODESIZ];
	snprintf(hole_name, sizeof(hole_name), "%s_hole", prefix);
	ng_mkpeer_node(cs, node_name, "hole", "upper", "data", hole_name);
	printf("Created hole node: %s\n", hole_name);

	printf("\nFinal: %s_source:output <-> %s_mss:lower\n", prefix, prefix);
	printf("                           %s_mss:upper <-> %s_hole:data\n", prefix, prefix);
}

/* Helper: Create peer node and name it */
static void
ng_mkpeer_node(int cs, const char *path, const char *type,
    const char *ourhook, const char *peerhook, const char *name)
{
	struct ngm_mkpeer mkp;
	char namepath[NG_PATHSIZ];

	/* Create peer */
	snprintf(mkp.type, sizeof(mkp.type), "%s", type);
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", ourhook);
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "%s", peerhook);

	if (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer %s", type);

	/* Name the node - construct path to newly created peer */
	if (strcmp(path, ".") == 0) {
		/* From socket node: use .:[hook] */
		snprintf(namepath, sizeof(namepath), ".:%s", ourhook);
	} else {
		/* From named node: use name:[hook] */
		snprintf(namepath, sizeof(namepath), "%s%s", path, ourhook);
	}
	ng_name_node(cs, namepath, name);
}

/* Helper: Name a node */
static void
ng_name_node(int cs, const char *path, const char *name)
{
	struct ngm_name nm;

	snprintf(nm.name, sizeof(nm.name), "%s", name);
	if (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name %s", name);
}

/* Helper: Connect two existing nodes */
static void
ng_connect_nodes(int cs, const char *path, const char *ourhook,
    const char *peerhook, const char *peerpath)
{
	struct ngm_connect cn;

	snprintf(cn.path, sizeof(cn.path), "%s", peerpath);
	snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", ourhook);
	snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", peerhook);

	if (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, NGM_CONNECT,
	    &cn, sizeof(cn)) < 0)
		err(1, "connect %s:%s to %s:%s", path, ourhook, peerpath, peerhook);
}

/* Helper: Disconnect a hook */
static void
ng_disconnect_hook(int cs, const char *path, const char *hook)
{
	struct ngm_rmhook rmh;

	snprintf(rmh.ourhook, sizeof(rmh.ourhook), "%s", hook);
	if (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, NGM_RMHOOK,
	    &rmh, sizeof(rmh)) < 0)
		err(1, "rmhook %s", hook);
}

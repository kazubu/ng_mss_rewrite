/*
 * Simple netgraph topology builder for benchmarking
 * Builds: source -> mss_rewrite -> hole
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

int
main(int argc, char *argv[])
{
	int cs, ds;
	struct ngm_mkpeer mkp;
	struct ngm_name nm;
	char path[NG_PATHSIZ];

	/* Create control socket */
	if (NgMkSockNode(NULL, &cs, &ds) < 0)
		err(1, "NgMkSockNode");

	printf("Building netgraph topology...\n");
	printf("Topology: source -> mss_rewrite -> hole\n");

	/* Create mss_rewrite first (connected to socket) */
	snprintf(mkp.type, sizeof(mkp.type), "mss_rewrite");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "hook1");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "upper");

	if (NgSendMsg(cs, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer mss_rewrite");

	/* Name the mss_rewrite node */
	snprintf(nm.name, sizeof(nm.name), "mss_bench_mss");
	if (NgSendMsg(cs, ".:hook1", NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name mss_rewrite");

	printf("Created mss_rewrite node: mss_bench_mss\n");

	/* Create source on mss_rewrite's lower hook */
	snprintf(mkp.type, sizeof(mkp.type), "source");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "lower");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "output");

	if (NgSendMsg(cs, "mss_bench_mss:", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer source");

	/* Name the source node */
	snprintf(nm.name, sizeof(nm.name), "mss_bench_source");
	if (NgSendMsg(cs, "mss_bench_mss:lower", NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name source");

	printf("Created source node: mss_bench_source\n");

	/* Create hole on mss_rewrite's upper hook (already used by socket, need to disconnect) */
	/* Actually, the socket is on upper, so we need to create hole differently */
	/* Disconnect socket first, then add hole */

	/* Disconnect the socket hook */
	struct ngm_rmhook rmh;
	snprintf(rmh.ourhook, sizeof(rmh.ourhook), "hook1");
	if (NgSendMsg(cs, ".", NGM_GENERIC_COOKIE, NGM_RMHOOK,
	    &rmh, sizeof(rmh)) < 0)
		err(1, "rmhook");

	printf("Disconnected socket from mss_rewrite\n");

	/* Now create hole on mss_rewrite's upper hook */
	snprintf(mkp.type, sizeof(mkp.type), "hole");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "upper");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "data");

	if (NgSendMsg(cs, "mss_bench_mss:", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer hole");

	/* Name the hole node */
	snprintf(nm.name, sizeof(nm.name), "mss_bench_hole");
	if (NgSendMsg(cs, "mss_bench_mss:upper", NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name hole");

	printf("Created hole node: mss_bench_hole\n");

	printf("Created hole node: mss_bench_hole\n");

	printf("\nTopology created successfully!\n");
	printf("Keeping socket alive (press Ctrl+C to destroy)...\n");

	/* Keep the socket open so nodes stay alive */
	/* The shell script can now configure and use these nodes */
	pause();

	close(cs);
	close(ds);
	return (0);
}

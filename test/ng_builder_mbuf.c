/*
 * Netgraph topology builder for mbuf shape testing
 * Builds: socket -> mbuf_inject -> mss_rewrite -> hole
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
	struct ngm_rmhook rmh;
	char path[NG_PATHSIZ];

	/* Create control socket */
	if (NgMkSockNode(NULL, &cs, &ds) < 0)
		err(1, "NgMkSockNode");

	printf("Building mbuf test topology...\n");
	printf("Strategy: Build chain without disconnecting, then disconnect socket at end\n");

	/* Step 1: Create mss_rewrite from socket */
	snprintf(mkp.type, sizeof(mkp.type), "mss_rewrite");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "hook1");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "upper");

	if (NgSendMsg(cs, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer mss_rewrite");

	/* Name the mss_rewrite node */
	snprintf(nm.name, sizeof(nm.name), "mbuf_test_mss");
	if (NgSendMsg(cs, ".:hook1", NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name mss_rewrite");

	printf("Created mss_rewrite node: mbuf_test_mss\n");

	/* Step 2: Create mbuf_inject on mss_rewrite's lower hook */
	snprintf(mkp.type, sizeof(mkp.type), "mbuf_inject");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "lower");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "output");

	if (NgSendMsg(cs, "mbuf_test_mss:", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer mbuf_inject");

	/* Name the mbuf_inject node */
	snprintf(nm.name, sizeof(nm.name), "mbuf_test_inject");
	snprintf(path, sizeof(path), "mbuf_test_mss:lower");
	if (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name mbuf_inject");

	printf("Created mbuf_inject node: mbuf_test_inject\n");

	/* Step 3: Create hole on mss_rewrite's upper hook (disconnect socket first) */
	snprintf(rmh.ourhook, sizeof(rmh.ourhook), "hook1");
	if (NgSendMsg(cs, ".", NGM_GENERIC_COOKIE, NGM_RMHOOK,
	    &rmh, sizeof(rmh)) < 0)
		err(1, "rmhook socket");

	printf("Disconnected socket from mss_rewrite\n");

	/* Step 4: Create hole on mss_rewrite's upper hook */
	snprintf(mkp.type, sizeof(mkp.type), "hole");
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "upper");
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "data");

	if (NgSendMsg(cs, "mbuf_test_mss:", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0)
		err(1, "mkpeer hole");

	/* Name the hole node */
	snprintf(nm.name, sizeof(nm.name), "mbuf_test_hole");
	snprintf(path, sizeof(path), "mbuf_test_mss:upper");
	if (NgSendMsg(cs, path, NGM_GENERIC_COOKIE, NGM_NAME,
	    &nm, sizeof(nm)) < 0)
		err(1, "name hole");

	printf("Created hole node: mbuf_test_hole\n");

	printf("\nTopology created successfully!\n");
	printf("Topology: mbuf_test_inject -> mbuf_test_mss -> mbuf_test_hole\n");

	/* Keep socket open so nodes stay alive */
	printf("\nPress Ctrl-C to exit and cleanup...\n");
	pause();

	close(cs);
	close(ds);
	return (0);
}

# Makefile for ng_mss_rewrite kernel module

KMOD=	ng_mss_rewrite
SRCS=	ng_mss_rewrite.c

# Optional: Enable debug statistics (set DEBUG_STATS=1 on make command line)
.if defined(DEBUG_STATS) && ${DEBUG_STATS} == "1"
CFLAGS+= -DENABLE_DEBUG_STATS=1
.endif

.include <bsd.kmod.mk>

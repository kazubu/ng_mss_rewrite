# Makefile for ng_mss_rewrite kernel module

KMOD=	ng_mss_rewrite
SRCS=	ng_mss_rewrite.c

.include <bsd.kmod.mk>

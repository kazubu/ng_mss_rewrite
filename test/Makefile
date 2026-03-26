PROG=	ng_builder
SRCS=	ng_builder.c
LDADD=	-lnetgraph

.include <bsd.prog.mk>

# Additional target for ng_builder_mbuf
ng_builder_mbuf: ng_builder_mbuf.c
	${CC} ${CFLAGS} -o ng_builder_mbuf ng_builder_mbuf.c -lnetgraph

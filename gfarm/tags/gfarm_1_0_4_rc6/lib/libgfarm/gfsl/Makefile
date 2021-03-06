# $Id$

top_builddir = ../../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

###
### variables
###

CFLAGS = $(COMMON_CFLAGS) -I$(GFUTIL_SRCDIR) -I$(GFSL_SRCDIR) \
	$(globus_includes) \
	-DGLOBUS_BUG=1 # see gsi.c

### for library

LIBRARY = libgfsl.la
LIBRARY_RESULT = # do not install

SRCS = gsi.c tcputil.c auth.c session.c misc.c
OBJS = gsi.lo tcputil.lo auth.lo session.lo misc.lo

### for programs

PURE = /usr/local/rational/releases/purify.sol.2001a.04.00/purify
# LINK = $(PURE) $(CC) $(CFLAGS)
# PROG_DEPLIBS = ../gfutil/.libs/libgfutil.a .libs/libgfsl.a

LINK = $(LTLINK)

PROGRAMS = server client server2 client2
SCOBJS = scarg.o scsub.o
SCOBJS2 = scarg.o
PROG_DEPLIBS = $(LIBRARY) ../gfutil/libgfutil.la
PROG_LDLIBS = $(COMMON_LDFLAGS) $(PROG_DEPLIBS) \
	$(globus_gssapi_libs) $(openssl_libs) $(LIBS)

###
### rules
###

all: $(LIBRARY) $(PROGRAMS)

### for library

$(GFSL_OBJS): $(DEPGFSLINC)

include $(top_srcdir)/makes/lib.mk

# dependencies
auth.lo: $(GFUTIL_SRCDIR)/hash.h $(GFUTIL_SRCDIR)/gfutil.h
auth.lo: gfsl_config.h gfarm_auth.h misc.h
gsi.lo: $(GFUTIL_SRCDIR)/gfevent.h $(GFUTIL_SRCDIR)/gfutil.h
gsi.lo: gfsl_config.h gfarm_gsi.h tcputil.h
misc.lo: gfsl_config.h gfarm_auth.h
session.lo: $(GFUTIL_SRCDIR)/gfevent.h $(GFUTIL_SRCDIR)/gfutil.h
session.lo: gfarm_secure_session.h tcputil.h misc.h
tcputil.lo: $(GFUTIL_SRCDIR)/gfutil.h tcputil.h

server.o server2.o client.o client2.o: $(GFUTIL_SRCDIR)/gfutil.h \
    tcputil.h gfsl_config.h gfarm_gsi.h gfarm_secure_session.h misc.h scarg.h
server.o server2.o: gfarm_auth.h
scarg.o: misc.h scarg.h
scsub.o: tcputil.h gfarm_secure_session.h gfarm_auth.h

### for programs

server:	server.o $(SCOBJS) $(PROG_DEPLIBS)
	rm -f $@
	$(LINK) -o $@ server.o $(SCOBJS) $(PROG_LDLIBS)

client:	client.o $(SCOBJS) $(PROG_DEPLIBS)
	rm -f $@
	$(LINK) -o $@ client.o $(SCOBJS) $(PROG_LDLIBS)

server2: server2.o $(SCOBJS2) $(PROG_DEPLIBS)
	rm -f $@
	$(LINK) -o $@ server2.o $(SCOBJS2) $(PROG_LDLIBS)

client2: client2.o $(SCOBJS2) $(PROG_DEPLIBS)
	rm -f $@
	$(LINK) -o $@ client2.o $(SCOBJS2) $(PROG_LDLIBS)

clean:
	$(RM) -f *.o

veryclean:
	$(LTCLEAN) $(PROGRAMS) *~ core *.core

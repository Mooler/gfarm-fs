# $Id$

top_builddir = ../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

PROGRAM = gfhost
SRCS = $(PROGRAM).c
OBJS = $(PROGRAM).o
CFLAGS = $(COMMON_CFLAGS) -I$(GFUTIL_SRCDIR) -I$(GFARMLIB_SRCDIR)
LDLIBS = $(COMMON_LDFLAGS) $(GFARMLIB) $(LIBS)
DEPLIBS = $(DEPGFARMLIB)

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk

###

$(OBJS): $(DEPGFARMINC) $(GFUTIL_SRCDIR)/gfevent.h $(GFARMLIB_SRCDIR)/gfs_client.h $(GFARMLIB_SRCDIR)/host.h 

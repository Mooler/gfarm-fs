# $Id$

top_objdir = ../..
top_srcdir = $(top_objdir)
srcdir = .

include $(top_srcdir)/makes/var.mk

SRCS = gfrcmd.c
OBJS = gfrcmd.o
PROGRAM = gfrcmd
CFLAGS = $(COMMON_CFLAGS) -I$(GFARMLIB_SRCDIR)
LDLIBS = $(COMMON_LDFLAGS) $(GFARMLIB) $(LIBS)
DEPLIBS = $(DEPGFARMLIB)

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk

###

$(OBJS): $(DEPGFARMINC)

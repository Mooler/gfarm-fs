top_builddir = ..
top_srcdir = $(top_builddir)
srcdir = .

GFVOMS_SYNC_TARGETS = gfvoms-sync

include $(top_srcdir)/makes/var.mk
PRIVATE_TARGETS = $(PRIVATE_GFTOOL_SUBDIRS)
PUBLIC_TARGETS =

SUBDIRS = \
	gfchmod \
	gfchown \
	gfdf \
	gfdump \
	gfexport \
	gffindxmlattr \
	gfgetfacl \
	gfgroup \
	gfhost \
	gfjournal \
	gfjournaladmin \
	gfkey \
	gfln \
	gfls \
	gfmdhost \
	gfmkdir \
	gfmv \
	gfncopy \
	gfpath \
	gfprep \
	gfquota \
	gfreg \
	gfrep \
	gfrm \
	gfrmdir \
	gfsched \
	gfsetfacl \
	gfspoolpath \
	gfstat \
	gfstatus \
	gftest \
	gfuser \
	gfusage \
	$(gfvoms_sync_targets) \
	gfwhere \
	gfwhoami \
	gfxattr \
	config-gfarm \
	$(optional_targets)

#	gfifo \
#	gfpwd \
#	gfsck \
#	gfsetdir \
#	gfq \

include $(top_srcdir)/makes/subdir.mk

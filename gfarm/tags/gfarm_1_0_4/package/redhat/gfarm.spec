# Part 1 data definition
%define pkg	gfarm
%define ver	1.0.4
%define rel	1

# a hook to make RPM version number different from %{ver}
%define pkgver	%{ver}

%define prefix		/usr
%define man_prefix	/usr/share/man
%define doc_prefix	/usr/share/doc/%{name}-%{ver}
%define html_prefix	%{doc_prefix}/html
%define rc_prefix	/etc/rc.d/init.d
%define etc_prefix	/etc
%define ldap_etc_prefix	%{etc_prefix}/gfarm-ldap
%define profile_etc_prefix %{etc_prefix}/profile.d

# whether "ns" is included in this release or not.
%define have_ns	0

#
# check && enable/disable MPI
#
# do the followings to make gfwc
#   # env MPI_PREFIX=/usr/local/mpich rpmbuild -bb gfarm.spec

%define	mpi_prefix	%(echo "${MPI_PREFIX}")

%define mpi	%(test -x %{mpi_prefix}/bin/mpicc && echo 1 || echo 0)

#
# check && enable/disable Globus
#
# do the followings to build gfarm-gsi-*.rpm:
#   # env GLOBUS_PREFIX=/usr/grid GLOBUS_FLAVOR=gcc32 rpmbuild -bb gfarm.spec
# or
#   # env GLOBUS_PREFIX=/usr/grid GLOBUS_FLAVOR=gcc32 \
#	GFARM_CONFIGURE_OPTION=--with-globus-static rpmbuild -bb gfarm.spec

%define	globus_prefix	%(echo "${GLOBUS_PREFIX}")
%define	globus_flavor	%(echo "${GLOBUS_FLAVOR}")

%define globus %(test -n "${GLOBUS_PREFIX}" -a -n "${GLOBUS_FLAVOR}" -a -r %{globus_prefix}/include/%{globus_flavor}/gssapi.h && echo 1 || echo 0)

%if %{globus}
%define package_name	%{pkg}-gsi
%else
%define package_name	%{pkg}
%endif

Summary: Grid Datafarm
Name: %{package_name}
Version: %pkgver
Release: %rel
Source: %{pkg}-%{ver}.tar.gz
#Patch: 
Copyright: National Institute of Advanced Industrial Science and Technology
Group: Local
Packager: Tohru Sotoyama <sotoyama@sra.co.jp>
BuildRoot: %{_tmppath}/%{name}-%{version}-buildroot

%package doc
Summary: document for gfarm
Group: Documentation

%package libs
Summary: runtime libraries for gfarm
Group: System Environment/Libraries

%package frontend
Summary: frontends for gfarm
Group: Applications/Internet

%package client
Summary: clients for gfarm
Group: Applications/Internet

%package gfptool
Summary: parallel tools installed under gfarm:/bin/
Group: System Environment/Daemons
Requires: %{package_name}-client

%package fsnode
Summary: gfsd for gfarm
Group: System Environment/Daemons

%package server
Summary: server for gfarm
Group: System Environment/Daemons

%package devel
Summary: development library for gfarm
Group: Development/Libraries

%package gfront
Summary: file system browser for gfarm
Group: Applications/Internet

%description
gfarm - Grid datafarm 

%description doc
doc for gfarm

%description libs
runtime libraries for gfarm

%description frontend
frontends for gfarm

%description client
clients for gfarm

%description gfptool
parallel tools installed under gfarm:/bin

%description fsnode
fsnode for gfarm

%description server
metadb server for gfarm

%description devel
development library for gfarm

%description gfront
file system browser for gfarm

%changelog
* Fri Apr 24 2003 Tohru Sotoyama <sotoyama@sra.co.jp>
- first public release for version 1.0b1

* Wed Nov 27 2002 Tohru Sotoyama <sotoyama@sra.co.jp>
- first release for version 0.1

# Part 2 script
%prep
rm -rf ${RPM_BUILD_ROOT}
mkdir -p $RPM_BUILD_ROOT

%setup -n %{pkg}-%{ver}
#%patch -p1

%build
./configure --prefix=%{prefix} \
	--with-openldap=/usr \
	--with-openssl=/usr \
	--with-readline=/usr \
	`test "%{globus}" -ne 0 && echo	\
		--with-globus=%{globus_prefix} \
		--with-globus-flavor=%{globus_flavor}` \
	`test "%{mpi}" -ne 0 && echo --with-mpi=%{mpi_prefix}` \
	${GFARM_CONFIGURE_OPTION}

make

%install
make prefix=${RPM_BUILD_ROOT}%{prefix} \
	default_docdir=${RPM_BUILD_ROOT}%{doc_prefix} \
	default_mandir=${RPM_BUILD_ROOT}%{man_prefix} \
	example_bindir=${RPM_BUILD_ROOT}%{prefix}/bin install 
mkdir -p ${RPM_BUILD_ROOT}%{rc_prefix}
cp -p package/redhat/gfmd package/redhat/gfsd package/redhat/gfarm-slapd \
	${RPM_BUILD_ROOT}%{rc_prefix}
chmod +x ${RPM_BUILD_ROOT}%{rc_prefix}/*
mkdir -p ${RPM_BUILD_ROOT}%{etc_prefix}
cp -p doc/conf/gfarm.conf ${RPM_BUILD_ROOT}%{etc_prefix}
mkdir -p ${RPM_BUILD_ROOT}%{ldap_etc_prefix}
cp -p doc/conf/gfarm.schema ${RPM_BUILD_ROOT}%{ldap_etc_prefix}
mkdir -p ${RPM_BUILD_ROOT}%{profile_etc_prefix}
cp -p package/redhat/gfarm.{csh,sh} ${RPM_BUILD_ROOT}%{profile_etc_prefix}
chmod +x ${RPM_BUILD_ROOT}%{profile_etc_prefix}/*

%clean
rm -rf ${RPM_BUILD_ROOT}

%post libs -p /sbin/ldconfig

%postun libs -p /sbin/ldconfig

%post fsnode
/sbin/chkconfig --add gfsd

%post server
/sbin/chkconfig --add gfmd
/sbin/chkconfig --add gfarm-slapd

%preun fsnode
if [ "$1" = 0 ]
then
	/sbin/service gfsd stop > /dev/null 2>&1 || :
	/sbin/chkconfig --del gfsd
fi

%preun server
if [ "$1" = 0 ]
then
	/sbin/service gfmd stop > /dev/null 2>&1 || :
	/sbin/service gfarm-slpad stop > /dev/null 2>&1 || :
	/sbin/chkconfig --del gfmd
	/sbin/chkconfig --del gfarm-slapd
fi

# Part 3  file list
%files doc
%{man_prefix}/man1/digest.1.gz
%{man_prefix}/man1/gfcd.1.gz
%{man_prefix}/man1/gfdf.1.gz
%{man_prefix}/man1/gfexec.1.gz
%{man_prefix}/man1/gfexport.1.gz
%{man_prefix}/man1/gfgrep.1.gz
%{man_prefix}/man1/gfhost.1.gz
%{man_prefix}/man1/gfimport_fixed.1.gz
%{man_prefix}/man1/gfimport_text.1.gz
%{man_prefix}/man1/gfkey.1.gz
%{man_prefix}/man1/gfls.1.gz
%{man_prefix}/man1/gfmkdir.1.gz
%{man_prefix}/man1/gfmpirun_p4.1.gz
%{man_prefix}/man1/gfps.1.gz
%{man_prefix}/man1/gfpwd.1.gz
%{man_prefix}/man1/gfrcmd.1.gz
%{man_prefix}/man1/gfreg.1.gz
%{man_prefix}/man1/gfrep.1.gz
%{man_prefix}/man1/gfrm.1.gz
%{man_prefix}/man1/gfrmdir.1.gz
%{man_prefix}/man1/gfront.1.gz
%{man_prefix}/man1/gfrsh.1.gz
%{man_prefix}/man1/gfrun.1.gz
%{man_prefix}/man1/gfsched.1.gz
%{man_prefix}/man1/gfsetdir.1.gz
%{man_prefix}/man1/gfssh.1.gz
%{man_prefix}/man1/gfstat.1.gz
%{man_prefix}/man1/gfwc.1.gz
%{man_prefix}/man1/gfwhere.1.gz
%{man_prefix}/man1/gfwhoami.1.gz
%{man_prefix}/man3/gfarm_initialize.3.gz
%{man_prefix}/man3/gfarm_strings_free_deeply.3.gz
%{man_prefix}/man3/gfarm_terminate.3.gz
%{man_prefix}/man3/gfarm_url_hosts_schedule.3.gz
%{man_prefix}/man3/gfs_chdir.3.gz
%{man_prefix}/man3/gfs_chmod.3.gz
%{man_prefix}/man3/gfs_closedir.3.gz
%{man_prefix}/man3/gfs_glob.3.gz
%{man_prefix}/man3/gfs_glob_add.3.gz
%{man_prefix}/man3/gfs_glob_free.3.gz
%{man_prefix}/man3/gfs_glob_init.3.gz
%{man_prefix}/man3/gfs_mkdir.3.gz
%{man_prefix}/man3/gfs_opendir.3.gz
%{man_prefix}/man3/gfs_pio_close.3.gz
%{man_prefix}/man3/gfs_pio_create.3.gz
%{man_prefix}/man3/gfs_pio_eof.3.gz
%{man_prefix}/man3/gfs_pio_error.3.gz
%{man_prefix}/man3/gfs_pio_flush.3.gz
%{man_prefix}/man3/gfs_pio_getc.3.gz
%{man_prefix}/man3/gfs_pio_getline.3.gz
%{man_prefix}/man3/gfs_pio_gets.3.gz
%{man_prefix}/man3/gfs_pio_open.3.gz
%{man_prefix}/man3/gfs_pio_putc.3.gz
%{man_prefix}/man3/gfs_pio_putline.3.gz
%{man_prefix}/man3/gfs_pio_puts.3.gz
%{man_prefix}/man3/gfs_pio_read.3.gz
%{man_prefix}/man3/gfs_pio_readdelim.3.gz
%{man_prefix}/man3/gfs_pio_readline.3.gz
%{man_prefix}/man3/gfs_pio_seek.3.gz
%{man_prefix}/man3/gfs_pio_set_local.3.gz
%{man_prefix}/man3/gfs_pio_set_view_index.3.gz
%{man_prefix}/man3/gfs_pio_set_view_local.3.gz
%{man_prefix}/man3/gfs_pio_ungetc.3.gz
%{man_prefix}/man3/gfs_pio_write.3.gz
%{man_prefix}/man3/gfs_readdir.3.gz
%{man_prefix}/man3/gfs_realpath.3.gz
%{man_prefix}/man3/gfs_rmdir.3.gz
%{man_prefix}/man3/gfs_stat.3.gz
%{man_prefix}/man3/gfs_stat_free.3.gz
%{man_prefix}/man3/gfs_unlink.3.gz
%{man_prefix}/man3/gfs_utimes.3.gz
%{man_prefix}/man5/gfarm.conf.5.gz
%{man_prefix}/man8/gfmd.8.gz
%{man_prefix}/man8/gfsd.8.gz
%{man_prefix}/ja/man1/digest.1.gz
%{man_prefix}/ja/man1/gfcd.1.gz
%{man_prefix}/ja/man1/gfdf.1.gz
%{man_prefix}/ja/man1/gfexec.1.gz
%{man_prefix}/ja/man1/gfexport.1.gz
%{man_prefix}/ja/man1/gfgrep.1.gz
%{man_prefix}/ja/man1/gfhost.1.gz
%{man_prefix}/ja/man1/gfimport_fixed.1.gz
%{man_prefix}/ja/man1/gfimport_text.1.gz
%{man_prefix}/ja/man1/gfkey.1.gz
%{man_prefix}/ja/man1/gfls.1.gz
%{man_prefix}/ja/man1/gfmkdir.1.gz
%{man_prefix}/ja/man1/gfmpirun_p4.1.gz
%{man_prefix}/ja/man1/gfps.1.gz
%{man_prefix}/ja/man1/gfpwd.1.gz
%{man_prefix}/ja/man1/gfrcmd.1.gz
%{man_prefix}/ja/man1/gfreg.1.gz
%{man_prefix}/ja/man1/gfrep.1.gz
%{man_prefix}/ja/man1/gfrm.1.gz
%{man_prefix}/ja/man1/gfrmdir.1.gz
%{man_prefix}/ja/man1/gfront.1.gz
%{man_prefix}/ja/man1/gfrsh.1.gz
%{man_prefix}/ja/man1/gfrun.1.gz
%{man_prefix}/ja/man1/gfsched.1.gz
%{man_prefix}/ja/man1/gfsetdir.1.gz
%{man_prefix}/ja/man1/gfssh.1.gz
%{man_prefix}/ja/man1/gfstat.1.gz
%{man_prefix}/ja/man1/gfwc.1.gz
%{man_prefix}/ja/man1/gfwhere.1.gz
%{man_prefix}/ja/man1/gfwhoami.1.gz
%{man_prefix}/ja/man3/GFARM_STRINGLIST_ELEM.3.gz
%{man_prefix}/ja/man3/GFARM_STRINGLIST_STRARRAY.3.gz
%{man_prefix}/ja/man3/gfarm_hostlist_read.3.gz
%{man_prefix}/ja/man3/gfarm_import_fragment_config_read.3.gz
%{man_prefix}/ja/man3/gfarm_import_fragment_size_alloc.3.gz
%{man_prefix}/ja/man3/gfarm_initialize.3.gz
%{man_prefix}/ja/man3/gfarm_stringlist_add.3.gz
%{man_prefix}/ja/man3/gfarm_stringlist_cat.3.gz
%{man_prefix}/ja/man3/gfarm_stringlist_elem.3.gz
%{man_prefix}/ja/man3/gfarm_stringlist_free.3.gz
%{man_prefix}/ja/man3/gfarm_stringlist_init.3.gz
%{man_prefix}/ja/man3/gfarm_stringlist_length.3.gz
%{man_prefix}/ja/man3/gfarm_strings_free_deeply.3.gz
%{man_prefix}/ja/man3/gfarm_terminate.3.gz
%{man_prefix}/ja/man3/gfarm_url_fragments_replicate.3.gz
%{man_prefix}/ja/man3/gfarm_url_hosts_schedule.3.gz
%{man_prefix}/ja/man3/gfarm_url_program_deliver.3.gz
%{man_prefix}/ja/man3/gfarm_url_program_register.3.gz
%{man_prefix}/ja/man3/gfarm_url_section_replicate_from_to.3.gz
%{man_prefix}/ja/man3/gfarm_url_section_replicate_to.3.gz
%{man_prefix}/ja/man3/gfs_chdir.3.gz
%{man_prefix}/ja/man3/gfs_chmod.3.gz
%{man_prefix}/ja/man3/gfs_closedir.3.gz
%{man_prefix}/ja/man3/gfs_glob.3.gz
%{man_prefix}/ja/man3/gfs_glob_add.3.gz
%{man_prefix}/ja/man3/gfs_glob_free.3.gz
%{man_prefix}/ja/man3/gfs_glob_init.3.gz
%{man_prefix}/ja/man3/gfs_mkdir.3.gz
%{man_prefix}/ja/man3/gfs_opendir.3.gz
%{man_prefix}/ja/man3/gfs_pio_close.3.gz
%{man_prefix}/ja/man3/gfs_pio_create.3.gz
%{man_prefix}/ja/man3/gfs_pio_eof.3.gz
%{man_prefix}/ja/man3/gfs_pio_error.3.gz
%{man_prefix}/ja/man3/gfs_pio_flush.3.gz
%{man_prefix}/ja/man3/gfs_pio_getc.3.gz
%{man_prefix}/ja/man3/gfs_pio_getline.3.gz
%{man_prefix}/ja/man3/gfs_pio_gets.3.gz
%{man_prefix}/ja/man3/gfs_pio_open.3.gz
%{man_prefix}/ja/man3/gfs_pio_putc.3.gz
%{man_prefix}/ja/man3/gfs_pio_putline.3.gz
%{man_prefix}/ja/man3/gfs_pio_puts.3.gz
%{man_prefix}/ja/man3/gfs_pio_read.3.gz
%{man_prefix}/ja/man3/gfs_pio_readdelim.3.gz
%{man_prefix}/ja/man3/gfs_pio_readline.3.gz
%{man_prefix}/ja/man3/gfs_pio_seek.3.gz
%{man_prefix}/ja/man3/gfs_pio_set_local.3.gz
%{man_prefix}/ja/man3/gfs_pio_set_view_index.3.gz
%{man_prefix}/ja/man3/gfs_pio_set_view_local.3.gz
%{man_prefix}/ja/man3/gfs_pio_ungetc.3.gz
%{man_prefix}/ja/man3/gfs_pio_write.3.gz
%{man_prefix}/ja/man3/gfs_readdir.3.gz
%{man_prefix}/ja/man3/gfs_realpath.3.gz
%{man_prefix}/ja/man3/gfs_rmdir.3.gz
%{man_prefix}/ja/man3/gfs_stat.3.gz
%{man_prefix}/ja/man3/gfs_stat_free.3.gz
%{man_prefix}/ja/man3/gfs_unlink.3.gz
%{man_prefix}/ja/man3/gfs_utimes.3.gz
%{man_prefix}/ja/man5/gfarm.conf.5.gz
%{man_prefix}/ja/man8/gfmd.8.gz
%{man_prefix}/ja/man8/gfsd.8.gz
%{html_prefix}/index.html
%{html_prefix}/en/ref/index.html
%{html_prefix}/en/ref/man1/digest.1.html
%{html_prefix}/en/ref/man1/gfcd.1.html
%{html_prefix}/en/ref/man1/gfdf.1.html
%{html_prefix}/en/ref/man1/gfexec.1.html
%{html_prefix}/en/ref/man1/gfexport.1.html
%{html_prefix}/en/ref/man1/gfgrep.1.html
%{html_prefix}/en/ref/man1/gfhost.1.html
%{html_prefix}/en/ref/man1/gfimport_fixed.1.html
%{html_prefix}/en/ref/man1/gfimport_text.1.html
%{html_prefix}/en/ref/man1/gfkey.1.html
%{html_prefix}/en/ref/man1/gfls.1.html
%{html_prefix}/en/ref/man1/gfmkdir.1.html
%{html_prefix}/en/ref/man1/gfmpirun_p4.1.html
%{html_prefix}/en/ref/man1/gfps.1.html
%{html_prefix}/en/ref/man1/gfpwd.1.html
%{html_prefix}/en/ref/man1/gfrcmd.1.html
%{html_prefix}/en/ref/man1/gfreg.1.html
%{html_prefix}/en/ref/man1/gfrep.1.html
%{html_prefix}/en/ref/man1/gfrm.1.html
%{html_prefix}/en/ref/man1/gfrmdir.1.html
%{html_prefix}/en/ref/man1/gfront.1.html
%{html_prefix}/en/ref/man1/gfrun.1.html
%{html_prefix}/en/ref/man1/gfsched.1.html
%{html_prefix}/en/ref/man1/gfsetdir.1.html
%{html_prefix}/en/ref/man1/gfstat.1.html
%{html_prefix}/en/ref/man1/gfwc.1.html
%{html_prefix}/en/ref/man1/gfwhere.1.html
%{html_prefix}/en/ref/man1/gfwhoami.1.html
%{html_prefix}/en/ref/man3/gfarm_initialize.3.html
%{html_prefix}/en/ref/man3/gfarm_strings_free_deeply.3.html
%{html_prefix}/en/ref/man3/gfarm_terminate.3.html
%{html_prefix}/en/ref/man3/gfarm_url_hosts_schedule.3.html
%{html_prefix}/en/ref/man3/gfs_chdir.3.html
%{html_prefix}/en/ref/man3/gfs_chmod.3.html
%{html_prefix}/en/ref/man3/gfs_closedir.3.html
%{html_prefix}/en/ref/man3/gfs_glob.3.html
%{html_prefix}/en/ref/man3/gfs_glob_add.3.html
%{html_prefix}/en/ref/man3/gfs_glob_free.3.html
%{html_prefix}/en/ref/man3/gfs_glob_init.3.html
%{html_prefix}/en/ref/man3/gfs_mkdir.3.html
%{html_prefix}/en/ref/man3/gfs_opendir.3.html
%{html_prefix}/en/ref/man3/gfs_pio_close.3.html
%{html_prefix}/en/ref/man3/gfs_pio_create.3.html
%{html_prefix}/en/ref/man3/gfs_pio_eof.3.html
%{html_prefix}/en/ref/man3/gfs_pio_error.3.html
%{html_prefix}/en/ref/man3/gfs_pio_flush.3.html
%{html_prefix}/en/ref/man3/gfs_pio_getc.3.html
%{html_prefix}/en/ref/man3/gfs_pio_getline.3.html
%{html_prefix}/en/ref/man3/gfs_pio_gets.3.html
%{html_prefix}/en/ref/man3/gfs_pio_open.3.html
%{html_prefix}/en/ref/man3/gfs_pio_putc.3.html
%{html_prefix}/en/ref/man3/gfs_pio_putline.3.html
%{html_prefix}/en/ref/man3/gfs_pio_puts.3.html
%{html_prefix}/en/ref/man3/gfs_pio_read.3.html
%{html_prefix}/en/ref/man3/gfs_pio_readdelim.3.html
%{html_prefix}/en/ref/man3/gfs_pio_readline.3.html
%{html_prefix}/en/ref/man3/gfs_pio_seek.3.html
%{html_prefix}/en/ref/man3/gfs_pio_set_local.3.html
%{html_prefix}/en/ref/man3/gfs_pio_set_view_index.3.html
%{html_prefix}/en/ref/man3/gfs_pio_set_view_local.3.html
%{html_prefix}/en/ref/man3/gfs_pio_ungetc.3.html
%{html_prefix}/en/ref/man3/gfs_pio_write.3.html
%{html_prefix}/en/ref/man3/gfs_readdir.3.html
%{html_prefix}/en/ref/man3/gfs_realpath.3.html
%{html_prefix}/en/ref/man3/gfs_rmdir.3.html
%{html_prefix}/en/ref/man3/gfs_stat.3.html
%{html_prefix}/en/ref/man3/gfs_stat_free.3.html
%{html_prefix}/en/ref/man3/gfs_unlink.3.html
%{html_prefix}/en/ref/man3/gfs_utimes.3.html
%{html_prefix}/en/ref/man5/gfarm.conf.5.html
%{html_prefix}/en/ref/man8/gfmd.8.html
%{html_prefix}/en/ref/man8/gfsd.8.html
%{html_prefix}/ja/ref/index.html
%{html_prefix}/ja/ref/man1/digest.1.html
%{html_prefix}/ja/ref/man1/gfcd.1.html
%{html_prefix}/ja/ref/man1/gfdf.1.html
%{html_prefix}/ja/ref/man1/gfexec.1.html
%{html_prefix}/ja/ref/man1/gfexport.1.html
%{html_prefix}/ja/ref/man1/gfgrep.1.html
%{html_prefix}/ja/ref/man1/gfhost.1.html
%{html_prefix}/ja/ref/man1/gfimport_fixed.1.html
%{html_prefix}/ja/ref/man1/gfimport_text.1.html
%{html_prefix}/ja/ref/man1/gfkey.1.html
%{html_prefix}/ja/ref/man1/gfls.1.html
%{html_prefix}/ja/ref/man1/gfmkdir.1.html
%{html_prefix}/ja/ref/man1/gfmpirun_p4.1.html
%{html_prefix}/ja/ref/man1/gfps.1.html
%{html_prefix}/ja/ref/man1/gfpwd.1.html
%{html_prefix}/ja/ref/man1/gfrcmd.1.html
%{html_prefix}/ja/ref/man1/gfreg.1.html
%{html_prefix}/ja/ref/man1/gfrep.1.html
%{html_prefix}/ja/ref/man1/gfrm.1.html
%{html_prefix}/ja/ref/man1/gfrmdir.1.html
%{html_prefix}/ja/ref/man1/gfront.1.html
%{html_prefix}/ja/ref/man1/gfrun.1.html
%{html_prefix}/ja/ref/man1/gfsched.1.html
%{html_prefix}/ja/ref/man1/gfsetdir.1.html
%{html_prefix}/ja/ref/man1/gfstat.1.html
%{html_prefix}/ja/ref/man1/gfwc.1.html
%{html_prefix}/ja/ref/man1/gfwhere.1.html
%{html_prefix}/ja/ref/man1/gfwhoami.1.html
%{html_prefix}/ja/ref/man3/GFARM_STRINGLIST_ELEM.3.html
%{html_prefix}/ja/ref/man3/GFARM_STRINGLIST_STRARRAY.3.html
%{html_prefix}/ja/ref/man3/gfarm_hostlist_read.3.html
%{html_prefix}/ja/ref/man3/gfarm_import_fragment_config_read.3.html
%{html_prefix}/ja/ref/man3/gfarm_import_fragment_size_alloc.3.html
%{html_prefix}/ja/ref/man3/gfarm_initialize.3.html
%{html_prefix}/ja/ref/man3/gfarm_stringlist_add.3.html
%{html_prefix}/ja/ref/man3/gfarm_stringlist_cat.3.html
%{html_prefix}/ja/ref/man3/gfarm_stringlist_elem.3.html
%{html_prefix}/ja/ref/man3/gfarm_stringlist_free.3.html
%{html_prefix}/ja/ref/man3/gfarm_stringlist_init.3.html
%{html_prefix}/ja/ref/man3/gfarm_stringlist_length.3.html
%{html_prefix}/ja/ref/man3/gfarm_strings_free_deeply.3.html
%{html_prefix}/ja/ref/man3/gfarm_terminate.3.html
%{html_prefix}/ja/ref/man3/gfarm_url_fragments_replicate.3.html
%{html_prefix}/ja/ref/man3/gfarm_url_hosts_schedule.3.html
%{html_prefix}/ja/ref/man3/gfarm_url_program_deliver.3.html
%{html_prefix}/ja/ref/man3/gfarm_url_program_register.3.html
%{html_prefix}/ja/ref/man3/gfarm_url_section_replicate_from_to.3.html
%{html_prefix}/ja/ref/man3/gfarm_url_section_replicate_to.3.html
%{html_prefix}/ja/ref/man3/gfs_chdir.3.html
%{html_prefix}/ja/ref/man3/gfs_chmod.3.html
%{html_prefix}/ja/ref/man3/gfs_closedir.3.html
%{html_prefix}/ja/ref/man3/gfs_glob.3.html
%{html_prefix}/ja/ref/man3/gfs_glob_add.3.html
%{html_prefix}/ja/ref/man3/gfs_glob_free.3.html
%{html_prefix}/ja/ref/man3/gfs_glob_init.3.html
%{html_prefix}/ja/ref/man3/gfs_mkdir.3.html
%{html_prefix}/ja/ref/man3/gfs_opendir.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_close.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_create.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_eof.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_error.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_flush.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_getc.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_getline.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_gets.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_open.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_putc.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_putline.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_puts.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_read.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_readdelim.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_readline.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_seek.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_set_local.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_set_view_index.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_set_view_local.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_ungetc.3.html
%{html_prefix}/ja/ref/man3/gfs_pio_write.3.html
%{html_prefix}/ja/ref/man3/gfs_readdir.3.html
%{html_prefix}/ja/ref/man3/gfs_realpath.3.html
%{html_prefix}/ja/ref/man3/gfs_rmdir.3.html
%{html_prefix}/ja/ref/man3/gfs_stat.3.html
%{html_prefix}/ja/ref/man3/gfs_stat_free.3.html
%{html_prefix}/ja/ref/man3/gfs_unlink.3.html
%{html_prefix}/ja/ref/man3/gfs_utimes.3.html
%{html_prefix}/ja/ref/man5/gfarm.conf.5.html
%{html_prefix}/ja/ref/man8/gfmd.8.html
%{html_prefix}/ja/ref/man8/gfsd.8.html
%{doc_prefix}/INSTALL.en
%{doc_prefix}/INSTALL.ja
%{doc_prefix}/LICENSE
%{doc_prefix}/RELNOTES
%{doc_prefix}/GUIDE.ja
%{doc_prefix}/Gfarm-FAQ.en
%{doc_prefix}/Gfarm-FAQ.ja
%{doc_prefix}/README.hook.en

%files libs
%{prefix}/lib/libgfarm.so.0
%{prefix}/lib/libgfarm.so.0.0.0
%{prefix}/lib/libgfs_hook.so.0
%{prefix}/lib/libgfs_hook.so.0.0.0
%{prefix}/lib/libgfs_hook_debug.so.0
%{prefix}/lib/libgfs_hook_debug.so.0.0.0
%{prefix}/lib/libgfs_hook_no_init.so.0
%{prefix}/lib/libgfs_hook_no_init.so.0.0.0
%{prefix}/lib/libgfs_hook_no_init_debug.so.0
%{prefix}/lib/libgfs_hook_no_init_debug.so.0.0.0
%if %{mpi}
%{prefix}/lib/libgfs_hook_mpi.so.0
%{prefix}/lib/libgfs_hook_mpi.so.0.0.0
%{prefix}/lib/libgfs_hook_mpi_debug.so.0
%{prefix}/lib/libgfs_hook_mpi_debug.so.0.0.0
%endif
%if %{have_ns}
%{prefix}/lib/libns.so.0
%{prefix}/lib/libns.so.0.0.0
%{prefix}/lib/libnsexec.so.0
%{prefix}/lib/libnsexec.so.0.0.0
%endif

%files frontend

%if %{have_ns}
%{prefix}/bin/gfarm
%endif

%files client
%{prefix}/bin/digest
%{prefix}/bin/gfdf
%{prefix}/bin/gfexport
%{prefix}/bin/gfhost
%{prefix}/bin/gfimport_fixed
%{prefix}/bin/gfimport_text
%{prefix}/bin/gfkey
%{prefix}/bin/gfls
%{prefix}/bin/gfmkdir
%{prefix}/bin/gfmpirun_p4
%{prefix}/bin/gfps
%{prefix}/bin/gfpwd
%{prefix}/bin/gfrcmd
%{prefix}/bin/gfreg
%{prefix}/bin/gfrep
%{prefix}/bin/gfrm
%{prefix}/bin/gfrmdir
%{prefix}/bin/gfrsh
%{prefix}/bin/gfrshl
%{prefix}/bin/gfrun
%{prefix}/bin/gfsched
%{prefix}/bin/gfsck
%{prefix}/bin/gfsetdir
%{prefix}/bin/gfssh
%{prefix}/bin/gfsshl
%{prefix}/bin/gfstat
%{prefix}/bin/gfwhere
%{prefix}/bin/gfwhoami
%{prefix}/bin/pcat
%{prefix}/bin/pcp
%{prefix}/bin/pdel
# %{prefix}/bin/pdiff
%{prefix}/bin/pdist
%{prefix}/bin/prun
%{profile_etc_prefix}/gfarm.sh
%{profile_etc_prefix}/gfarm.csh

%if %{have_ns}
%{prefix}/sbin/gfarmd
%{prefix}/bin/ns_get
%{prefix}/bin/ns_lstat
%{prefix}/bin/ns_mkdir
%{prefix}/bin/ns_put
%{prefix}/bin/ns_readdir
%{prefix}/bin/ns_readlink
%{prefix}/bin/ns_rename
%{prefix}/bin/ns_stat
%{prefix}/bin/ns_symlink
%{prefix}/bin/ns_unlink
%{prefix}/bin/ns_unlink_dir
%endif

%files gfptool
%{prefix}/bin/gfcombine
%{prefix}/bin/gfcombine_hook
%{prefix}/bin/gfcp
%{prefix}/bin/gfcp_hook

%{prefix}/bin/gfgrep
%if %{mpi}
%{prefix}/bin/gfwc
%endif

%{prefix}/libexec/gfrepbe_client
%{prefix}/libexec/gfrepbe_server
%{prefix}/sbin/gfregister

%files fsnode
%{prefix}/bin/gfexec
%{prefix}/bin/gfsplck
%{prefix}/bin/thput-gfpio
%{prefix}/sbin/gfsd
%{rc_prefix}/gfsd

%config(noreplace) %{etc_prefix}/gfarm.conf

%files server
%{prefix}/sbin/gfmd
%{rc_prefix}/gfmd
%{rc_prefix}/gfarm-slapd
%dir %{ldap_etc_prefix}
%{ldap_etc_prefix}/gfarm.schema

%files devel
%{prefix}/include/gfarm/gfarm.h
%{prefix}/include/gfarm/gfarm_config.h
%{prefix}/include/gfarm/gfarm_error.h
%{prefix}/include/gfarm/gfarm_metadb.h
%{prefix}/include/gfarm/gfarm_misc.h
%{prefix}/include/gfarm/gfarm_stringlist.h
%{prefix}/include/gfarm/gfs.h
%{prefix}/include/gfarm/gfs_glob.h
%{prefix}/include/gfarm/gfs_hook.h
%{prefix}/lib/gfs_hook.o
%{prefix}/lib/gfs_hook_debug.o
%{prefix}/lib/gfs_hook_no_init.o
%{prefix}/lib/gfs_hook_no_init_debug.o
%{prefix}/lib/hooks_init_mpi.c
%{prefix}/lib/libgfarm.a
%{prefix}/lib/libgfarm.la
%{prefix}/lib/libgfarm.so
%{prefix}/lib/libgfs_hook.a
%{prefix}/lib/libgfs_hook.la
%{prefix}/lib/libgfs_hook.so
%{prefix}/lib/libgfs_hook_debug.a
%{prefix}/lib/libgfs_hook_debug.la
%{prefix}/lib/libgfs_hook_debug.so
%{prefix}/lib/libgfs_hook_no_init.a
%{prefix}/lib/libgfs_hook_no_init.la
%{prefix}/lib/libgfs_hook_no_init.so
%{prefix}/lib/libgfs_hook_no_init_debug.a
%{prefix}/lib/libgfs_hook_no_init_debug.la
%{prefix}/lib/libgfs_hook_no_init_debug.so
%if %{mpi}
%{prefix}/lib/gfs_hook_mpi.o
%{prefix}/lib/gfs_hook_mpi_debug.o
%{prefix}/lib/libgfs_hook_mpi.a
%{prefix}/lib/libgfs_hook_mpi.la
%{prefix}/lib/libgfs_hook_mpi.so
%{prefix}/lib/libgfs_hook_mpi_debug.a
%{prefix}/lib/libgfs_hook_mpi_debug.la
%{prefix}/lib/libgfs_hook_mpi_debug.so
%endif


%if %{have_ns}
%{prefix}/include/gfarm/comm.h
%{prefix}/include/gfarm/debug.h
%{prefix}/include/gfarm/gflib.h
%{prefix}/include/gfarm/ns.h
%{prefix}/include/gfarm/nsclnt.h
%{prefix}/include/gfarm/nscom.h
%{prefix}/include/gfarm/nsexec.h
%{prefix}/include/gfarm/soc-lxdr.h
%{prefix}/include/gfarm/soc.h
%{prefix}/include/gfarm/type.h
%{prefix}/lib/libns.a
%{prefix}/lib/libns.la
%{prefix}/lib/libns.so
%{prefix}/lib/libnsexec.a
%{prefix}/lib/libnsexec.la
%{prefix}/lib/libnsexec.so
%endif

%files gfront
%{prefix}/bin/gfront
%{prefix}/share/java/gfront.jar

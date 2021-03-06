#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT([gfarm2fs], [1.2.1])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AM_MAINTAINER_MODE
AC_CONFIG_SRCDIR([gfarm2fs.c])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile])
AC_SYS_LARGEFILE

# Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL

### --with-gfarm=GFARM_ROOT
AC_MSG_CHECKING([for Gfarm file system])
AC_ARG_WITH([gfarm],
  AC_HELP_STRING([--with-gfarm=GFARM_ROOT], [Gfarm root directory [[/usr]]]))
case x$with_gfarm in
  x|xyes|xno) with_gfarm=/usr ;;
esac
if test x"$with_gfarm" != x/usr; then
  CPPFLAGS="$CPPFLAGS -I$with_gfarm/include"
  LDFLAGS="$LDFLAGS -R$with_gfarm/lib -L$with_gfarm/lib"
fi
AC_MSG_RESULT([$with_gfarm])

### --with-private-srcdir
### This is provided to replace some functionalities with your private code.
AC_MSG_CHECKING([for private source directory])
AC_ARG_WITH([private_srcdir],
  AC_HELP_STRING([--with-private-srcdir=DIR], [private source directory]))
case x$with_private_srcdir in
x|xno)
  top_private_dir=""
  private_srcs=""
  AC_MSG_RESULT([no]) ;;
*)
  if test -d "${with_private_srcdir}"; then
    top_private_dir="`(cd ${with_private_srcdir}; /bin/pwd)`"
    private_srcs="replicate_private.c private_gfarm2fs_msg_enums.h"
    AC_MSG_RESULT([${top_private_dir}])
    AC_DEFINE(HAVE_PRIVATE_SRCS, [1], [Define to 1 if you have private sources])
  else
    AC_MSG_ERROR([${with_private_srcdir} is not a directory.])
  fi
esac
AC_SUBST(top_private_dir)
AC_SUBST(private_srcs)

### --enable-xattr
### by default this is disabled since it slows down the response of 'ls -l'
AC_ARG_ENABLE(xattr,
  AS_HELP_STRING([--enable-xattr], [support extended attribute [[default=no]]]),
  [if test "x$enable_xattr" != xno; then
    AC_DEFINE(ENABLE_XATTR, [1], [support extended attribute])
   fi], [])

# Checks for header files.
AC_CHECK_HEADERS([gfarm/gfarm.h],, [AC_MSG_ERROR([gfarm/gfarm.h not found])])
# fuse.h requres _FILE_OFFSET_BITS is defined in any case, but
# AC_SYS_LARGEFILE does not define it on a 64bit platform like x86_64
# since it is not necessary.  To avoid this problem we just skip the check.
#AC_CHECK_HEADERS([fuse.h],, [AC_MSG_ERROR([fuse.h not found])])
AC_CHECK_HEADERS(sys/xattr.h)

# Checks for libraries.
AC_CHECK_LIB([gfarm], [gfarm_initialize],, [AC_MSG_ERROR([libgfarm not found])])
AC_CHECK_LIB([fuse], [fuse_main],, [AC_MSG_ERROR([libfuse not found])])


# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.
AC_CHECK_FUNCS(gfs_replicate_file_to gfarm_schedule_cache_dump)

AC_OUTPUT

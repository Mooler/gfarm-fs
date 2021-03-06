#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT([gfarm2fs], [0.9])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AM_MAINTAINER_MODE
AC_CONFIG_SRCDIR([gfarm2fs.c])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile])
AC_SYS_LARGEFILE

# Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL

### --with-gfarm=gfarm_ROOT
AC_MSG_CHECKING([Gfarm file system package])
AC_ARG_WITH([gfarm],
  AC_HELP_STRING([--with-gfarm=PATH], [path to gfarm file system [[/usr]]]),
   [gfarm_prefix=$withval],
   [gfarm_prefix=no])

if test x"$gfarm_prefix" != xno; then
  AC_MSG_RESULT([$gfarm_prefix])
  CPPFLAGS="$CPPFLAGS -I$gfarm_prefix/include"
  LDFLAGS="$LDFLAGS -R$gfarm_prefix/lib -L$gfarm_prefix/lib"
else
  AC_MSG_RESULT([/usr])
fi

# Checks for header files.
AC_CHECK_HEADERS([gfarm/gfarm.h],, [AC_MSG_ERROR([gfarm/gfarm.h not found])])
# fuse.h requres _FILE_OFFSET_BITS is defined in any case, but
# AC_SYS_LARGEFILE does not define it on a 64bit platform like x86_64
# since it is not necessary.  To avoid this problem we just skip the check.
#AC_CHECK_HEADERS([fuse.h],, [AC_MSG_ERROR([fuse.h not found])])

# Checks for libraries.
AC_CHECK_LIB([gfarm], [gfarm_initialize],, [AC_MSG_ERROR([libgfarm not found])])
AC_CHECK_LIB([fuse], [fuse_main],, [AC_MSG_ERROR([libfuse not found])])


# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_OUTPUT

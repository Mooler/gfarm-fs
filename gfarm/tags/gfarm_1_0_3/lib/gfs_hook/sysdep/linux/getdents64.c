/* Copyright (C) 1993, 1995-2002 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#include "getdents.h"

#define OFF_T off64_t

#define SYSCALL_GETDENTS(filedes, buf, nbyte) \
	gfs_hook_syscall_getdents64(filedes, buf, nbyte)

#define DIRENT_TYPE struct dirent64

#define __LINUX_KERNEL_VERSION 132097

#include "getdents_common.c"

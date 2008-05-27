/*	from NetBSD: getcwd.c,v 1.32 2003/08/07 16:42:49 agc Exp	*/

/*
 * Copyright (c) 1989, 1991, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jan-Simon Pendry.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <gfarm/gfarm_misc.h>
#include "hooks_subr.h"

#ifdef SYS_getcwd

char *
gfs_hook_syscall_getcwd(char *buf, size_t size)
{
	int errno_save;
	size_t sz;
	int allocated = 0, realloc_needed = 0;
	char *tmp;

	errno_save = errno;
	if (buf == NULL) {
		if (size > 0)
			GFARM_MALLOC_ARRAY(buf, size);
		else {
			GFARM_MALLOC_ARRAY(buf, PATH_MAX);
			size = PATH_MAX;
			realloc_needed = 1;
		}
		if (buf == NULL)
			return (NULL);
		allocated = 1;
	}

	errno = 0;
	sz = syscall(SYS_getcwd, buf, size);
	if (errno == 0) {
		if (realloc_needed) {
			GFARM_REALLOC_ARRAY(tmp, buf, sz);
			if (tmp != NULL)
				buf = tmp;
		}
		errno = errno_save;
		return (buf);
	}
	if (allocated)
		free(buf);
	return (NULL);
}

#else /* do not have SYS_getcwd */

#define	ISDOT(dp) \
	(dp->d_name[0] == '.' && (dp->d_name[1] == '\0' || \
	    (dp->d_name[1] == '.' && dp->d_name[2] == '\0')))


#if defined(__SVR4) || defined(__svr4__)
#define d_fileno d_ino
#endif

char *
gfs_hook_syscall_getcwd(char *pt, size_t size)
{
	struct dirent *dp;
	DIR *dir;
	dev_t dev;
	ino_t ino;
	int first;
	char *bpt, *bup;
	struct stat s;
	dev_t root_dev;
	ino_t root_ino;
	size_t ptsize, upsize;
	int save_errno;
	char *ept, *eup, *up;
	size_t dlen;

	/*
	 * If no buffer specified by the user, allocate one as necessary.
	 * If a buffer is specified, the size has to be non-zero.  The path
	 * is built from the end of the buffer backwards.
	 */
	if (pt) {
		ptsize = 0;
		if (!size) {
			errno = EINVAL;
			return (NULL);
		}
		ept = pt + size;
	} else {
		ptsize = 1024 - 4;
		GFARM_MALLOC_ARRAY(pt, ptsize);
		if (pt == NULL)
			return (NULL);
		ept = pt + ptsize;
	}
	bpt = ept - 1;
	*bpt = '\0';

	/*
	 * Allocate bytes (1024 - malloc space) for the string of "../"'s.
	 * Should always be enough (it's 340 levels).  If it's not, allocate
	 * as necessary.  Special case the first stat, it's ".", not "..".
	 */
	upsize = 1024 - 4;
	GFARM_MALLOC_ARRAY(up, upsize);
	if (up == NULL)
		goto err;
	eup = up + MAXPATHLEN;
	bup = up;
	up[0] = '.';
	up[1] = '\0';

	/* Save root values, so know when to stop. */
	if (stat("/", &s))
		goto err;
	root_dev = s.st_dev;
	root_ino = s.st_ino;

	errno = 0;			/* XXX readdir has no error return. */

	for (first = 1;; first = 0) {
		/* Stat the current level. */
		if (lstat(up, &s))
			goto err;

		/* Save current node values. */
		ino = s.st_ino;
		dev = s.st_dev;

		/* Check for reaching root. */
		if (root_dev == dev && root_ino == ino) {
			*--bpt = '/';
			/*
			 * It's unclear that it's a requirement to copy the
			 * path to the beginning of the buffer, but it's always
			 * been that way and stuff would probably break.
			 */
			memmove(pt, bpt,  (size_t)(ept - bpt));
			free(up);
			return (pt);
		}

		/*
		 * Build pointer to the parent directory, allocating memory
		 * as necessary.  Max length is 3 for "../", the largest
		 * possible component name, plus a trailing NULL.
		 */
		if (bup + 3  + MAXNAMLEN + 1 >= eup) {
			if (GFARM_REALLOC_ARRAY(up, up, upsize *= 2) == NULL)
				goto err;
			bup = up;
			eup = up + upsize;
		}
		*bup++ = '.';
		*bup++ = '.';
		*bup = '\0';

		/* Open and stat parent directory. */
		if (!(dir = opendir(up)) || stat(up, &s))
			goto err;

		/* Add trailing slash for next directory. */
		*bup++ = '/';

		/*
		 * If it's a mount point, have to stat each element because
		 * the inode number in the directory is for the entry in the
		 * parent directory, not the inode number of the mounted file.
		 */
		save_errno = 0;
		if (s.st_dev == dev) {
			for (;;) {
				if (!(dp = readdir(dir)))
					goto notfound;
				if (dp->d_fileno == ino) {
#if defined(__SVR4) || defined(__svr4__) || defined(__linux__)
					dlen = strlen(dp->d_name);
#else
					dlen = dp->d_namlen;
#endif
					break;
				}
			}
		} else
			for (;;) {
				if (!(dp = readdir(dir)))
					goto notfound;
				if (ISDOT(dp))
					continue;
#if defined(__SVR4) || defined(__svr4__) || defined(__linux__)
				dlen = strlen(dp->d_name);
#else
				dlen = dp->d_namlen;
#endif
				memmove(bup, dp->d_name, dlen + 1);

				/* Save the first error for later. */
				if (lstat(up, &s)) {
					if (!save_errno)
						save_errno = errno;
					errno = 0;
					continue;
				}
				if (s.st_dev == dev && s.st_ino == ino)
					break;
			}

		/*
		 * Check for length of the current name, preceding slash,
		 * leading slash.
		 */
		if (bpt - pt <= dlen + (first ? 1 : 2)) {
			size_t len, off;

			if (!ptsize) {
				errno = ERANGE;
				goto err;
			}
			off = bpt - pt;
			len = ept - bpt;
			if (GFARM_REALLOC_ARRAY(pt, pt, ptsize *= 2) == NULL)
				goto err;
			bpt = pt + off;
			ept = pt + ptsize;
			memmove(ept - len, bpt, len);
			bpt = ept - len;
		}
		if (!first)
			*--bpt = '/';
		bpt -= dlen;
		memmove(bpt, dp->d_name, dlen);
		(void)closedir(dir);

		/* Truncate any file name. */
		*bup = '\0';
	}

notfound:
	/*
	 * If readdir set errno, use it, not any saved error; otherwise,
	 * didn't find the current directory in its parent directory, set
	 * errno to ENOENT.
	 */
	if (!errno)
		errno = save_errno ? save_errno : ENOENT;
	/* FALLTHROUGH */
err:
	save_errno = errno;
	if (ptsize)
		free(pt);
	free(up);
	errno = save_errno;
	return (NULL);
}

#endif /* do not have SYS_getcwd */

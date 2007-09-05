/*
 * GfarmFS-FUSE for Gfarm version 2
 *
 * $Id$
 */


#include "config.h"

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#define FUSE_USE_VERSION 25
#include <fuse.h>

#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#include <gfarm/gfarm.h>

/* XXX FIXME */
#define GFS_DEV		((dev_t)-1)
#define GFS_BLKSIZE	8192
#define STAT_BLKSIZ	512	/* for st_blocks */

static int
get_uid(char *user)
{
	/* XXX FIXME */
	return (0);
}

static int
get_gid(char *group)
{
	/* XXX FIXME */
	return (0);
}

static int
get_nlink(struct gfs_stat *st)
{
	/* XXX FIXME */
	return (GFARM_S_ISDIR(st->st_mode) ? 32000 : st->st_nlink);
}

static int gfarm2fs_getattr(const char *path, struct stat *stbuf)
{
	struct gfs_stat st;
	gfarm_error_t e;

	e = gfs_stat(path, &st);
	if (e != GFARM_ERR_NO_ERROR)
		return (-gfarm_error_to_errno(e));

	memset(stbuf, 0, sizeof(*stbuf));
	stbuf->st_dev = GFS_DEV;
	stbuf->st_ino = st.st_ino;
	stbuf->st_mode = st.st_mode;
	stbuf->st_nlink = get_nlink(&st);
	stbuf->st_uid = get_uid(st.st_user);
	stbuf->st_gid = get_gid(st.st_group);
	stbuf->st_size = st.st_size;
	stbuf->st_blksize = GFS_BLKSIZE;
	stbuf->st_blocks = (st.st_size + STAT_BLKSIZ - 1) / STAT_BLKSIZ;
	stbuf->st_atime = st.st_atimespec.tv_sec;
	stbuf->st_mtime = st.st_mtimespec.tv_sec;
	stbuf->st_ctime = st.st_ctimespec.tv_sec;
	gfs_stat_free(&st);

	return (0);
}

static int gfarm2fs_fgetattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi)
{
	return (gfarm2fs_getattr(path, stbuf));
}

static int gfarm2fs_access(const char *path, int mask)
{
	/* XXX FIXME */
	return (-ENOSYS);
#if 0
	gfarm_error_t e;

	e = gfs_access(path, mask);
	return (-gfarm_error_to_errno(e));
#endif
}

static int gfarm2fs_readlink(const char *path, char *buf, size_t size)
{
	/* XXX FIXME */
	return (-ENOSYS);
#if 0
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
#endif
}

static int gfarm2fs_opendir(const char *path, struct fuse_file_info *fi)
{
	gfarm_error_t e;
	GFS_Dir dp;

	e = gfs_opendir(path, &dp);
	if (e != GFARM_ERR_NO_ERROR)
		return (-gfarm_error_to_errno(e));

	fi->fh = (unsigned long) dp;
	return (0);
}

static inline GFS_Dir get_dirp(struct fuse_file_info *fi)
{
	return (GFS_Dir) (uintptr_t) fi->fh;
}

static int
gfarm2fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info *fi)
{
	GFS_Dir dp = get_dirp(fi);
	struct gfs_dirent *de;
	struct stat st;
	/* gfarm_off_t off = 0; */
	gfarm_error_t e;

	(void) path;
	/* XXX gfs_seekdir(dp, offset); */
	while ((e = gfs_readdir(dp, &de)) == GFARM_ERR_NO_ERROR &&
		de != NULL) {
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_fileno;
		st.st_mode = de->d_type << 12;
		/* XXX (void)gfs_telldir(dp, &off); */
		if (filler(buf, de->d_name, &st, 0))
			break;
	}

	return (0);
}

static int gfarm2fs_releasedir(const char *path, struct fuse_file_info *fi)
{
	GFS_Dir dp = get_dirp(fi);
	(void) path;
	gfs_closedir(dp);
	return (0);
}

static int gfarm2fs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	GFS_File gf;
	gfarm_error_t e;

	if (!S_ISREG(mode))
		return (-ENOSYS);

	e = gfs_pio_create(path, GFARM_FILE_WRONLY, mode & GFARM_S_ALLPERM,
		&gf);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfs_pio_close(gf);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_mkdir(const char *path, mode_t mode)
{
	gfarm_error_t e;

	e = gfs_mkdir(path, mode & GFARM_S_ALLPERM);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_unlink(const char *path)
{
	gfarm_error_t e;

	e = gfs_unlink(path);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_rmdir(const char *path)
{
	gfarm_error_t e;

	e = gfs_rmdir(path);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_symlink(const char *from, const char *to)
{
	/* XXX FIXME */
	return (-ENOSYS);
#if 0
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
#endif
}

static int gfarm2fs_rename(const char *from, const char *to)
{
	gfarm_error_t e;

	e = gfs_rename(from, to);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_link(const char *from, const char *to)
{
	/* XXX FIXME */
	return (-ENOSYS);
#if 0
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
#endif
}

static int gfarm2fs_chmod(const char *path, mode_t mode)
{
	gfarm_error_t e;

	e = gfs_chmod(path, mode & GFARM_S_ALLPERM);
	return (-gfarm_error_to_errno(e));
}

static char *
get_user(uid_t uid)
{
	/* XXX FIXME */
	return "user";
}

static char *
get_group(uid_t gid)
{
	/* XXX FIXME */
	return "group";
}

static int gfarm2fs_chown(const char *path, uid_t uid, gid_t gid)
{
	gfarm_error_t e;
	char *user, *group;

	user = get_user(uid);
	group = get_group(gid);

	e = gfs_chown(path, user, group);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_truncate(const char *path, off_t size)
{
	/* XXX FIXME */
	return (-ENOSYS);
#if 0
	int res;

	res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
#endif
}

static inline GFS_File get_filep(struct fuse_file_info *fi)
{
	return (GFS_File) (uintptr_t) fi->fh;
}

static int gfarm2fs_ftruncate(const char *path, off_t size,
                         struct fuse_file_info *fi)
{
	gfarm_error_t e;

	(void) path;

	e = gfs_pio_truncate(get_filep(fi), size);
	return (-gfarm_error_to_errno(e));
}

static int gfarm2fs_utime(const char *path, struct utimbuf *buf)
{
	struct gfarm_timespec gt[2];
	gfarm_error_t e;

	if (buf != NULL) {
		gt[0].tv_sec = buf->actime;
		gt[0].tv_nsec= 0;
		gt[1].tv_sec = buf->modtime;
		gt[1].tv_nsec= 0;
	}
	e = gfs_utimes(path, gt);
	return (-gfarm_error_to_errno(e));
}

static int
gfs_hook_open_flags_gfarmize(int open_flags)
{
	int gfs_flags;

	switch (open_flags & O_ACCMODE) {
	case O_RDONLY:	gfs_flags = GFARM_FILE_RDONLY; break;
	case O_WRONLY:	gfs_flags = GFARM_FILE_WRONLY; break;
	case O_RDWR:	gfs_flags = GFARM_FILE_RDWR; break;
	default: return (-1);
	}

#if 0 /* this is unnecessary */
	if ((open_flags & O_CREAT) != 0)
		gfs_flags |= GFARM_FILE_CREATE;
#endif
	if ((open_flags & O_TRUNC) != 0)
		gfs_flags |= GFARM_FILE_TRUNC;
#if 0 /* not yet on Gfarm v2 */
	if ((open_flags & O_APPEND) != 0)
		gfs_flags |= GFARM_FILE_APPEND;
	if ((open_flags & O_EXCL) != 0)
		gfs_flags |= GFARM_FILE_EXCLUSIVE;
#endif
#if 0 /* not yet on Gfarm v2 */
	/* open(2) and creat(2) should be unbuffered */
	gfs_flags |= GFARM_FILE_UNBUFFERED;
#endif
	return (gfs_flags);
}

static int gfarm2fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	gfarm_error_t e;
	GFS_File gf;
	int flags;

	flags = gfs_hook_open_flags_gfarmize(fi->flags);
	e = gfs_pio_create(path, flags, mode & GFARM_S_ALLPERM, &gf);
	if (e != GFARM_ERR_NO_ERROR)
		return (-gfarm_error_to_errno(e));

	fi->fh = (unsigned long) gf;
	return (0);
}

static int gfarm2fs_open(const char *path, struct fuse_file_info *fi)
{
	GFS_File gf;
	int flags;
	gfarm_error_t e;

	flags = gfs_hook_open_flags_gfarmize(fi->flags);
	e = gfs_pio_open(path, flags, &gf);
	if (e != GFARM_ERR_NO_ERROR)
		return (-gfarm_error_to_errno(e));

	fi->fh = (unsigned long) gf;
	return (0);
}

static int gfarm2fs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
	gfarm_error_t e;
	gfarm_off_t off;
	int rv;

	(void) path;
	e = gfs_pio_seek(get_filep(fi), offset, GFARM_SEEK_SET, &off);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfs_pio_read(get_filep(fi), buf, size, &rv);
	if (e != GFARM_ERR_NO_ERROR)
		rv = -gfarm_error_to_errno(e);

	return (rv);
}

static int gfarm2fs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
	gfarm_error_t e;
	gfarm_off_t off;
	int rv;

	(void) path;
	e = gfs_pio_seek(get_filep(fi), offset, GFARM_SEEK_SET, &off);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfs_pio_write(get_filep(fi), buf, size, &rv);
	if (e != GFARM_ERR_NO_ERROR)
		rv = -gfarm_error_to_errno(e);

	return (rv);
}

static int gfarm2fs_statfs(const char *path, struct statvfs *stbuf)
{
	/* XXX FIXME */
	return (-ENOSYS);
#if 0
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
#endif
}

static int gfarm2fs_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	gfs_pio_close(get_filep(fi));

	return (0);
}

static int gfarm2fs_fsync(const char *path, int isdatasync,
                     struct fuse_file_info *fi)
{
	gfarm_error_t e;
	(void) path;

	if (isdatasync)
		e = gfs_pio_datasync(get_filep(fi));
	else
		e = gfs_pio_sync(get_filep(fi));
	return (-gfarm_error_to_errno(e));
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int gfarm2fs_setxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags)
{
    int res = lsetxattr(path, name, value, size, flags);
    if (res == -1)
        return -errno;
    return 0;
}

static int gfarm2fs_getxattr(const char *path, const char *name, char *value,
                    size_t size)
{
    int res = lgetxattr(path, name, value, size);
    if (res == -1)
        return -errno;
    return res;
}

static int gfarm2fs_listxattr(const char *path, char *list, size_t size)
{
    int res = llistxattr(path, list, size);
    if (res == -1)
        return -errno;
    return res;
}

static int gfarm2fs_removexattr(const char *path, const char *name)
{
    int res = lremovexattr(path, name);
    if (res == -1)
        return -errno;
    return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations gfarm2fs_oper = {
    .getattr	= gfarm2fs_getattr,
    .fgetattr	= gfarm2fs_fgetattr,
    .access	= gfarm2fs_access,
    .readlink	= gfarm2fs_readlink,
    .opendir	= gfarm2fs_opendir,
    .readdir	= gfarm2fs_readdir,
    .releasedir	= gfarm2fs_releasedir,
    .mknod	= gfarm2fs_mknod,
    .mkdir	= gfarm2fs_mkdir,
    .symlink	= gfarm2fs_symlink,
    .unlink	= gfarm2fs_unlink,
    .rmdir	= gfarm2fs_rmdir,
    .rename	= gfarm2fs_rename,
    .link	= gfarm2fs_link,
    .chmod	= gfarm2fs_chmod,
    .chown	= gfarm2fs_chown,
    .truncate	= gfarm2fs_truncate,
    .ftruncate	= gfarm2fs_ftruncate,
    .utime	= gfarm2fs_utime,
    .create	= gfarm2fs_create,
    .open	= gfarm2fs_open,
    .read	= gfarm2fs_read,
    .write	= gfarm2fs_write,
    .statfs	= gfarm2fs_statfs,
    .release	= gfarm2fs_release,
    .fsync	= gfarm2fs_fsync,
#ifdef HAVE_SETXATTR
    .setxattr	= gfarm2fs_setxattr,
    .getxattr	= gfarm2fs_getxattr,
    .listxattr	= gfarm2fs_listxattr,
    .removexattr= gfarm2fs_removexattr,
#endif
};

int main(int argc, char *argv[])
{
	gfarm_error_t e;
	char **argv_tmp;

	umask(0);
	e = gfarm_initialize(&argc, &argv);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s: %s\n", *argv, gfarm_error_string(e));
		exit(1);
	}
	++argc;
	printf("argc = %d, %d\n", argc, sizeof(*argv) * (argc + 1));
	argv_tmp = realloc(argv, sizeof(*argv) * (argc + 1));
	if (argv_tmp == NULL) {
		fprintf(stderr, "%s: no memory\n", *argv);
		exit(1);
	}
	argv = argv_tmp;
	argv[argc - 1] = "-s";
	argv[argc] = "";
	return (fuse_main(argc, argv, &gfarm2fs_oper));
}

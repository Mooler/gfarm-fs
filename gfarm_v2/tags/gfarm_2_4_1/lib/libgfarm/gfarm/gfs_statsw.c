#include <stddef.h>
#include <gfarm/gfarm.h>

#include "gfs_dircache.h"

struct gfs_statsw {
	gfarm_error_t (*opendir)(const char *, GFS_Dir *);
	gfarm_error_t (*stat)(const char *, struct gfs_stat *);
	gfarm_error_t (*getxattr)(const char *path, const char *name,
		void *value, size_t *size);
};

/*
 * for gfs_statsw_uncached
 */

static struct gfs_statsw gfs_statsw_uncached = {
	gfs_opendir,
	gfs_stat,
	gfs_getxattr,
};

/*
 * for gfs_statsw_cached
 */

static struct gfs_statsw gfs_statsw_cached = {
	gfs_opendir_caching_internal,
	gfs_stat_cached_internal,
	gfs_getxattr_cached_internal,
};

/*
 * for gfs_statsw
 */
static struct gfs_statsw *gfs_statsw = &gfs_statsw_cached;

gfarm_error_t
gfs_opendir_caching(const char *path, GFS_Dir *dirp)
{
	return ((*gfs_statsw->opendir)(path, dirp));
}

gfarm_error_t
gfs_stat_cached(const char *path, struct gfs_stat *st)
{
	return ((*gfs_statsw->stat)(path, st));
}

gfarm_error_t
gfs_lstat_cached(const char *path, struct gfs_stat *st)
{
	return ((*gfs_statsw->stat)(path, st)); /* XXX FIXME */
}

gfarm_error_t
gfs_getxattr_cached(const char *path, const char *name,
	void *value, size_t *sizep)
{
	return ((*gfs_statsw->getxattr)(path, name, value, sizep));
}

void
gfs_stat_cache_enable(int enable)
{
	gfs_statsw = enable ? &gfs_statsw_cached : &gfs_statsw_uncached;
}

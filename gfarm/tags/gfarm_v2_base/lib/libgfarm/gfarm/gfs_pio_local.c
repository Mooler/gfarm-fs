/*
 * pio operations for local section
 *
 * $Id$
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <libgen.h>
#include <openssl/evp.h>
#include <gfarm/gfarm.h>
#include "gfs_proto.h" /* GFARM_FILE_CREATE, gfs_digest_calculate_local() */
#include "gfs_pio.h"
#include "gfs_misc.h"

int gfarm_node = -1;
int gfarm_nnode = -1;

char *
gfs_pio_set_local(int node, int nnode)
{
	if (node < 0 || node >= nnode || nnode < 0)
		return (GFARM_ERR_INVALID_ARGUMENT);

	gfarm_node = node;
	gfarm_nnode = nnode;
	return (NULL);
}

char *
gfs_pio_set_local_check(void)
{
	if (gfarm_node < 0 || gfarm_nnode <= 0)
		return ("gfs_pio_set_local() is not correctly called");
	return (NULL);
}

char *
gfs_pio_get_node_rank(int *node)
{
	char *e = gfs_pio_set_local_check();

	if (e != NULL)
		return (e);
	*node = gfarm_node;
	return (NULL);
}

char *
gfs_pio_get_node_size(int *nnode)
{
	char *e = gfs_pio_set_local_check();

	if (e != NULL)
		return (e);
	*nnode = gfarm_nnode;
	return (NULL);
}

char *
gfs_pio_set_view_local(GFS_File gf, int flags)
{
	char *e, *arch;

	if (GFS_FILE_IS_PROGRAM(gf)) {
		e = gfarm_host_get_self_architecture(&arch);
		if (e != NULL)
			return (gf->error = e);
		return (gfs_pio_set_view_section(gf, arch, NULL, flags));
	}
	e = gfs_pio_set_local_check();
	if (e != NULL)
		return (gf->error = e);
	return (gfs_pio_set_view_index(gf, gfarm_nnode, gfarm_node,
				       NULL, flags));
}

/***********************************************************************/

static char *
gfs_pio_local_storage_close(GFS_File gf)
{
	struct gfs_file_section_context *vc = gf->view_context;

	if (close(vc->fd) == -1)
		return (gfarm_errno_to_error(errno));
	return (NULL);
}

static char *
gfs_pio_local_storage_write(GFS_File gf, const char *buffer, size_t size,
			    size_t *lengthp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	int rv = write(vc->fd, buffer, size);

	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	*lengthp = rv;
	return (NULL);
}

static char *
gfs_pio_local_storage_read(GFS_File gf, char *buffer, size_t size,
			   size_t *lengthp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	int rv = read(vc->fd, buffer, size);

	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	*lengthp = rv;
	return (NULL);
}

static char *
gfs_pio_local_storage_seek(GFS_File gf, file_offset_t offset, int whence,
			   file_offset_t *resultp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	off_t rv = lseek(vc->fd, (off_t)offset, whence);

	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	if (resultp != NULL)
		*resultp = rv;
	return (NULL);
}

static char *
gfs_pio_local_storage_ftruncate(GFS_File gf, file_offset_t length)
{
	struct gfs_file_section_context *vc = gf->view_context;
	int rv;

	rv = ftruncate(vc->fd, length);
	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	return (NULL);
}

static char *
gfs_pio_local_storage_fsync(GFS_File gf, int operation)
{
	struct gfs_file_section_context *vc = gf->view_context;
	int rv;

	switch (operation) {
	case GFS_PROTO_FSYNC_WITHOUT_METADATA:
#ifdef HAVE_FDATASYNC
		rv = fdatasync(vc->fd);
		break;
#else
		/*FALLTHROUGH*/
#endif
	case GFS_PROTO_FSYNC_WITH_METADATA:
		rv = fsync(vc->fd);
		break;
	default:
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	return (NULL);
}

static char *
gfs_pio_local_storage_calculate_digest(GFS_File gf, char *digest_type,
				       size_t digest_size,
				       size_t *digest_lengthp,
				       unsigned char *digest,
				       file_offset_t *filesizep)
{
	struct gfs_file_section_context *vc = gf->view_context;
	const EVP_MD *md_type;
	int rv;
	static int openssl_initialized = 0;

	if (!openssl_initialized) {
		OpenSSL_add_all_digests(); /* for EVP_get_digestbyname() */
		openssl_initialized = 1;
	}
	if ((md_type = EVP_get_digestbyname(digest_type)) == NULL)
		return (GFARM_ERR_INVALID_ARGUMENT);

	/* note that this effectively breaks file offset. */
	rv = gfs_digest_calculate_local(
	    vc->fd, gf->buffer, GFS_FILE_BUFSIZE,
	    md_type, &vc->md_ctx, digest_lengthp, digest,
	    filesizep);
	if (rv != 0)
		return (gfarm_errno_to_error(rv));
	return (NULL);
}

static int
gfs_pio_local_storage_fd(GFS_File gf)
{
	struct gfs_file_section_context *vc = gf->view_context;

	return (vc->fd);
}

struct gfs_storage_ops gfs_pio_local_storage_ops = {
	gfs_pio_local_storage_close,
	gfs_pio_local_storage_write,
	gfs_pio_local_storage_read,
	gfs_pio_local_storage_seek,
	gfs_pio_local_storage_ftruncate,
	gfs_pio_local_storage_fsync,
	gfs_pio_local_storage_calculate_digest,
	gfs_pio_local_storage_fd,
};

char *
gfs_pio_open_local_section(GFS_File gf, int flags)
{
	struct gfs_file_section_context *vc = gf->view_context;
	char *e, *local_path;
	/*
	 * We won't use GFARM_FILE_EXCLUSIVE flag for the actual storage
	 * level access (at least for now) to avoid the effect of
	 * remaining junk files.
	 * It's already handled anyway at the metadata level.
	 *
	 * NOTE: Same thing must be done in gfs_pio_remote.c.
	 */
	int oflags = (gf->open_flags & ~GFARM_FILE_EXCLUSIVE) |
	    (flags & GFARM_FILE_CREATE);
	int fd, local_oflags = gfs_open_flags_localize(oflags);
	int saved_errno;
	mode_t saved_umask;

	if (local_oflags == -1)
		return (GFARM_ERR_INVALID_ARGUMENT);

	e = gfarm_path_localize_file_section(gf->pi.pathname, vc->section,
					     &local_path);
	if (e != NULL)
		return (e);

	saved_umask = umask(0);
	fd = open(local_path, local_oflags,
		  gf->pi.status.st_mode & GFARM_S_ALLPERM);
	saved_errno = errno;
	umask(saved_umask);
	/* FT - the parent directory may be missing */
	if (fd == -1 && (oflags & GFARM_FILE_CREATE) != 0
	    && saved_errno == ENOENT) {
		/* the parent directory can be created by some other process */
		(void)gfs_pio_local_mkdir_parent_canonical_path(
			gf->pi.pathname);
		umask(0);
		fd = open(local_path, local_oflags,
			  gf->pi.status.st_mode & GFARM_S_ALLPERM);
		saved_errno = errno;
		umask(saved_umask);
	}
	free(local_path);
	/* FT - physical file should be missing */
	if (fd == -1 && (oflags & GFARM_FILE_CREATE) == 0
	    && saved_errno == ENOENT) {
		/* Delete the section copy info */
		char *localhost;
		if (gfarm_host_get_canonical_self_name(&localhost) == NULL) {
			/* section copy may be removed by some other process */
			(void)gfarm_file_section_copy_info_remove(
				gf->pi.pathname, vc->section, localhost);
			return (GFARM_ERR_INCONSISTENT_RECOVERABLE);
		}
	}
	if (fd == -1)
		return (gfarm_errno_to_error(saved_errno));

	vc->ops = &gfs_pio_local_storage_ops;
	vc->storage_context = NULL; /* not needed */
	vc->fd = fd;
	return (NULL);
}

static char *
gfs_pio_local_mkdir_p(char *canonic_dir)
{
	struct gfs_stat stata;
	struct stat statb;
	gfarm_mode_t mode;
	char *e, *local_path, *user;

	/* dirname(3) may return '.'.  This means the spool root directory. */
	if (strcmp(canonic_dir, "/") == 0 || strcmp(canonic_dir, ".") == 0)
		return (NULL); /* should exist */

	user = gfarm_get_global_username();
	if (user == NULL)
		return ("gfs_pio_local_mkdir_p(): programming error, "
			"gfarm library isn't properly initialized");

	e = gfs_stat_canonical_path(canonic_dir, &stata);
	if (e != NULL)
		return (e);
	mode = stata.st_mode;
	/*
	 * XXX - if the owner of a directory is not the same, create a
	 * directory with permission 0777 - This should be fixed in
	 * the next major release.
	 */
	if (strcmp(stata.st_user, user) != 0)
		mode |= 0777;
	gfs_stat_free(&stata);
	if (!GFARM_S_ISDIR(mode))
		return (GFARM_ERR_NOT_A_DIRECTORY);

	e = gfarm_path_localize(canonic_dir, &local_path);
	if (e != NULL)
		return (e);
	if (stat(local_path, &statb)) {
		char *par_dir, *saved_par_dir;
		mode_t saved_umask;
		int r;

		par_dir = saved_par_dir = strdup(canonic_dir);
		if (par_dir == NULL) {
			free(local_path);
			return (GFARM_ERR_NO_MEMORY);
		}
		par_dir = dirname(par_dir);
		e = gfs_pio_local_mkdir_p(par_dir);
		free(saved_par_dir);
		if (e != NULL) {
			free(local_path);
			return (e);
		}
		saved_umask = umask(0);
		r = mkdir(local_path, mode);
		umask(saved_umask);
		if (r == -1) {
			free(local_path);
			return (gfarm_errno_to_error(errno));
		}
	}
	free(local_path);
	return (NULL);
}

char *
gfs_pio_local_mkdir_parent_canonical_path(char *canonic_dir)
{
	char *par_dir, *saved_par_dir, *local_path, *e;
	struct stat statb;

	par_dir = saved_par_dir = strdup(canonic_dir);
	if (par_dir == NULL)
		return (GFARM_ERR_NO_MEMORY);

	par_dir = dirname(par_dir);
	e = gfarm_path_localize(par_dir, &local_path);
	if (e != NULL)
		goto finish_free_par_dir;

	if (stat(local_path, &statb))
		e = gfs_pio_local_mkdir_p(par_dir);
	else
		e = GFARM_ERR_ALREADY_EXISTS;

	free(local_path);
 finish_free_par_dir:
	free(saved_par_dir);

	return (e);
}

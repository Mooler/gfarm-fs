/*
 * pio operations for file fragments or programs
 *
 * $Id$
 */

#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <gfarm/gfarm.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include "gfs_pio.h"
#include "host.h"
#include "schedule.h"
#include "gfs_client.h"
#include "gfs_proto.h"
#include "timer.h"

static char *
gfs_pio_view_section_close(GFS_File gf)
{
	struct gfs_file_section_context *vc = gf->view_context;
	char *e = NULL, *e_save = NULL;
	int md_calculated = 1;
	file_offset_t filesize;
	unsigned int md_len;
	unsigned char md_value[EVP_MAX_MD_SIZE];

	if ((gf->mode & GFS_FILE_MODE_WRITE) != 0) {
		e = gfs_pio_flush(gf);
		if (e != NULL)
			goto finish;
	}

	/* calculate checksum */
	if ((gf->mode & GFS_FILE_MODE_CALC_DIGEST) != 0) {
		if ((gf->mode & GFS_FILE_MODE_WRITE) == 0 &&
		    (gf->error != GFS_FILE_ERROR_EOF)) {
			/*
			 * sequential and read-only case, but
			 * either error occurred or gf doesn't reach EOF,
			 * we don't confirm checksum in this case.
			 */
			md_calculated = 0;
		} else if ((gf->mode & GFS_FILE_MODE_WRITE) != 0 &&
		    (gf->open_flags & GFARM_FILE_TRUNC) == 0) {
			/* we have to read rest of the file in this case */
			abort(); /* XXX - not supported for now */
		} else {
			EVP_DigestFinal(&vc->md_ctx, md_value, &md_len);
			filesize = gf->offset + gf->length;
		}
	} else {
		if ((gf->mode & GFS_FILE_MODE_WRITE) == 0) {
			/*
			 * random-access and read-only case,
			 * we don't confirm checksum for this case,
			 * because of its high overhead.
			 */
			md_calculated = 0;
		} else {
			/*
			 * re-read whole file to calculate digest value
			 * for writing.
			 * note that this effectively breaks file offset.
			 */
			e = (*vc->ops->storage_calculate_digest)(gf,
			    GFS_DEFAULT_DIGEST_NAME, sizeof(md_value),
			    &md_len, md_value, &filesize);
			if (e != NULL) {
				md_calculated = 0;
				if (e_save == NULL)
					e_save = e;
			}
		}
	}

	if (md_calculated) {
		int i;
		char md_value_string[EVP_MAX_MD_SIZE * 2 + 1];
		struct gfarm_file_section_info fi;
		struct gfarm_file_section_copy_info fci;

		for (i = 0; i < md_len; i++)
			sprintf(&md_value_string[i + i], "%02x",
				md_value[i]);

		if (gf->mode & GFS_FILE_MODE_WRITE) {
			fi.filesize = filesize;
			fi.checksum_type = GFS_DEFAULT_DIGEST_NAME;
			fi.checksum = md_value_string;
				
			e = gfarm_file_section_info_set(
				gf->pi.pathname, vc->section, &fi);
			if (e == NULL) {
				fci.hostname = vc->canonical_hostname;
				e = gfarm_file_section_copy_info_set(
				    gf->pi.pathname, vc->section,
				    fci.hostname, &fci);
			}
		} else {
			e = gfarm_file_section_info_get(
				gf->pi.pathname, vc->section, &fi);
			if (filesize != fi.filesize)
				e = "filesize mismatch";
			else if (strcasecmp(fi.checksum_type,
				       GFS_DEFAULT_DIGEST_NAME) != 0 ||
			    strcasecmp(fi.checksum, md_value_string) != 0)
				e = "checksum mismatch";
		}
	}

finish:
	if (e_save == NULL)
		e_save = e;

	e = (*vc->ops->storage_close)(gf);
	if (e_save == NULL)
		e_save = e;

	free(vc->canonical_hostname);
	free(vc->section);
	free(vc);
	gf->view_context = NULL;
	gfs_pio_set_view_default(gf);
	return (e_save);
}

static char *
gfs_pio_view_section_write(GFS_File gf, const char *buffer, size_t size,
			   size_t *lengthp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	char *e = (*vc->ops->storage_write)(gf, buffer, size, lengthp);

	if (e == NULL && *lengthp > 0 &&
	    (gf->mode & GFS_FILE_MODE_CALC_DIGEST) != 0)
		EVP_DigestUpdate(&vc->md_ctx, buffer, *lengthp);
	return (e);
}

static char *
gfs_pio_view_section_read(GFS_File gf, char *buffer, size_t size,
			  size_t *lengthp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	char *e = (*vc->ops->storage_read)(gf, buffer, size, lengthp);

	if (e == NULL && *lengthp > 0 &&
	    (gf->mode & GFS_FILE_MODE_CALC_DIGEST) != 0)
		EVP_DigestUpdate(&vc->md_ctx, buffer, *lengthp);
	return (e);
}

static char *
gfs_pio_view_section_seek(GFS_File gf, file_offset_t offset, int whence,
			  file_offset_t *resultp)
{
	struct gfs_file_section_context *vc = gf->view_context;

	gf->mode &= ~GFS_FILE_MODE_CALC_DIGEST;
	return ((*vc->ops->storage_seek)(gf, offset, whence, resultp));
}

static int
gfs_pio_view_section_fd(GFS_File gf)
{
	struct gfs_file_section_context *vc = gf->view_context;

	return ((*vc->ops->storage_fd)(gf));
}

static char *
gfs_pio_view_section_stat(GFS_File gf, struct gfs_stat *status)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfarm_file_section_info sinfo;
	char *e;

	*status = gf->pi.status;
	status->st_user = strdup(status->st_user);
	if (status->st_user == NULL)
		return (GFARM_ERR_NO_MEMORY);
	status->st_group = strdup(status->st_group);
	if (status->st_group == NULL) {
		free(status->st_user);
		return (GFARM_ERR_NO_MEMORY);
	}
	e = gfarm_file_section_info_get(gf->pi.pathname, vc->section, &sinfo);
	if (e != NULL) {
		free(status->st_user);
		free(status->st_group);
		return (e);
	}
	status->st_size = sinfo.filesize;
	status->st_nsections = 1;

	gfarm_file_section_info_free(&sinfo);

	return (NULL);
}

struct gfs_pio_ops gfs_pio_view_section_ops = {
	gfs_pio_view_section_close,
	gfs_pio_view_section_write,
	gfs_pio_view_section_read,
	gfs_pio_view_section_seek,
	gfs_pio_view_section_fd,
	gfs_pio_view_section_stat
};

static char *
replicate_section_to_local(GFS_File gf, char *section, char *peer_hostname)

{
	char *e;
	struct sockaddr peer_addr;
	struct gfs_connection *peer_conn;
	char *path_section;
	char *local_path;
	int fd, peer_fd;

	e = gfarm_host_address_get(peer_hostname, gfarm_spool_server_port,
	    &peer_addr, NULL);
	if (e != NULL)
		return (e);

	e = gfs_client_connection(peer_hostname, &peer_addr, &peer_conn);
	if (e != NULL)
		return (e);

	e = gfarm_path_section(gf->pi.pathname, section, &path_section);
	if (e != NULL) 
		return (e);

	e = gfs_client_open(peer_conn, path_section, GFARM_FILE_RDONLY, 0,
	    &peer_fd);
	/* FT - source file should be missing */
	if (e == GFARM_ERR_NO_SUCH_OBJECT)
		/* Delete the section copy info */
		if (gfarm_file_section_copy_info_remove(gf->pi.pathname,
			section, peer_hostname) == NULL)
			e = GFARM_ERR_INCONSISTENT_RECOVERABLE;
	if (e != NULL)
		goto finish_free_path_section;

	e = gfarm_path_localize(path_section, &local_path);
	if (e != NULL)
		goto finish_peer_close;

	fd = open(local_path, O_WRONLY|O_CREAT|O_TRUNC, gf->pi.status.st_mode);
	/* FT - the parent directory may be missing */
	if (fd == -1
	    && gfs_proto_error_string(errno) == GFARM_ERR_NO_SUCH_OBJECT) {
		if (gfs_pio_local_mkdir_parent_canonical_path(
			    gf->pi.pathname) == NULL)
			fd = open(local_path, O_WRONLY|O_CREAT|O_TRUNC,
				  gf->pi.status.st_mode);
	}
	if (fd < 0) {
		e = gfs_proto_error_string(errno);
		goto finish_free_local_path;
	}

	/* XXX FIXME: this should honor sync_rate */
	e = gfs_client_copyin(peer_conn, peer_fd, fd, 0);
	/* XXX - copyin() should return the digest value */
	if (e == NULL)
		e = gfs_pio_set_fragment_info_local(local_path,
		    gf->pi.pathname, section);    
	close(fd);
finish_free_local_path:
	free(local_path);
finish_peer_close:
	gfs_client_close(peer_conn, peer_fd);
finish_free_path_section:
	free(path_section);
	return (e);
}

double gfs_pio_set_view_section_time;

char *
gfs_pio_set_view_section(GFS_File gf, char *section,
			 char *if_hostname, int flags)
{
	struct gfs_file_section_context *vc;
	char *e;
	int is_local_host;
	gfarm_timerval_t t1, t2;

	gfs_profile(gfarm_gettimerval(&t1));

	e = gfs_pio_set_view_default(gf);
	if (e != NULL)
		goto profile_finish;

	vc = malloc(sizeof(struct gfs_file_section_context));
	if (vc == NULL) {
		e = gf->error = GFARM_ERR_NO_MEMORY;
		goto profile_finish;
	}

	vc->section = strdup(section);
	if (vc->section == NULL) {
		free(vc);
		e = gf->error = GFARM_ERR_NO_MEMORY;
		goto profile_finish;
	}

 retry:
	if (if_hostname != NULL) {
		e = gfarm_host_get_canonical_name(if_hostname,
		    &vc->canonical_hostname);
		if (e == GFARM_ERR_UNKNOWN_HOST) {
			/* FT - invalid hostname, delete section copy info */
			if (gfarm_file_section_copy_info_remove(
				    gf->pi.pathname, vc->section, if_hostname)
			    == NULL)
				e = GFARM_ERR_INCONSISTENT_RECOVERABLE;

			if (e == GFARM_ERR_INCONSISTENT_RECOVERABLE
			    && (flags & GFARM_FILE_NOT_RETRY) == 0
			    && (gf->open_flags & GFARM_FILE_NOT_RETRY) == 0) {
				if_hostname = NULL;
				goto retry;
			}
			goto finish;
		}
		else if (e != NULL)
			goto finish;
	} else if ((gf->open_flags & GFARM_FILE_CREATE) != 0) {
		e = gfarm_host_get_canonical_self_name(&if_hostname);
		if (e == NULL) {
			vc->canonical_hostname = strdup(if_hostname);
			if (vc->canonical_hostname == NULL) {
				e = GFARM_ERR_NO_MEMORY;
				goto finish;
			}
		} else {
			e = gfarm_schedule_search_idle_by_all(1, &if_hostname);
			if (e != NULL)
				goto finish;
			vc->canonical_hostname = if_hostname;
		}
	} else {
		e = gfarm_file_section_host_schedule_with_priority_to_local(
		    gf->pi.pathname, vc->section, &if_hostname);
		if (e != NULL)
			goto finish;
		vc->canonical_hostname = if_hostname; /* must be already
							 canonical */
	}
	is_local_host = gfarm_canonical_hostname_is_local(
					vc->canonical_hostname);

	if ((gf->open_flags & GFARM_FILE_CREATE) != 0) {
		struct gfarm_path_info pi;

		e = gfarm_path_info_set(gf->pi.pathname, &gf->pi);
		if (e == GFARM_ERR_ALREADY_EXISTS &&
		    (e = gfarm_path_info_get(gf->pi.pathname, &pi)) == NULL) {
			if (GFS_FILE_IS_PROGRAM(gf) !=
			    GFARM_S_IS_PROGRAM(pi.status.st_mode))
				e = GFARM_ERR_OPERATION_NOT_PERMITTED;
			if (e == NULL && !GFS_FILE_IS_PROGRAM(gf)) {
				if (gf->pi.status.st_nsections !=
				    pi.status.st_nsections) {
					e = GFARM_ERR_FRAGMENT_NUMBER_DOES_NOT_MATCH;
				} else {
#if 0
					 assert(gf->pi.status.st_mode &
					     GFS_FILE_MODE_NSEGMENTS_FIXED);
#endif
				}
			}
			if (e != NULL) {
				gfarm_path_info_free(&pi);
			} else {
				gfarm_path_info_free(&gf->pi);
				gf->pi = pi;
			}
			/*
			 * XXX should check the follows:
			 * - creator of the metainfo has same job id
			 * - mode is consistent among same job
			 * - nfragments is consistent among same job
			 */
		}
		if (e != NULL)
			goto free_host;
	}

	/* delete section info when opening with a trancation flag. */
	if ((gf->open_flags & GFARM_FILE_TRUNC) != 0)
		(void)gfs_unlink_section(gf->pi.pathname, vc->section);

	gf->ops = &gfs_pio_view_section_ops;
	gf->view_context = vc;
	gf->view_flags = flags;
	gf->p = gf->length = 0;
	gf->io_offset = gf->offset = 0;

	gf->mode |= GFS_FILE_MODE_CALC_DIGEST;
	EVP_DigestInit(&vc->md_ctx, GFS_DEFAULT_DIGEST_MODE);

	if (!is_local_host && 
	    ((((gf->open_flags & GFARM_FILE_REPLICATE) != 0
	       || gf_on_demand_replication ) &&  
	      (flags & GFARM_FILE_NOT_REPLICATE) == 0) ||
	     (flags & GFARM_FILE_REPLICATE) != 0)) {
		e = replicate_section_to_local(gf, vc->section, if_hostname);
		/* FT - inconsistent metadata has been fixed.  try again. */
		if (e == GFARM_ERR_INCONSISTENT_RECOVERABLE
		    && (flags & GFARM_FILE_NOT_RETRY) == 0
		    && (gf->open_flags & GFARM_FILE_NOT_RETRY) == 0) {
			if_hostname = NULL;
			free(vc->canonical_hostname);
			goto retry;
		}
		if (e != NULL)
			goto free_host;
		free(vc->canonical_hostname);
		e = gfarm_host_get_canonical_self_name(
		    &vc->canonical_hostname); 
		if (e != NULL)
			goto finish;
		vc->canonical_hostname = strdup(vc->canonical_hostname);
		if (vc->canonical_hostname == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			goto finish;
		}
		is_local_host = 1;
	}

	if (is_local_host)
		e = gfs_pio_open_local_section(gf, flags);
	else
		e = gfs_pio_open_remote_section(gf, if_hostname, flags);

	/* FT - inconsistent metadata has been fixed.  try again. */
	if (e == GFARM_ERR_INCONSISTENT_RECOVERABLE
	    && (flags & GFARM_FILE_NOT_RETRY) == 0
	    && (gf->open_flags & GFARM_FILE_NOT_RETRY) == 0) {
		if_hostname = NULL;
		free(vc->canonical_hostname);
		goto retry;
	}

free_host:
	if (e != NULL)
		free(vc->canonical_hostname);

finish:
	if (e != NULL) {
		free(vc->section);
		free(vc);
		gf->view_context = NULL;
		gfs_pio_set_view_default(gf);
	}
	gf->error = e;

profile_finish:
	gfs_profile(gfarm_gettimerval(&t2));
	gfs_profile(gfs_pio_set_view_section_time
		    += gfarm_timerval_sub(&t2, &t1));

	return (e);
}

char *
gfs_pio_set_view_index(GFS_File gf, int nfragments, int fragment_index,
		       char *host, int flags)
{
	char section_string[GFARM_INT32STRLEN + 1];

	if (GFS_FILE_IS_PROGRAM(gf)) {
		gf->error = GFARM_ERR_OPERATION_NOT_PERMITTED;
		return (gf->error);
	}

	if (nfragments == GFARM_FILE_DONTCARE) {
		if ((gf->mode & GFARM_FILE_CREATE) != 0 &&
		    !GFARM_S_IS_PROGRAM(gf->pi.status.st_mode)) {
			/* DONTCARE isn't permitted in this case */
			gf->error = GFARM_ERR_INVALID_ARGUMENT;
			return (gf->error);
		}
	} else {
		if ((gf->mode & GFS_FILE_MODE_NSEGMENTS_FIXED) == 0) {
			gf->pi.status.st_nsections = nfragments;
			gf->mode |= GFS_FILE_MODE_NSEGMENTS_FIXED;
		} else if (nfragments != gf->pi.status.st_nsections) {
			gf->error = GFARM_ERR_FRAGMENT_NUMBER_DOES_NOT_MATCH;
			return (gf->error);
		}
		if (fragment_index < 0
		    || fragment_index >= nfragments) {
			gf->error = GFARM_ERR_INVALID_ARGUMENT;
			return (gf->error);
		}
	}

	sprintf(section_string, "%d", fragment_index);

	return (gfs_pio_set_view_section(gf, section_string, host, flags));
}

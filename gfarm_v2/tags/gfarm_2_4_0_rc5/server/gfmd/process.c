#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GFARM_INTERNAL_USE
#include <gfarm/gflog.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>

#include "gfutil.h"
#include "auth.h"
#include "gfm_proto.h"
#include "timespec.h"

#include "subr.h"
#include "db_access.h"
#include "peer.h"
#include "inode.h"
#include "process.h"
#include "id_table.h"
#include "host.h"

#define FILETAB_INITIAL		16
#define FILETAB_MULTIPLY	2
#define FILETAB_MAX		1024

#define PROCESS_ID_MIN			300
#define PROCESS_TABLE_INITIAL_SIZE	100


struct process_link {
	struct process_link *next, *prev;
};

/* XXX hack */
#define SIBLINGS_PTR_TO_PROCESS(sp) \
	((struct process *)((char *)(sp) - offsetof(struct process, siblings)))

struct process {
	struct process_link siblings;
	struct process_link children; /* dummy header of siblings list */
	struct process *parent;

	char sharedkey[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];
	gfarm_pid_t pid;
	struct user *user;
	int refcount;

	int nfiles;
	struct file_opening **filetab;
};

static struct gfarm_id_table *process_id_table = NULL;
static struct gfarm_id_table_entry_ops process_id_table_ops = {
	sizeof(struct process)
};

static struct file_opening *
file_opening_alloc(struct inode *inode,
	struct peer *peer, struct host *spool_host, int flag)
{
	struct file_opening *fo;

	GFARM_MALLOC(fo);
	if (fo == NULL) {
		gflog_debug(GFARM_MSG_1001593,
			"allocation of 'file_opening' failed");
		return (NULL);
	}
	fo->inode = inode;
	fo->flag = flag;
	fo->opener = peer;

	if (inode_is_file(inode)) {
		if (spool_host == NULL) {
			fo->u.f.spool_opener = NULL;
			fo->u.f.spool_host = NULL;
		} else {
			fo->u.f.spool_opener = peer;
			fo->u.f.spool_host = spool_host;
		}
		fo->u.f.replicating = NULL;
	} else if (inode_is_dir(inode)) {
		fo->u.d.offset = 0;
		fo->u.d.key = NULL;
	}

	return (fo);
}

void
file_opening_free(struct file_opening *fo, gfarm_mode_t mode)
{
	if (GFARM_S_ISREG(mode)) {
		if (fo->u.f.replicating != NULL) {
			gflog_debug(GFARM_MSG_1002236, "orphan replicating");
			file_replicating_free(fo->u.f.replicating);
			fo->u.f.replicating = NULL;
		}
	} else if (GFARM_S_ISDIR(mode)) {
		if (fo->u.d.key != NULL)
			free(fo->u.d.key);
	}
	free(fo);
}

gfarm_error_t
process_alloc(struct user *user,
	gfarm_int32_t keytype, size_t keylen, char *sharedkey,
	struct process **processp, gfarm_pid_t *pidp)
{
	struct process *process;
	struct file_opening **filetab;
	int fd;
	gfarm_int32_t pid32;

	if (process_id_table == NULL) {
		process_id_table = gfarm_id_table_alloc(&process_id_table_ops);
		if (process_id_table == NULL)
			gflog_fatal(GFARM_MSG_1000293,
			    "allocating pid table: no memory");
		gfarm_id_table_set_base(process_id_table, PROCESS_ID_MIN);
		gfarm_id_table_set_initial_size(process_id_table,
		    PROCESS_TABLE_INITIAL_SIZE);
	}

	if (keytype != GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET ||
	    keylen != GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET) {
		gflog_debug(GFARM_MSG_1001594,
			"'keytype' or 'keylen' is invalid");
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	GFARM_MALLOC_ARRAY(filetab, FILETAB_INITIAL);
	if (filetab == NULL) {
		gflog_debug(GFARM_MSG_1001595,
			"allocation of 'filetab' failed");
		return (GFARM_ERR_NO_MEMORY);
	}
	process = gfarm_id_alloc(process_id_table, &pid32);
	if (process == NULL) {
		free(filetab);
		gflog_debug(GFARM_MSG_1001596,
			"gfarm_id_alloc() failed");
		return (GFARM_ERR_NO_MEMORY);
	}
	process->siblings.next = process->siblings.prev = &process->siblings;
	process->children.next = process->children.prev = &process->children;
	process->parent = NULL;
	memcpy(process->sharedkey, sharedkey, keylen);
	process->pid = pid32;
	process->user = user;
	process->refcount = 0;
	process->nfiles = FILETAB_INITIAL;
	process->filetab = filetab;
	for (fd = 0; fd < FILETAB_INITIAL; fd++)
		filetab[fd] = NULL;

	*processp = process;
	*pidp = pid32;
	return (GFARM_ERR_NO_ERROR);
}

static void
process_add_child(struct process *parent, struct process *child)
{
	child->siblings.next = &parent->children;
	child->siblings.prev = parent->children.prev;
	parent->children.prev->next = &child->siblings;
	parent->children.prev = &child->siblings;
	child->parent = parent;
}

static void
process_add_ref(struct process *process)
{
	++process->refcount;
}

/* NOTE: caller of this function should acquire giant_lock as well */
static int
process_del_ref(struct process *process)
{
	int fd;
	gfarm_mode_t mode;
	struct file_opening *fo;
	struct process_link *pl, *pln;
	struct process *child;

	if (--process->refcount > 0)
		return (1); /* still referenced */

	/* make all children orphan */
	for (pl = process->children.next; pl != &process->children; pl = pln) {
		pln = pl->next;
		child = SIBLINGS_PTR_TO_PROCESS(pl);
		child->parent = NULL;
		pl->next = pl->prev = pl;
	}
	/* detach myself from children list */
	process->siblings.next->prev = process->siblings.prev;
	process->siblings.prev->next = process->siblings.next;

	for (fd = 0; fd < process->nfiles; fd++) {
		fo = process->filetab[fd];
		if (fo != NULL) {
			mode = inode_get_mode(fo->inode);
			inode_close_read(fo, NULL);
			file_opening_free(fo, mode);
		}
	}
	free(process->filetab);
	gfarm_id_free(process_id_table, (gfarm_int32_t)process->pid);

	return (0); /* process freed */
}

/* NOTE: caller of this function should acquire giant_lock as well */
void
process_attach_peer(struct process *process, struct peer *peer)
{
	process_add_ref(process);
	/* We are currently not using peer here */
}

/* NOTE: caller of this function should acquire giant_lock as well */
void
process_detach_peer(struct process *process, struct peer *peer)
{
	int fd;

	if (!process_del_ref(process)) /* process freed */
		return;

	for (fd = 0; fd < process->nfiles; fd++) {
		/*
		 * XXX This shouldn't be done,
		 * if we'll support gfmd reconnection.
		 */
		if (process->filetab[fd] != NULL)
			process_close_file(process, peer, fd);
	}
}

struct user *
process_get_user(struct process *process)
{
	return (process->user);
}

gfarm_error_t
process_verify_fd(struct process *process, int fd)
{
	struct file_opening *fo;

	if (fd < 0 || fd >= process->nfiles) {
		gflog_debug(GFARM_MSG_1001597,
			"bad file descriptor");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	fo = process->filetab[fd];
	if (fo == NULL) {
		gflog_debug(GFARM_MSG_1001598,
			"'file_opening' is NULL");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_get_file_opening(struct process *process, int fd,
	struct file_opening **fop)
{
	struct file_opening *fo;

	if (fd < 0 || fd >= process->nfiles) {
		gflog_debug(GFARM_MSG_1001599,
			"bad file descriptor");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	fo = process->filetab[fd];
	if (fo == NULL) {
		gflog_debug(GFARM_MSG_1001600,
			"'file_opening' is NULL");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	*fop = fo;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_get_file_inode(struct process *process,	int fd, struct inode **inp)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001601,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	*inp = fo->inode;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_get_file_writable(struct process *process, struct peer *peer, int fd)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001602,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if ((accmode_to_op(fo->flag) & GFS_W_OK) == 0) {
		gflog_debug(GFARM_MSG_1001603,
			"permission is denied");
		return (GFARM_ERR_PERMISSION_DENIED);
	}

	if (fo->opener == peer)
		return (GFARM_ERR_NO_ERROR);
	if (inode_is_file(fo->inode) && fo->u.f.spool_opener == peer)
		return (GFARM_ERR_NO_ERROR);
	gflog_debug(GFARM_MSG_1001604,
		"operation is not permitted");
	return (GFARM_ERR_OPERATION_NOT_PERMITTED);
}

gfarm_error_t
process_get_dir_offset(struct process *process, struct peer *peer,
	int fd, gfarm_off_t *offsetp)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001605,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_dir(fo->inode)) {
		gflog_debug(GFARM_MSG_1001606,
			"inode is not a directory");
		return (GFARM_ERR_NOT_A_DIRECTORY);
	}
	if (fo->opener != peer) {
		gflog_debug(GFARM_MSG_1001607,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	*offsetp = fo->u.d.offset;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_set_dir_offset(struct process *process, struct peer *peer,
	int fd, gfarm_off_t offset)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001608,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_dir(fo->inode)) {
		gflog_debug(GFARM_MSG_1001609,
			"inode is not a directory");
		return (GFARM_ERR_NOT_A_DIRECTORY);
	}
	if (fo->opener != peer) {
		gflog_debug(GFARM_MSG_1001610,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	fo->u.d.offset = offset;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * The reason why we provide fo->u.d.key (not only fo->u.d.offset) is
 * because fo->u.d.key is more robust with directory entry insertion/deletion.
 */

gfarm_error_t
process_get_dir_key(struct process *process, struct peer *peer,
	int fd, char **keyp, int *keylenp)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001611,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_dir(fo->inode)) {
		gflog_debug(GFARM_MSG_1001612,
			"inode is not a directory");
		return (GFARM_ERR_NOT_A_DIRECTORY);
	}
	if (fo->opener != peer) {
		gflog_debug(GFARM_MSG_1001613,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.d.key == NULL) {
		*keyp = NULL;
		*keylenp = 0;
	} else {
		*keyp = fo->u.d.key;
		*keylenp = strlen(fo->u.d.key);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_set_dir_key(struct process *process, struct peer *peer,
	int fd, char *key, int keylen)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);
	char *s;

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001614,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_dir(fo->inode)) {
		gflog_debug(GFARM_MSG_1001615,
			"inode is not a directory");
		return (GFARM_ERR_NOT_A_DIRECTORY);
	}
	if (fo->opener != peer) {
		gflog_debug(GFARM_MSG_1001616,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	s = malloc(keylen + 1);
	if (s == NULL) {
		gflog_debug(GFARM_MSG_1001617,
			"allocation of string failed");
		return (GFARM_ERR_NO_MEMORY);
	}
	memcpy(s, key, keylen);
	s[keylen] = '\0';

	if (fo->u.d.key != NULL)
		free(fo->u.d.key);
	fo->u.d.key = s;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_clear_dir_key(struct process *process, struct peer *peer, int fd)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001618,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_dir(fo->inode)) {
		gflog_debug(GFARM_MSG_1001619,
			"inode is not a directory");
		return (GFARM_ERR_NOT_A_DIRECTORY);
	}
	if (fo->opener != peer) {
		gflog_debug(GFARM_MSG_1001620,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	if (fo->u.d.key != NULL)
		free(fo->u.d.key);
	fo->u.d.key = NULL;
	return (GFARM_ERR_NO_ERROR);
}

struct process *
process_lookup(gfarm_pid_t pid)
{
	if (process_id_table == NULL)
		return (NULL);
	return (gfarm_id_lookup(process_id_table, (gfarm_int32_t)pid));
}

gfarm_error_t
process_does_match(gfarm_pid_t pid,
	gfarm_int32_t keytype, size_t keylen, char *sharedkey,
	struct process **processp)
{
	struct process *process = process_lookup((gfarm_int32_t)pid);

	if (process == NULL) {
		gflog_debug(GFARM_MSG_1001621,
			"process_lookup() failed");
		return (GFARM_ERR_NO_SUCH_PROCESS);
	}
	if (keytype != GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET ||
	    keylen != GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET ||
	    memcmp(sharedkey, process->sharedkey,
	    GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET) != 0) {
		gflog_debug(GFARM_MSG_1001622,
			"authentication failed");
		return (GFARM_ERR_AUTHENTICATION);
	}
	*processp = process;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_new_generation_wait(struct peer *peer, int fd,
	gfarm_error_t (*action)(struct peer *, void *, int *), void *arg)
{
	struct process *process;
	struct file_opening *fo;
	gfarm_error_t e;
	static const char diag[] = "process_new_generation_wait";

	if ((process = peer_get_process(peer)) == NULL) {
		gflog_debug(GFARM_MSG_1002237,
		    "%s: peer_get_process() failed", diag);
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if ((e = process_get_file_opening(process, fd, &fo))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002238,
		    "%s: process_get_file_opening(%d) failed", diag, fd);
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else {
		e = inode_new_generation_wait(fo->inode, peer, action, arg);
	}
	return (e);
}

gfarm_error_t
process_new_generation_done(struct process *process, struct peer *peer, int fd,
	gfarm_int32_t result)
{
	struct file_opening *fo;
	gfarm_mode_t mode;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);
	static const char diag[] = "process_new_generation_done";

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002325,
		    "%s: pid %lld descriptor %d: %s", diag,
		    (long long)process->pid, fd, gfarm_error_string(e));
		return (e);
	} else if ((e = inode_new_generation_done(fo->inode, peer,
	    result)) == GFARM_ERR_NO_ERROR) {

		/* resume deferred operaton: close the file */

		if (fo->opener != peer && fo->opener != NULL) {
			/*
			 * closing REOPENed file,
			 * but the client is still opening
			 */
			fo->u.f.spool_opener = NULL;
			fo->u.f.spool_host = NULL;
		} else {
			mode = inode_get_mode(fo->inode);
			inode_close(fo);

			file_opening_free(fo, mode);
			process->filetab[fd] = NULL;
		}
	}
	return (e);
}

gfarm_error_t
process_open_file(struct process *process, struct inode *file,
	gfarm_int32_t flag, int created,
	struct peer *peer, struct host *spool_host,
	gfarm_int32_t *fdp)
{
	gfarm_error_t e;
	int fd, fd2;
	struct file_opening **p, *fo;

	/* XXX FIXME cache minimum unused fd, and avoid liner search */
	for (fd = 0; fd < process->nfiles; fd++) {
		if (process->filetab[fd] == NULL)
			break;
	}
	if (fd >= process->nfiles) {
		if (fd >= FILETAB_MAX) {
			gflog_debug(GFARM_MSG_1001623,
				"too many open files");
			return (GFARM_ERR_TOO_MANY_OPEN_FILES);
		}
		p = realloc(process->filetab,
		    sizeof(*p) * (process->nfiles * FILETAB_MULTIPLY));
		if (p == NULL) {
			gflog_debug(GFARM_MSG_1001624,
				"re-allocation of 'process' failed");
			return (GFARM_ERR_NO_MEMORY);
		}
		process->filetab = p;
		process->nfiles *= FILETAB_MULTIPLY;
		for (fd2 = fd + 1; fd2 < process->nfiles; fd2++)
			process->filetab[fd2] = NULL;
	}
	fo = file_opening_alloc(file, peer, spool_host,
	    flag | (created ? GFARM_FILE_CREATE : 0));
	if (fo == NULL) {
		gflog_debug(GFARM_MSG_1001625,
			"file_opening_alloc() failed");
		return (GFARM_ERR_NO_MEMORY);
	}
	e = inode_open(fo);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001626,
			"inode_open() failed: %s",
			gfarm_error_string(e));
		file_opening_free(fo, inode_get_mode(file));
		return (e);
	}
	process->filetab[fd] = fo;
	
	*fdp = fd;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_reopen_file(struct process *process,
	struct peer *peer, struct host *spool_host, int fd,
	gfarm_ino_t *inump, gfarm_uint64_t *genp, gfarm_int32_t *modep,
	gfarm_int32_t *flagsp, gfarm_int32_t *to_createp)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);
	int to_create, is_creating_file_replica;

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001627,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_file(fo->inode)) { /* i.e. is a directory */
		gflog_debug(GFARM_MSG_1001628,
			"inode is not file");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.f.spool_opener != NULL) { /* already REOPENed */
		gflog_debug(GFARM_MSG_1001629,
			"file already reopened");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (inode_new_generation_is_pending(fo->inode)) {
		gflog_debug(GFARM_MSG_1002240,
		    "process_reopen_file: new_generation pending %lld:%lld",
		    (long long)inode_get_number(fo->inode),
		    (long long)inode_get_gen(fo->inode));
		return (GFARM_ERR_RESOURCE_TEMPORARILY_UNAVAILABLE);
	}

	to_create = inode_is_creating_file(fo->inode);
	is_creating_file_replica = (fo->flag & GFARM_FILE_CREATE_REPLICA) != 0;

	if ((accmode_to_op(fo->flag) & GFS_W_OK) != 0 || to_create) {
		if (is_creating_file_replica) {
			e = inode_add_replica(fo->inode, spool_host, 0);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_debug(GFARM_MSG_1001630,
					"inode_add_replica() failed: %s",
					gfarm_error_string(e));
				return (e);
			}
		}
		else if (!inode_schedule_confirm_for_write(fo->inode,
		    spool_host, to_create)) {
			gflog_debug(GFARM_MSG_1001631,
				"file migrated");
			return (GFARM_ERR_FILE_MIGRATED);
		}
		if (to_create) {
			e = inode_add_replica(fo->inode, spool_host, 1);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_debug(GFARM_MSG_1001632,
					"inode_add_replica() failed: %s",
					gfarm_error_string(e));
				return (e);
			}
		}
	} else {
		if (!inode_has_replica(fo->inode, spool_host)) {
			gflog_debug(GFARM_MSG_1001633,
				"file migrated");
			return (GFARM_ERR_FILE_MIGRATED);
		}
	}

	fo->u.f.spool_opener = peer;
	fo->u.f.spool_host = spool_host;
	*inump = inode_get_number(fo->inode);
	*genp = inode_get_gen(fo->inode);
	*modep = inode_get_mode(fo->inode);
	*flagsp = fo->flag & GFARM_FILE_USER_MODE;
	*to_createp = to_create || is_creating_file_replica;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_close_file(struct process *process, struct peer *peer, int fd)
{
	struct file_opening *fo;
	gfarm_mode_t mode;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001634,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}

	mode = inode_get_mode(fo->inode);

	if (fo->opener != peer) {
		if (!GFARM_S_ISREG(mode)) {
			gflog_debug(GFARM_MSG_1001635,
				"inode is not file");
			return (GFARM_ERR_OPERATION_NOT_PERMITTED);
		}
		if (fo->u.f.spool_opener != peer) {
			gflog_debug(GFARM_MSG_1001636,
				"operation is not permitted");
			return (GFARM_ERR_OPERATION_NOT_PERMITTED);
		}
		/* i.e. REOPENed file, and I am a gfsd. */
		if (fo->opener != NULL) {
			/*
			 * a gfsd is closing a REOPENed file,
			 * but the client is still opening it.
			 */
			fo->u.f.spool_opener = NULL;
			fo->u.f.spool_host = NULL;
			return (GFARM_ERR_NO_ERROR);
		}
	} else {
		if (GFARM_S_ISREG(mode) &&
		    fo->u.f.spool_opener != NULL &&
		    fo->u.f.spool_opener != peer) {
			/*
			 * a client is closing a file,
			 * but the gfsd is still opening it.
			 */
			fo->opener = NULL;
			return (GFARM_ERR_NO_ERROR);
			
		}
	}

	inode_close(fo);
	file_opening_free(fo, mode);
	process->filetab[fd] = NULL;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_close_file_read(struct process *process, struct peer *peer, int fd,
	struct gfarm_timespec *atime)
{
	struct file_opening *fo;
	gfarm_mode_t mode;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001637,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	mode = inode_get_mode(fo->inode);
	if (!GFARM_S_ISREG(mode)) {
		gflog_debug(GFARM_MSG_1001638,
			"inode is not file");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.f.spool_opener != peer) {
		gflog_debug(GFARM_MSG_1001639,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	if (fo->opener != peer && fo->opener != NULL) {
		/* closing REOPENed file, but the client is still opening */
		fo->u.f.spool_opener = NULL;
		fo->u.f.spool_host = NULL;
		inode_set_atime(fo->inode, atime);
		return (GFARM_ERR_NO_ERROR);
	}

	inode_close_read(fo, atime);
	file_opening_free(fo, mode);
	process->filetab[fd] = NULL;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_close_file_write(struct process *process, struct peer *peer, int fd,
	gfarm_off_t size,
	struct gfarm_timespec *atime, struct gfarm_timespec *mtime,
	gfarm_int32_t *flagsp,
	gfarm_int64_t *old_genp, gfarm_int64_t *new_genp)
{
	struct file_opening *fo;
	gfarm_mode_t mode;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);
	gfarm_int32_t flags = 0;
	int is_v2_4 = (flagsp != NULL);
	static const char diag[] = "process_close_file_write";

	/*
	 * NOTE: gfsd uses CLOSE_FILE_WRITE protocol only if the file is
	 * really updated.
	 */

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001640,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	mode = inode_get_mode(fo->inode);
	if (!GFARM_S_ISREG(mode)) {
		gflog_debug(GFARM_MSG_1001641,
			"inode is not file");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.f.spool_opener != peer) {
		gflog_debug(GFARM_MSG_1001642,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if ((accmode_to_op(fo->flag) & GFS_W_OK) == 0) {
		gflog_debug(GFARM_MSG_1001643,
			"bad file descriptor");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	if (is_v2_4 && inode_new_generation_is_pending(fo->inode)) {
		gflog_debug(GFARM_MSG_1002241,
		    "%s: new_generation pending %lld:%lld", diag,
		    (long long)inode_get_number(fo->inode),
		    (long long)inode_get_gen(fo->inode));
		return (GFARM_ERR_RESOURCE_TEMPORARILY_UNAVAILABLE);
	}

	if (inode_is_updated(fo->inode, mtime) &&

	    /*
	     * GFARM_FILE_CREATE_REPLICA flag means to create and add a
	     * file replica by gfs_pio_write if this file already has file
	     * replicas.  GFARM_ERR_ALREADY_EXISTS error means this is the
	     * first one and this file has only one replica.  If it is
	     * not, do not change the status.
	     */
	    (((fo->flag & GFARM_FILE_CREATE_REPLICA) == 0) ||
	    (inode_add_replica(fo->inode, fo->u.f.spool_host, 1)
	    == GFARM_ERR_ALREADY_EXISTS)) &&

	    inode_file_update(fo, size, atime, mtime, old_genp, new_genp)) {

		flags = GFM_PROTO_CLOSE_WRITE_GENERATION_UPDATE_NEEDED;
	}

	if ((flags & GFM_PROTO_CLOSE_WRITE_GENERATION_UPDATE_NEEDED) != 0) {
		/* defer file close for GFM_PROTO_GENERATION_UPDATED */
		e = inode_new_generation_wait_start(fo->inode, peer);
		if (e != GFARM_ERR_NO_ERROR)
			return (e); /* XXX FIXME: it's better to close fd */
		peer_set_pending_new_generation(peer, fo->inode);
	} else if (fo->opener != peer && fo->opener != NULL) {
		/* closing REOPENed file, but the client is still opening */
		fo->u.f.spool_opener = NULL;
		fo->u.f.spool_host = NULL;
	} else {
		inode_close(fo);

		file_opening_free(fo, mode);
		process->filetab[fd] = NULL;
	}
	if (flagsp != NULL)
		*flagsp = flags;

	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_cksum_set(struct process *process, struct peer *peer, int fd,
	const char *cksum_type, size_t cksum_len, const char *cksum,
	gfarm_int32_t flags, struct gfarm_timespec *mtime)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001644,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_file(fo->inode)) {
		gflog_debug(GFARM_MSG_1001645,
			"inode is not file");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.f.spool_opener != peer) {
		gflog_debug(GFARM_MSG_1001646,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if ((accmode_to_op(fo->flag) & GFS_W_OK) == 0) {
		gflog_debug(GFARM_MSG_1001647,
			"bad file descriptor");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}

	return (inode_cksum_set(fo, cksum_type, cksum_len, cksum,
	    flags, mtime));
}

gfarm_error_t
process_cksum_get(struct process *process, struct peer *peer, int fd,
	char **cksum_typep, size_t *cksum_lenp, char **cksump,
	gfarm_int32_t *flagsp)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001648,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_file(fo->inode)) {
		gflog_debug(GFARM_MSG_1001649,
			"inode is not file");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	return (inode_cksum_get(fo, cksum_typep, cksum_lenp, cksump,
	    flagsp));
}

gfarm_error_t
process_bequeath_fd(struct process *process, gfarm_int32_t fd)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001650,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	fo->flag |= GFARM_FILE_BEQUEATHED;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_inherit_fd(struct process *process, gfarm_int32_t parent_fd,
	struct peer *peer, struct host *spool_host, gfarm_int32_t *fdp)
{
	struct file_opening *fo;
	gfarm_error_t e;

	if (process->parent == NULL) {
		gflog_debug(GFARM_MSG_1001651,
			"process->parent does not exist");
		return (GFARM_ERR_NO_SUCH_PROCESS);
	}
	e = process_get_file_opening(process, parent_fd, &fo);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001652,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if ((fo->flag & GFARM_FILE_BEQUEATHED) == 0) {
		gflog_debug(GFARM_MSG_1001653,
			"bad file descriptor");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	return (process_open_file(process, fo->inode, fo->flag,
	    (fo->flag & GFARM_FILE_CREATE) != 0, peer, spool_host, fdp));
}

gfarm_error_t
process_prepare_to_replicate(struct process *process, struct peer *peer,
	struct host *src, struct host *dst, int fd, gfarm_int32_t flags,
	struct file_replicating **frp, struct inode **inodep)
{
	gfarm_error_t e;
	struct file_opening *fo;
	struct user *user;

	if ((e = process_get_file_opening(process, fd, &fo))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001654,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (fo->u.f.spool_opener != NULL) { /* already REOPENed */
		gflog_debug(GFARM_MSG_1001655,
			"operation is not permitted, already reopened");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if ((user = process_get_user(process)) == NULL) {
		gflog_debug(GFARM_MSG_1001656,
			"process_get_user() failed");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	e = inode_prepare_to_replicate(fo->inode, user, src, dst, flags, frp);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001657,
			"inode_prepare_to_replicate() failed: %s",
			gfarm_error_string(e));
		return (e);
	}

	*inodep = fo->inode;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
process_replica_adding(struct process *process, struct peer *peer,
	struct host *src, struct host *dst, int fd,
	struct inode **inodep)
{
	struct file_opening *fo;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (fo->u.f.replicating != NULL)
		return (GFARM_ERR_FILE_BUSY);
	if (inode_new_generation_is_pending(fo->inode)) {
		gflog_debug(GFARM_MSG_1002242,
		    "process_replica_adding: new_generation pending %lld:%lld",
		    (long long)inode_get_number(fo->inode),
		    (long long)inode_get_gen(fo->inode));
		return (GFARM_ERR_RESOURCE_TEMPORARILY_UNAVAILABLE);
	}

	e = process_prepare_to_replicate(process, peer, src, dst, fd,
	    GFS_REPLICATE_FILE_FORCE, &fo->u.f.replicating, inodep);
	if (e == GFARM_ERR_NO_ERROR) {
		fo->u.f.spool_opener = peer;
		/*
		 * do not set spool_host
		 * since replica is now creating to this host
		 */
	}
	return (e);
}

gfarm_error_t
process_replica_added(struct process *process,
	struct peer *peer, struct host *spool_host, int fd,
	int flags, gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec,
	gfarm_off_t size)
{
	struct file_opening *fo;
	struct gfarm_timespec *mtime;
	gfarm_error_t e = process_get_file_opening(process, fd, &fo), e2;

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001658,
			"process_get_file_opening() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!inode_is_file(fo->inode)) { /* i.e. is a directory */
		gflog_debug(GFARM_MSG_1001659,
			"inode is not file");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.f.spool_opener != peer) {
		gflog_debug(GFARM_MSG_1001660,
			"operation is not permitted");
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	if (fo->u.f.replicating == NULL) {
		gflog_debug(GFARM_MSG_1002243,
		    "replica_added was called witout adding");
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	if (inode_is_creating_file(fo->inode)) { /* no file copy */
		gflog_debug(GFARM_MSG_1001661,
			"inode has no file copy");
		e = GFARM_ERR_NO_SUCH_OBJECT;
	} else if (inode_has_replica(fo->inode, spool_host)) {
		gflog_debug(GFARM_MSG_1001662,
			"spool_host already has replica");
		e = GFARM_ERR_ALREADY_EXISTS;
	} else if (mtime_sec != (mtime = inode_get_mtime(fo->inode))->tv_sec ||
	    mtime_nsec != mtime->tv_nsec ||
	    (size != -1 && size != inode_get_size(fo->inode)) ||
	    file_replicating_get_gen(fo->u.f.replicating) !=
	    inode_get_gen(fo->inode)) {
		gflog_debug(GFARM_MSG_1002244,
		    "inode(%lld) updated while replication: "
		    "mtime %lld.%09lld/%lld.%09lld, "
		    "size: %lld/%lld, gen:%lld/%lld",
		    (long long)inode_get_number(fo->inode),
		    (long long)mtime_sec, (long long)mtime_nsec,
		    (long long)inode_get_mtime(fo->inode)->tv_sec,
		    (long long)inode_get_mtime(fo->inode)->tv_nsec,
		    (long long)size, (long long)inode_get_size(fo->inode),
		    (long long)file_replicating_get_gen(fo->u.f.replicating),
		    (long long)inode_get_gen(fo->inode));
		e = inode_remove_replica_gen(fo->inode, spool_host,
		    file_replicating_get_gen(fo->u.f.replicating), 0);
		if (e == GFARM_ERR_NO_ERROR || e == GFARM_ERR_NO_SUCH_OBJECT)
			e = GFARM_ERR_INVALID_FILE_REPLICA;
	} else
		e = inode_add_replica(fo->inode, spool_host, 1);
	file_replicating_free(fo->u.f.replicating);
	fo->u.f.replicating = NULL;
	e2 = process_close_file_read(process, peer, fd, NULL);
	return (e != GFARM_ERR_NO_ERROR ? e : e2);
}

/*
 * protocol handler
 */

gfarm_error_t
gfm_server_process_alloc(struct peer *peer, int from_client, int skip)
{
	gfarm_int32_t e;
	struct user *user;
	gfarm_int32_t keytype;
	size_t keylen;
	char sharedkey[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];
	struct process *process;
	gfarm_pid_t pid;
	static const char diag[] = "GFM_PROTO_PROCESS_ALLOC";

	e = gfm_server_get_request(peer, diag,
	    "ib", &keytype, sizeof(sharedkey), &keylen, sharedkey);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001663,
			"process_alloc request failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	giant_lock();
	if (peer_get_process(peer) != NULL) {
		gflog_debug(GFARM_MSG_1001664,
			"peer_get_process() failed");
		e = GFARM_ERR_ALREADY_EXISTS;
	} else if (!from_client || (user = peer_get_user(peer)) == NULL) {
		gflog_debug(GFARM_MSG_1001665,
			"operation is not permitted");
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if ((e = process_alloc(user, keytype, keylen, sharedkey,
	    &process, &pid)) == GFARM_ERR_NO_ERROR) {
		peer_set_process(peer, process);
	}
	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, "l", pid));
}

gfarm_error_t
gfm_server_process_alloc_child(struct peer *peer, int from_client, int skip)
{
	gfarm_int32_t e;
	struct user *user;
	gfarm_int32_t parent_keytype, keytype;
	size_t parent_keylen, keylen;
	char parent_sharedkey[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];
	char sharedkey[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];
	struct process *parent_process, *process;
	gfarm_pid_t parent_pid, pid;
	static const char diag[] = "GFM_PROTO_PROCESS_ALLOC_CHILD";

	e = gfm_server_get_request(peer, diag, "iblib",
	    &parent_keytype,
	    sizeof(parent_sharedkey), &parent_keylen, parent_sharedkey,
	    &parent_pid,
	    &keytype, sizeof(sharedkey), &keylen, sharedkey);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001666,
			"process_alloc_child request failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	giant_lock();
	if (peer_get_process(peer) != NULL) {
		gflog_debug(GFARM_MSG_1001667,
			"peer_get_process() failed");
		e = GFARM_ERR_ALREADY_EXISTS;
	} else if (!from_client || (user = peer_get_user(peer)) == NULL) {
		gflog_debug(GFARM_MSG_1001668,
			"operation is not permitted");
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if (parent_keytype != GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET ||
	    parent_keylen != GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET) {
		gflog_debug(GFARM_MSG_1001669,
			"'parent_keytype' or 'parent_keylen' is invalid");
		e = GFARM_ERR_INVALID_ARGUMENT;
	} else if ((e = process_does_match(parent_pid,
	    parent_keytype, parent_keylen, parent_sharedkey,
	    &parent_process)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001670,
			"process_does_match() failed: %s",
			gfarm_error_string(e));
		/* error */
	} else if ((e = process_alloc(user, keytype, keylen, sharedkey,
	    &process, &pid)) == GFARM_ERR_NO_ERROR) {
		peer_set_process(peer, process);
		process_add_child(parent_process, process);
	}
	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, "l", pid));
}

gfarm_error_t
gfm_server_process_set(struct peer *peer, int from_client, int skip)
{
	gfarm_int32_t e;
	gfarm_pid_t pid;
	gfarm_int32_t keytype;
	size_t keylen;
	char sharedkey[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];
	struct process *process;
	static const char diag[] = "GFM_PROTO_PROCESS_SET";

	e = gfm_server_get_request(peer, diag,
	    "ibl", &keytype, sizeof(sharedkey), &keylen, sharedkey, &pid);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001671,
			"process_set request failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	giant_lock();
	if (peer_get_process(peer) != NULL) {
		gflog_debug(GFARM_MSG_1001672,
			"peer_get_process() failed");
		e = GFARM_ERR_ALREADY_EXISTS;
	} else if (keytype != GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET ||
	    keylen != GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET) {
		gflog_debug(GFARM_MSG_1001673,
			"'parent_keytype' or 'parent_keylen' is invalid");
		e = GFARM_ERR_INVALID_ARGUMENT;
	} else if ((e = process_does_match(pid, keytype, keylen, sharedkey,
	    &process)) == GFARM_ERR_NO_ERROR) {
		peer_set_process(peer, process);
		if (!from_client)
			peer_set_user(peer, process_get_user(process));
	}
	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, ""));
}

gfarm_error_t
gfm_server_process_free(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	int transaction = 0;
	static const char diag[] = "GFM_PROTO_PROCESS_FREE";

	if (skip)
		return (GFARM_ERR_NO_ERROR);
	
	giant_lock();
	if (peer_get_process(peer) == NULL) {
		gflog_debug(GFARM_MSG_1001674,
			"peer_get_process() failed");
		e = GFARM_ERR_NO_SUCH_PROCESS;
	}
	else {
		if (db_begin(diag) == GFARM_ERR_NO_ERROR)
			transaction = 1;
		/*
		 * the following internally calls inode_close*() and
		 * closing must be done regardless of the result of db_begin().
		 * because not closing may cause descriptor leak.
		 */
		peer_unset_process(peer);
		e = GFARM_ERR_NO_ERROR;
		if (transaction)
			db_end(diag);
	}

	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, ""));
}

gfarm_error_t
gfm_server_bequeath_fd(struct peer *peer, int from_client, int skip)
{
	gfarm_int32_t e;
	struct host *spool_host;
	struct process *process;
	gfarm_int32_t fd;
	static const char diag[] = "GFM_PROTO_BEQUEATH_FD";

	if (skip)
		return (GFARM_ERR_NO_ERROR);
	giant_lock();

	if (!from_client && (spool_host = peer_get_host(peer)) == NULL) {
		gflog_debug(GFARM_MSG_1001675,
			"operation is not permitted ");
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if ((process = peer_get_process(peer)) == NULL) {
		gflog_debug(GFARM_MSG_1001676,
			"peer_get_process() failed");
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if ((e = peer_fdpair_get_current(peer, &fd)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001677,
			"peer_fdpair_get_current() failed");
	} else
		e = process_bequeath_fd(process, fd);

	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, ""));
}

gfarm_error_t
gfm_server_inherit_fd(struct peer *peer, int from_client, int skip)
{
	gfarm_int32_t e;
	gfarm_int32_t parent_fd, fd;
	struct host *spool_host;
	struct process *process;
	static const char diag[] = "GFM_PROTO_INHERIT_FD";

	e = gfm_server_get_request(peer, diag, "i", &parent_fd);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001678,
			"inherit_fd request failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (skip)
		return (GFARM_ERR_NO_ERROR);
	giant_lock();

	if (!from_client && (spool_host = peer_get_host(peer)) == NULL) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1001679,
			"operation is not permitted");
	} else if ((process = peer_get_process(peer)) == NULL) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1001680,
			"peer_get_process() failed");
	} else if ((e = process_inherit_fd(process, parent_fd, peer, NULL,
	    &fd)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001681,
			"process_inherit_fd() failed: %s",
			gfarm_error_string(e));
	} else
		peer_fdpair_set_current(peer, fd);

	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, ""));
}

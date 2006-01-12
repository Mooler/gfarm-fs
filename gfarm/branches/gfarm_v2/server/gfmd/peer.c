#include <pthread.h>

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>

#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>

#include "gfutil.h"
#include "gfp_xdr.h"
#include "io_fd.h"
#include "auth.h"

#include "user.h"
#include "host.h"
#include "peer.h"
#include "inode.h"
#include "process.h"
#include "job.h"

struct peer {
	struct gfp_xdr *conn;

#if 0 /* We don't use id_type for now */
	enum gfarm_auth_id_type id_type;
#endif
	char *username, *hostname;
	struct user *user;
	struct host *host;
	struct process *process;
	int protocol_error;

	union {
		struct {
			/* only used by "gfrun" client */
			struct job_table_entry *jobs;
			gfarm_int32_t fd_current, fd_saved;
		} client;
	} u;
};

static struct peer *peer_table;
static int peer_table_size;

void
peer_init(int max_peers)
{
	int i;

	peer_table = malloc(max_peers * sizeof(*peer_table));
	if (peer_table == NULL)
		gflog_fatal("peer table: %s", strerror(ENOMEM));
	peer_table_size = max_peers;

	for (i = 0; i < peer_table_size; i++) {
		peer_table[i].conn = NULL;
		peer_table[i].username = NULL;
		peer_table[i].hostname = NULL;
		peer_table[i].user = NULL;
		peer_table[i].host = NULL;
		peer_table[i].process = NULL;
		peer_table[i].protocol_error = 0;
		peer_table[i].u.client.jobs = NULL;
		peer_table[i].u.client.fd_current = -1;
		peer_table[i].u.client.fd_saved = -1;
	}
}

gfarm_error_t
peer_alloc(int fd, struct peer **peerp)
{
	gfarm_error_t e;
	struct peer *peer;
	int sockopt;

	if (fd < 0)
		return (GFARM_ERR_INVALID_ARGUMENT);
	if (fd >= peer_table_size)
		return (GFARM_ERR_TOO_MANY_OPEN_FILES);
	peer = &peer_table[fd];
	if (peer->conn != NULL) /* must be an implementation error */
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	/* XXX FIXME gfp_xdr requires too much memory */
	e = gfp_xdr_new_fd(fd, &peer->conn);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	peer->username = NULL;
	peer->hostname = NULL;
	peer->user = NULL;
	peer->host = NULL;
	peer->process = NULL;
	peer->protocol_error = 0;
	peer->u.client.jobs = NULL;
	peer->u.client.fd_current = -1;
	peer->u.client.fd_saved = -1;

	/* deal with reboots or network problems */
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &sockopt, sizeof(sockopt))
	    == -1)
		gflog_warning_errno("SO_KEEPALIVE");

	return (GFARM_ERR_NO_ERROR);
}

/* caller should allocate the storage for username and hostname */
void
peer_authorized(struct peer *peer,
	enum gfarm_auth_id_type id_type, char *username, char *hostname,
	enum gfarm_auth_method auth_method)
{
#if 0 /* We don't record id_type for now */
	peer->id_type = id_type;
#endif
	if (id_type == GFARM_AUTH_ID_TYPE_USER) {
		peer->user = user_lookup(username);
		if (peer->user != NULL) {
			free(username);
			peer->username = NULL;
		} else {
			peer->username = username;
		}
	} else {
		peer->user = NULL;
		peer->username = username;
	}
	peer->host = host_lookup(hostname);
	if (peer->host != NULL) {
		free(hostname);
		peer->hostname = NULL;
	} else {
		peer->hostname = hostname;
	}
	/* We don't record auth_method for now */
}

void
peer_free(struct peer *peer)
{
	char *username = peer_get_username(peer);
	char *hostname = peer_get_hostname(peer);

	/*XXX XXX*/
	while (peer->u.client.jobs != NULL)
		job_table_remove(job_get_id(peer->u.client.jobs), username,
		    &peer->u.client.jobs);
	peer->u.client.jobs = NULL;

	gflog_notice("(%s@%s) disconnected", username, hostname);

	peer->protocol_error = 0;
	if (peer->process != NULL) {
		process_del_ref(peer->process); peer->process = NULL;
	}

	peer->user = NULL;
	peer->host = NULL;
	if (peer->username != NULL) {
		free(peer->username); peer->username = NULL;
	}
	if (peer->hostname != NULL) {
		free(peer->hostname); peer->hostname = NULL;
	}

	gfp_xdr_free(peer->conn); peer->conn = NULL;
}

struct peer *
peer_by_fd(int fd)
{
	if (fd < 0 || fd >= peer_table_size || peer_table[fd].conn == NULL)
		return (NULL);
	return (&peer_table[fd]);
}

gfarm_error_t
peer_free_by_fd(int fd)
{
	struct peer *peer = peer_by_fd(fd);

	if (peer == NULL)
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	peer_free(peer);
	return (GFARM_ERR_NO_ERROR);
}

struct gfp_xdr *
peer_get_conn(struct peer *peer)
{
	return (peer->conn);
}

#if 0
int
peer_get_fd(struct peer *peer)
{
	int fd = peer - peer_table;

	if (fd < 0 || fd >= peer_table_size)
		gflog_fatal("peer_get_fd: invalid peer pointer");
	return (fd);
}
#endif

char *
peer_get_username(struct peer *peer)
{
	return (peer->user != NULL ? user_name(peer->user) : peer->username);
}

char *
peer_get_hostname(struct peer *peer)
{
	return (peer->host != NULL ? host_name(peer->host) : peer->hostname);
}

struct user *
peer_get_user(struct peer *peer)
{
	return (peer->user);
}

void
peer_set_user(struct peer *peer, struct user *user)
{
	if (peer->user != NULL)
		gflog_fatal("peer_set_user: overriding user");
	peer->user = user;
}

struct host *
peer_get_host(struct peer *peer)
{
	return (peer->host);
}

struct process *
peer_get_process(struct peer *peer)
{
	return (peer->process);
}

void
peer_set_process(struct peer *peer, struct process *process)
{
	if (peer->process != NULL)
		gflog_fatal("peer_set_process: overriding process");
	peer->process = process;
	process_add_ref(process);
}

void
peer_record_protocol_error(struct peer *peer)
{
	peer->protocol_error = 1;
}

int
peer_had_protocol_error(struct peer *peer)
{
	return (peer->protocol_error);
}

struct job_table_entry **
peer_get_jobs_ref(struct peer *peer)
{
	return (&peer->u.client.jobs);
}

gfarm_error_t
peer_fdstack_push(struct peer *peer, gfarm_int32_t fd)
{
	gfarm_error_t e;
	struct process *process;

	if ((process = peer_get_process(peer)) == NULL)
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	else if ((e = process_verify_fd(process, fd)) != GFARM_ERR_NO_ERROR)
		;
	else {
		e = GFARM_ERR_NO_ERROR;
		peer->u.client.fd_saved = peer->u.client.fd_current;
		peer->u.client.fd_current = fd;
	}
	return (e);
}

gfarm_error_t
peer_fdstack_swap(struct peer *peer)
{
	gfarm_int32_t fd;

	fd = peer->u.client.fd_saved;
	if (fd < 0)
		return (GFARM_ERR_PROTOCOL);
	peer->u.client.fd_saved = peer->u.client.fd_current;
	peer->u.client.fd_current = fd;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
peer_fdstack_pop(struct peer *peer)
{
	if (peer->u.client.fd_current < 0)
		return (GFARM_ERR_PROTOCOL);
	peer->u.client.fd_current = peer->u.client.fd_saved;
	peer->u.client.fd_saved = -1;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
peer_fdstack_top(struct peer *peer, gfarm_int32_t *fdp)
{
	gfarm_error_t e;
	struct process *process;
	int fd;

	if ((process = peer_get_process(peer)) == NULL)
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	fd = peer->u.client.fd_current;;
	if ((e = process_verify_fd(process, fd)) != GFARM_ERR_NO_ERROR)
		return (e);
	*fdp = fd;
	return (GFARM_ERR_NO_ERROR);
}

/* XXX FIXME too many threads */
gfarm_error_t
peer_schedule(struct peer *peer, void *(*handler)(void *))
{
	int err;
	pthread_t thread_id;
	static int initialized = 0;
	static pthread_attr_t attr;

	if (!initialized) {
		err = pthread_attr_init(&attr);
		if (err != 0)
			gflog_fatal("pthread_attr_init(): %s", strerror(err));
		err = pthread_attr_setdetachstate(&attr,
		    PTHREAD_CREATE_DETACHED);
		if (err != 0)
			gflog_fatal("PTHREAD_CREATE_DETACHED: %s",
			    strerror(err));
		initialized = 1;
	}

	err = pthread_create(&thread_id, &attr, handler, peer);
	if (err != 0)
		return (gfarm_errno_to_error(err));
	return (GFARM_ERR_NO_ERROR);
}

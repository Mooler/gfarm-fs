#define _POSIX_PII_SOCKET /* to use struct msghdr on Tru64 */
#include <pthread.h>
#include <assert.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h> /* sprintf() */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <openssl/evp.h>

#include <gfarm/gfarm_config.h>
#include <gfarm/gflog.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>
#include <gfarm/gfarm_iostat.h>

#if defined(SCM_RIGHTS) && \
		(!defined(sun) || (!defined(__svr4__) && !defined(__SVR4)))
#define HAVE_MSG_CONTROL 1
#endif

#include "gfutil.h"
#include "gfevent.h"
#include "hash.h"
#include "lru_cache.h"

#include "liberror.h"
#include "sockutil.h"
#include "iobuffer.h"
#include "gfp_xdr.h"
#include "io_fd.h"
#include "host.h"
#include "param.h"
#include "sockopt.h"
#include "auth.h"
#include "config.h"
#include "conn_cache.h"
#include "gfs_proto.h"
#include "gfs_client.h"
#include "gfm_client.h"
#include "iostat.h"

#define GFS_CLIENT_COMMAND_TIMEOUT	20 /* seconds */

#define XAUTH_NEXTRACT_MAXLEN	512

struct gfs_connection {
	struct gfp_cached_connection *cache_entry;

	struct gfp_xdr *conn;
	char *hostname;
	int port;
	enum gfarm_auth_method auth_method;

	int is_local;
	gfarm_pid_t pid; /* parallel process ID */

	int opened; /* reference counter */

	void *context; /* work area for RPC (esp. GFS_PROTO_COMMAND) */

	int failover_count; /* compare to gfm_connection.failover_count */
};

#define SERVER_HASHTAB_SIZE	3079	/* prime number */

static gfarm_error_t gfs_client_connection_dispose(void *);

static struct gfp_conn_cache gfs_server_cache =
	GFP_CONN_CACHE_INITIALIZER(gfs_server_cache,
		gfs_client_connection_dispose,
		"gfs_connection",
		SERVER_HASHTAB_SIZE,
		&gfarm_gfsd_connection_cache);

static gfarm_error_t
	(*gfs_client_hook_for_connection_error)(struct gfs_connection *) =
		NULL;

/*
 * Currently this supports only one hook function.
 * And the function is always gfarm_schedule_host_cache_clear_auth() (or NULL).
 */
void
gfs_client_add_hook_for_connection_error(
	gfarm_error_t (*hook)(struct gfs_connection *))
{
	gfs_client_hook_for_connection_error = hook;
}

static void
gfs_client_execute_hook_for_connection_error(struct gfs_connection *gfs_server)
{
	if (gfs_client_hook_for_connection_error != NULL)
		(*gfs_client_hook_for_connection_error)(gfs_server);
}

int
gfs_client_is_connection_error(gfarm_error_t e)
{
	return (IS_CONNECTION_ERROR(e));
}

int
gfs_client_connection_fd(struct gfs_connection *gfs_server)
{
	return (gfp_xdr_fd(gfs_server->conn));
}

enum gfarm_auth_method
gfs_client_connection_auth_method(struct gfs_connection *gfs_server)
{
	return (gfs_server->auth_method);
}

const char *
gfs_client_hostname(struct gfs_connection *gfs_server)
{
	return (gfs_server->hostname);
}

const char *
gfs_client_username(struct gfs_connection *gfs_server)
{
	return (gfp_cached_connection_username(gfs_server->cache_entry));
}

int
gfs_client_port(struct gfs_connection *gfs_server)
{
	return (gfs_server->port);
}

int
gfs_client_connection_is_local(struct gfs_connection *gfs_server)
{
	return (gfs_server->is_local);
}

gfarm_pid_t
gfs_client_pid(struct gfs_connection *gfs_server)
{
	return (gfs_server->pid);
}

int
gfs_client_connection_failover_count(struct gfs_connection *gfs_server)
{
	return (gfs_server->failover_count);
}

void
gfs_client_connection_set_failover_count(
	struct gfs_connection *gfs_server, int count)
{
	gfs_server->failover_count = count;
}

#define gfs_client_connection_is_cached(gfs_server) \
	gfp_is_cached_connection((gfs_server)->cache_entry)

#define gfs_client_connection_used(gfs_server) \
	gfp_cached_connection_used(&gfs_server_cache, \
	    (gfs_server)->cache_entry)

void
gfs_client_purge_from_cache(struct gfs_connection *gfs_server)
{
	gfp_cached_connection_purge_from_cache(&gfs_server_cache,
	    gfs_server->cache_entry);
}

void
gfs_client_connection_gc(void)
{
	gfp_cached_connection_gc_all(&gfs_server_cache);
}

int
gfs_client_sockaddr_is_local(struct sockaddr *peer_addr)
{
	static int self_ip_asked = 0;
	static int self_ip_count = 0;
	static struct in_addr *self_ip_list;

	struct sockaddr_in *peer_in;
	int i;

	if (!gfarm_direct_local_access)
		return (0);
	if (!self_ip_asked) {
		self_ip_asked = 1;
		if (gfarm_get_ip_addresses(&self_ip_count, &self_ip_list) !=
		    GFARM_ERR_NO_ERROR) {
			/* self_ip_count remains 0 */
			return (0);
		}
	}
	if (peer_addr->sa_family != AF_INET)
		return (0);
	peer_in = (struct sockaddr_in *)peer_addr;
	/* XXX if there are lots of IP address on this host, this is slow */
	for (i = 0; i < self_ip_count; i++) {
		if (peer_in->sin_addr.s_addr == self_ip_list[i].s_addr)
			return (1);
	}
	return (0);
}

static gfarm_error_t
gfs_client_connect_unix(struct sockaddr *peer_addr, int *sockp)
{
	int rv, sock, save_errno;
	struct sockaddr_un peer_un;
	struct sockaddr_in *peer_in;
	socklen_t socklen;

	assert(peer_addr->sa_family == AF_INET);
	peer_in = (struct sockaddr_in *)peer_addr;

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock == -1 && (errno == ENFILE || errno == EMFILE)) {
		gfs_client_connection_gc(); /* XXX FIXME: GC all descriptors */
		sock = socket(PF_UNIX, SOCK_STREAM, 0);
	}
	if (sock == -1) {
		save_errno = errno;
		gflog_debug(GFARM_MSG_1001172,
			"creation of UNIX socket failed: %s",
			strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}

	memset(&peer_un, 0, sizeof(peer_un));
	/* XXX inet_ntoa() is not MT-safe on some platforms */
	socklen = snprintf(peer_un.sun_path, sizeof(peer_un.sun_path),
	    GFSD_LOCAL_SOCKET_NAME, inet_ntoa(peer_in->sin_addr),
	    ntohs(peer_in->sin_port));
	peer_un.sun_family = AF_UNIX;
#ifdef SUN_LEN /* derived from 4.4BSD */
	socklen = SUN_LEN(&peer_un);
#else
	socklen += sizeof(peer_un) - sizeof(peer_un.sun_path);
#endif
	rv = connect(sock, (struct sockaddr *)&peer_un, socklen);
	if (rv == -1) {
		save_errno = errno;
		close(sock);
		if (save_errno != ENOENT) {
			gflog_debug(GFARM_MSG_1001173,
				"connect() with UNIX socket failed: %s",
				strerror(save_errno));
			return (gfarm_errno_to_error(save_errno));
		}

		/* older gfsd doesn't support UNIX connection, try INET */
		sock = -1;
	}
	*sockp = sock;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfs_client_connect_inet(const char *canonical_hostname,
	struct sockaddr *peer_addr,
	int *connection_in_progress_p, int *sockp, const char *source_ip)
{
	int rv, sock, save_errno;
	gfarm_error_t e;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1 && (errno == ENFILE || errno == EMFILE)) {
		gfs_client_connection_gc(); /* XXX FIXME: GC all descriptors */
		sock = socket(PF_INET, SOCK_STREAM, 0);
	}
	if (sock == -1) {
		save_errno = errno;
		gflog_debug(GFARM_MSG_1001174,
			"Creation of inet socket failed: %s",
			strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}

	/* XXX - how to report setsockopt(2) failure ? */
	gfarm_sockopt_apply_by_name_addr(sock, canonical_hostname, peer_addr);

	/*
	 * this fcntl should never fail, or even if this fails, that's OK
	 * because its only effect is that TCP timeout becomes longer.
	 */
	fcntl(sock, F_SETFL, O_NONBLOCK);

	if (source_ip != NULL) {
		e = gfarm_bind_source_ip(sock, source_ip);
		if (e != GFARM_ERR_NO_ERROR) {
			close(sock);
			gflog_debug(GFARM_MSG_1001175,
				"bind() with inet socket (%s) failed: %s",
				source_ip,
				gfarm_error_string(e));
			return (e);
		}
	}

	rv = connect(sock, peer_addr, sizeof(*peer_addr));
	if (rv < 0) {
		if (errno != EINPROGRESS) {
			save_errno = errno;
			close(sock);
			gflog_debug(GFARM_MSG_1001176,
				"connect() with inet socket failed: %s",
				strerror(save_errno));
			return (gfarm_errno_to_error(save_errno));
		}
		*connection_in_progress_p = 1;
	} else {
		*connection_in_progress_p = 0;
	}
	fcntl(sock, F_SETFL, 0); /* clear O_NONBLOCK, this should never fail */
	*sockp = sock;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfs_client_connection_alloc(const char *canonical_hostname,
	struct sockaddr *peer_addr, struct gfp_cached_connection *cache_entry,
	int *connection_in_progress_p, struct gfs_connection **gfs_serverp,
	const char *source_ip)
{
	gfarm_error_t e;
	struct gfs_connection *gfs_server;
	int connection_in_progress, sock = -1, is_local = 0;

#ifdef __GNUC__ /* workaround gcc warning: may be used uninitialized */
	connection_in_progress = 0;
#endif
	if (gfs_client_sockaddr_is_local(peer_addr)) {
		e = gfs_client_connect_unix(peer_addr, &sock);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001177,
				"connect with unix socket failed: %s",
				gfarm_error_string(e));
			return (e);
		}
		connection_in_progress = 0;
		is_local = 1;
	}
	if (sock == -1) {
		e = gfs_client_connect_inet(canonical_hostname,
		    peer_addr, &connection_in_progress, &sock, source_ip);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001178,
				"connect with inet socket failed: %s",
				gfarm_error_string(e));
			return (e);
		}
	}
	fcntl(sock, F_SETFD, 1); /* automatically close() on exec(2) */

	GFARM_MALLOC(gfs_server);
	if (gfs_server == NULL) {
		close(sock);
		gflog_debug(GFARM_MSG_1001179,
			"allocation of 'gfs_server' failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	e = gfp_xdr_new_socket(sock, &gfs_server->conn);
	if (e != GFARM_ERR_NO_ERROR) {
		free(gfs_server);
		close(sock);
		gflog_debug(GFARM_MSG_1001180,
			"gfp_xdr_new_socket() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	gfs_server->hostname = strdup(canonical_hostname);
	if (gfs_server->hostname == NULL) {
		e = gfp_xdr_free(gfs_server->conn);
		free(gfs_server);
		gflog_debug(GFARM_MSG_1001181,
			"allocation of 'gfs_server->hostname' failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	gfs_server->port = ntohs(((struct sockaddr_in *)peer_addr)->sin_port);
	gfs_server->is_local = is_local;
	gfs_server->pid = 0;
	gfs_server->context = NULL;
	gfs_server->opened = 0;
	gfs_server->failover_count = 0;

	gfs_server->cache_entry = cache_entry;
	gfp_cached_connection_set_data(cache_entry, gfs_server);

	*connection_in_progress_p = connection_in_progress;
	*gfs_serverp = gfs_server;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfs_client_connection_alloc_and_auth(const char *canonical_hostname,
	const char *user, struct sockaddr *peer_addr,
	struct gfp_cached_connection *cache_entry,
	struct gfs_connection **gfs_serverp, const char *source_ip)
{
	gfarm_error_t e;
	int connection_in_progress;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_alloc(canonical_hostname, peer_addr,
	    cache_entry, &connection_in_progress, &gfs_server, source_ip);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001182,
			"gfs_client_connection_alloc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (connection_in_progress)
		e = gfarm_connect_wait(gfp_xdr_fd(gfs_server->conn),
		    gfarm_gfsd_connection_timeout);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfarm_auth_request(gfs_server->conn, GFS_SERVICE_TAG,
		    gfs_server->hostname, peer_addr, gfarm_get_auth_id_type(),
		    user, &gfs_server->auth_method, NULL);
	if (e == GFARM_ERR_NO_ERROR) {
		/*
		 * In GSI authentication, small packets are sent frequently,
		 * which requires TCP_NODELAY for reasonable performance.
		 */
		if (gfs_server->auth_method == GFARM_AUTH_METHOD_GSI) {
			gfarm_error_t e1 = gfarm_sockopt_set_option(
			    gfp_xdr_fd(gfs_server->conn), "tcp_nodelay");

			if (e1 == GFARM_ERR_NO_ERROR)
				gflog_debug(GFARM_MSG_1003373, "tcp_nodelay "
				    "is specified for performance in GSI");
			else
				gflog_debug(GFARM_MSG_1003374, "tcp_nodelay "
				    "is specified, but fails: %s",
				    gfarm_error_string(e1));
		}
		*gfs_serverp = gfs_server;
	} else {
		free(gfs_server->hostname);
		gfp_xdr_free(gfs_server->conn);
		free(gfs_server);
		gflog_debug(GFARM_MSG_1001183,
			"connection or authentication failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

static gfarm_error_t
gfs_client_connection_dispose(void *connection_data)
{
	struct gfs_connection *gfs_server = connection_data;
	gfarm_error_t e = gfp_xdr_free(gfs_server->conn);

	gfp_uncached_connection_dispose(gfs_server->cache_entry);
	free(gfs_server->hostname);
	/* XXX - gfs_server->context should be NULL here */
	free(gfs_server);
	return (e);
}

/*
 * gfs_client_connection_free() can be used for both 
 * an uncached connection which was created by gfs_client_connect(), and
 * a cached connection which was created by gfs_client_connection_acquire().
 * The connection will be immediately closed in the former uncached case.
 * 
 */
void
gfs_client_connection_free(struct gfs_connection *gfs_server)
{
	gfp_cached_or_uncached_connection_free(&gfs_server_cache,
	    gfs_server->cache_entry);
}

void
gfs_client_terminate(void)
{
	gfp_cached_connection_terminate(&gfs_server_cache);
}

/*
 * gfs_client_connection_acquire - create or lookup a cached connection
 */
gfarm_error_t
gfs_client_connection_acquire(const char *canonical_hostname, const char *user,
	struct sockaddr *peer_addr, struct gfs_connection **gfs_serverp)
{
	gfarm_error_t e;
	struct gfp_cached_connection *cache_entry;
	int created;

	e = gfp_cached_connection_acquire(&gfs_server_cache,
	    canonical_hostname,
	    ntohs(((struct sockaddr_in *)peer_addr)->sin_port),
	    user, &cache_entry, &created);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001184,
			"acquirement of cached connection (%s) failed: %s",
			canonical_hostname,
			gfarm_error_string(e));
		return (e);
	}
	if (!created) {
		*gfs_serverp = gfp_cached_connection_get_data(cache_entry);
		return (GFARM_ERR_NO_ERROR);
	}
	e = gfs_client_connection_alloc_and_auth(canonical_hostname, user,
	    peer_addr, cache_entry, gfs_serverp, NULL);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_cached_connection_purge_from_cache(&gfs_server_cache,
		    cache_entry);
		gfp_uncached_connection_dispose(cache_entry);
		gflog_debug(GFARM_MSG_1001185,
			"allocation or authentication failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

gfarm_error_t
gfs_client_connection_acquire_by_host(struct gfm_connection *gfm_server,
	const char *canonical_hostname, int port,
	struct gfs_connection **gfs_serverp, const char *source_ip)
{
	gfarm_error_t e;
	struct gfp_cached_connection *cache_entry;
	int created;
	struct sockaddr peer_addr;
	const char *user = gfm_client_username(gfm_server);

	/*
	 * lookup gfs_server_cache first,
	 * to eliminate hostname -> IP-address conversion in a cached case.
	 */
	e = gfp_cached_connection_acquire(&gfs_server_cache,
	    canonical_hostname, port, user, &cache_entry, &created);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001186,
			"acquirement of cached connection (%s) failed: %s",
			canonical_hostname,
			gfarm_error_string(e));
		return (e);
	}
	if (!created) {
		*gfs_serverp = gfp_cached_connection_get_data(cache_entry);
		return (GFARM_ERR_NO_ERROR);
	}
	e = gfm_host_address_get(gfm_server, canonical_hostname, port,
	    &peer_addr, NULL);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfs_client_connection_alloc_and_auth(canonical_hostname,
		    user, &peer_addr, cache_entry, gfs_serverp, source_ip);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_cached_connection_purge_from_cache(&gfs_server_cache,
		    cache_entry);
		gfp_uncached_connection_dispose(cache_entry);
		gflog_debug(GFARM_MSG_1001187,
			"client authentication failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

/*
 * gfs_client_connect - create an uncached connection
 *
 * XXX FIXME
 * `hostname' to `addr' conversion really should be done in this function,
 * rather than a caller of this function.
 */
gfarm_error_t
gfs_client_connect(const char *canonical_hostname, int port, const char *user,
	struct sockaddr *peer_addr, struct gfs_connection **gfs_serverp)
{
	gfarm_error_t e;
	struct gfp_cached_connection *cache_entry;

	e = gfp_uncached_connection_new(canonical_hostname, port, user,
		&cache_entry);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001188,
			"making new uncached connection failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	e = gfs_client_connection_alloc_and_auth(canonical_hostname, user,
	    peer_addr, cache_entry, gfs_serverp, NULL);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_uncached_connection_dispose(cache_entry);
		gflog_debug(GFARM_MSG_1001189,
			"client authentication failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

/* convert from uncached connection to cached */
gfarm_error_t
gfs_client_connection_enter_cache(struct gfs_connection *gfs_server)
{
	if (gfs_client_connection_is_cached(gfs_server)) {
		gflog_error(GFARM_MSG_1000068,
		    "gfs_client_connection_enter_cache: "
		    "programming error");
		abort();
	}
	return (gfp_uncached_connection_enter_cache(&gfs_server_cache,
	    gfs_server->cache_entry));
}

/*
 * multiplexed version of gfs_client_connect() for parallel authentication
 * for parallel authentication
 */

struct gfs_client_connect_state {
	struct gfarm_eventqueue *q;
	struct gfarm_event *writable;
	struct sockaddr peer_addr;
	void (*continuation)(void *);
	void *closure;

	struct gfs_connection *gfs_server;

	struct gfarm_auth_request_state *auth_state;

	/* results */
	gfarm_error_t error;
};

static void
gfs_client_connect_end_auth(void *closure)
{
	struct gfs_client_connect_state *state = closure;
	gfarm_error_t e;

	state->error = gfarm_auth_result_multiplexed(
	    state->auth_state,
	    &state->gfs_server->auth_method);
	if (state->error == GFARM_ERR_NO_ERROR) {
		/*
		 * In GSI authentication, small packets are sent frequently,
		 * which requires TCP_NODELAY for reasonable performance.
		 */
		if (state->gfs_server->auth_method == GFARM_AUTH_METHOD_GSI) {
			e = gfarm_sockopt_set_option(
			    gfp_xdr_fd(state->gfs_server->conn), "tcp_nodelay");
			if (e == GFARM_ERR_NO_ERROR)
				gflog_debug(GFARM_MSG_1003375, "tcp_nodelay "
				    "is specified for performance in GSI");
			else
				gflog_debug(GFARM_MSG_1003376, "tcp_nodelay "
				    "is specified, but fails: %s",
				    gfarm_error_string(e));
		}

	}
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

static void
gfs_client_connect_start_auth(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_connect_state *state = closure;
	int error;
	socklen_t error_size = sizeof(error);
	int rv = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size);

	if (rv == -1) { /* Solaris, see UNP by rstevens */
		state->error = gfarm_errno_to_error(errno);
	} else if (error != 0) {
		state->error = gfarm_errno_to_error(error);
	} else { /* successfully connected */
		state->error = gfarm_auth_request_multiplexed(state->q,
		    state->gfs_server->conn, GFS_SERVICE_TAG,
		    state->gfs_server->hostname,
		    &state->peer_addr,
		    gfarm_get_auth_id_type(),
		    gfs_client_username(state->gfs_server),
		    gfs_client_connect_end_auth, state,
		    &state->auth_state, NULL);
		if (state->error == GFARM_ERR_NO_ERROR) {
			/*
			 * call auth_request,
			 * then go to gfs_client_connect_end_auth()
			 */
			return;
		}
	}
	gflog_debug(GFARM_MSG_1001190,
		"starting client connection auth failed: %s",
		gfarm_error_string(state->error));
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

gfarm_error_t
gfs_client_connect_request_multiplexed(struct gfarm_eventqueue *q,
	const char *canonical_hostname, int port, const char *user,
	struct sockaddr *peer_addr,
	void (*continuation)(void *), void *closure,
	struct gfs_client_connect_state **statepp)
{
	gfarm_error_t e;
	int rv;
	struct gfp_cached_connection *cache_entry;
	struct gfs_connection *gfs_server;
	struct gfs_client_connect_state *state;
	int connection_in_progress;

	/* clone of gfs_client_connect() */

	GFARM_MALLOC(state);
	if (state == NULL) {
		gflog_debug(GFARM_MSG_1001191,
			"allocation of client connect state failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}

	e = gfp_uncached_connection_new(canonical_hostname,
	    port, user, &cache_entry);
	if (e != GFARM_ERR_NO_ERROR) {
		free(state);
		gflog_debug(GFARM_MSG_1001192,
			"making new uncached connection failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	e = gfs_client_connection_alloc(canonical_hostname, peer_addr,
	    cache_entry, &connection_in_progress, &gfs_server, NULL);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_uncached_connection_dispose(cache_entry);
		free(state);
		gflog_debug(GFARM_MSG_1001193,
			"allocation of client connection failed: %s",
			gfarm_error_string(e));
		return (e);
	}

	state->q = q;
	state->peer_addr = *peer_addr;
	state->continuation = continuation;
	state->closure = closure;
	state->gfs_server = gfs_server;
	state->auth_state = NULL;
	state->error = GFARM_ERR_NO_ERROR;
	if (connection_in_progress) {
		state->writable = gfarm_fd_event_alloc(GFARM_EVENT_WRITE,
		    gfp_xdr_fd(gfs_server->conn),
		    gfs_client_connect_start_auth, state);
		if (state->writable == NULL) {
			e = GFARM_ERR_NO_MEMORY;
		} else if ((rv = gfarm_eventqueue_add_event(q, state->writable,
		    NULL)) == 0) {
			*statepp = state;
			/* go to gfs_client_connect_start_auth() */
			return (GFARM_ERR_NO_ERROR);
		} else {
			e = gfarm_errno_to_error(rv);
			gfarm_event_free(state->writable);
		}
	} else {
		state->writable = NULL;
		e = gfarm_auth_request_multiplexed(q,
		    gfs_server->conn, GFS_SERVICE_TAG,
		    gfs_server->hostname, &state->peer_addr,
		    gfarm_get_auth_id_type(), user,
		    gfs_client_connect_end_auth, state,
		    &state->auth_state, NULL);
		if (e == GFARM_ERR_NO_ERROR) {
			*statepp = state;
			/*
			 * call gfarm_auth_request,
			 * then go to gfs_client_connect_end_auth()
			 */
			return (GFARM_ERR_NO_ERROR);
		}
	}
	free(state);
	gfs_client_connection_dispose(gfs_server);
	gflog_debug(GFARM_MSG_1001194,
		"request for multiplexed client connect failed: %s",
		gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_client_connect_result_multiplexed(
	struct gfs_client_connect_state *state,
	struct gfs_connection **gfs_serverp)
{
	gfarm_error_t e = state->error;
	struct gfs_connection *gfs_server = state->gfs_server;

	if (state->writable != NULL)
		gfarm_event_free(state->writable);
	free(state);
	if (e != GFARM_ERR_NO_ERROR) {
		gfs_client_connection_dispose(gfs_server);
		gflog_debug(GFARM_MSG_1001195,
			"error in result of multiplexed client connect: %s",
			gfarm_error_string(e));
		return (e);
	}

	*gfs_serverp = gfs_server;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * gfs_client RPC
 */

int
gfarm_fd_receive_message(int fd, void *buf, size_t size,
	int fdc, int *fdv)
{
	char *buffer = buf;
	int i, rv;
	struct iovec iov[1];
	struct msghdr msg;
#ifdef HAVE_MSG_CONTROL /* 4.3BSD Reno or later */
	struct {
		struct cmsghdr hdr;
		char data[CMSG_SPACE(sizeof(*fdv) * GFSD_MAX_PASSING_FD)
			  - sizeof(struct cmsghdr)];
	} cmsg;

	if (fdc > GFSD_MAX_PASSING_FD) {
#if 0
		fprintf(stderr, "gfarm_fd_receive_message(%s): "
			"fd count %d > %d\n", fdc, GFSD_MAX_PASSING_FD);
#endif
		return (EINVAL);
	}
#endif

	while (size > 0) {
		iov[0].iov_base = buffer;
		iov[0].iov_len = size;
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
#ifndef HAVE_MSG_CONTROL
		if (fdc > 0) {
			msg.msg_accrights = (caddr_t)fdv;
			msg.msg_accrightslen = sizeof(*fdv) * fdc;
			for (i = 0; i < fdc; i++)
				fdv[i] = -1;
		} else {
			msg.msg_accrights = NULL;
			msg.msg_accrightslen = 0;
		}
#else /* 4.3BSD Reno or later */
		if (fdc > 0) {
			msg.msg_control = (caddr_t)&cmsg.hdr;
			msg.msg_controllen = CMSG_SPACE(sizeof(*fdv) * fdc);
			memset(msg.msg_control, 0, msg.msg_controllen);
			for (i = 0; i < fdc; i++)
				((int *)CMSG_DATA(&cmsg.hdr))[i] = -1;
		} else {
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
		}
#endif
		rv = recvmsg(fd, &msg, 0);
		if (rv == -1) {
			if (errno == EINTR)
				continue;
			return (errno); /* failure */
		} else if (rv == 0) {
			return (-1); /* EOF */
		}
#ifdef HAVE_MSG_CONTROL /* 4.3BSD Reno or later */
		if (fdc > 0) {
			if (msg.msg_controllen !=
			    CMSG_SPACE(sizeof(*fdv) * fdc) ||
			    cmsg.hdr.cmsg_len !=
			    CMSG_LEN(sizeof(*fdv) * fdc) ||
			    cmsg.hdr.cmsg_level != SOL_SOCKET ||
			    cmsg.hdr.cmsg_type != SCM_RIGHTS) {
#if 0
				fprintf(stderr,
					"gfarm_fd_receive_message():"
					" descriptor not passed"
					" msg_controllen: %d (%d),"
					" cmsg_len: %d (%d),"
					" cmsg_level: %d,"
					" cmsg_type: %d\n",
					msg.msg_controllen,
					CMSG_SPACE(sizeof(*fdv) * fdc),
					cmsg.hdr.cmsg_len,
					CMSG_LEN(sizeof(*fdv) * fdc),
					cmsg.hdr.cmsg_level,
					cmsg.hdr.cmsg_type);
#endif
			}
			for (i = 0; i < fdc; i++)
				fdv[i] = ((int *)CMSG_DATA(&cmsg.hdr))[i];
		}
#endif
		fdc = 0; fdv = NULL;
		buffer += rv;
		size -= rv;
	}
	return (0); /* success */
}

gfarm_error_t
gfs_client_rpc_request(struct gfs_connection *gfs_server, int command,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrpc_request(gfs_server->conn, command, &format, &ap);
	va_end(ap);
	if (IS_CONNECTION_ERROR(e)) {
		gfs_client_execute_hook_for_connection_error(gfs_server);
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001196,
			"gfp_xdr_vrpc_request() failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

gfarm_error_t
gfs_client_rpc_result(struct gfs_connection *gfs_server, int just,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	int errcode;

	gfs_client_connection_used(gfs_server);

	e = gfp_xdr_flush(gfs_server->conn);
	if (IS_CONNECTION_ERROR(e)) {
		gfs_client_execute_hook_for_connection_error(gfs_server);
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001197,
			"gfp_xdr_flush() failed : %s",
			gfarm_error_string(e));
		return (e);
	}

	va_start(ap, format);
	e = gfp_xdr_vrpc_result(gfs_server->conn, just, 1,
	    &errcode, &format, &ap);
	va_end(ap);

	if (IS_CONNECTION_ERROR(e)) {
		gfs_client_execute_hook_for_connection_error(gfs_server);
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001198,
			"gfp_xdr_vrpc_result() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (errcode != 0) {
		/*
		 * We just use gfarm_error_t as the errcode,
		 * Note that GFARM_ERR_NO_ERROR == 0.
		 */
		gflog_debug(GFARM_MSG_1001199,
			"gfp_xdr_vrpc_result() failed errcode=%d",
			errcode);
		return (errcode);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfs_client_vrpc(struct gfs_connection *gfs_server, int just, int do_timeout,
	int command, const char *format, va_list *app)
{
	gfarm_error_t e;
	int errcode;

	gfs_client_connection_used(gfs_server);

	e = gfp_xdr_vrpc(gfs_server->conn, just, do_timeout,
	    command, &errcode, &format, app);
	if (IS_CONNECTION_ERROR(e)) {
		gfs_client_execute_hook_for_connection_error(gfs_server);
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1003561, "gfp_xdr_vrpc(%d) failed: %s",
		    command, gfarm_error_string(e));
		return (e);
	}
	if (errcode != 0) {
		/*
		 * We just use gfarm_error_t as the errcode,
		 * Note that GFARM_ERR_NO_ERROR == 0.
		 */
		gflog_debug(GFARM_MSG_1003562, "gfp_xdr_vrpc(%d) errcode=%d",
		    command, errcode);
		return (errcode);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_rpc(struct gfs_connection *gfs_server, int just,
	int command, const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = gfs_client_vrpc(gfs_server, just, 1, command, format, &ap);
	va_end(ap);
	return (e);
}

gfarm_error_t
gfs_client_rpc_notimeout(struct gfs_connection *gfs_server, int just,
	int command, const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = gfs_client_vrpc(gfs_server, just, 0, command, format, &ap);
	va_end(ap);
	return (e);
}

gfarm_error_t
gfs_client_process_set(struct gfs_connection *gfs_server,
	gfarm_int32_t type, const char *key, size_t size, gfarm_pid_t pid)
{
	gfarm_error_t e;

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_PROCESS_SET, "ibl/",
	    type, size, key, pid);
	if (e == GFARM_ERR_NO_ERROR)
		gfs_server->pid = pid;
	else
		gflog_debug(GFARM_MSG_1001202,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_client_process_reset(struct gfs_connection *gfs_server,
	gfarm_int32_t type, const char *key, size_t size, gfarm_pid_t pid)
{
	gfarm_error_t e;

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_PROCESS_RESET, "ibli/",
	    type, size, key, pid, gfs_server->failover_count);
	if (e == GFARM_ERR_NO_ERROR)
		gfs_server->pid = pid;
	else {
		gfs_server->pid = 0;
		gflog_debug(GFARM_MSG_1003377,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

gfarm_error_t
gfs_client_open(struct gfs_connection *gfs_server, gfarm_int32_t fd)
{
	gfarm_error_t e;

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_OPEN, "i/", fd);
	if (e == GFARM_ERR_NO_ERROR)
		++gfs_server->opened;
	else
		gflog_debug(GFARM_MSG_1001203,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_client_open_local(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	int *fd_ret)
{
	gfarm_error_t e;
	int rv, local_fd;
	gfarm_int8_t dummy; /* needs at least 1 byte */

	if (!gfs_server->is_local) {
		gflog_debug(GFARM_MSG_1001204,
			"gfs server is local: %s",
			gfarm_error_string(GFARM_ERR_OPERATION_NOT_SUPPORTED));
		return (GFARM_ERR_OPERATION_NOT_SUPPORTED);
	}

	/* we have to set `just' flag here */
	e = gfs_client_rpc(gfs_server, 1, GFS_PROTO_OPEN_LOCAL, "i/", fd);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001205,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}

	/* layering violation, but... */
	rv = gfarm_fd_receive_message(gfp_xdr_fd(gfs_server->conn),
	    &dummy, sizeof(dummy), 1, &local_fd);
	if (rv == -1) { /* EOF */
		gflog_debug(GFARM_MSG_1001206,
			"Unexpected EOF when receiving message: %s",
			gfarm_error_string(GFARM_ERR_UNEXPECTED_EOF));
		return (GFARM_ERR_UNEXPECTED_EOF);
	}
	if (rv != 0) {
		gflog_debug(GFARM_MSG_1001207,
			"receiving message failed: %s",
			gfarm_error_string(gfarm_errno_to_error(rv)));
		return (gfarm_errno_to_error(rv));
	}
	/* both `dummy' and `local_fd` are passed by using host byte order. */
	*fd_ret = local_fd;
	++gfs_server->opened;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_close(struct gfs_connection *gfs_server, gfarm_int32_t fd)
{
	gfarm_error_t e;

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_CLOSE, "i/", fd);
	if (e == GFARM_ERR_NO_ERROR)
		--gfs_server->opened;
	else
		gflog_debug(GFARM_MSG_1001208,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_client_pread(struct gfs_connection *gfs_server,
	gfarm_int32_t fd, void *buffer, size_t size,
	gfarm_off_t off, size_t *np)
{
	gfarm_error_t e;

	if ((e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_PREAD, "iil/b",
	    fd, (int)size, off,
	    size, np, buffer)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001209,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (*np > size) {
		gflog_debug(GFARM_MSG_1001210,
			"Protocol error in client pread (%llu)>(%llu)",
			(unsigned long long)*np, (unsigned long long)size);
		return (GFARM_ERRMSG_GFS_PROTO_PREAD_PROTOCOL);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_pwrite(struct gfs_connection *gfs_server,
	gfarm_int32_t fd, const void *buffer, size_t size,
	gfarm_off_t off,
	size_t *np)
{
	gfarm_error_t e;
	gfarm_int32_t n; /* size_t may be 64bit */

	if ((e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_PWRITE, "ibl/i",
	    fd, size, buffer, off, &n)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001211,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	*np = n;
	if (n > size) {
		gflog_debug(GFARM_MSG_1001212,
			"Protocol error in client pwrite (%llu)>(%llu)",
			(unsigned long long)*np, (unsigned long long)size);
		return (GFARM_ERRMSG_GFS_PROTO_PWRITE_PROTOCOL);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_write(struct gfs_connection *gfs_server,
	gfarm_int32_t fd, const void *buffer, size_t size,
	size_t *np, gfarm_off_t *offp, gfarm_off_t *total_sizep)
{
	gfarm_error_t e;
	gfarm_int32_t n; /* size_t may be 64bit */

	if ((e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_WRITE, "ib/ill",
	    fd, size, buffer, &n, offp, total_sizep)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1003686,
			"gfs_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	*np = n;
	assert(n <= size);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_ftruncate(struct gfs_connection *gfs_server,
	gfarm_int32_t fd, gfarm_off_t size)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_FTRUNCATE, "il/",
	    fd, size));
}

gfarm_error_t
gfs_client_fsync(struct gfs_connection *gfs_server,
	gfarm_int32_t fd, gfarm_int32_t op)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_FSYNC, "ii/",
	    fd, op));
}

gfarm_error_t
gfs_client_fstat(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	gfarm_off_t *size,
	gfarm_int64_t *atime_sec, gfarm_int32_t *atime_nsec,
	gfarm_int64_t *mtime_sec, gfarm_int32_t *mtime_nsec)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_FSTAT, "i/llili",
	    fd, size, atime_sec, atime_nsec, mtime_sec, mtime_nsec));
}

gfarm_error_t
gfs_client_cksum(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	const char *type, char *cksum, size_t size, size_t *np)
{
	gfarm_error_t e;

	if ((e = gfs_client_rpc_notimeout(gfs_server, 0, GFS_PROTO_CKSUM,
	    "is/b", fd, type, size, np, cksum)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1003724,
		    "gfs_client_cksum: %s", gfarm_error_string(e));
		return (e);
	}
	if (*np > size) {
		gflog_error(GFARM_MSG_1003725,
		    "Internal protocol error (%llu)>(%llu)",
		    (unsigned long long)*np, (unsigned long long)size);
		return (GFARM_ERR_PROTOCOL);
	}
	if (size > *np)
		cksum[*np] = '\0';
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_lock(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_LOCK, "illii/",
	    fd, start, len, type, whence));
}

gfarm_error_t
gfs_client_trylock(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_TRYLOCK, "illii/",
	    fd, start, len, type, whence));
}

gfarm_error_t
gfs_client_unlock(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_UNLOCK, "illii/",
	    fd, start, len, type, whence));
}

gfarm_error_t
gfs_client_lock_info(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence,
	gfarm_off_t *start_ret, gfarm_off_t *len_ret,
	gfarm_int32_t *type_ret, char **host_ret, gfarm_pid_t **pid_ret)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_LOCK_INFO,
	    "illii/llisl",
	    fd, start, len, type, whence,
	    start_ret, len_ret, type_ret, host_ret, pid_ret));
}

gfarm_error_t
gfs_client_replica_add_from(struct gfs_connection *gfs_server,
	char *host, gfarm_int32_t port, gfarm_int32_t fd)
{
	return (gfs_client_rpc_notimeout(gfs_server, 0,
	    GFS_PROTO_REPLICA_ADD_FROM, "sii/", host, port, fd));
}

gfarm_error_t
gfs_client_statfs(struct gfs_connection *gfs_server, char *path,
	gfarm_int32_t *bsizep,
	gfarm_off_t *blocksp, gfarm_off_t *bfreep, gfarm_off_t *bavailp,
	gfarm_off_t *filesp, gfarm_off_t *ffreep, gfarm_off_t *favailp)
{
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_STATFS, "s/illllll",
	    path, bsizep, blocksp, bfreep, bavailp, filesp, ffreep, favailp));
}

/*
 * multiplexed version of gfs_client_statfs()
 */
struct gfs_client_statfs_state {
	struct gfarm_eventqueue *q;
	struct gfarm_event *writable, *readable;
	void (*continuation)(void *);
	void *closure;
	struct gfs_connection *gfs_server;
	char *path;

	/* results */
	gfarm_error_t error;
	gfarm_int32_t bsize;
	gfarm_off_t blocks, bfree, bavail;
	gfarm_off_t files, ffree, favail;
};

static void
gfs_client_statfs_send_request(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_statfs_state *state = closure;
	int rv;
	struct timeval timeout;

	state->error = gfs_client_rpc_request(state->gfs_server,
	    GFS_PROTO_STATFS, "s", state->path);
	if (state->error == GFARM_ERR_NO_ERROR) {
		state->error = gfp_xdr_flush(state->gfs_server->conn);
		if (IS_CONNECTION_ERROR(state->error)) {
			gfs_client_execute_hook_for_connection_error(
			    state->gfs_server);
			gfs_client_purge_from_cache(state->gfs_server);
		}
		if (state->error == GFARM_ERR_NO_ERROR) {
			timeout.tv_sec = GFS_CLIENT_COMMAND_TIMEOUT;
			timeout.tv_usec = 0;
			if ((rv = gfarm_eventqueue_add_event(state->q,
			    state->readable, &timeout)) == 0) {
				/* go to gfs_client_statfs_recv_result() */
				return;
			}
			state->error = gfarm_errno_to_error(rv);
		}
	}
	gflog_debug(GFARM_MSG_1001213,
		"request for client statfs failed: %s",
		gfarm_error_string(state->error));
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

static void
gfs_client_statfs_recv_result(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_statfs_state *state = closure;

	if ((events & GFARM_EVENT_TIMEOUT) != 0) {
		assert(events == GFARM_EVENT_TIMEOUT);
		state->error = GFARM_ERR_OPERATION_TIMED_OUT;
	} else {
		assert(events == GFARM_EVENT_READ);
		state->error = gfs_client_rpc_result(state->gfs_server, 0,
		    "illllll", &state->bsize,
		    &state->blocks, &state->bfree, &state->bavail,
		    &state->files, &state->ffree, &state->favail);
	}
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

gfarm_error_t
gfs_client_statfs_request_multiplexed(struct gfarm_eventqueue *q,
	struct gfs_connection *gfs_server, char *path,
	void (*continuation)(void *), void *closure,
	struct gfs_client_statfs_state **statepp)
{
	gfarm_error_t e;
	int rv;
	struct gfs_client_statfs_state *state;

	GFARM_MALLOC(state);
	if (state == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1001214,
			"allocation of client statfs state failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		goto error_return;
	}

	state->q = q;
	state->continuation = continuation;
	state->closure = closure;
	state->gfs_server = gfs_server;
	state->path = path;
	state->error = GFARM_ERR_NO_ERROR;
	state->writable = gfarm_fd_event_alloc(GFARM_EVENT_WRITE,
	    gfs_client_connection_fd(gfs_server),
	    gfs_client_statfs_send_request, state);
	if (state->writable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1001215,
			"allocation of state->writable failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		goto error_free_state;
	}
	/*
	 * We cannot use two independent events (i.e. a fd_event with
	 * GFARM_EVENT_READ flag and a timer_event) here, because
	 * it's possible that both event handlers are called at once.
	 */
	state->readable = gfarm_fd_event_alloc(
	    GFARM_EVENT_READ|GFARM_EVENT_TIMEOUT,
	    gfs_client_connection_fd(gfs_server),
	    gfs_client_statfs_recv_result, state);
	if (state->readable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1001216,
			"allocation of state->readable failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		goto error_free_writable;
	}
	/* go to gfs_client_statfs_send_request() */
	rv = gfarm_eventqueue_add_event(q, state->writable, NULL);
	if (rv != 0) {
		e = gfarm_errno_to_error(rv);
		gflog_debug(GFARM_MSG_1001217,
			"adding event to event queue failed: %s",
			gfarm_error_string(e));
		goto error_free_readable;
	}
	*statepp = state;
	return (GFARM_ERR_NO_ERROR);
		
error_free_readable:
	gfarm_event_free(state->readable);
error_free_writable:
	gfarm_event_free(state->writable);
error_free_state:
	free(state);
error_return:
	return (e);
}

gfarm_error_t
gfs_client_statfs_result_multiplexed(struct gfs_client_statfs_state *state,
	gfarm_int32_t *bsizep,
	gfarm_off_t *blocksp, gfarm_off_t *bfreep, gfarm_off_t *bavailp,
	gfarm_off_t *filesp, gfarm_off_t *ffreep, gfarm_off_t *favailp)
{
	gfarm_error_t e = state->error;

	gfarm_event_free(state->writable);
	gfarm_event_free(state->readable);
	if (e == GFARM_ERR_NO_ERROR) {
		*bsizep = state->bsize;
		*blocksp = state->blocks;
		*bfreep = state->bfree;
		*bavailp = state->bavail;
		*filesp = state->files;
		*ffreep = state->ffree;
		*favailp = state->favail;
	}
	free(state);
	return (e);
}


/*
 * GFS_PROTO_REPLICA_RECV is only used by gfsd,
 * but defined here for better maintainability.
 */

gfarm_error_t
gfs_client_replica_recv(struct gfs_connection *gfs_server,
	gfarm_ino_t ino, gfarm_uint64_t gen, gfarm_int32_t local_fd)
{
	gfarm_error_t e, e_write = GFARM_ERR_NO_ERROR, e_rpc;
	int i, rv, eof;
	char buffer[GFS_PROTO_MAX_IOSIZE];

	e = gfs_client_rpc_request(gfs_server, GFS_PROTO_REPLICA_RECV, "ll",
	    ino, gen);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfp_xdr_flush(gfs_server->conn);
	if (IS_CONNECTION_ERROR(e)) {
		gfs_client_execute_hook_for_connection_error(gfs_server);
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001218,
			"gfs_request_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}

	for (;;) {
		gfarm_int32_t size;
		int skip = 0;

		/* XXX - FIXME layering violation */
		e = gfp_xdr_recv(gfs_server->conn, 0, &eof, "i", &size);
		if (IS_CONNECTION_ERROR(e)) {
			gfs_client_execute_hook_for_connection_error(
			    gfs_server);
			gfs_client_purge_from_cache(gfs_server);
		}
		if (e != GFARM_ERR_NO_ERROR)
			break;
		if (eof) {
			e = GFARM_ERR_PROTOCOL;
			break;
		}
		if (size <= 0)
			break;
		do {
			/* XXX - FIXME layering violation */
			int partial;

			e = gfp_xdr_recv_partial(gfs_server->conn, 0,
				buffer, size, &partial);
			if (e == GFARM_ERR_NO_ERROR) {
				/* OK */
			} else if (IS_CONNECTION_ERROR(e)) {
				gfs_client_execute_hook_for_connection_error(
				    gfs_server);
				gfs_client_purge_from_cache(gfs_server);
				break;
			} else {
				break;
			}
			if (partial <= 0) {
				e = GFARM_ERR_PROTOCOL;
				break;
			}
			size -= partial;
#ifdef __GNUC__ /* shut up stupid warning by gcc */
			rv = 0;
#endif
			i = 0;
			if (skip) /* write(2) returns error */
				i = partial;
			for (; i < partial; i += rv) {
				rv = write(local_fd, buffer + i, partial - i);
				if (rv <= 0)
					break;
				gfarm_iostat_local_add(GFARM_IOSTAT_IO_WCOUNT,
						1);
				gfarm_iostat_local_add(GFARM_IOSTAT_IO_WBYTES,
						rv);
			}
			if (i < partial) {
				/*
				 * write(2) never returns 0,
				 * so the following rv == 0 case is
				 * just warm fuzzy.
				 */
				e_write = gfarm_errno_to_error(
						rv == 0 ? ENOSPC : errno);
				/*
				 * we should receive rest of data,
				 * even if write(2) fails.
				 */
				skip = 1;
			}
		} while (size > 0);
		if (e != GFARM_ERR_NO_ERROR)
			break;
	}
	e_rpc = gfs_client_rpc_result(gfs_server, 0, "");
	if (e == GFARM_ERR_NO_ERROR)
		e = e_write;
	if (e == GFARM_ERR_NO_ERROR)
		e = e_rpc;
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001219,
			"receiving client replica failed: %s",
			gfarm_error_string(e));
	}
	return (e);
}

/*
 **********************************************************************
 * gfsd datagram service
 **********************************************************************
 */

int gfs_client_datagram_timeouts[] = { /* milli seconds */
	8000, 12000
};
int gfs_client_datagram_ntimeouts =
	GFARM_ARRAY_LENGTH(gfs_client_datagram_timeouts);

/*
 * `server_addr_size' should be socklen_t, but that requires <sys/socket.h>
 * for all source files which include "gfs_client.h".
 */
gfarm_error_t
gfs_client_get_load_request(int sock,
	struct sockaddr *server_addr, int server_addr_size)
{
	int rv, command = 0;

	if (server_addr == NULL || server_addr_size == 0) {
		/* using connected UDP socket */
		rv = write(sock, &command, sizeof(command));
	} else {
		rv = sendto(sock, &command, sizeof(command), 0,
		    server_addr, server_addr_size);
	}
	if (rv == -1) {
		int save_errno = errno;
		gflog_debug(GFARM_MSG_1001247,
			"write or send operation on socket failed: %s",
			strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}
	return (GFARM_ERR_NO_ERROR);
}

/*
 * `*server_addr_sizep' is an IN/OUT parameter.
 *
 * `*server_addr_sizep' should be socklen_t, but that requires <sys/socket.h>
 * for all source files which include "gfs_client.h".
 */
gfarm_error_t
gfs_client_get_load_result(int sock,
	struct sockaddr *server_addr, socklen_t *server_addr_sizep,
	struct gfs_client_load *result)
{
	int rv;
	double loadavg[3];
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nloadavg[3];
#else
#	define nloadavg loadavg
#endif

	if (server_addr == NULL || server_addr_sizep == NULL) {
		/* caller doesn't need server_addr */
		rv = read(sock, nloadavg, sizeof(nloadavg));
	} else {
		rv = recvfrom(sock, nloadavg, sizeof(nloadavg), 0,
		    server_addr, server_addr_sizep);
	}
	if (rv == -1) {
		int save_errno = errno;
		gflog_debug(GFARM_MSG_1001248,
			"read or receive operation from socket failed: %s",
			strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}

#ifndef WORDS_BIGENDIAN
	swab(&nloadavg[0], &loadavg[0], sizeof(loadavg[0]));
	swab(&nloadavg[1], &loadavg[1], sizeof(loadavg[1]));
	swab(&nloadavg[2], &loadavg[2], sizeof(loadavg[2]));
#endif
	result->loadavg_1min = loadavg[0];
	result->loadavg_5min = loadavg[1];
	result->loadavg_15min = loadavg[2];
	return (GFARM_ERR_NO_ERROR);
}

/*
 * multiplexed version of gfs_client_get_load()
 */

struct gfs_client_get_load_state {
	struct gfarm_eventqueue *q;
	struct gfarm_event *writable, *readable;
	void (*continuation)(void *);
	void *closure;

	int sock;
	int try;

	/* results */
	gfarm_error_t error;
	struct gfs_client_load load;
};

static void
gfs_client_get_load_send(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_get_load_state *state = closure;
	int rv;
	struct timeval timeout;

	state->error = gfs_client_get_load_request(state->sock, NULL, 0);
	if (state->error == GFARM_ERR_NO_ERROR) {
		timeout.tv_sec = timeout.tv_usec = 0;
		gfarm_timeval_add_microsec(&timeout,
		    gfs_client_datagram_timeouts[state->try] *
		    GFARM_MILLISEC_BY_MICROSEC);
		if ((rv = gfarm_eventqueue_add_event(state->q, state->readable,
		    &timeout)) == 0) {
			/* go to gfs_client_get_load_receive() */
			return;
		}
		state->error = gfarm_errno_to_error(rv);
	}
	close(state->sock);
	state->sock = -1;
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

static void
gfs_client_get_load_receive(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_get_load_state *state = closure;
	int rv;

	if ((events & GFARM_EVENT_TIMEOUT) != 0) {
		assert(events == GFARM_EVENT_TIMEOUT);
		++state->try;
		if (state->try >= gfs_client_datagram_ntimeouts) {
			state->error = GFARM_ERR_OPERATION_TIMED_OUT;
		} else if ((rv = gfarm_eventqueue_add_event(state->q,
		    state->writable, NULL)) == 0) {
			/* go to gfs_client_get_load_send() */
			return;
		} else {
			state->error = gfarm_errno_to_error(rv);
		}
	} else {
		assert(events == GFARM_EVENT_READ);
		state->error = gfs_client_get_load_result(state->sock,
		    NULL, NULL, &state->load);
	}
	close(state->sock);
	state->sock = -1;
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

gfarm_error_t
gfs_client_get_load_request_multiplexed(struct gfarm_eventqueue *q,
	struct sockaddr *peer_addr,
	void (*continuation)(void *), void *closure,
	struct gfs_client_get_load_state **statepp)
{
	gfarm_error_t e;
	int rv, sock;
	struct gfs_client_get_load_state *state;

	/* use different socket for each peer, to identify error code */
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		e = gfarm_errno_to_error(errno);
		goto error_return;
	}
	fcntl(sock, F_SETFD, 1); /* automatically close() on exec(2) */
	/* connect UDP socket, to get error code */
	if (connect(sock, peer_addr, sizeof(*peer_addr)) == -1) {
		e = gfarm_errno_to_error(errno);
		goto error_close_sock;
	}

	GFARM_MALLOC(state);
	if (state == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1001249,
			"allocation of client get load state failed: %s",
			gfarm_error_string(e));
		goto error_close_sock;
	}

	state->writable = gfarm_fd_event_alloc(
	    GFARM_EVENT_WRITE, sock,
	    gfs_client_get_load_send, state);
	if (state->writable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1001250,
			"allocation of client get load state->writable "
			"failed: %s",
			gfarm_error_string(e));
		goto error_free_state;
	}
	/*
	 * We cannot use two independent events (i.e. a fd_event with
	 * GFARM_EVENT_READ flag and a timer_event) here, because
	 * it's possible that both event handlers are called at once.
	 */
	state->readable = gfarm_fd_event_alloc(
	    GFARM_EVENT_READ|GFARM_EVENT_TIMEOUT, sock,
	    gfs_client_get_load_receive, state);
	if (state->readable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1001251,
			"allocation of client get load state->readable "
			"failed: %s",
			gfarm_error_string(e));
		goto error_free_writable;
	}
	/* go to gfs_client_get_load_send() */
	rv = gfarm_eventqueue_add_event(q, state->writable, NULL);
	if (rv != 0) {
		e = gfarm_errno_to_error(rv);
		gflog_debug(GFARM_MSG_1001252,
			"adding event to event queue failed: %s",
			gfarm_error_string(e));
		goto error_free_readable;
	}

	state->q = q;
	state->continuation = continuation;
	state->closure = closure;
	state->sock = sock;
	state->try = 0;
	state->error = GFARM_ERR_NO_ERROR;
	*statepp = state;
	return (GFARM_ERR_NO_ERROR);

error_free_readable:
	gfarm_event_free(state->readable);
error_free_writable:
	gfarm_event_free(state->writable);
error_free_state:
	free(state);
error_close_sock:
	close(sock);
error_return:
	return (e);
}

gfarm_error_t
gfs_client_get_load_result_multiplexed(
	struct gfs_client_get_load_state *state, struct gfs_client_load *loadp)
{
	gfarm_error_t error = state->error;

	if (state->sock >= 0) { /* sanity */
		close(state->sock);
		state->sock = -1;
	}
	if (error == GFARM_ERR_NO_ERROR)
		*loadp = state->load;
	gfarm_event_free(state->readable);
	gfarm_event_free(state->writable);
	free(state);
	return (error);
}

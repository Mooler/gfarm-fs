struct peer;

void peer_init(int);

gfarm_error_t peer_alloc(int, struct peer **);
void peer_authorized(struct peer *,
	enum gfarm_auth_id_type, char *, char *, enum gfarm_auth_method);
void peer_free(struct peer *);

struct peer *peer_by_fd(int);
gfarm_error_t peer_free_by_fd(int);

struct gfp_xdr *peer_get_conn(struct peer *);
int peer_get_fd(struct peer *);
char *peer_get_username(struct peer *);
char *peer_get_hostname(struct peer *);

struct user;
struct user *peer_get_user(struct peer *);
void peer_set_user(struct peer *, struct user *);
struct host;
struct host *peer_get_host(struct peer *);
struct process;
struct process *peer_get_process(struct peer *);
void peer_set_process(struct peer *, struct process *);
void peer_unset_process(struct peer *);

void peer_record_protocol_error(struct peer *);
int peer_had_protocol_error(struct peer *);

/* XXX */
struct job_table_entry;
struct job_table_entry **peer_get_jobs_ref(struct peer *);

gfarm_error_t peer_fdstack_push(struct peer *peer, gfarm_int32_t fd);
gfarm_error_t peer_fdstack_swap(struct peer *peer);
gfarm_error_t peer_fdstack_pop(struct peer *peer);
gfarm_error_t peer_fdstack_top(struct peer *peer, gfarm_int32_t *);
gfarm_error_t peer_fdstack_next(struct peer *peer, gfarm_int32_t *);

gfarm_error_t peer_schedule(struct peer *, void *(*)(void *));

#ifndef GFMD_DEFAULT_PORT
#define GFMD_DEFAULT_PORT	601
#endif

enum gfm_proto_command {
	GFJ_PROTO_LOCK_REGISTER,
	GFJ_PROTO_UNLOCK_REGISTER,
	GFJ_PROTO_REGISTER,
	GFJ_PROTO_UNREGISTER,
	GFJ_PROTO_REGISTER_NODE,
	GFJ_PROTO_LIST,
	GFJ_PROTO_INFO,
	GFJ_PROTO_HOSTINFO
};

enum gfm_proto_error {
	GFM_ERROR_NOERROR,
	GFM_ERROR_NO_MEMORY,
	GFM_ERROR_NO_SUCH_OBJECT,

	GFJ_ERROR_TOO_MANY_JOBS
};

#define GFJ_ERROR_NOERROR		GFM_ERROR_NOERROR
#define GFJ_ERROR_NO_MEMORY	 	GFM_ERROR_NO_MEMORY
#define GFJ_ERROR_NO_SUCH_OBJECT	GFM_ERROR_NO_SUCH_OBJECT

#if 0 /* There isn't gfm_proto.c for now. */
extern char GFM_SERVICE_TAG[];
#else
#define GFM_SERVICE_TAG "gfarm-metadata"
#endif

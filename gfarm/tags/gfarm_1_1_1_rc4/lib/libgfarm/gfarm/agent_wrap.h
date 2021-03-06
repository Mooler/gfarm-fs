/*
 * $Id$
 */

void gfarm_agent_disable(void);

/* for direct access without agent */

char *gfarm_i_path_info_get(const char *, struct gfarm_path_info *);
char *gfarm_i_path_info_set(char *, struct gfarm_path_info *);
char *gfarm_i_path_info_replace(char *,	struct gfarm_path_info *);
char *gfarm_i_path_info_remove(const char *);
char *gfs_i_realpath_canonical(const char *, char **);
char *gfs_i_get_ino(const char *, long *);
char *gfs_i_opendir(const char *, GFS_Dir *);
char *gfs_i_readdir(GFS_Dir, struct gfs_dirent **);
char *gfs_i_closedir(GFS_Dir);
char *gfs_i_dirname(GFS_Dir);
void gfs_i_uncachedir(void);

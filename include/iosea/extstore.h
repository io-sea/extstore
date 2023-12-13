/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) CEA, 2016
 * Author: Philippe Deniel  philippe.deniel@cea.fr
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/* extstore.h
 * KVSNS/extstore: header file for external storage interface
 */

#ifndef _POSIX_STORE_H
#define _POSIX_STORE_H

#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <ini_config.h>
#include <iosea/kvsal.h>

#define DATALEN 256
#define UUID_LEN 37   // 36 + 1 for the '\0'

typedef struct extstore_id {
	unsigned int len;
	char	 data[DATALEN];
	char     uuid[UUID_LEN];
} extstore_id_t;

int extstore_init(struct collection_item *cfg_items,
		  struct kvsal_ops *kvsalops);
int extstore_create(extstore_id_t eid);
int extstore_new_objectid(extstore_id_t *eid,
			  unsigned int seedlen,
			  char *seed);
int extstore_read(extstore_id_t *eid,
		  off_t offset,
		  size_t buffer_size,
		  void *buffer,
		  bool *end_of_file,
		  struct stat *stat);
int extstore_write(extstore_id_t *eid,
		   off_t offset,
		   size_t buffer_size,
		   void *buffer,
		   bool *fsal_stable,
		   struct stat *stat);
int extstore_del(extstore_id_t *eid);
int extstore_truncate(extstore_id_t *eid,
		      off_t filesize,
		      bool running_attach,
		      struct stat *stat);
int extstore_attach(extstore_id_t *eid);
int extstore_getattr(extstore_id_t *eid,
		     struct stat *stat);

//struct extstore_id eid_null = { .len = 0, .data = NULL};

/* Pseudo HSM */
int extstore_archive(extstore_id_t *eid);
int extstore_restore(extstore_id_t *eid);
int extstore_release(extstore_id_t *eid);
int extstore_state(extstore_id_t *eid, char *state);

/* Bulk CP */
int extstore_cp_to(int fd,
		   extstore_id_t *eid,
		   int iolen,
		   size_t filesize);
int extstore_cp_from(int fd,
		     extstore_id_t *eid,
		     int iolen,
		     size_t filesize);

struct extstore_ops {
	int (*init)(struct collection_item *cfg_items,
		    struct kvsal_ops *kvsalops);
	int (*create)(extstore_id_t eid);
	int (*new_objectid)(extstore_id_t *eid,
			    unsigned int seedlen,
			    char *seed);
	int (*read)(extstore_id_t *eid,
		    off_t offset,
		    size_t buffer_size,
		    void *buffer,
		    bool *end_of_file,
		    struct stat *stat);
	int (*write)(extstore_id_t *eid,
		     off_t offset,
		     size_t buffer_size,
		     void *buffer,
		     bool *fsal_stable,
		     struct stat *stat);
	int (*del)(extstore_id_t *eid);
	int (*truncate)(extstore_id_t *eid,
			off_t filesize,
			bool running_attach,
			struct stat *stat);
	int (*attach)(extstore_id_t *eid);
	int (*getattr)(extstore_id_t *eid,
		       struct stat *stat);
	int (*archive)(extstore_id_t *eid);
	int (*restore)(extstore_id_t *eid);
	int (*release)(extstore_id_t *eid);
	int (*state)(extstore_id_t *eid,
		     char *state);
	int (*cp_to)(int fd,
		     extstore_id_t *eid,
		     int iolen,
		     size_t filesize);
	int (*cp_from)(int fd,
		       extstore_id_t *eid,
		       int iolen,
		       size_t filesize);
};

typedef int build_extstore_path_func(extstore_id_t eid,
				     char *extstore_path,
				     size_t pathlen);

/* Objstore */

/**
 * objstore_init: performs the initialisation
 *
 * XXX: should probably remove @param bespf
 *
 * @param [IN] cfg_items: configuration, managed by init_config
 * @param [IN] kvsalops: operation vector for kvsal operations
 * @param [IN] bespf: build extstore path function
 *
 * @return: 0 on success, negative value on error.
 */
int objstore_init(struct collection_item *cfg_items,
		  struct kvsal_ops *kvsalops,
		  build_extstore_path_func *bespf);

/**
 * objstore_put: puts a file to the object store
 *
 * @param [IN] path: path of a POSIX file to be copied to the object store
 * @param [IN] eid: extstore ID for the object
 *
 * @return: 0 on success, negative value on error.
 */
int objstore_put(char *path, extstore_id_t *eid);

/**
 * objstore_get: gets a file from the object store
 *
 * @param [IN] path: path of a POSIX file to be read from the object store
 * @param [IN] eid: extstore ID for the object
 *
 * @return: 0 on success, negative value on error.
 */
int objstore_get(char *path, extstore_id_t *eid);

/**
 * objstore_del: deletes an entry in the object store
 *
 * @param [IN] eid: extstore ID for the object
 *
 * @return: 0 on success, negative value on error.
 */
int objstore_del(extstore_id_t *eid);

struct objstore_ops {
	int (*init)(struct collection_item *cfg_items,
		    struct kvsal_ops *kvsalops,
		    build_extstore_path_func *bespf);
	int (*put)(char *path, extstore_id_t *eid);
	int (*get)(char *path, extstore_id_t *eid);
	int (*del)(extstore_id_t *eid);
};

/* Ganesha Request Handler communication */
enum grh_request_type {
	GRH_PUT = 0,
	GRH_GET = 1,
	GRH_DELETE = 2,

	GRH_FIRST = GRH_PUT,
	GRH_LAST = GRH_DELETE,
};

int handle_request(char *grh_url, const char **uuids, const char **paths,
		   const char **backends, enum grh_request_type *types,
		   int *errors, size_t n_paths);

int handle_request_wait(char *grh_url, const char **uuids, const char **paths,
			const char **backends, enum grh_request_type *types,
			int *errors, size_t n_paths,
			const struct timeval *timeout);

#endif

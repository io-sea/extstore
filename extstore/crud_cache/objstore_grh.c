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

/* extstore.c
 * KVSNS: implement a dummy object store inside a POSIX directory
 */

#include <sys/time.h>   /* for gettimeofday */
#include <iosea/extstore.h>

#include "extstore_crud_cache_comm.h"

#define RC_WRAP(__function, ...) ({\
	int __rc = __function(__VA_ARGS__);\
	if (__rc != 0)	\
		return __rc; })

#define RC_WRAP_LABEL(__rc, __label, __function, ...) ({\
	__rc = __function(__VA_ARGS__);\
	if (__rc != 0)	\
		goto __label; })


#define URL_LEN 512

static char grh_url[URL_LEN];

static struct collection_item *conf = NULL;
struct kvsal_ops kvsal;
struct objstore_ops objstore;

build_extstore_path_func *build_extstore_path;

int objstore_put(char *path, extstore_id_t *eid)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	enum grh_request_type type = GRH_PUT;
	char *backend = "hestia";
	char *uuid = eid->uuid;
	char *storepath;
	char k[KLEN];

	if (!eid)
		return -EINVAL;

	storepath = malloc(MAXPATHLEN);
	if (!storepath)
		return -ENOMEM;

	RC_WRAP(build_extstore_path, *eid, storepath, MAXPATHLEN);

	snprintf(k, KLEN, "%s.data_obj", eid->data);
	RC_WRAP(kvsal.set_char, k, storepath);

	RC_WRAP(handle_request_wait, grh_url, (const char **)&uuid,
				     (const char **)&storepath,
				     (const char **)&backend, &type, NULL, 1,
				     &timeout);

	return 0;
}

int objstore_get(char *path, extstore_id_t *eid)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	enum grh_request_type type = GRH_GET;
	char *backend = "hestia";
	char *uuid = eid->uuid;
	char *storepath;
	char k[KLEN];

	if (!eid)
		return -EINVAL;

	storepath = malloc(MAXPATHLEN);
	if (!storepath)
		return -ENOMEM;

	snprintf(k, KLEN, "%s.data_obj", eid->data);
	RC_WRAP(kvsal.get_char, k, storepath);

	RC_WRAP(handle_request_wait, grh_url, (const char **)&uuid,
				     (const char **)&storepath,
				     (const char **)&backend, &type, NULL, 1,
				     &timeout);

	return 0;
}

int objstore_del(extstore_id_t *eid)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
	enum grh_request_type type = GRH_DELETE;
	char *backend = "hestia";
	char *uuid = eid->uuid;
	char *storepath;
	char k[KLEN];

	if (!eid)
		return -EINVAL;

	storepath = malloc(MAXPATHLEN);
	if (!storepath)
		return -ENOMEM;

	snprintf(k, KLEN, "%s.data_obj", eid->data);

	if (kvsal.exists(k) != -ENOENT) {
		RC_WRAP(kvsal.get_char, k, storepath);
		RC_WRAP(kvsal.del, k);

		RC_WRAP(handle_request_wait, grh_url, (const char **)&uuid,
					     (const char **)&storepath,
					     (const char **)&backend, &type,
					     NULL, 1, &timeout);
	}

	return 0;
}

int objstore_init(struct collection_item *cfg_items,
		  struct kvsal_ops *kvsalops,
		  build_extstore_path_func *bespf)
{
	struct collection_item *item;

	build_extstore_path = bespf;

	if (cfg_items != NULL)
		conf = cfg_items;

	memcpy(&kvsal, kvsalops, sizeof(struct kvsal_ops));

	item = NULL;
	RC_WRAP(get_config_item, "objstore_grh", "grh_url",
				 cfg_items, &item);
	if (item == NULL)
		return -EINVAL;
	else
		strncpy(grh_url, get_string_config_value(item, NULL),
			URL_LEN);

	return 0;
}


#ifndef _EXTSTORE_CRUD_CACHE_COMM_H
#define _EXTSTORE_CRUD_CACHE_COMM_H

#include <curl/curl.h>
#include <gmodule.h>
#include <jansson.h>
#include <sys/time.h>

#include <gmodule.h>
#include <stdbool.h>

#include <iosea/extstore.h>

#define GRH_REQUEST_ID_LEN 64
#define MAX_GRH_REQUEST_PER_REQUEST 100

struct grh_request {
    char uuid[UUID_LEN];
    char file_id[1024];
    char backend[16];
    enum grh_request_type type;
    char grh_request_id[1024];
    int error;
    bool still_running;
};

struct grh {
    struct timeval eta;
    char *url;
};

enum request_type {         /* When parsing response : */
    ASYNC_GRH_REQUEST = 0,  /* - ignore returned id and eta */
    SYNC_GRH_REQUEST = 1,   /* - get back id and eta */
    STATUS = 2,             /* - get back eta */
};

struct request_context {
    CURL *curl;
    int rc; /* 0 on success, and -1 * posix error code on error (first error) */
    enum request_type type;
    unsigned int eta_delta;
};

int prepare_curl_handle(CURL **curl, struct curl_slist **headers);

void free_grh(struct grh *grh);

int json_array_append_grh_request(const struct grh_request *request,
                                  json_t *rq_array);

int json_array_append_status_request(const struct grh_request *request,
                                     json_t *rq_array);

int request2json(const struct grh_request *requests, size_t count,
                 const struct request_context *request_context,
                 char **req_body);

int parse_response_one_elt(json_t *rq_resp_elt, struct grh_request *request,
                           struct request_context *request_context);

int parse_response(json_t *response, struct grh_request *requests, size_t count,
                   struct request_context *request_context);

void do_request_or_status(struct request_context *request_context,
                          const char *grh_base_url,
                          struct grh_request *requests, size_t count);

void grh_request_setter(const char *uuid, const char *path, const char *backend,
                        enum grh_request_type type, int error,
                        struct grh_request *request);

int prepare_request_one_entry(char *grh_url, const char *uuid, const char *path,
                              const char *backend, enum grh_request_type type,
                              int error, GHashTable *requests);

int prepare_request(char *grh_url, const char * const *uuids,
                    const char * const *paths, const char * const *backends,
                    enum grh_request_type *types, int *errors,
                    GHashTable *requests, size_t n_paths);

int compute_eta(unsigned int eta_delta_msec, struct timeval *eta);

gboolean request_or_status_one_url(struct grh *grh, GArray *requests,
                                   struct request_context *request_context);

guint hash_url(const struct grh *grh);

gboolean url_equal(const struct grh *grh1, const struct grh *grh2);

int check_timeout_is_reached(const struct timeval *start_time,
                             const struct timeval *timeout);

void set_all_requests_error(struct grh *grh, GArray *requests, int *error_code);

gboolean status_if_eta_one_url(struct grh *grh, GArray *requests,
                               struct request_context *request_context);

#endif /* _EXTSTORE_CRUD_CACHE_COMM_H */

/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:softtabstop=4:
 */

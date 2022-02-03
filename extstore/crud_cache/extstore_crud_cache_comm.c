/**
 * Implementation of the grh_request that performs a remote asynchronous
 * request by asking a data grh server to do it.
 */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "extstore_crud_cache_comm.h"

#define USER_AGENT "libcurl-agent/extstore"
#define GOTO(_label, _stmt) \
    do {                    \
        _stmt;              \
        goto _label;        \
    } while (0)

/**
 * Libcurl should not be initialized multiple times, however there is no way to
 * check if other libraries will initialize it. According to a libcurl
 * developer, the best thing to do is to initialize it in each lib that uses it.
 *
 * See: https://stackoverflow.com/a/22857324
 */
__attribute__((constructor))
static void init_libcurl(void)
{
    curl_global_init(CURL_GLOBAL_ALL);
}

__attribute__((destructor))
static void cleanup_libcurl(void)
{
    curl_global_cleanup();
}

/**
 * Allocate, prepare and return a new curl handle with its http headers
 *
 * The returned curl handle must be clean by calling curl_easy_cleanup.
 * The returned http headers list must be clean by calling curl_slist_free_all.
 *
 * @param[out] curl   the new curl handle on success, irrelevant on failure
 * @param[out] header the new header on success, irrelevant on failure
 *
 * @return     0 if success, -1 * ENOMEM
 */
int prepare_curl_handle(CURL **curl, struct curl_slist **headers)
{
    /* Prepare the curl handle */
    *curl = curl_easy_init();
    if (!*curl)
        return -ENOMEM;

    /* TODO: auth, see https://curl.haxx.se/libcurl/c/anyauthput.html */
    curl_easy_setopt(*curl, CURLOPT_USERAGENT, USER_AGENT);
    *headers = curl_slist_append(*headers, "Content-Type: application/json");
    if (!*headers) {
        curl_easy_cleanup(*curl);
        return -ENOMEM;
    }

    curl_easy_setopt(*curl, CURLOPT_HTTPHEADER, *headers);

    return 0;
}

/*
 * Alloc and fill the new grh with \a grh_url and a zero eta
 *
 * @param[in]   grh_url   url of the grh
 * @param[out]  grh       newly allocated and filled grh, irrelevant if an
 *                        error is returned
 *
 * @return 0 on success, and -1 * posix error code on error
 */
static int alloc_grh(char *grh_url, struct grh **grh)
{
    int rc = 0;

    /* alloc, with a zero eta */
    *grh = calloc(1, sizeof(struct grh));
    if (!*grh)
        return -errno;

    /* copy grh url */
    (*grh)->url = strdup(grh_url);
    if (!(*grh)->url) {
        rc = -errno;
        free(*grh);
    }

    return rc;
}

/*
 * Free the url of the grh and the grh
 *
 * @param[in] grh grh to free
 */
void free_grh(struct grh *grh)
{
    if (grh) {
        free(grh->url);
        free(grh);
    }
}

static const char * const REQUEST_TYPE_TO_STR[] = {
    [GRH_PUT] = "put",
    [GRH_GET] = "get",
    [GRH_DELETE] = "delete",
};

static bool valid_types(enum grh_request_type *types, int n_types)
{
    int i;

    for (i = 0; i < n_types; ++i) {
        if (types[i] < GRH_FIRST || types[i] > GRH_LAST) {
            fprintf(stderr, "Request type '%d' (number '%d') is invalid\n",
                    types[i], i);
            return false;
        }
    }

    return true;
}

/**
 * Serialize one grh_request to a json array
 *
 * @param[in]       request   a pointer to a struct grh_request
 * @param[in/out]   rq_array  json array to which the new element is appended
 *
 * @return 0 on success, and -1 * posix error code on error
 */
int json_array_append_grh_request(const struct grh_request *request,
                                  json_t *rq_array)
{
    json_t *rq = NULL;
    int rc = 0;

    /* Build the element json */
    rq = json_pack("{s: s, s: s, s: s}",
                   "file_id", request->file_id,
                   "backend", request->backend,
                   "action", REQUEST_TYPE_TO_STR[request->type]);
    if (!rq)
        GOTO(out, rc = -ENOMEM);

    /* And append it to the request array */
    if (json_array_append(rq_array, rq) == -1)
        GOTO(out, rc = -ENOMEM);

out:
    if (rq)
        json_decref(rq);
    return rc;
}

/**
 * Serialize one grh_request_id entry to a json array for a status request
 *
 * @param[in]       request    a pointer to a struct grh_request
 * @param[in/out]   rq_array   json array to which the new element is appended
 *
 * @return 0 on success, and -1 * posix error code on error
 */
int json_array_append_status_request(const struct grh_request *request,
                                     json_t *rq_array)
{
    json_t *rq;
    int rc = 0;

    /* Build the element json */
    rq = json_pack("{s: s}", "request_id", request->grh_request_id);
    if (!rq)
        GOTO(out, rc = -ENOMEM);

    /* And append it to the request array */
    if (json_array_append(rq_array, rq) == -1)
        GOTO(out, rc = -ENOMEM);
out:
    if (rq)
        json_decref(rq);

    return rc;
}

/**
 * Build a json request body for several grh_request or status requests
 *
 * @param[in]   requests            array of \a count struct grh_request
 * @param[in]   count               number of elements in \a requests
 * @param[in]   request_context     context to build grh_request or status
 *                                  request
 * @param[out]  req_body            allocated string containing the json
 *                                  request (NULL on error)
 *
 * @return 0 on success, and -1 * posix error code on error (first encountered
 *         error)
 */
int request2json(const struct grh_request *requests, size_t count,
                 const struct request_context *request_context, char **req_body)
{
    json_t *request_array;
    int rc = 0;
    size_t i;

    *req_body = NULL;

    request_array = json_array();
    if (!request_array)
        return -ENOMEM;

    for (i = 0; i < count; i++) {
        int rc2;

        switch (request_context->type) {
        case STATUS:
            rc2 = json_array_append_status_request(&requests[i], request_array);
            break;
        case ASYNC_GRH_REQUEST:
        case SYNC_GRH_REQUEST:
            rc2 = json_array_append_grh_request(&requests[i], request_array);
            break;
        default:
            return -EINVAL;
        }

        if (rc2)
            rc = rc ? : rc2;
    }

    *req_body = json_dumps(request_array, JSON_COMPACT);
    if (!*req_body)
        rc = -ENOMEM;

    json_decref(request_array);
    return rc;
}

static int parse_error_response(json_t *rq_resp_elt, int *error)
{
    json_t *json_object;

    json_object = json_object_get(rq_resp_elt, "errno");
    if (!json_object || !json_is_integer(json_object)) {
        if (error)
            *error = -EPROTO;

        return -EPROTO;
    }

    if (error) {
        *error = json_integer_value(json_object);
        return *error;
    }

    return json_integer_value(json_object);
}

static int parse_running_response(json_t *rq_resp_elt, int *error,
                                  struct request_context *request_context,
                                  struct grh_request *request)
{
    const char *request_id;
    unsigned int eta_delta;
    json_t *json_object;

    /* update eta_delta */
    json_object = json_object_get(rq_resp_elt, "eta");
    if (!json_object || !json_is_integer(json_object)) {
        if (error)
            *error = -EPROTO;

        return -EPROTO;
    }

    eta_delta = json_integer_value(json_object);
    if (eta_delta > request_context->eta_delta)
        request_context->eta_delta = eta_delta;

    /* get request id */
    if (request_context->type == SYNC_GRH_REQUEST) {
        request_id = json_string_value(json_object_get(rq_resp_elt,
                                                       "request_id"));
        if (!request_id || strlen(request_id) >= GRH_REQUEST_ID_LEN) {
            if (error)
                *error = -EPROTO;

            return -EPROTO;
        }

        strcpy(request->grh_request_id, request_id);
        if (error) /* ack we asked this request */
            *error = -EINPROGRESS; /* false error, request is ongoing */
    }

    request->still_running = true;
    return 0;
}

/**
 * Parse one element into the json response from the data grh and fill the
 * error pointers accordingly.
 *
 * @param[in]       act_resp_elt      json element to be parsed
 * @param[in, out]  request           request that the response element is
 *                                    about
 * @param[in, out]  request_context   context to get id/eta
 *
 * @return 0 on success, and -1 * posix error code on error
 */
int parse_response_one_elt(json_t *rq_resp_elt, struct grh_request *request,
                           struct request_context *request_context)
{
    const char *status;

    /* until we face a "running" status, request isn't "running" */
    request->still_running = false;
    status = json_string_value(json_object_get(rq_resp_elt, "status"));
    if (!status) {
        request->error = -EPROTO;

        return -EPROTO;
    }

    if (strcmp(status, "error") == 0)
        /* get error code */
        return parse_error_response(rq_resp_elt, &request->error);

    /* no error , async case, only says "it's OK" */
    if (request_context->type == ASYNC_GRH_REQUEST) {
        request->error = 0;

        return 0;
    }

    /* no error, sync or status case */
    /* migrated */
    if (strcmp(status, "completed") == 0) {
        request->error = 0;

        return 0;
    }

    /* if status is neither error, nor completed, nor running : -EPROTO */
    if (strcmp(status, "running") != 0) {
        request->error = -EPROTO;

        return -EPROTO;
    }

    /* running */
    return parse_running_response(rq_resp_elt, &request->error, request_context,
                                  request);
}

/**
 * Parse the json response from the data grh and fill the error pointers
 * accordingly.
 *
 * @param[in]       response            full JSON response to be parsed.
 * @param[in, out]  requests            array of \a count struct grh_request
 *                                      which will be filled by \a response
 * @param[in]       count               number of elements in \a requests
 * @param[in, out]  request_context     context to get id/eta
 *
 * @return 0 on success, and -1 * posix error code on error (first encountered
 *         error)
 */
int parse_response(json_t *response, struct grh_request *requests, size_t count,
                   struct request_context *request_context)
{
    json_t *rq_resp_elt = NULL;
    int rc = 0;
    size_t i;

    if (!json_is_array(response)) {
        for (i = 0; i < count; i++)
            requests[i].error = -EPROTO;

        return -EPROTO;
    }

    /* Note: this implementation does not check that returned file_id
     * match a given path, it just assumes responses are properly ordered, as
     * they should be.
     */
    json_array_foreach(response, i, rq_resp_elt) {
        int rc2;

        /* We reached the end of the requests but there are still some json
         * elements to handle: this is an unexpected case.
         */
        if (i >= count) {
            rc = rc ? : -EPROTO;
            break;
        }

        rc2 = parse_response_one_elt(rq_resp_elt, &requests[i],
                                     request_context);
        rc = rc ? : rc2;
    }

    /* We get less entries in response than expected. */
    if (i < count) {
        for (; i < count; i++)
            requests[i].error = -EPROTO;

        rc = rc ? : -EPROTO;
    }

    return rc;
}

/**
 * Perform one request or status request of \a count requests to one grh
 *
 * If needed, the global request context error code is updated on first error.
 *
 * @param[in, out]  request_context     request context
 * @param[in]       grh_url             request server url
 * @param[in, out]  requests            requests to send, if not NULL, error
 *                                      fields are filled
 * @param[in]       count               number of request to perform from \a
 *                                      requests
 */
void do_request_or_status(struct request_context *request_context,
                          const char *grh_url, struct grh_request *requests,
                          size_t count)
{
    CURL *curl = request_context->curl;
    char *request_url = NULL;
    FILE *resp_stream = NULL;
    json_t *response = NULL;
    char *resp_body = NULL;
    char *req_body = NULL;
    size_t resp_body_size;
    json_error_t json_err;
    long http_status;
    CURLcode res;
    int rc = 0;

    /* Build and set the full request URL for this batch */
    switch (request_context->type) {
    case STATUS:
        if (asprintf(&request_url, "%s/requests/status", grh_url) == -1)
            GOTO(out, rc = -ENOMEM);

        break;
    case ASYNC_GRH_REQUEST:
    case SYNC_GRH_REQUEST:
        if (asprintf(&request_url, "%s/requests", grh_url) == -1)
            GOTO(out, rc = -ENOMEM);

        break;
    default:
        GOTO(out, rc = -EINVAL);
    }

    curl_easy_setopt(curl, CURLOPT_URL, request_url);

    /* Build request body */
    rc = request2json(requests, count, request_context, &req_body);
    if (rc)
        goto out;

    /* Prepare the response stream */
    resp_stream = open_memstream(&resp_body, &resp_body_size);
    if (!resp_stream)
        GOTO(out, rc = -errno);

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp_stream);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);

    /* Follow 'redirect' responses if the 'Location' header is set */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    /* Perform the HTTP request */
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        /* @FIXME: we really need a logging mechanism */
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
        GOTO(out, rc = -ECOMM);
    }

    /* check HTTP answer code */
    res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl can't get HTTP answer code: %s\n",
                curl_easy_strerror(res));
        GOTO(out, rc = -ECOMM);
    }

    switch (http_status) {
    case 200:
    case 201:
    case 304:
        break;
    case 400:
        fprintf(stderr, "grh answered: 400 bad request");
        GOTO(out, rc = -ECOMM);
    case 403:
        fprintf(stderr, "grh answered: 403 forbidden");
        GOTO(out, rc = -ECOMM);
    default:
        fprintf(stderr, "grh answered with an unexpected HTTP status "
                        "code: %ld", http_status);
        GOTO(out, rc = -EPROTO);
    }

    /* Parse and examine the response */
    fflush(resp_stream);
    if (fseek(resp_stream, 0, SEEK_SET) != 0) {
        fprintf(stderr, "error when seeking response stream: %d , %s\n",
                errno, strerror(errno));
        GOTO(out, rc = -ECOMM);
    }

    response = json_loadf(resp_stream, 0, &json_err);
    if (!response) {
        fprintf(stderr, "error while parsing json response from the data grh "
                "server: %s ('%s')\n", json_err.text, resp_body);
        GOTO(out, rc = -EPROTO);
    }

    rc = parse_response(response, requests, count, request_context);

out:
    if (resp_stream)
        fclose(resp_stream);
    if (response)
        json_decref(response);
    free(request_url);
    free(req_body);
    free(resp_body);

    request_context->rc = request_context->rc ? : rc;
}

/**
 * Fill a struct grh_request for one entry
 *
 * @param[in]       path         path to act on
 * @param[in]       backend      backend to use
 * @param[in]       type         type to perform
 * @param[in]       error        error code
 * @param[out]      request      the struct grh_request to fill
 *
 * @return 0 on success, and -1 * posix error code on failure
 */
void grh_request_setter(const char *path, const char *backend,
                        enum grh_request_type type, int error,
                        struct grh_request *request)
{
    int rc;

    rc = snprintf(request->file_id, strlen(path) + 1, "%s", path);
    /* There shouldn't be any error with snprintf */
    assert(rc >= 0);
    /* And the buffer's size should always be enough */
    assert((unsigned int)rc < sizeof(request->file_id));

    rc = snprintf(request->backend, strlen(backend) + 1, "%s", backend);
    /* There shouldn't be any error with snprintf */
    assert(rc >= 0);
    /* And the buffer's size should always be enough */
    assert((unsigned int)rc < sizeof(request->backend));

    request->type = type;
    request->error = error;
}

/**
 * Prepare one request and groupe by GRH URL
 *
 * @param[in]      grh_url      URL of the Ganesha Request Handler
 * @param[in]      path         path to act on
 * @param[in]      backend      backend to use
 * @param[in]      type         type of the request
 * @param[in, out] error        error code, ignored if NULL
 * @param[in, out] requests     caller allocated GHashTable, key: grh url,
 *                              value: GArray of requests
 *
 * @return 0 on success, and -1 * posix error code on failure
 */
int prepare_request_one_entry(char *grh_url, const char *path,
                              const char *backend, enum grh_request_type type,
                              int error, GHashTable *requests)
{
    GArray *requests_for_one_url;
    struct grh_request request;
    struct grh grh;
    int rc = 0;

    /* build struct grh_request */
    grh_request_setter(path, backend, type, error, &request);

    /* add request */
    grh.url = grh_url;
    requests_for_one_url = g_hash_table_lookup(requests, &grh);
    if (!requests_for_one_url) {
        struct grh *new_grh;

        /* get grh_url from config */
        rc = alloc_grh(grh_url, &new_grh);
        if (rc)
            goto out;

        requests_for_one_url = g_array_new(false, false, sizeof(request));
        g_hash_table_insert(requests, new_grh, requests_for_one_url);
    }

    g_array_append_val(requests_for_one_url, request);

out:
    return rc;
}

/**
 * Prepare a request and groupe by GRH URL
 *
 * @param[in]       grh_url     URL of the Ganesha Request Handler to use
 * @param[in]       paths       array of paths to act on
 * @param[in]       backends    array of backends to use
 * @param[in]       types       array of request types
 * @param[in, out]  errors      array of errors code per path, ignored if NULL
 * @param[in, out]  requests    caller allocated GHashTable per grh
 *                              url of GArray of requests
 * @param[in]       n_paths     number of path in \a paths
 *
 * @return 0 on success, and -1 * posix error code on failure (first error)
 */
int prepare_request(char *grh_url, const char * const *paths,
                    const char * const *backends, enum grh_request_type *types,
                    int *errors, GHashTable *requests, size_t n_paths)
{
    int rc = 0;
    size_t i;

    /* build struct grh_request per entry */
    for (i = 0; i < n_paths; i++) {
        int rc2;

        rc2 = prepare_request_one_entry(grh_url, paths[i], backends[i],
                                        types[i], errors ? errors[i] : 0,
                                        requests);
        if (errors)
            errors[i] = rc2;

        rc = rc ? : rc2;
    }

    return rc;
}

/**
 * Compute timeval eta time by adding eta_delta_msec milliseconds to now
 *
 * @param[in]  eta_delta_msec  number of milliseconds from now
 * @param[out] eta             filled to now + eta_delta_msec if
 *                             success, unrelevant on error
 *
 * @return                     0 if success, -1 * posix error code on error
 */
/* we prepare future async request with status loop */
int compute_eta(unsigned int eta_delta_msec, struct timeval *eta)
{
    struct timeval eta_delta;
    struct timeval now;

    /* compute eta timeval value */
    eta_delta.tv_sec = eta_delta_msec / 1000;
    eta_delta.tv_usec = (eta_delta_msec % 1000) * 1000;

    if (gettimeofday(&now, NULL))
        return -errno;

    timeradd(&now, &eta_delta, eta);
    return 0;
}

#ifndef MIN
# define MIN(a, b) (a < b ? a : b)
#endif

/**
 * Execute or status all requests of one same grh url
 *
 * Requests are grouped by chunk of MAX_GRH_REQUEST_PER_REQUEST.
 * This function is a GHRFunc for GHashTable (key : grh_url, value: GArray of
 * struct grh_request).
 *
 * In case of SYNC_GRH_REQUEST or STATUS, the requests are sifted and only still
 * "running" requests remain.
 *
 * @param[in, out] grh                  grh url (with updated eta)
 * @param[in, out] requests             array of struct grh_request
 * @param[in, out] request_context      the context contains opened curl handle
 *                                      and current error code value for the
 *                                      first error
 *
 * @return true if the \a requests array is now empty and should be removed,
 *         else false
 */
gboolean request_or_status_one_url(struct grh *grh, GArray *requests,
                                   struct request_context *request_context)
{
    size_t start;
    size_t count;

    /* eta_delta to zero if we need to compute a new one */
    if (request_context->type == SYNC_GRH_REQUEST ||
        request_context->type == STATUS)
        request_context->eta_delta = 0;

    for (start = 0; start < requests->len; start = start + count) {
        count = MIN(requests->len - start, MAX_GRH_REQUEST_PER_REQUEST);
        do_request_or_status(request_context, grh->url,
                             &g_array_index(requests, struct grh_request,
                                            start),
                             count);
    }

    /* If needed, requests are sifted and we get eta. */
    if (request_context->type == SYNC_GRH_REQUEST ||
        request_context->type == STATUS) {
        size_t i;
        int rc;

        /* sift requests */
        for (i = requests->len; i > 0; i--)
            if (!g_array_index(requests, struct grh_request,
                               i - 1).still_running)
                g_array_remove_index_fast(requests, i - 1);

        /* get eta */
        rc = compute_eta(request_context->eta_delta, &grh->eta);
        /*
         * If an error occurs : we record it and remove all requests.
         * With no more request into it, the requests entry will be
         * remove from the hash table.
         */
        if (rc) {
            request_context->rc = request_context->rc ? : rc;
            for (i = requests->len; i > 0; i--) {
                struct grh_request *rq = &g_array_index(requests,
                                                        struct grh_request,
                                                        i - 1);
                rq->error = rc;
                g_array_remove_index_fast(requests, i - 1);
            }
        }
    }

    return requests->len == 0;
}

/**
 * GHashFunc to hash a grh by its url
 */
guint hash_url(const struct grh *grh)
{
    return g_str_hash(grh->url);
}

/**
 * GEqualFunc to compare two grhs in GHashTable by grh url
 */
gboolean url_equal(const struct grh *grh1, const struct grh *grh2)
{
    return strcmp(grh1->url, grh2->url) ? FALSE : TRUE;
}

int handle_request(char *grh_url, const char **paths,
                   const char **backends, enum grh_request_type *types,
                   int *errors, size_t n_paths)
{
    struct request_context request_context = {
        .curl = NULL,
        .rc = 0,
        .type = ASYNC_GRH_REQUEST,
        .eta_delta = 0,
    };
    struct curl_slist *headers = NULL;
    GHashTable *requests;

    if (grh_url == NULL) {
        fprintf(stderr, "GRH URL was not provided\n");
        return -EINVAL;
    }

    if (!valid_types(types, n_paths)) {
        fprintf(stderr, "A requested type of operation is invalid, abort the "
                        "request\n");
        return -EINVAL;
    }

    /* Initialize all errors in case of early return */
    if (errors) {
        size_t i;

        for (i = 0; i < n_paths; i++)
            errors[i] = -ECANCELED;
    }

    /* prepare curl handle with http headers */
    request_context.rc = prepare_curl_handle(&request_context.curl, &headers);
    if (request_context.rc)
        goto out;

    requests = g_hash_table_new_full((GHashFunc)hash_url,
                                     (GEqualFunc)url_equal,
                                     (GDestroyNotify)free_grh,
                                     (GDestroyNotify)g_array_unref);
    request_context.rc = prepare_request(grh_url, paths, backends, types,
                                         errors, requests, n_paths);
    g_hash_table_foreach_remove(requests, (GHRFunc)request_or_status_one_url,
                                &request_context);
    g_hash_table_unref(requests);

out:
    curl_easy_cleanup(request_context.curl);
    curl_slist_free_all(headers);
    return request_context.rc;
}

/**
 * Check if a timeout is reached
 *
 * @param[in]   start_time  start time to compare to current time
 * @param[in]   timeout     timeout value
 *
 * @return 0 is the timeout is not reached, -ETIME if the timeout is reached,
 *         else -1 * posix error code on error
 */
int check_timeout_is_reached(const struct timeval *start_time,
                             const struct timeval *timeout)
{
    struct timeval elapsed;
    struct timeval now;

    if (gettimeofday(&now, NULL))
        return -errno;

    timersub(&now, start_time, &elapsed);
    if (timercmp(&elapsed, timeout, >))
        return -ETIME;

    return 0;
}

/* A thin wrapper around nanosleep(2) that handles interrupts */
static int nanosleep_nointr(const struct timespec *_req)
{
    struct timespec req = *_req;
    struct timespec rem;

    while (nanosleep(&req, &rem)) {
        if (errno != EINTR)
            return -1;
        req = rem;
    }
    return 0;
}

/**
 * Update error code of request into \a requests
 *
 * This function is a GHFunc for GHashTable (key : grh_url, value: GArray of
 * struct grh_requests).
 *
 * @param[in]       grh         grh as key
 * @param[in, out]  requests    grh_requests with error code to update
 * @param[in]       error_code  new error code
 */
void set_all_requests_error(struct grh *grh, GArray *requests, int *error_code)
{
    size_t i;
    (void)grh; /* only here as key when browsing with g_hash_table_foreach */

    for (i = 0; i < requests->len; i++) {
        struct grh_request *rq = &g_array_index(requests, struct grh_request,
                                                i);
        rq->error = *error_code;
    }
}

/**
 * Check if eta is reached and status all requests of one same migrator url
 *
 * This function is a GHRFunc for GHashTable (key : grh_url, value: GArray of
 * struct grh_requests).
 *
 * If the eta of the grh is reached, we execute one status request. The
 * requests are sifted and only still "running" request remains. The
 * grh eta is updated.
 *
 * @param[in, out] grh_url              grh url (with eta updated)
 * @param[in, out] requests             array of struct grh_request
 * @param[in, out] request_context      the context contains opened curl handle
 *                                      and current error code value for the
 *                                      first error
 *
 * @return true if the \a requests array is now empty and should be removed,
 *         else false
 */
gboolean status_if_eta_one_url(struct grh *grh, GArray *requests,
                               struct request_context *request_context)
{
    struct timeval now;

    assert(request_context->type == STATUS);

    /* check eta */
    if (gettimeofday(&now, NULL)) {
        request_context->rc = request_context->rc ? : -errno;
        /*
         * We let -EINPROGRESS into requests error code but we remove these
         * requests to prevent endless looping over continuous gettimeofday
         * errors.
         */
        return TRUE;
    }

    if (!timercmp(&now, &grh->eta, >))
        return FALSE;

    /* perform one status which sifts requests and update grh eta */
    return request_or_status_one_url(grh, requests, request_context);
}

int handle_request_wait(char *grh_url, const char **paths,
                        const char **backends, enum grh_request_type *types,
                        int *errors, size_t n_paths,
                        const struct timeval *timeout)
{
    struct request_context request_context = {
        .curl = NULL,
        .rc = 0,
        .type = SYNC_GRH_REQUEST, /* one SYNC_GRH_REQUEST before a
                                     * STATUS loop.
                                     */
        .eta_delta = 0,
    };
    struct curl_slist *headers = NULL;
    struct timeval start_time;
    GHashTable *requests;

    if (grh_url == NULL) {
        fprintf(stderr, "GRH URL was not provided\n");
        return -EINVAL;
    }

    if (!valid_types(types, n_paths)) {
        fprintf(stderr, "A requested type of operation is invalid, abort the "
                        "request\n");
        return -EINVAL;
    }

    /* Initialize all errors in case of early return */
    if (errors) {
        size_t i;

        for (i = 0; i < n_paths; i++)
            errors[i] = -ECANCELED;
    }

    /* get start time to check timeout */
    if (timeout && gettimeofday(&start_time, NULL) < 0)
        GOTO(out, request_context.rc = -errno);

    /* prepare curl handle with http headers */
    request_context.rc = prepare_curl_handle(&request_context.curl, &headers);
    if (request_context.rc)
        goto out;

    /* prepare requests */
    requests = g_hash_table_new_full((GHashFunc)hash_url,
                                     (GEqualFunc)url_equal,
                                     (GDestroyNotify)free_grh,
                                     (GDestroyNotify)g_array_unref);
    request_context.rc = prepare_request(grh_url, paths, backends, types,
                                         errors, requests, n_paths);

    /* request request */
    g_hash_table_foreach_remove(requests, (GHRFunc)request_or_status_one_url,
                                &request_context);

    /* Wait all requests are ended */
    request_context.type = STATUS; /* from SYNC_GRH_REQUEST step to STATUS */
    /* Status loop */
    while (g_hash_table_size(requests)) {
        struct timespec wait_step = { /* 10 milliseconds */
            .tv_sec = 0,
            .tv_nsec = 10000000,
        };

        /* check timeout */
        if (timeout) {
            int rc;

            switch ((rc = check_timeout_is_reached(&start_time, timeout))) {
            case 0:
                break;
            case -ETIME:
                /* update all remaining errors from -EINPROGRESS to -ETIME */
                g_hash_table_foreach(requests,
                                     (GHFunc)set_all_requests_error, &rc);
                /* fall through */
            default:
                request_context.rc = request_context.rc ? : rc;
                goto out_requests;
            }
        }

        /* for each grh, we execute status if eta is reached */
        g_hash_table_foreach_remove(requests, (GHRFunc)status_if_eta_one_url,
                                    &request_context);

        /* wait one step */
        if (g_hash_table_size(requests) && nanosleep_nointr(&wait_step)) {
            request_context.rc = request_context.rc ? : -errno;
            goto out_requests;
        }
    }

out_requests:
    g_hash_table_unref(requests);
out:
    curl_easy_cleanup(request_context.curl);
    curl_slist_free_all(headers);
    return request_context.rc;
}

/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */

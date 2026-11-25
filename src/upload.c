/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <vectus/vectus.h>

#include "internal.h"


/* curl takes the request size as a signed 64-bit curl_off_t, which caps the upload length */
#define UPLOAD_MAX_LENGTH 9223372036854775807ULL
typedef char upload_curl_off_t_is_64_bits[sizeof(curl_off_t) == 8 ? 1 : -1];

struct vectus_upload_s {
    /**
     * read_callback will be called when performing the upload to get the byte stream
     */
    vectus_read_callback read_callback;

    /**
     * seek_callback moves the byte stream to where the server stopped, NULL if it cannot
     */
    vectus_seek_callback seek_callback;

    /**
     * userdata is passed to both callbacks
     */
    void *userdata;

    /**
     * the number of bytes to upload
     */
    unsigned long long length;

    /**
     * how far the byte stream has been read
     */
    unsigned long long position;

    /**
     * how many bytes the server confirmed it has
     */
    unsigned long long offset;

    /**
     * the upload's URL on the server, NULL until created
     */
    char *url;

    /**
     * set when the read callback aborted or ended early during the current request
     */
    int source_failed;
};

/**
 * What one response said about the upload
 */
struct upload_response {
    long status;
    char *location;
    int has_offset;
    unsigned long long offset;
    int has_length;
    unsigned long long length;
};

enum upload_method { UPLOAD_POST, UPLOAD_HEAD, UPLOAD_PATCH };

static void upload_handle_header(void *target, const char *name, char *value) {
    struct upload_response *response = target;

    if (curl_strequal(name, "Location")) {
        free(response->location);
        response->location = malloc(strlen(value) + 1);
        if (response->location != NULL) strcpy(response->location, value);
    } else if (curl_strequal(name, "Upload-Offset")) {
        response->has_offset = vectus_parse_size(value, &response->offset);
    } else if (curl_strequal(name, "Upload-Length")) {
        response->has_length = vectus_parse_size(value, &response->length);
    }
}

static size_t upload_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    return vectus_parse_header_line(buffer, size * nitems, upload_handle_header, userdata);
}

/**
 * Feed curl from the read callback, never past the upload's length, keeping track of the stream position
 */
static size_t upload_read_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    vectus_upload upload = userdata;
    unsigned long long remaining = upload->length - upload->position;
    size_t wanted = size * nitems;
    size_t bytes;

    if (wanted > remaining) wanted = (size_t) remaining;
    if (wanted == 0) {
        return 0;
    }

    bytes = upload->read_callback(buffer, 1, wanted, upload->userdata);
    if (bytes == 0 || bytes > wanted) {  /* VECTUS_READ_ABORT, the data ending before length, or a broken callback */
        upload->source_failed = 1;
        return CURL_READFUNC_ABORT;
    }
    upload->position += bytes;
    return bytes;
}

/**
 * curl_slist_append keeps the list when it fails, so note the failure and carry on with the old list
 */
static struct curl_slist *upload_add_header(struct curl_slist *headers, const char *header, int *failed) {
    struct curl_slist *appended = curl_slist_append(headers, header);

    if (appended == NULL) {
        *failed = 1;
        return headers;
    }
    return appended;
}

/**
 * Send one tus request about the upload and collect what the response says
 *
 * @return VECTUS_OK when a response arrived, whatever its status
 */
static vectus_code upload_request(vectus_client client, vectus_upload upload, enum upload_method method, const char *url,
                                  struct upload_response *response) {
    CURL *c = curl_easy_init();
    struct curl_slist *headers = NULL;
    char header[64];
    int failed = 0;
    vectus_code code = VECTUS_ERR_MEMORY;

    if (c == NULL) {
        return VECTUS_ERR_MEMORY;
    }

    headers = upload_add_header(headers, "Tus-Resumable: 1.0.0", &failed);
    if (method == UPLOAD_POST) {
        snprintf(header, sizeof header, "Upload-Length: %llu", upload->length);
        headers = upload_add_header(headers, header, &failed);
        headers = upload_add_header(headers, "Content-Type:", &failed);  /* drop the form type curl gives a POST */
    } else if (method == UPLOAD_PATCH) {
        snprintf(header, sizeof header, "Upload-Offset: %llu", upload->offset);
        headers = upload_add_header(headers, header, &failed);
        headers = upload_add_header(headers, "Content-Type: application/offset+octet-stream", &failed);
        headers = upload_add_header(headers, "Expect:", &failed);  /* send the body at once instead of waiting for 100 Continue */
    }
    if (failed) {
        goto done;
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, upload_header_callback);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *) response);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* give up on a stalled connection, so the upload can resume */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, client->stall_timeout);
    switch (method) {
    case UPLOAD_POST:
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
        break;
    case UPLOAD_HEAD:
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        break;
    case UPLOAD_PATCH:
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(c, CURLOPT_READFUNCTION, upload_read_callback);
        curl_easy_setopt(c, CURLOPT_READDATA, (void *) upload);
        curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t) (upload->length - upload->offset));
        break;
    }

    upload->source_failed = 0;
    if (curl_easy_perform(c) != CURLE_OK) {
        code = upload->source_failed ? VECTUS_ERR_SOURCE : VECTUS_ERR_TRANSPORT;
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response->status);
        code = VECTUS_OK;
    }

done:
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return code;
}

/**
 * Resolve a Location header, which may be relative, against the URL the request went to
 *
 * @return the absolute URL, to be freed, or NULL if it cannot be resolved
 */
static char *upload_resolve_url(const char *base, const char *location) {
    CURLU *u = curl_url();
    char *resolved = NULL;
    char *copy = NULL;

    if (u != NULL && curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK
        && curl_url_set(u, CURLUPART_URL, location, 0) == CURLUE_OK
        && curl_url_get(u, CURLUPART_URL, &resolved, 0) == CURLUE_OK) {
        copy = malloc(strlen(resolved) + 1);
        if (copy != NULL) strcpy(copy, resolved);
    }
    curl_free(resolved);
    curl_url_cleanup(u);
    return copy;
}

/**
 * The statuses tus servers answer for an upload that no longer exists
 */
static int upload_gone(long status) {
    return status == 403 || status == 404 || status == 410;
}

/**
 * Create the upload on the server, keeping the URL it answers in Location
 */
static vectus_code upload_create_remote(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    vectus_code code = upload_request(client, upload, UPLOAD_POST, client->url, &response);

    if (code != VECTUS_OK) {
        /* keep the code */
    } else if (response.status == 413) {  /* the server's own limit, when it did not advertise Tus-Max-Size */
        code = VECTUS_ERR_TOO_LARGE;
    } else if (response.status != 201) {
        code = VECTUS_ERR_HTTP_STATUS;
    } else if (response.location == NULL) {
        code = VECTUS_ERR_PROTOCOL;
    } else {
        upload->url = upload_resolve_url(client->url, response.location);
        upload->offset = 0;
        if (upload->url == NULL) code = VECTUS_ERR_PROTOCOL;
    }
    free(response.location);
    return code;
}

/**
 * Ask the server how many bytes of the upload it has
 */
static vectus_code upload_fetch_offset(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    vectus_code code = upload_request(client, upload, UPLOAD_HEAD, upload->url, &response);

    if (code != VECTUS_OK) {
        /* keep the code */
    } else if (upload_gone(response.status)) {
        code = VECTUS_ERR_GONE;
    } else if (response.status != 200 && response.status != 204) {
        code = VECTUS_ERR_HTTP_STATUS;
    } else if (!response.has_offset || response.offset > upload->length
               || (response.has_length && response.length != upload->length)) {
        code = VECTUS_ERR_PROTOCOL;
    } else {
        upload->offset = response.offset;
    }
    free(response.location);
    return code;
}

/**
 * Send the rest of the byte stream, from the offset the server has
 */
static vectus_code upload_send(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    vectus_code code;

    if (upload->position != upload->offset) {
        if (upload->seek_callback == NULL || upload->seek_callback(upload->userdata, upload->offset) != 0) {
            return VECTUS_ERR_SOURCE;
        }
        upload->position = upload->offset;
    }

    code = upload_request(client, upload, UPLOAD_PATCH, upload->url, &response);
    if (code != VECTUS_OK) {
        /* keep the code; the next perform asks the server where it stopped */
    } else if (upload_gone(response.status)) {
        code = VECTUS_ERR_GONE;
    } else if (response.status != 204) {
        code = VECTUS_ERR_HTTP_STATUS;  /* 409 when the offset was stale; the next perform asks again */
    } else if (!response.has_offset || response.offset <= upload->offset || response.offset > upload->length) {
        code = VECTUS_ERR_PROTOCOL;  /* an offset that does not move forward would loop forever */
    } else {
        upload->offset = response.offset;
    }
    free(response.location);
    return code;
}

vectus_code vectus_upload_create(vectus_upload *upload, vectus_read_callback read_callback, vectus_seek_callback seek_callback,
                                 void *userdata, unsigned long long length) {
    *upload = NULL;
    if (length > UPLOAD_MAX_LENGTH) {
        return VECTUS_ERR_TOO_LARGE;
    }

    *upload = calloc(1, sizeof(struct vectus_upload_s));
    if (*upload == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    (*upload)->read_callback = read_callback;
    (*upload)->seek_callback = seek_callback;
    (*upload)->userdata = userdata;
    (*upload)->length = length;
    return VECTUS_OK;
}

vectus_code vectus_upload_perform(vectus_client client, vectus_upload upload) {
    vectus_code code;

    if (upload->url == NULL) {
        if (client->max_size != 0 && upload->length > client->max_size) {
            return VECTUS_ERR_TOO_LARGE;
        }
        code = upload_create_remote(client, upload);
    } else {
        code = upload_fetch_offset(client, upload);
    }

    while (code == VECTUS_OK && upload->offset < upload->length) {
        code = upload_send(client, upload);
    }
    return code;
}

const char *vectus_upload_url(vectus_upload upload) {
    return upload->url;
}

vectus_code vectus_upload_set_url(vectus_upload upload, const char *url) {
    char *copy = NULL;

    if (url != NULL) {
        copy = malloc(strlen(url) + 1);
        if (copy == NULL) {
            return VECTUS_ERR_MEMORY;
        }
        strcpy(copy, url);
    }
    free(upload->url);
    upload->url = copy;
    upload->offset = 0;  /* unknown until perform asks the server */
    return VECTUS_OK;
}

unsigned long long vectus_upload_offset(vectus_upload upload) {
    return upload->offset;
}

void vectus_upload_cleanup(vectus_upload upload) {
    if (upload == NULL) {
        return;
    }
    free(upload->url);
    free(upload);
}

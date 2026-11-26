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
     * the number of bytes to upload, VECTUS_LENGTH_UNKNOWN until a deferred upload learns it
     */
    unsigned long long length;

    /**
     * set when created with VECTUS_LENGTH_UNKNOWN: the length comes from the stream's end, or from the server when resuming
     */
    int deferred;

    /**
     * whether the server knows the length; a deferred upload's next PATCH declares it once the stream has ended
     */
    int length_declared;

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
     * when the server will remove the unfinished upload, 0 if unknown
     */
    time_t expires;

    /**
     * the whole Upload-Metadata header line sent on creation, NULL until a pair is added
     */
    char *metadata;

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
    int length_deferred;  /* Upload-Defer-Length: 1, the server still waits for the length */
    time_t expires;  /* 0 when absent or not a date */
};

enum upload_method { UPLOAD_POST, UPLOAD_HEAD, UPLOAD_PATCH, UPLOAD_DELETE };

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
    } else if (curl_strequal(name, "Upload-Defer-Length")) {
        response->length_deferred = strcmp(value, "1") == 0;
    } else if (curl_strequal(name, "Upload-Expires")) {
        time_t expires = curl_getdate(value, NULL);  /* -1 when it is not a date */

        if (expires > 0) response->expires = expires;
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
    if (bytes == 0 && upload->length == VECTUS_LENGTH_UNKNOWN) {
        upload->length = upload->position;  /* the end of a stream announced without a length */
        return 0;
    }
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

/* shortcut: only uploads up to 1 MiB ride in the creation POST, since bytes sent before the server answers with the URL
   cannot be resumed; make it a client setting when callers need another limit */
#define UPLOAD_CREATION_DATA_MAX (1024ULL * 1024ULL)

/**
 * Whether the creation POST carries the whole upload, as the creation-with-upload extension allows
 */
static int upload_creates_with_data(vectus_client client, vectus_upload upload) {
    return (client->extensions & VECTUS_EXT_CREATION_WITH_UPLOAD) && upload->length > 0 && upload->length <= UPLOAD_CREATION_DATA_MAX;
}

/**
 * Whether the next PATCH declares a deferred upload's length: the stream has ended, and the server still waits for it
 */
static int upload_declares_length(vectus_upload upload) {
    return upload->deferred && !upload->length_declared && upload->length != VECTUS_LENGTH_UNKNOWN;
}

/**
 * Move the byte stream to the offset the server has, before sending from there
 */
static vectus_code upload_seek(vectus_upload upload) {
    if (upload->position != upload->offset) {
        if (upload->seek_callback == NULL || upload->seek_callback(upload->userdata, upload->offset) != 0) {
            return VECTUS_ERR_SOURCE;
        }
        upload->position = upload->offset;
    }
    return VECTUS_OK;
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
    int with_data = method == UPLOAD_PATCH || (method == UPLOAD_POST && upload_creates_with_data(client, upload));
    vectus_code code = VECTUS_ERR_MEMORY;

    if (c == NULL) {
        return VECTUS_ERR_MEMORY;
    }

    headers = upload_add_header(headers, "Tus-Resumable: 1.0.0", &failed);
    if (method == UPLOAD_POST) {
        if (upload->length == VECTUS_LENGTH_UNKNOWN) {
            headers = upload_add_header(headers, "Upload-Defer-Length: 1", &failed);
        } else {
            snprintf(header, sizeof header, "Upload-Length: %llu", upload->length);
            headers = upload_add_header(headers, header, &failed);
        }
        if (upload->metadata != NULL) headers = upload_add_header(headers, upload->metadata, &failed);
    } else if (method == UPLOAD_PATCH) {
        snprintf(header, sizeof header, "Upload-Offset: %llu", upload->offset);
        headers = upload_add_header(headers, header, &failed);
        if (upload_declares_length(upload)) {
            snprintf(header, sizeof header, "Upload-Length: %llu", upload->length);
            headers = upload_add_header(headers, header, &failed);
        }
    }
    if (with_data) {
        headers = upload_add_header(headers, "Content-Type: application/offset+octet-stream", &failed);
        /* A creation POST lets the server refuse before the data goes out, as the spec asks; a PATCH sends it at once */
        headers = upload_add_header(headers, method == UPLOAD_POST ? "Expect: 100-continue" : "Expect:", &failed);
    } else if (method == UPLOAD_POST) {
        headers = upload_add_header(headers, "Content-Type:", &failed);  /* drop the form type curl gives a POST */
    }
    if (failed) {
        goto done;
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, upload_header_callback);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *) response);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, vectus_discard_body);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* give up on a stalled connection, so the upload can resume */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, client->stall_timeout);
    switch (method) {
    case UPLOAD_POST:
        if (with_data) {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) upload->length);
        } else {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
        }
        break;
    case UPLOAD_HEAD:
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        break;
    case UPLOAD_PATCH:
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (upload->length != VECTUS_LENGTH_UNKNOWN) {  /* otherwise curl sends the stream chunked */
            curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t) (upload->length - upload->offset));
        }
        break;
    case UPLOAD_DELETE:
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    }
    if (with_data) {
        curl_easy_setopt(c, CURLOPT_READFUNCTION, upload_read_callback);
        curl_easy_setopt(c, CURLOPT_READDATA, (void *) upload);
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
 * Create the upload on the server, keeping the URL it answers in Location, and send the data along when it may
 */
static vectus_code upload_create_remote(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    int with_data = upload_creates_with_data(client, upload);
    vectus_code code = with_data ? upload_seek(upload) : VECTUS_OK;  /* back to the start after an earlier attempt */

    if (code == VECTUS_OK) {
        code = upload_request(client, upload, UPLOAD_POST, client->url, &response);
    }
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
        upload->expires = response.expires;
        upload->length_declared = upload->length != VECTUS_LENGTH_UNKNOWN;
        if (upload->url == NULL) {
            code = VECTUS_ERR_PROTOCOL;
        } else if (with_data && (!response.has_offset || response.offset > upload->length)) {
            code = VECTUS_ERR_PROTOCOL;  /* the URL stays, so the next perform asks the server what it kept */
        } else if (with_data) {
            upload->offset = response.offset;
        }
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
    } else if (!response.has_offset
               || (response.has_length && upload->length != VECTUS_LENGTH_UNKNOWN && response.length != upload->length)
               || response.offset > (response.has_length ? response.length : upload->length)) {
        code = VECTUS_ERR_PROTOCOL;
    } else {
        if (response.has_length) upload->length = response.length;  /* a deferred length declared earlier, maybe by another process */
        upload->length_declared = !response.length_deferred;
        upload->offset = response.offset;
        if (response.expires != 0) upload->expires = response.expires;  /* servers need only send it on POST and PATCH */
    }
    free(response.location);
    return code;
}

/**
 * Send the rest of the byte stream, from the offset the server has
 */
static vectus_code upload_send(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    int declaring = upload_declares_length(upload);  /* before the request, which may end the stream */
    vectus_code code = upload_seek(upload);

    if (code != VECTUS_OK) {
        return code;
    }

    code = upload_request(client, upload, UPLOAD_PATCH, upload->url, &response);
    if (code != VECTUS_OK) {
        /* keep the code; the next perform asks the server where it stopped */
    } else if (upload_gone(response.status)) {
        code = VECTUS_ERR_GONE;
    } else if (response.status != 204) {
        code = VECTUS_ERR_HTTP_STATUS;  /* 409 when the offset was stale; the next perform asks again */
    } else if (!response.has_offset || response.offset < upload->offset || response.offset > upload->length
               || (response.offset == upload->offset && upload->offset < upload->length)) {
        /* An offset going back, or standing still with data left to send, would loop forever.
           Only a PATCH declaring the length of a fully sent stream may leave it where it is */
        code = VECTUS_ERR_PROTOCOL;
    } else {
        upload->offset = response.offset;
        if (declaring) upload->length_declared = 1;
        if (response.expires != 0) upload->expires = response.expires;  /* a finished upload may no longer expire */
    }
    free(response.location);
    return code;
}

vectus_code vectus_upload_create(vectus_upload *upload, vectus_read_callback read_callback, vectus_seek_callback seek_callback,
                                 void *userdata, unsigned long long length) {
    *upload = NULL;
    if (length > UPLOAD_MAX_LENGTH && length != VECTUS_LENGTH_UNKNOWN) {
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
    (*upload)->deferred = length == VECTUS_LENGTH_UNKNOWN;
    (*upload)->length_declared = !(*upload)->deferred;
    return VECTUS_OK;
}

vectus_code vectus_upload_perform(vectus_client client, vectus_upload upload) {
    vectus_code code;

    if (upload->url == NULL) {
        if (!(client->extensions & VECTUS_EXT_CREATION)
            || (upload->length == VECTUS_LENGTH_UNKNOWN && !(client->extensions & VECTUS_EXT_CREATION_DEFER_LENGTH))) {
            return VECTUS_ERR_UNSUPPORTED;
        }
        if (client->max_size != 0 && upload->length != VECTUS_LENGTH_UNKNOWN && upload->length > client->max_size) {
            return VECTUS_ERR_TOO_LARGE;
        }
        code = upload_create_remote(client, upload);
    } else {
        code = upload_fetch_offset(client, upload);
    }

    while (code == VECTUS_OK && (upload->offset < upload->length || upload_declares_length(upload))) {
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
    upload->expires = 0;
    if (upload->deferred) upload->length = VECTUS_LENGTH_UNKNOWN;  /* learnt again from the stream or the server */
    return VECTUS_OK;
}

unsigned long long vectus_upload_offset(vectus_upload upload) {
    return upload->offset;
}

time_t vectus_upload_expires(vectus_upload upload) {
    return upload->expires;
}

/**
 * Base64-encode len bytes of in, padded as in RFC 4648, into out, which needs room for (len + 2) / 3 * 4 + 1 bytes
 */
static void upload_base64(const unsigned char *in, size_t len, char *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;

    for (i = 0; i < len; i += 3) {
        unsigned long bits = (unsigned long) in[i] << 16;

        if (i + 1 < len) bits |= (unsigned long) in[i + 1] << 8;
        if (i + 2 < len) bits |= in[i + 2];
        *out++ = alphabet[bits >> 18];
        *out++ = alphabet[(bits >> 12) & 63];
        *out++ = i + 1 < len ? alphabet[(bits >> 6) & 63] : '=';
        *out++ = i + 2 < len ? alphabet[bits & 63] : '=';
    }
    *out = '\0';
}

vectus_code vectus_upload_add_metadata(vectus_upload upload, const char *key, const char *value) {
    static const char prefix[] = "Upload-Metadata: ";
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    size_t used = upload->metadata != NULL ? strlen(upload->metadata) : 0;
    const char *pair = upload->metadata != NULL ? upload->metadata + sizeof prefix - 1 : NULL;
    size_t size;
    char *grown;
    char *end;
    size_t i;

    /* The key goes into the header as it is, where spaces and commas separate the pairs */
    if (key_len == 0) {
        return VECTUS_ERR_INVALID;
    }
    for (i = 0; i < key_len; i++) {
        unsigned char ch = (unsigned char) key[i];

        if (ch <= ' ' || ch > '~' || ch == ',') return VECTUS_ERR_INVALID;
    }
    while (pair != NULL) {
        if (strncmp(pair, key, key_len) == 0 && (pair[key_len] == ' ' || pair[key_len] == ',' || pair[key_len] == '\0')) {
            return VECTUS_ERR_INVALID;  /* keys must be unique */
        }
        pair = strchr(pair, ',');
        if (pair != NULL) pair++;
    }

    /* The prefix before the first pair and a comma before the others, then the key, a space and the Base64 value */
    size = (used == 0 ? sizeof prefix : used + 2) + key_len + 1 + (value_len + 2) / 3 * 4;
    grown = realloc(upload->metadata, size);
    if (grown == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    upload->metadata = grown;
    end = grown + used;
    end += snprintf(end, size - used, "%s%s", used == 0 ? prefix : ",", key);
    if (value_len > 0) {  /* an empty value may leave out the space too */
        *end++ = ' ';
        upload_base64((const unsigned char *) value, value_len, end);
    }
    return VECTUS_OK;
}

vectus_code vectus_upload_terminate(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    vectus_code code;

    if (upload->url == NULL) {
        return VECTUS_OK;
    }
    if (!(client->extensions & VECTUS_EXT_TERMINATION)) {
        return VECTUS_ERR_UNSUPPORTED;
    }

    code = upload_request(client, upload, UPLOAD_DELETE, upload->url, &response);
    if (code != VECTUS_OK) {
        /* keep the code */
    } else if (upload_gone(response.status)) {
        code = VECTUS_ERR_GONE;
    } else if (response.status != 204) {
        code = VECTUS_ERR_HTTP_STATUS;
    } else {
        vectus_upload_set_url(upload, NULL);  /* cannot fail with NULL */
    }
    free(response.location);
    return code;
}

void vectus_upload_cleanup(vectus_upload upload) {
    if (upload == NULL) {
        return;
    }
    free(upload->url);
    free(upload->metadata);
    free(upload);
}

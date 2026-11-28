/* SPDX-License-Identifier: MIT */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  /* fstat and fseeko, for files over 2 GiB */
#endif
#define _FILE_OFFSET_BITS 64  /* the same on 32-bit systems */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <vectus/vectus.h>

#include "internal.h"


/* HTTP clients such as curl count bytes in signed 64 bits, which caps the upload length */
#define UPLOAD_MAX_LENGTH 9223372036854775807ULL

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
     * the Upload-Metadata header's value sent on creation, NULL until a pair is added
     */
    char *metadata;

    /**
     * set for a partial upload, created with Upload-Concat: partial
     */
    int partial;

    /**
     * the Upload-Concat header's value creating a final upload, NULL for any other
     */
    char *final;

    /**
     * what went wrong with the read callback during the current request, NULL if nothing did
     */
    const char *source_failed;

    /**
     * called as the data goes out, NULL for none
     */
    vectus_progress_callback progress;
    void *progress_userdata;

    /**
     * set when the progress callback stopped the current request
     */
    int cancelled;

    /**
     * the file vectus_upload_create_file opened, closed on cleanup
     */
    FILE *file;

    /**
     * reused for every request, so they can share a connection
     */
    struct vectus_connection connection;

    /**
     * the HTTP status of the last response, 0 if none came
     */
    long status;

    /**
     * why the last perform or terminate failed, empty after one succeeded
     */
    char error[VECTUS_ERROR_SIZE];
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
    int out_of_memory;  /* storing Location failed */
};

enum upload_method { UPLOAD_POST, UPLOAD_HEAD, UPLOAD_PATCH, UPLOAD_DELETE };

/**
 * One request about the upload, and its response, which the transport's callbacks get as their userdata
 */
struct upload_exchange {
    vectus_upload upload;
    struct upload_response *response;
};

static void upload_handle_header(void *userdata, const char *name, const char *value) {
    struct upload_response *response = ((struct upload_exchange *) userdata)->response;

    if (vectus_strequal(name, "Location")) {
        free(response->location);
        response->location = malloc(strlen(value) + 1);
        if (response->location != NULL) strcpy(response->location, value);
        else response->out_of_memory = 1;
    } else if (vectus_strequal(name, "Upload-Offset")) {
        response->has_offset = vectus_parse_size(value, &response->offset);
    } else if (vectus_strequal(name, "Upload-Length")) {
        response->has_length = vectus_parse_size(value, &response->length);
    } else if (vectus_strequal(name, "Upload-Defer-Length")) {
        response->length_deferred = strcmp(value, "1") == 0;
    } else if (vectus_strequal(name, "Upload-Expires")) {
        long long expires = vectus_parse_http_date(value);  /* -1 when it is not a date */

        if (expires > 0 && (long long) (time_t) expires == expires) response->expires = (time_t) expires;
    }
}

/**
 * Feed the transport from the read callback, never past the upload's length, keeping track of the stream position
 */
static size_t upload_read(void *userdata, char *buffer, size_t wanted) {
    vectus_upload upload = ((struct upload_exchange *) userdata)->upload;
    unsigned long long remaining = upload->length - upload->position;
    size_t bytes;

    if (wanted > remaining) wanted = (size_t) remaining;
    if (wanted == 0) {
        return 0;
    }

    bytes = upload->read_callback(upload->userdata, buffer, wanted);
    if (bytes == 0 && upload->length == VECTUS_LENGTH_UNKNOWN) {
        upload->length = upload->position;  /* the end of a stream announced without a length */
        return 0;
    }
    if (bytes == VECTUS_READ_ABORT) {
        upload->source_failed = "the read callback aborted";
    } else if (bytes == 0) {
        upload->source_failed = "the data ended before the upload's length";
    } else if (bytes > wanted) {
        upload->source_failed = "the read callback returned more than asked for";
    }
    if (upload->source_failed != NULL) {
        return VECTUS_READ_ABORT;
    }
    upload->position += bytes;
    return bytes;
}

/**
 * Report to the progress callback how far the data has gone, from where the server stood when the request started
 */
static int upload_progress(void *userdata, unsigned long long sent) {
    vectus_upload upload = ((struct upload_exchange *) userdata)->upload;
    unsigned long long offset = upload->offset + sent;

    if (sent > 0 && offset > upload->position) {
        offset = upload->position;  /* a transport may count the chunked encoding's framing too, but no more went than was read */
    }
    if (upload->progress(upload->progress_userdata, offset, upload->length) != 0) {
        upload->cancelled = 1;
        return 1;
    }
    return 0;
}

/**
 * Note why the upload stopped, for vectus_upload_error
 */
static vectus_code upload_fail(vectus_upload upload, vectus_code code, const char *message) {
    snprintf(upload->error, sizeof upload->error, "%s", message);
    return code;
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
            snprintf(upload->error, sizeof upload->error, "could not seek the data to byte %llu%s", upload->offset,
                     upload->seek_callback == NULL ? ": the upload has no seek callback" : "");
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
    static const char *const names[] = {"POST", "HEAD", "PATCH", "DELETE"};
    struct upload_exchange exchange;
    vectus_request request = {0};
    const char *headers[16];
    char length[24];
    char offset[24];
    size_t n = 0;
    int with_data = method == UPLOAD_PATCH || (method == UPLOAD_POST && upload_creates_with_data(client, upload));
    int overridden = client->method_override && (method == UPLOAD_PATCH || method == UPLOAD_DELETE);
    vectus_code code;

    snprintf(length, sizeof length, "%llu", upload->length);
    snprintf(offset, sizeof offset, "%llu", upload->offset);
    headers[n++] = "Tus-Resumable";
    headers[n++] = "1.0.0";
    if (method == UPLOAD_POST) {
        if (upload->final != NULL) {
            headers[n++] = "Upload-Concat";  /* the server sums up the length */
            headers[n++] = upload->final;
        } else if (upload->length == VECTUS_LENGTH_UNKNOWN) {
            headers[n++] = "Upload-Defer-Length";
            headers[n++] = "1";
        } else {
            headers[n++] = "Upload-Length";
            headers[n++] = length;
        }
        if (upload->partial && upload->final == NULL) {
            headers[n++] = "Upload-Concat";
            headers[n++] = "partial";
        }
        if (upload->metadata != NULL) {
            headers[n++] = "Upload-Metadata";
            headers[n++] = upload->metadata;
        }
    } else if (method == UPLOAD_PATCH) {
        headers[n++] = "Upload-Offset";
        headers[n++] = offset;
        if (upload_declares_length(upload)) {
            headers[n++] = "Upload-Length";
            headers[n++] = length;
        }
    }
    if (overridden) {
        headers[n++] = "X-HTTP-Method-Override";
        headers[n++] = names[method];
    }
    if (with_data) {
        headers[n++] = "Content-Type";
        headers[n++] = "application/offset+octet-stream";
    }
    headers[n] = NULL;

    exchange.upload = upload;
    exchange.response = response;
    request.method = overridden ? "POST" : names[method];
    request.url = url;
    request.headers = headers;
    if (method == UPLOAD_POST || method == UPLOAD_PATCH) {
        request.read = upload_read;  /* a POST without data still goes out with an empty body */
        request.length = !with_data ? 0
                         : method == UPLOAD_POST || upload->length == VECTUS_LENGTH_UNKNOWN ? upload->length
                         : upload->length - upload->offset;
        /* A creation POST lets the server refuse before the data goes out, as the spec asks; a PATCH sends it at once */
        request.expect_continue = method == UPLOAD_POST && with_data;
    }
    if (upload->progress != NULL) request.progress = upload_progress;
    request.header = upload_handle_header;
    request.userdata = &exchange;

    upload->source_failed = NULL;
    upload->cancelled = 0;
    code = vectus_send(client, &upload->connection, names[method], &request, upload->error, &upload->status);
    response->status = upload->status;
    if (code == VECTUS_OK && response->out_of_memory) {
        snprintf(upload->error, sizeof upload->error, "%s: out of memory", names[method]);
        code = VECTUS_ERR_MEMORY;
    } else if (code != VECTUS_OK && upload->cancelled) {
        snprintf(upload->error, sizeof upload->error, "%s: cancelled by the progress callback", names[method]);
        code = VECTUS_ERR_CANCELLED;
    } else if (code != VECTUS_OK && upload->source_failed != NULL) {
        snprintf(upload->error, sizeof upload->error, "%s, at byte %llu", upload->source_failed, upload->position);
        code = VECTUS_ERR_SOURCE;
    }
    return code;
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
        code = upload_fail(upload, VECTUS_ERR_PROTOCOL, "POST answered 201 without a Location header");
    } else {
        vectus_code resolved = vectus_resolve_url(client->url, response.location, &upload->url);  /* it may be relative */

        upload->offset = 0;
        upload->expires = response.expires;
        upload->length_declared = upload->length != VECTUS_LENGTH_UNKNOWN;
        if (resolved == VECTUS_ERR_MEMORY) {
            code = upload_fail(upload, VECTUS_ERR_MEMORY, "POST: out of memory");
        } else if (resolved != VECTUS_OK) {
            code = upload_fail(upload, VECTUS_ERR_PROTOCOL, "POST answered a Location header that is not a URL");
        } else if (with_data && (!response.has_offset || response.offset > upload->length)) {
            /* the URL stays, so the next perform asks the server what it kept */
            code = upload_fail(upload, VECTUS_ERR_PROTOCOL, "POST carrying data answered without a valid Upload-Offset");
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
    } else if (!response.has_offset) {
        code = upload_fail(upload, VECTUS_ERR_PROTOCOL, "HEAD answered without Upload-Offset");
    } else if (response.has_length && upload->length != VECTUS_LENGTH_UNKNOWN && response.length != upload->length) {
        snprintf(upload->error, sizeof upload->error, "the server has the upload as %llu bytes, not %llu", response.length, upload->length);
        code = VECTUS_ERR_PROTOCOL;
    } else if (response.offset > (response.has_length ? response.length : upload->length)) {
        code = upload_fail(upload, VECTUS_ERR_PROTOCOL, "HEAD answered an Upload-Offset past the upload's length");
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
    } else if (!response.has_offset) {
        code = upload_fail(upload, VECTUS_ERR_PROTOCOL, "PATCH answered without Upload-Offset");
    } else if (response.offset < upload->offset || response.offset > upload->length
               || (response.offset == upload->offset && upload->offset < upload->length)) {
        /* An offset going back, or standing still with data left to send, would loop forever.
           Only a PATCH declaring the length of a fully sent stream may leave it where it is */
        snprintf(upload->error, sizeof upload->error, "PATCH from byte %llu answered an impossible Upload-Offset of %llu",
                 upload->offset, response.offset);
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
    if (upload == NULL) {
        return VECTUS_ERR_INVALID;
    }
    *upload = NULL;
    if (read_callback == NULL) {
        return VECTUS_ERR_INVALID;
    }
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

static size_t upload_file_read(void *userdata, char *buffer, size_t size) {
    FILE *file = userdata;
    size_t bytes = fread(buffer, 1, size, file);

    return bytes == 0 && ferror(file) ? VECTUS_READ_ABORT : bytes;
}

static int upload_file_seek(void *userdata, unsigned long long offset) {
    return fseeko(userdata, (off_t) offset, SEEK_SET);
}

vectus_code vectus_upload_create_file(vectus_upload *upload, const char *path) {
    struct stat st;
    FILE *file;
    vectus_code code;
    int reason;

    if (upload == NULL) {
        return VECTUS_ERR_INVALID;
    }
    *upload = NULL;
    if (path == NULL) {
        return VECTUS_ERR_INVALID;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return VECTUS_ERR_SOURCE;
    }
    if (fstat(fileno(file), &st) != 0) {
        reason = errno;
    } else if (!S_ISREG(st.st_mode)) {
        reason = S_ISDIR(st.st_mode) ? EISDIR : ESPIPE;  /* nothing with a length to seek in */
    } else {
        code = vectus_upload_create(upload, upload_file_read, upload_file_seek, file, (unsigned long long) st.st_size);
        if (code == VECTUS_OK) {
            (*upload)->file = file;
        } else {
            fclose(file);
        }
        return code;
    }
    fclose(file);
    errno = reason;  /* for the caller to tell why */
    return VECTUS_ERR_SOURCE;
}

void vectus_upload_set_partial(vectus_upload upload) {
    if (upload == NULL) {
        return;
    }
    upload->partial = 1;
}

void vectus_upload_set_progress(vectus_upload upload, vectus_progress_callback callback, void *userdata) {
    if (upload == NULL) {
        return;
    }
    upload->progress = callback;
    upload->progress_userdata = userdata;
}

vectus_code vectus_upload_create_final(vectus_upload *upload, const char *const *partial_urls, size_t count) {
    static const char prefix[] = "final;";
    size_t size = sizeof prefix;
    char *end;
    size_t i;

    if (upload == NULL) {
        return VECTUS_ERR_INVALID;
    }
    *upload = NULL;
    if (partial_urls == NULL || count == 0) {
        return VECTUS_ERR_INVALID;
    }
    /* The URLs go into the header as they are, separated by spaces */
    for (i = 0; i < count; i++) {
        const char *c = partial_urls[i];

        if (c == NULL || *c == '\0') return VECTUS_ERR_INVALID;
        for (; *c != '\0'; c++) {
            if ((unsigned char) *c <= ' ' || (unsigned char) *c > '~') return VECTUS_ERR_INVALID;
        }
        size += (size_t) (c - partial_urls[i]) + 1;  /* and a space or the terminator */
    }

    *upload = calloc(1, sizeof(struct vectus_upload_s));
    if (*upload == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    (*upload)->length_declared = 1;
    (*upload)->final = malloc(size);
    if ((*upload)->final == NULL) {
        vectus_upload_cleanup(*upload);
        *upload = NULL;
        return VECTUS_ERR_MEMORY;
    }
    end = (*upload)->final + snprintf((*upload)->final, size, "%s%s", prefix, partial_urls[0]);
    for (i = 1; i < count; i++) {
        end += snprintf(end, size - (size_t) (end - (*upload)->final), " %s", partial_urls[i]);
    }
    return VECTUS_OK;
}

/**
 * Ask the server what it offers, unless the client already knows, keeping the details on the upload
 */
static vectus_code upload_discover(vectus_client client, vectus_upload upload) {
    vectus_code code;

    if (client->discovered) {
        return VECTUS_OK;
    }
    code = vectus_discover(client, &upload->connection);
    upload->status = client->status;
    memcpy(upload->error, client->error, sizeof upload->error);
    return code;
}

/**
 * Check the server offers what creating the upload needs
 */
static vectus_code upload_creatable(vectus_client client, vectus_upload upload) {
    if (!(client->extensions & VECTUS_EXT_CREATION)) {
        return upload_fail(upload, VECTUS_ERR_UNSUPPORTED, "the server does not offer the creation extension");
    }
    if (upload->length == VECTUS_LENGTH_UNKNOWN && !(client->extensions & VECTUS_EXT_CREATION_DEFER_LENGTH)) {
        return upload_fail(upload, VECTUS_ERR_UNSUPPORTED, "the server does not offer creation-defer-length, for an unknown length");
    }
    if ((upload->partial || upload->final != NULL) && !(client->extensions & VECTUS_EXT_CONCATENATION)) {
        return upload_fail(upload, VECTUS_ERR_UNSUPPORTED, "the server does not offer concatenation, for partial and final uploads");
    }
    if (client->max_size != 0 && upload->length != VECTUS_LENGTH_UNKNOWN && upload->length > client->max_size) {
        snprintf(upload->error, sizeof upload->error, "the upload's %llu bytes are over the server's Tus-Max-Size of %llu",
                 upload->length, client->max_size);
        return VECTUS_ERR_TOO_LARGE;
    }
    return VECTUS_OK;
}

vectus_code vectus_upload_perform(vectus_client client, vectus_upload upload) {
    vectus_code code;

    if (client == NULL || upload == NULL) {
        return VECTUS_ERR_INVALID;
    }
    upload->status = 0;
    upload->error[0] = '\0';

    if (upload->url == NULL) {
        code = upload_discover(client, upload);  /* only creating needs to know the extensions */
        if (code == VECTUS_OK) code = upload_creatable(client, upload);
        if (code == VECTUS_OK) code = upload_create_remote(client, upload);
    } else if (upload->final != NULL) {
        return VECTUS_OK;  /* the server assembled it: nothing to send, and a PATCH would get 403 */
    } else {
        code = upload_fetch_offset(client, upload);
    }

    while (code == VECTUS_OK && (upload->offset < upload->length || upload_declares_length(upload))) {
        code = upload_send(client, upload);
    }
    if (code == VECTUS_OK) upload->error[0] = '\0';  /* drop the status of the last, successful, response */
    return code;
}

const char *vectus_upload_url(vectus_upload upload) {
    return upload != NULL ? upload->url : NULL;
}

vectus_code vectus_upload_set_url(vectus_upload upload, const char *url) {
    char *copy = NULL;

    if (upload == NULL) {
        return VECTUS_ERR_INVALID;
    }
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
    return upload != NULL ? upload->offset : 0;
}

time_t vectus_upload_expires(vectus_upload upload) {
    return upload != NULL ? upload->expires : 0;
}

long vectus_upload_http_status(vectus_upload upload) {
    return upload != NULL ? upload->status : 0;
}

const char *vectus_upload_error(vectus_upload upload) {
    return upload != NULL ? upload->error : "";
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
    size_t key_len;
    size_t value_len;
    size_t used;
    const char *pair;
    size_t size;
    char *grown;
    char *end;
    size_t i;

    if (upload == NULL || key == NULL || value == NULL) {
        return VECTUS_ERR_INVALID;
    }
    key_len = strlen(key);
    value_len = strlen(value);
    used = upload->metadata != NULL ? strlen(upload->metadata) : 0;
    pair = upload->metadata;

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

    /* A comma before all but the first pair, then the key, a space and the Base64 value */
    size = (used == 0 ? 1 : used + 2) + key_len + 1 + (value_len + 2) / 3 * 4;
    grown = realloc(upload->metadata, size);
    if (grown == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    upload->metadata = grown;
    end = grown + used;
    end += snprintf(end, size - used, "%s%s", used == 0 ? "" : ",", key);
    if (value_len > 0) {  /* an empty value may leave out the space too */
        *end++ = ' ';
        upload_base64((const unsigned char *) value, value_len, end);
    }
    return VECTUS_OK;
}

vectus_code vectus_upload_terminate(vectus_client client, vectus_upload upload) {
    struct upload_response response = {0};
    vectus_code code;

    if (client == NULL || upload == NULL) {
        return VECTUS_ERR_INVALID;
    }
    upload->status = 0;
    upload->error[0] = '\0';
    if (upload->url == NULL) {
        return VECTUS_OK;
    }
    code = upload_discover(client, upload);
    if (code != VECTUS_OK) {
        return code;
    }
    if (!(client->extensions & VECTUS_EXT_TERMINATION)) {
        return upload_fail(upload, VECTUS_ERR_UNSUPPORTED, "the server does not offer the termination extension");
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
        upload->error[0] = '\0';
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
    free(upload->final);
    if (upload->file != NULL) fclose(upload->file);
    vectus_close(&upload->connection);
    free(upload);
}

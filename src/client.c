/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <vectus/client.h>

#include "internal.h"


/**
 * Convert Tus-Extensions into the extensions flags
 * Something like creation,creation-with-upload,termination,concatenation,creation-defer-length
 *
 * @param extensions
 * @return
 */
static unsigned int parse_extensions(char *extensions) {
    unsigned int ext_flag = 0;
    char *current = extensions;
    char *next = NULL;

    for (;;) {
        next = strchr(current, ',');
        if (next != NULL) *next = '\0';

        /* HTTP lists allow whitespace around the commas */
        current += strspn(current, " \t");
        current[strcspn(current, " \t")] = '\0';

        if (strcmp(current, "creation") == 0) {
            ext_flag |= VECTUS_EXT_CREATION;
        } else if (strcmp(current, "creation-with-upload") == 0) {
            ext_flag |= VECTUS_EXT_CREATION_WITH_UPLOAD;
        } else if (strcmp(current, "termination") == 0) {
            ext_flag |= VECTUS_EXT_TERMINATION;
        } else if (strcmp(current, "concatenation") == 0) {
            ext_flag |= VECTUS_EXT_CONCATENATION;
        } else if (strcmp(current, "expiration") == 0) {
            ext_flag |= VECTUS_EXT_EXPIRATION;
        } else if (strcmp(current, "checksum") == 0) {
            ext_flag |= VECTUS_EXT_CHECKSUM;
        } else if (strcmp(current, "creation-defer-length") == 0) {
            ext_flag |= VECTUS_EXT_CREATION_DEFER_LENGTH;
        }

        if (next == NULL) break;
        current = next + 1;
    }

    return ext_flag;
}

size_t vectus_parse_header_line(const char *buffer, size_t bytes, void (*handle)(void *target, const char *name, char *value), void *target) {
    char *line = malloc(bytes + 1);  /* curl's buffer is not null-terminated, so parse a terminated copy */
    char *value;
    char *end;

    if (line == NULL) {
        return 0;
    }
    memcpy(line, buffer, bytes);
    line[bytes] = '\0';

    value = strchr(line, ':');
    if (value != NULL) {
        *value++ = '\0';  /* line is now the null-terminated header name */

        /* Trim whitespace and the CRLF around the value */
        value += strspn(value, " \t");
        end = value + strlen(value);
        while (end > value && strchr(" \t\r\n", end[-1]) != NULL) end--;
        *end = '\0';

        handle(target, line, value);
    }

    free(line);
    return bytes;
}

/**
 * A curl write callback that drops the response body, which curl would otherwise print to the application's stdout
 */
static size_t discard_body(char *buffer, size_t size, size_t nitems, void *userdata) {
    (void) buffer;
    (void) userdata;
    return size * nitems;
}

int vectus_parse_size(const char *value, unsigned long long *size) {
    unsigned long long parsed;

    /* Digits only: strtoull would also accept a sign and wrap "-1" around to the maximum */
    if (*value == '\0' || value[strspn(value, "0123456789")] != '\0') {
        return 0;
    }
    errno = 0;
    parsed = strtoull(value, NULL, 10);
    if (errno == ERANGE) {
        return 0;
    }
    *size = parsed;
    return 1;
}

/**
 * Store one response header into the client asking the server what it offers
 */
static void client_handle_header(void *target, const char *name, char *value) {
    vectus_client v = target;

    /* Header names are case-insensitive, and HTTP/2 sends them lowercase */
    if (curl_strequal(name, "Tus-Version")) {
        free(v->version);
        v->version = malloc(strlen(value) + 1);
        if (v->version != NULL) strcpy(v->version, value);
        else v->out_of_memory = 1;
    } else if (curl_strequal(name, "Tus-Extension")) {
        v->extensions = parse_extensions(value);
    } else if (curl_strequal(name, "Tus-Max-Size")) {
        vectus_parse_size(value, &v->max_size);  /* an invalid value leaves it at 0, meaning no known limit */
    }
}

/**
 * Used to fetch the tus extensions the server implements while discovering it
 *
 * @param buffer one header line, not null-terminated
 * @param size
 * @param nitems
 * @param userdata the vectus_client discovering the server
 * @return the bytes handled; anything else makes curl abort the transfer
 */
static size_t client_init_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    return vectus_parse_header_line(buffer, size * nitems, client_handle_header, userdata);
}

/**
 * Whether a Tus-Version list such as "1.0.0,0.2.2" offers 1.0.0, the version vectus speaks
 */
static int offers_tus_1_0_0(const char *versions) {
    while (*versions != '\0') {
        size_t len;

        versions += strspn(versions, " \t,");
        len = strcspn(versions, " \t,");
        if (len == 5 && strncmp(versions, "1.0.0", 5) == 0) return 1;
        versions += len;
    }
    return 0;
}

const char *vectus_strerror(vectus_code code) {
    switch (code) {
    case VECTUS_OK: return "no error";
    case VECTUS_ERR_MEMORY: return "out of memory";
    case VECTUS_ERR_TRANSPORT: return "could not reach the server";
    case VECTUS_ERR_HTTP_STATUS: return "the server answered with an unexpected HTTP status";
    case VECTUS_ERR_PROTOCOL: return "the server does not offer tus 1.0.0, or broke the protocol";
    case VECTUS_ERR_TOO_LARGE: return "the upload is too large";
    case VECTUS_ERR_GONE: return "the upload no longer exists on the server";
    case VECTUS_ERR_SOURCE: return "the data to upload could not be read";
    case VECTUS_ERR_UNSUPPORTED: return "the server does not offer the tus extension this needs";
    case VECTUS_ERR_INVALID: return "invalid argument";
    case VECTUS_ERR_CANCELLED: return "cancelled by the progress callback";
    }
    return "unknown error";
}

vectus_code vectus_request(vectus_client client, CURL *c, const char *method, struct curl_slist **headers, char *error, long *status) {
    char reason[CURL_ERROR_SIZE] = "";
    struct curl_slist *h;
    CURLcode res;

    *status = 0;
    for (h = client->headers; h != NULL; h = h->next) {
        struct curl_slist *appended = curl_slist_append(*headers, h->data);

        if (appended == NULL) {
            snprintf(error, VECTUS_ERROR_SIZE, "%s: out of memory", method);
            return VECTUS_ERR_MEMORY;
        }
        *headers = appended;
    }

    curl_easy_setopt(c, CURLOPT_HTTPHEADER, *headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, reason);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  /* curl's signal-based DNS timeout is not safe with threads */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* give up on a stalled connection, so the upload can resume */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, client->stall_timeout);
    if (client->curl_callback != NULL) {
        client->curl_callback(client->curl_userdata, c);
    }

    res = curl_easy_perform(c);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, NULL);  /* reason goes out of scope */
    if (res != CURLE_OK) {
        /* shortcut: every curl failure becomes VECTUS_ERR_TRANSPORT; split it when callers need to tell DNS, connection and TLS errors apart */
        snprintf(error, VECTUS_ERROR_SIZE, "%s: %.200s", method, reason[0] != '\0' ? reason : curl_easy_strerror(res));
        return res == CURLE_OUT_OF_MEMORY ? VECTUS_ERR_MEMORY : VECTUS_ERR_TRANSPORT;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    snprintf(error, VECTUS_ERROR_SIZE, "%s answered HTTP %ld", method, *status);  /* what went wrong, unless the caller finds otherwise */
    return VECTUS_OK;
}

vectus_code vectus_client_create(vectus_client *client, const char *url) {
    CURLU *u;
    char *scheme = NULL;
    vectus_code code = VECTUS_ERR_MEMORY;

    if (client == NULL) {
        return VECTUS_ERR_INVALID;
    }
    *client = NULL;
    if (url == NULL) {
        return VECTUS_ERR_INVALID;
    }
    u = curl_url();
    if (u == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    if (curl_url_set(u, CURLUPART_URL, url, 0) != CURLUE_OK || curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK
        || !(curl_strequal(scheme, "http") || curl_strequal(scheme, "https"))) {
        code = VECTUS_ERR_INVALID;
    } else {
        *client = calloc(1, sizeof(struct vectus_client_s));  /* zeroed: no headers, nothing discovered */
        if (*client != NULL) (*client)->url = malloc(strlen(url) + 1);
        if (*client != NULL && (*client)->url != NULL) {
            strcpy((*client)->url, url);
            (*client)->stall_timeout = VECTUS_DEFAULT_STALL_TIMEOUT;
            code = VECTUS_OK;
        } else {
            vectus_client_cleanup(*client);
            *client = NULL;
        }
    }
    curl_free(scheme);
    curl_url_cleanup(u);
    return code;
}

/**
 * Whether name is one of the headers tus or vectus set themselves
 */
static int header_reserved(const char *name) {
    static const char *const reserved[] = {"Content-Type", "Content-Length", "Transfer-Encoding", "Expect", "X-HTTP-Method-Override"};
    size_t i;

    if (curl_strnequal(name, "Tus-", 4) || curl_strnequal(name, "Upload-", 7)) {
        return 1;
    }
    for (i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        if (curl_strequal(name, reserved[i])) return 1;
    }
    return 0;
}

vectus_code vectus_client_set_header(vectus_client client, const char *name, const char *value) {
    size_t name_len;
    struct curl_slist *kept = NULL;
    struct curl_slist *appended;
    struct curl_slist *h;
    char *line;
    const char *ch;

    if (client == NULL || name == NULL) {
        return VECTUS_ERR_INVALID;
    }
    /* A token, as HTTP defines header names, which also keeps out spaces, colons and line breaks */
    name_len = strlen(name);
    if (name_len == 0 || name[strspn(name, "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")] != '\0'
        || header_reserved(name)) {
        return VECTUS_ERR_INVALID;
    }
    for (ch = value; ch != NULL && *ch != '\0'; ch++) {
        if (((unsigned char) *ch < ' ' && *ch != '\t') || *ch == 0x7f) return VECTUS_ERR_INVALID;
    }

    /* Build the new list aside, so running out of memory leaves the old one as it was */
    for (h = client->headers; h != NULL; h = h->next) {
        if (curl_strnequal(h->data, name, name_len) && (h->data[name_len] == ':' || h->data[name_len] == ';')) {
            continue;  /* the header being replaced or removed */
        }
        appended = curl_slist_append(kept, h->data);
        if (appended == NULL) {
            curl_slist_free_all(kept);
            return VECTUS_ERR_MEMORY;
        }
        kept = appended;
    }
    if (value != NULL) {
        size_t size = name_len + strlen(value) + 3;

        line = malloc(size);
        appended = NULL;
        if (line != NULL) {
            /* curl drops a header written "Name:", and sends "Name;" as one with an empty value */
            snprintf(line, size, *value == '\0' ? "%s;" : "%s: %s", name, value);
            appended = curl_slist_append(kept, line);
            free(line);
        }
        if (appended == NULL) {
            curl_slist_free_all(kept);
            return VECTUS_ERR_MEMORY;
        }
        kept = appended;
    }
    curl_slist_free_all(client->headers);
    client->headers = kept;
    return VECTUS_OK;
}

void vectus_client_set_curl_callback(vectus_client client, vectus_curl_callback callback, void *userdata) {
    if (client == NULL) {
        return;
    }
    client->curl_callback = callback;
    client->curl_userdata = userdata;
}

vectus_code vectus_client_discover(vectus_client client) {
    CURL *c;
    vectus_code code;

    if (client == NULL) {
        return VECTUS_ERR_INVALID;
    }
    c = curl_easy_init();
    if (c == NULL) {
        snprintf(client->error, sizeof client->error, "OPTIONS: out of memory");
        return VECTUS_ERR_MEMORY;
    }
    code = vectus_discover(client, c);
    curl_easy_cleanup(c);
    return code;
}

vectus_code vectus_discover(vectus_client client, CURL *c) {
    struct curl_slist *headers = NULL;
    vectus_code code;

    /* Forget what an earlier answer said */
    free(client->version);
    client->version = NULL;
    client->extensions = 0;
    client->max_size = 0;
    client->discovered = 0;
    client->out_of_memory = 0;
    client->status = 0;

    curl_easy_setopt(c, CURLOPT_URL, client->url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "OPTIONS");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, client_init_header_callback);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *) client);
    code = vectus_request(client, c, "OPTIONS", &headers, client->error, &client->status);
    curl_slist_free_all(headers);

    if (code != VECTUS_OK) {
        /* keep the code and curl's reason */
    } else if (client->out_of_memory) {
        snprintf(client->error, sizeof client->error, "OPTIONS: out of memory");
        code = VECTUS_ERR_MEMORY;
    } else if (client->status < 200 || client->status > 299) {  /* tus servers answer OPTIONS with 200 or 204 */
        code = VECTUS_ERR_HTTP_STATUS;
    } else if (client->version == NULL) {
        snprintf(client->error, sizeof client->error, "OPTIONS answered without Tus-Version: is this a tus endpoint?");
        code = VECTUS_ERR_PROTOCOL;
    } else if (!offers_tus_1_0_0(client->version)) {
        snprintf(client->error, sizeof client->error, "the server offers tus %.200s, not 1.0.0", client->version);
        code = VECTUS_ERR_PROTOCOL;
    } else {
        client->error[0] = '\0';
        client->discovered = 1;
    }
    return code;
}

unsigned int vectus_client_extensions(vectus_client client) {
    return client != NULL ? client->extensions : 0;
}

unsigned long long vectus_client_max_size(vectus_client client) {
    return client != NULL ? client->max_size : 0;
}

long vectus_client_http_status(vectus_client client) {
    return client != NULL ? client->status : 0;
}

const char *vectus_client_error(vectus_client client) {
    return client != NULL ? client->error : "";
}

void vectus_client_set_method_override(vectus_client client, int enabled) {
    if (client == NULL) {
        return;
    }
    client->method_override = enabled != 0;
}

void vectus_client_cleanup(vectus_client client) {
    if (client == NULL) {
        return;
    }
    free(client->url);
    free(client->version);
    curl_slist_free_all(client->headers);
    free(client);
}

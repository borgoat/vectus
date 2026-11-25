/* SPDX-License-Identifier: MIT */

#include <errno.h>
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
 * Store one response header into the client being initialised
 */
static void client_handle_header(void *target, const char *name, char *value) {
    vectus_client v = target;

    /* Header names are case-insensitive, and HTTP/2 sends them lowercase */
    if (curl_strequal(name, "Tus-Version")) {
        free(v->version);
        v->version = malloc(strlen(value) + 1);
        if (v->version != NULL) strcpy(v->version, value);
    } else if (curl_strequal(name, "Tus-Resumable")) {
        /* TODO */
    } else if (curl_strequal(name, "Tus-Extension")) {
        v->extensions = parse_extensions(value);
    } else if (curl_strequal(name, "Tus-Max-Size")) {
        vectus_parse_size(value, &v->max_size);  /* an invalid value leaves it at 0, meaning no known limit */
    }
}

/**
 * Used to fetch the tus extensions the server implements while initialising our vectus_client
 *
 * @param buffer one header line, not null-terminated
 * @param size
 * @param nitems
 * @param userdata the vectus_client being initialised
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
    }
    return "unknown error";
}

vectus_code vectus_client_init(vectus_client *client, const char *url) {
    vectus_client v = calloc(1, sizeof(struct vectus_client_s));  /* zeroed, so cleanup is safe when the server sends no Tus-Version */
    vectus_code code = VECTUS_ERR_MEMORY;
    CURL *c = NULL;
    long status = 0;

    *client = NULL;
    if (v == NULL) {
        return VECTUS_ERR_MEMORY;
    }

    v->url = calloc(strlen(url) + 1, sizeof(char));
    c = curl_easy_init();
    if (v->url == NULL || c == NULL) {
        goto fail;
    }
    strcpy(v->url, url);
    v->stall_timeout = VECTUS_DEFAULT_STALL_TIMEOUT;

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "OPTIONS");
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* give up on a stalled server instead of hanging */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, v->stall_timeout);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, client_init_header_callback);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *) v);

    /* shortcut: every curl failure becomes VECTUS_ERR_TRANSPORT; split it when callers need to tell DNS, connection and TLS errors apart */
    code = VECTUS_ERR_TRANSPORT;
    if (curl_easy_perform(c) != CURLE_OK) {
        goto fail;
    }

    code = VECTUS_ERR_HTTP_STATUS;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status > 299) {  /* tus servers answer OPTIONS with 200 or 204 */
        goto fail;
    }

    code = VECTUS_ERR_PROTOCOL;
    if (v->version == NULL || !offers_tus_1_0_0(v->version)) {
        goto fail;
    }

    curl_easy_cleanup(c);
    *client = v;
    return VECTUS_OK;

fail:
    curl_easy_cleanup(c);  /* no-op on NULL */
    vectus_client_cleanup(v);
    return code;
}

unsigned int vectus_client_extensions(vectus_client client) {
    return client != NULL ? client->extensions : 0;
}

unsigned long long vectus_client_max_size(vectus_client client) {
    return client != NULL ? client->max_size : 0;
}

void vectus_client_cleanup(vectus_client client) {
    if (client == NULL) {
        return;
    }
    free(client->url);
    free(client->version);
    free(client);
}

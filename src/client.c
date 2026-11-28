/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <vectus/client.h>

#include "internal.h"


/**
 * A copy of s to be freed, or NULL out of memory
 */
static char *client_copy(const char *s) {
    char *copy = malloc(strlen(s) + 1);

    if (copy != NULL) strcpy(copy, s);
    return copy;
}

/**
 * Convert Tus-Extension, such as "creation,creation-with-upload,termination", into the extensions flags
 */
static unsigned int parse_extensions(const char *extensions) {
    static const struct {
        const char *name;
        unsigned int flag;
    } known[] = {
        {"creation", VECTUS_EXT_CREATION},
        {"creation-with-upload", VECTUS_EXT_CREATION_WITH_UPLOAD},
        {"termination", VECTUS_EXT_TERMINATION},
        {"concatenation", VECTUS_EXT_CONCATENATION},
        {"expiration", VECTUS_EXT_EXPIRATION},
        {"checksum", VECTUS_EXT_CHECKSUM},
        {"creation-defer-length", VECTUS_EXT_CREATION_DEFER_LENGTH},
    };
    unsigned int flags = 0;
    size_t i;

    while (*extensions != '\0') {
        size_t len;

        extensions += strspn(extensions, " \t,");  /* HTTP lists allow whitespace around the commas */
        len = strcspn(extensions, " \t,");
        for (i = 0; i < sizeof known / sizeof known[0]; i++) {
            if (strlen(known[i].name) == len && strncmp(extensions, known[i].name, len) == 0) flags |= known[i].flag;
        }
        extensions += len;
    }
    return flags;
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
static void client_handle_header(void *userdata, const char *name, const char *value) {
    vectus_client v = userdata;

    /* Header names are case-insensitive, and HTTP/2 sends them lowercase */
    if (curl_strequal(name, "Tus-Version")) {
        free(v->version);
        v->version = client_copy(value);
        if (v->version == NULL) v->out_of_memory = 1;
    } else if (curl_strequal(name, "Tus-Extension")) {
        v->extensions = parse_extensions(value);
    } else if (curl_strequal(name, "Tus-Max-Size")) {
        vectus_parse_size(value, &v->max_size);  /* an invalid value leaves it at 0, meaning no known limit */
    }
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

void vectus_close(struct vectus_connection *connection) {
    if (connection->handle != NULL) connection->transport->close(connection->handle);
    connection->handle = NULL;
}

vectus_code vectus_send(vectus_client client, struct vectus_connection *connection, const char *name, vectus_request *request,
                        char *error, long *status) {
    const char *const *own = request->headers;
    const char **headers;
    char reason[VECTUS_ERROR_SIZE] = "";
    size_t own_count = 0;
    size_t app_count = 0;
    vectus_code code;

    *status = 0;
    while (own[own_count] != NULL) own_count++;
    while (client->headers != NULL && client->headers[app_count] != NULL) app_count++;

    /* An upload may move to a client with another transport, whose connections the old one cannot use */
    if (connection->transport != client->transport || connection->userdata != client->transport_userdata) {
        vectus_close(connection);
    }
    if (connection->handle == NULL) {
        connection->transport = client->transport;
        connection->userdata = client->transport_userdata;
        if (client->transport->open(client->transport_userdata, &connection->handle) != VECTUS_OK) {
            connection->handle = NULL;
            snprintf(error, VECTUS_ERROR_SIZE, "%s: out of memory", name);
            return VECTUS_ERR_MEMORY;
        }
    }
    headers = malloc((own_count + app_count + 1) * sizeof *headers);
    if (headers == NULL) {
        snprintf(error, VECTUS_ERROR_SIZE, "%s: out of memory", name);
        return VECTUS_ERR_MEMORY;
    }
    memcpy(headers, own, own_count * sizeof *headers);
    if (app_count > 0) memcpy(headers + own_count, client->headers, app_count * sizeof *headers);
    headers[own_count + app_count] = NULL;

    request->headers = headers;
    code = client->transport->perform(connection->handle, request, status, reason, sizeof reason);
    request->headers = own;
    free(headers);
    if (code != VECTUS_OK) {
        /* shortcut: every transport failure becomes VECTUS_ERR_TRANSPORT; split it when callers need to tell DNS, connection and TLS errors apart */
        *status = 0;
        snprintf(error, VECTUS_ERROR_SIZE, "%s: %.200s", name, reason[0] != '\0' ? reason : vectus_strerror(code));
        return code == VECTUS_ERR_MEMORY ? VECTUS_ERR_MEMORY : VECTUS_ERR_TRANSPORT;
    }
    snprintf(error, VECTUS_ERROR_SIZE, "%s answered HTTP %ld", name, *status);  /* what went wrong, unless the caller finds otherwise */
    return VECTUS_OK;
}

vectus_code vectus_client_create(vectus_client *client, const char *url, const struct vectus_transport *transport,
                                 void *transport_userdata) {
    CURLU *u;
    char *scheme = NULL;
    vectus_code code = VECTUS_ERR_MEMORY;

    if (client == NULL) {
        return VECTUS_ERR_INVALID;
    }
    *client = NULL;
    if (url == NULL || transport == NULL || transport->open == NULL || transport->perform == NULL || transport->close == NULL) {
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
        if (*client != NULL) (*client)->url = client_copy(url);
        if (*client != NULL && (*client)->url != NULL) {
            (*client)->transport = transport;
            (*client)->transport_userdata = transport_userdata;
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
    char *name_copy = NULL;
    char *value_copy = NULL;
    char **headers;
    size_t count = 0;
    size_t kept = 0;
    size_t i;
    const char *ch;

    if (client == NULL || name == NULL) {
        return VECTUS_ERR_INVALID;
    }
    /* A token, as HTTP defines header names, which also keeps out spaces, colons and line breaks */
    if (*name == '\0' || name[strspn(name, "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")] != '\0'
        || header_reserved(name)) {
        return VECTUS_ERR_INVALID;
    }
    for (ch = value; ch != NULL && *ch != '\0'; ch++) {
        if (((unsigned char) *ch < ' ' && *ch != '\t') || *ch == 0x7f) return VECTUS_ERR_INVALID;
    }

    /* Allocate everything first, so running out of memory leaves the old headers as they were */
    while (client->headers != NULL && client->headers[count] != NULL) count += 2;
    if (value != NULL && ((name_copy = client_copy(name)) == NULL || (value_copy = client_copy(value)) == NULL)) {
        free(name_copy);
        return VECTUS_ERR_MEMORY;
    }
    headers = malloc((count + 3) * sizeof *headers);
    if (headers == NULL) {
        free(name_copy);
        free(value_copy);
        return VECTUS_ERR_MEMORY;
    }
    for (i = 0; i < count; i += 2) {
        if (curl_strequal(client->headers[i], name)) {  /* the header being replaced or removed */
            free(client->headers[i]);
            free(client->headers[i + 1]);
        } else {
            headers[kept++] = client->headers[i];
            headers[kept++] = client->headers[i + 1];
        }
    }
    if (value != NULL) {
        headers[kept++] = name_copy;
        headers[kept++] = value_copy;
    }
    headers[kept] = NULL;
    free(client->headers);
    client->headers = headers;
    return VECTUS_OK;
}

vectus_code vectus_client_discover(vectus_client client) {
    struct vectus_connection connection = {0};
    vectus_code code;

    if (client == NULL) {
        return VECTUS_ERR_INVALID;
    }
    code = vectus_discover(client, &connection);
    vectus_close(&connection);
    return code;
}

vectus_code vectus_discover(vectus_client client, struct vectus_connection *connection) {
    static const char *const none[] = {NULL};  /* not even Tus-Resumable, which the spec asks OPTIONS to leave out */
    vectus_request request = {0};
    vectus_code code;

    /* Forget what an earlier answer said */
    free(client->version);
    client->version = NULL;
    client->extensions = 0;
    client->max_size = 0;
    client->discovered = 0;
    client->out_of_memory = 0;
    client->status = 0;

    request.method = "OPTIONS";
    request.url = client->url;
    request.headers = none;
    request.header = client_handle_header;
    request.userdata = client;
    code = vectus_send(client, connection, "OPTIONS", &request, client->error, &client->status);

    if (code != VECTUS_OK) {
        /* keep the code and the transport's reason */
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
    size_t i;

    if (client == NULL) {
        return;
    }
    free(client->url);
    free(client->version);
    for (i = 0; client->headers != NULL && client->headers[i] != NULL; i++) free(client->headers[i]);
    free(client->headers);
    free(client);
}

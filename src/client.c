/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int vectus_strnequal(const char *a, const char *b, size_t n) {
    for (; n > 0; a++, b++, n--) {
        if (ascii_lower((unsigned char) *a) != ascii_lower((unsigned char) *b)) return 0;
        if (*a == '\0') return 1;
    }
    return 1;
}

/**
 * A URL or reference split into the parts of RFC 3986 appendix B, with the fragment cut off; a part is absent when NULL
 */
struct url_parts {
    const char *scheme;
    size_t scheme_len;
    const char *authority;
    size_t authority_len;
    const char *path;  /* never absent, but possibly empty */
    size_t path_len;
    const char *query;  /* with its '?' */
    size_t query_len;
};

/**
 * How many of the len bytes at s come before one of stops
 */
static size_t url_span(const char *s, size_t len, const char *stops) {
    size_t i = 0;

    while (i < len && strchr(stops, s[i]) == NULL) i++;
    return i;
}

static void url_split(const char *s, size_t len, struct url_parts *p) {
    size_t i = url_span(s, len, ":/?");

    memset(p, 0, sizeof *p);
    /* A scheme is a letter, then letters, digits, "+", "-" or ".", then a colon; "a:b" without one is a path */
    if (i > 0 && i < len && s[i] == ':' && ascii_lower((unsigned char) s[0]) >= 'a' && ascii_lower((unsigned char) s[0]) <= 'z'
        && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-.") >= i) {
        p->scheme = s;
        p->scheme_len = i;
        s += i + 1;
        len -= i + 1;
    }
    if (len >= 2 && s[0] == '/' && s[1] == '/') {
        p->authority = s + 2;
        p->authority_len = url_span(s + 2, len - 2, "/?");
        s += 2 + p->authority_len;
        len -= 2 + p->authority_len;
    }
    p->path = s;
    p->path_len = url_span(s, len, "?");
    if (p->path_len < len) {
        p->query = s + p->path_len;
        p->query_len = len - p->path_len;
    }
}

/**
 * Whether an authority, such as "user@host:8080" or "[::1]:8080", has a host, an IPv6 one within brackets
 */
static int url_has_host(const char *authority, size_t len) {
    const char *host = authority;
    const char *close;
    size_t i;

    for (i = len; i > 0; i--) {
        if (authority[i - 1] == '@') {
            host = authority + i;  /* after the user information */
            break;
        }
    }
    len -= (size_t) (host - authority);
    if (len > 0 && host[0] == '[') {
        close = memchr(host, ']', len);
        return close != NULL && close > host + 1 && (close + 1 == host + len || close[1] == ':');
    }
    return len > 0 && host[0] != ':' && memchr(host, '[', len) == NULL && memchr(host, ']', len) == NULL;
}

/**
 * Remove the "." and ".." segments from the len bytes of path, in place, as RFC 3986 section 5.2.4 does
 *
 * @return the new length, never more than len
 */
static size_t url_remove_dots(char *path, size_t len) {
    const char *in = path;
    const char *end = path + len;
    size_t out = 0;  /* path[out] never passes in, so the output can overwrite what was already read */

    while (in < end) {
        size_t left = (size_t) (end - in);

        if (left >= 3 && memcmp(in, "../", 3) == 0) {
            in += 3;
        } else if (left >= 2 && memcmp(in, "./", 2) == 0) {
            in += 2;
        } else if (left >= 3 && memcmp(in, "/./", 3) == 0) {
            in += 2;
        } else if (left == 2 && memcmp(in, "/.", 2) == 0) {
            path[out++] = '/';
            in = end;
        } else if (left >= 4 && memcmp(in, "/../", 4) == 0) {
            in += 3;
            while (out > 0 && path[--out] != '/') {}  /* drop the last segment written */
        } else if (left == 3 && memcmp(in, "/..", 3) == 0) {
            while (out > 0 && path[--out] != '/') {}
            path[out++] = '/';
            in = end;
        } else if ((left == 1 && in[0] == '.') || (left == 2 && memcmp(in, "..", 2) == 0)) {
            in = end;
        } else {
            do path[out++] = *in++; while (in < end && *in != '/');  /* the next segment, with the '/' before it */
        }
    }
    return out;
}

vectus_code vectus_resolve_url(const char *base, const char *reference, char **resolved) {
    struct url_parts b;
    struct url_parts r;
    struct url_parts t;
    size_t length = strcspn(reference, "#");  /* fragments stay with the client */
    int merge;
    char *url;
    char *path;
    size_t n = 0;
    size_t i;

    /* Anything else could break the request line it goes into */
    for (i = 0; i < length; i++) {
        if ((unsigned char) reference[i] <= ' ' || (unsigned char) reference[i] > '~') return VECTUS_ERR_INVALID;
    }
    if (length == 0) {
        return VECTUS_ERR_INVALID;
    }
    url_split(base, strcspn(base, "#"), &b);
    url_split(reference, length, &r);

    t = r;
    merge = r.scheme == NULL && r.authority == NULL && r.path_len > 0 && r.path[0] != '/';
    if (r.scheme == NULL) {
        t.scheme = b.scheme;
        t.scheme_len = b.scheme_len;
        if (r.authority == NULL) {
            t.authority = b.authority;
            t.authority_len = b.authority_len;
        }
        if (r.authority == NULL && r.path_len == 0) {
            t.path = b.path;
            t.path_len = b.path_len;
            if (r.query == NULL) {
                t.query = b.query;
                t.query_len = b.query_len;
            }
        }
    }
    if (t.scheme == NULL || !((t.scheme_len == 4 && vectus_strnequal(t.scheme, "http", 4)) || (t.scheme_len == 5 && vectus_strnequal(t.scheme, "https", 5)))
        || t.authority == NULL || !url_has_host(t.authority, t.authority_len)) {
        return VECTUS_ERR_INVALID;
    }

    url = malloc(t.scheme_len + 3 + t.authority_len + b.path_len + 1 + t.path_len + t.query_len + 1);
    if (url == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    memcpy(url, t.scheme, t.scheme_len);
    n += t.scheme_len;
    memcpy(url + n, "://", 3);
    n += 3;
    memcpy(url + n, t.authority, t.authority_len);
    n += t.authority_len;
    path = url + n;
    if (merge) {
        size_t dir = b.path_len;  /* the base's path up to its last '/', or "/" for an empty one */

        while (dir > 0 && b.path[dir - 1] != '/') dir--;
        if (b.path_len == 0) url[n++] = '/';
        memcpy(url + n, b.path, dir);
        n += dir;
    }
    memcpy(url + n, t.path, t.path_len);
    n += t.path_len;
    n = (size_t) (path - url) + url_remove_dots(path, (size_t) (url + n - path));
    if (t.query != NULL) memcpy(url + n, t.query, t.query_len);
    n += t.query_len;
    url[n] = '\0';
    *resolved = url;
    return VECTUS_OK;
}

long long vectus_parse_http_date(const char *value) {
    /* shortcut: only the IMF-fixdate format, which tus servers send; RFC 9110 also asks to accept the obsolete RFC 850 and asctime ones, add them if a server sends those */
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char weekday[4];
    char month[4];
    int day;
    int year;
    int hour;
    int minute;
    int second;
    int end = 0;
    const char *found;
    long long m;
    long long y;
    long long days;

    if (sscanf(value, "%3s, %2d %3s %4d %2d:%2d:%2d GMT%n", weekday, &day, month, &year, &hour, &minute, &second, &end) != 7
        || end == 0 || value[end] != '\0') {
        return -1;
    }
    found = strlen(month) == 3 ? strstr(months, month) : NULL;
    if (found == NULL || (found - months) % 3 != 0 || day < 1 || day > 31 || year < 1970 || hour < 0 || hour > 23
        || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return -1;
    }
    /* Days since the epoch, as Howard Hinnant's days_from_civil counts them, for years from 1970 */
    m = (found - months) / 3 + 1;
    y = year - (m <= 2);
    days = y * 365 + y / 4 - y / 100 + y / 400 + (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1 - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
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
    if (vectus_strequal(name, "Tus-Version")) {
        free(v->version);
        v->version = client_copy(value);
        if (v->version == NULL) v->out_of_memory = 1;
    } else if (vectus_strequal(name, "Tus-Extension")) {
        v->extensions = parse_extensions(value);
    } else if (vectus_strequal(name, "Tus-Max-Size")) {
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
    char *resolved = NULL;
    vectus_code code;

    if (client == NULL) {
        return VECTUS_ERR_INVALID;
    }
    *client = NULL;
    if (url == NULL || transport == NULL || transport->open == NULL || transport->perform == NULL || transport->close == NULL) {
        return VECTUS_ERR_INVALID;
    }
    code = vectus_resolve_url(url, url, &resolved);  /* checks it is an http or https URL, and drops any fragment */
    if (code != VECTUS_OK) {
        return code;
    }
    *client = calloc(1, sizeof(struct vectus_client_s));  /* zeroed: no headers, nothing discovered */
    if (*client == NULL) {
        free(resolved);
        return VECTUS_ERR_MEMORY;
    }
    (*client)->url = resolved;
    (*client)->transport = transport;
    (*client)->transport_userdata = transport_userdata;
    return VECTUS_OK;
}

/**
 * Whether name is one of the headers tus or vectus set themselves
 */
static int header_reserved(const char *name) {
    static const char *const reserved[] = {"Content-Type", "Content-Length", "Transfer-Encoding", "Expect", "X-HTTP-Method-Override"};
    size_t i;

    if (vectus_strnequal(name, "Tus-", 4) || vectus_strnequal(name, "Upload-", 7)) {
        return 1;
    }
    for (i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        if (vectus_strequal(name, reserved[i])) return 1;
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
        if (vectus_strequal(client->headers[i], name)) {  /* the header being replaced or removed */
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

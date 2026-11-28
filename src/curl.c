/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <vectus/curl.h>

/* curl takes the request size as a signed 64-bit curl_off_t, as large as vectus lets an upload be */
typedef char easy_off_t_is_64_bits[sizeof(curl_off_t) == 8 ? 1 : -1];

/* shortcut: a stall timeout only the options' callback can change (CURLOPT_LOW_SPEED_TIME); add a setting when callers need another */
#define EASY_STALL_TIMEOUT 30L

struct easy_connection {
    CURL *curl;  /* reused for every request, so they can share a connection */
    const struct vectus_curl_options *options;
};

/**
 * Parse one response header line, as curl hands it to a header callback, and pass its name and trimmed value on
 *
 * @param buffer the header line, not null-terminated
 * @param bytes the length of buffer
 * @param handle called with the name and value, unless the line has no colon (the status line, or the blank line ending the headers)
 * @param userdata passed to handle
 * @return what the header callback returns to curl: bytes, or 0 to abort when out of memory
 */
static size_t easy_parse_header_line(const char *buffer, size_t bytes, void (*handle)(void *userdata, const char *name, const char *value),
                                     void *userdata) {
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

        handle(userdata, line, value);
    }

    free(line);
    return bytes;
}

static size_t easy_header(char *buffer, size_t size, size_t nitems, void *userdata) {
    const vectus_request *request = userdata;

    return easy_parse_header_line(buffer, size * nitems, request->header, request->userdata);
}

static size_t easy_read(char *buffer, size_t size, size_t nitems, void *userdata) {
    const vectus_request *request = userdata;
    size_t bytes = request->read(request->userdata, buffer, size * nitems);

    return bytes == VECTUS_READ_ABORT ? CURL_READFUNC_ABORT : bytes;
}

static int easy_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    const vectus_request *request = clientp;

    (void) dltotal;
    (void) dlnow;
    (void) ultotal;
    return request->progress(request->userdata, (unsigned long long) ulnow) != 0;
}

/**
 * A curl write callback that drops the response body, which curl would otherwise print to the application's stdout
 */
static size_t easy_discard_body(char *buffer, size_t size, size_t nitems, void *userdata) {
    (void) buffer;
    (void) userdata;
    return size * nitems;
}

static vectus_code easy_open(void *userdata, void **connection) {
    struct easy_connection *c = malloc(sizeof *c);

    if (c == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    c->curl = curl_easy_init();
    if (c->curl == NULL) {
        free(c);
        return VECTUS_ERR_MEMORY;
    }
    c->options = userdata;
    *connection = c;
    return VECTUS_OK;
}

static vectus_code easy_perform(void *connection, const vectus_request *request, long *status, char *error, size_t error_size) {
    struct easy_connection *c = connection;
    CURL *h = c->curl;
    struct curl_slist *headers = NULL;
    struct curl_slist *appended;
    char reason[CURL_ERROR_SIZE] = "";
    CURLcode res;
    size_t i;

    curl_easy_reset(h);  /* clear the last request's options, keeping its connection */
    for (i = 0; request->headers[i] != NULL; i += 2) {
        const char *name = request->headers[i];
        const char *value = request->headers[i + 1];
        size_t size = strlen(name) + strlen(value) + 3;
        char *line = malloc(size);

        appended = NULL;
        if (line != NULL) {
            /* curl drops a header written "Name:", and sends "Name;" as one with an empty value */
            snprintf(line, size, *value == '\0' ? "%s;" : "%s: %s", name, value);
            appended = curl_slist_append(headers, line);
            free(line);
        }
        if (appended == NULL) {
            curl_slist_free_all(headers);
            return VECTUS_ERR_MEMORY;
        }
        headers = appended;
    }
    if (request->read != NULL) {
        /* "Expect:" stops curl from adding it to a large or chunked body, which would wait for the server */
        appended = curl_slist_append(headers, request->expect_continue ? "Expect: 100-continue" : "Expect:");
        if (appended == NULL) {
            curl_slist_free_all(headers);
            return VECTUS_ERR_MEMORY;
        }
        headers = appended;
    }

    curl_easy_setopt(h, CURLOPT_URL, request->url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    if (strcmp(request->method, "HEAD") == 0) {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, request->method);
    }
    if (request->read != NULL) {
        /* An upload, as curl calls a body from a read callback, goes out with the request's own method */
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h, CURLOPT_READFUNCTION, easy_read);
        curl_easy_setopt(h, CURLOPT_READDATA, (void *) request);
        if (request->length != VECTUS_LENGTH_UNKNOWN) {  /* otherwise curl sends it chunked */
            curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t) request->length);
        }
    }
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, easy_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, (void *) request);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, easy_discard_body);
    if (request->progress != NULL) {
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, easy_progress);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, (void *) request);
    }
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, reason);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  /* curl's signal-based DNS timeout is not safe with threads */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* give up on a stalled connection, so the upload can resume */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, EASY_STALL_TIMEOUT);
    if (c->options != NULL && c->options->callback != NULL) {
        c->options->callback(c->options->userdata, h);
    }

    res = curl_easy_perform(h);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, NULL);  /* reason goes out of scope */
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        snprintf(error, error_size, "%s", reason[0] != '\0' ? reason : curl_easy_strerror(res));
        return res == CURLE_OUT_OF_MEMORY ? VECTUS_ERR_MEMORY : VECTUS_ERR_TRANSPORT;
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    return VECTUS_OK;
}

static void easy_close(void *connection) {
    struct easy_connection *c = connection;

    curl_easy_cleanup(c->curl);
    free(c);
}

const vectus_transport *vectus_curl_transport(void) {
    static const vectus_transport transport = {easy_open, easy_perform, easy_close};

    return &transport;
}

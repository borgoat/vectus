/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_client.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include <vectus/esp.h>

/* bytes read from the body and written per call */
#define IDF_CHUNK 4096

/* shortcut: a stall timeout only the options' callback can change (timeout_ms); add a setting when callers need another */
#define IDF_TIMEOUT_MS 30000

struct idf_connection {
    esp_http_client_handle_t http;  /* reused for every request, so they can share a connection */
    int tx_size;                    /* the buffer_size_tx it was made with */
    const struct vectus_esp_options *options;
    const vectus_request *current;  /* the request whose response headers are arriving, NULL between requests */
    int keep_alive;                 /* cleared when the response says Connection: close */
    int out_of_memory;              /* set when a header could not be copied */
};

static vectus_code idf_fail(char *error, size_t error_size, esp_http_client_handle_t http, const char *what, esp_err_t err) {
    snprintf(error, error_size, "%s: %s (errno %d)", what, esp_err_to_name(err), esp_http_client_get_errno(http));
    return VECTUS_ERR_TRANSPORT;
}

static esp_err_t idf_event(esp_http_client_event_t *evt) {
    struct idf_connection *c = evt->user_data;
    char *value;
    char *end;

    if (evt->event_id != HTTP_EVENT_ON_HEADER || c->current == NULL) {
        return ESP_OK;
    }
    value = strdup(evt->header_value + strspn(evt->header_value, " \t"));  /* trimmed in a copy, not in esp_http_client's */
    if (value == NULL) {
        c->out_of_memory = 1;
        return ESP_OK;
    }
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    /* esp_http_client_is_persistent_connection only came with ESP-IDF 5.4, and ESP-ADF 2.x supports 5.1 */
    if (strcasecmp(evt->header_key, "Connection") == 0 && strcasecmp(value, "close") == 0) c->keep_alive = 0;
    c->current->header(c->current->userdata, evt->header_key, value);
    free(value);
    return ESP_OK;
}

static int idf_method(const char *name, esp_http_client_method_t *method) {
    static const struct {
        const char *name;
        esp_http_client_method_t method;
    } methods[] = {{"OPTIONS", HTTP_METHOD_OPTIONS}, {"POST", HTTP_METHOD_POST}, {"HEAD", HTTP_METHOD_HEAD},
                   {"PATCH", HTTP_METHOD_PATCH}, {"DELETE", HTTP_METHOD_DELETE}};
    size_t i;

    for (i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (strcmp(name, methods[i].name) == 0) {
            *method = methods[i].method;
            return 1;
        }
    }
    return 0;
}

/*
 * esp_http_client writes the request line and headers through one buffer of buffer_size_tx bytes, fixed when the handle
 * is made. Before ESP-IDF 6.1 a header that does not fit is dropped without an error, along with the headers after it
 * and the blank line ending them, and splitting them over several writes logs an error each time: size the buffer
 * for all of them
 */
static int idf_tx_size(const vectus_request *r) {
    /* the request line, Host, which the URL holds, and esp_http_client's own: User-Agent and Content-Length */
    size_t size = strlen(r->method) + 2 * strlen(r->url) + 128;
    size_t i;

    for (i = 0; r->headers[i] != NULL; i += 2) {
        size += strlen(r->headers[i]) + strlen(r->headers[i + 1]) + 4;
    }
    return (int) size;
}

/**
 * Make the handle, or make it again when this request's headers need a bigger buffer, which drops the connection
 */
static int idf_handle(struct idf_connection *c, const vectus_request *r) {
    esp_http_client_config_t config = {0};
    int size = idf_tx_size(r);

    if (c->http != NULL && size <= c->tx_size) {
        return 1;
    }
    if (size < 1024) size = 1024;
    if (c->http != NULL) esp_http_client_cleanup(c->http);
    c->http = NULL;
    config.timeout_ms = IDF_TIMEOUT_MS;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;  /* so https works without the application naming a CA */
#endif
    if (c->options != NULL && c->options->callback != NULL) {
        c->options->callback(c->options->userdata, &config);
    }
    config.url = r->url;
    config.event_handler = idf_event;
    config.user_data = c;
    config.buffer_size_tx = size;
    c->http = esp_http_client_init(&config);
    c->tx_size = size;
    return c->http != NULL;
}

static int idf_write(struct idf_connection *c, const char *data, int len) {
    return esp_http_client_write(c->http, data, len) == len;
}

/**
 * Send the body from the read callback, framing it as chunks when chunked is set.
 * esp_http_client_chunk_write_begin and _end would do the framing, but only from ESP-IDF 6.0
 */
static vectus_code idf_send_body(struct idf_connection *c, const vectus_request *r, int chunked, char *buffer, char *error,
                                 size_t error_size) {
    unsigned long long sent = 0;

    for (;;) {
        size_t wanted = IDF_CHUNK;
        size_t bytes;
        char frame[24];

        if (!chunked && wanted > r->length - sent) wanted = (size_t) (r->length - sent);
        if (wanted == 0) break;
        bytes = r->read(r->userdata, buffer, wanted);
        if (bytes == 0 && chunked) break;
        if (bytes == 0 || bytes == VECTUS_READ_ABORT || bytes > wanted) {
            snprintf(error, error_size, "the body failed at byte %llu", sent);
            return VECTUS_ERR_TRANSPORT;
        }
        if ((chunked && !idf_write(c, frame, snprintf(frame, sizeof frame, "%lx\r\n", (unsigned long) bytes)))
            || !idf_write(c, buffer, (int) bytes) || (chunked && !idf_write(c, "\r\n", 2))) {
            return idf_fail(error, error_size, c->http, "write", ESP_ERR_HTTP_WRITE_DATA);
        }
        sent += bytes;
        if (r->progress != NULL && r->progress(r->userdata, sent) != 0) {
            snprintf(error, error_size, "cancelled by the progress callback");
            return VECTUS_ERR_TRANSPORT;
        }
    }
    if (chunked && !idf_write(c, "0\r\n\r\n", 5)) {
        return idf_fail(error, error_size, c->http, "write", ESP_ERR_HTTP_WRITE_DATA);
    }
    return VECTUS_OK;
}

static vectus_code idf_open(void *userdata, void **connection) {
    struct idf_connection *c = calloc(1, sizeof *c);

    if (c == NULL) {
        return VECTUS_ERR_MEMORY;
    }
    c->options = userdata;
    *connection = c;
    return VECTUS_OK;
}

static vectus_code idf_perform(void *connection, const vectus_request *r, long *status, char *error, size_t error_size) {
    struct idf_connection *c = connection;
    /* esp_http_client takes the Content-Length as an int, so a larger body goes out chunked */
    int chunked = r->read != NULL && (r->length == VECTUS_LENGTH_UNKNOWN || r->length > INT_MAX);
    esp_http_client_method_t method;
    char *buffer = NULL;
    int64_t fetched;
    esp_err_t err;
    vectus_code code = VECTUS_OK;
    size_t i;

    if (!idf_method(r->method, &method)) {
        snprintf(error, error_size, "%s is not a method this transport sends", r->method);
        return VECTUS_ERR_TRANSPORT;
    }
    if (!idf_handle(c, r) || (r->read != NULL && (buffer = malloc(IDF_CHUNK)) == NULL)) {
        return VECTUS_ERR_MEMORY;
    }

    err = esp_http_client_set_url(c->http, r->url);
    if (err != ESP_OK) {
        code = idf_fail(error, error_size, c->http, "URL", err);
        goto done;
    }
    esp_http_client_set_method(c->http, method);
    for (i = 0; r->headers[i] != NULL; i += 2) {
        if (esp_http_client_set_header(c->http, r->headers[i], r->headers[i + 1]) != ESP_OK) {
            code = VECTUS_ERR_MEMORY;
            goto done;
        }
    }
    c->current = r;
    c->keep_alive = 1;
    c->out_of_memory = 0;

    /* Once as each request starts, so it can cancel even one without a body */
    if (r->progress != NULL && r->progress(r->userdata, 0) != 0) {
        snprintf(error, error_size, "cancelled by the progress callback");
        code = VECTUS_ERR_TRANSPORT;
        goto done;
    }
    /* No Expect: 100-continue: esp_http_client does not wait for the 100, and would take it for the final status */
    err = esp_http_client_open(c->http, r->read == NULL ? 0 : chunked ? -1 : (int) r->length);
    if (err != ESP_OK) {
        code = idf_fail(error, error_size, c->http, "connect or send headers", err);
        goto done;
    }
    if (r->read != NULL && (code = idf_send_body(c, r, chunked, buffer, error, error_size)) != VECTUS_OK) {
        goto done;
    }
    fetched = esp_http_client_fetch_headers(c->http);
    if (fetched < 0) {
        code = idf_fail(error, error_size, c->http, "read the response", fetched == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL);
        goto done;
    }
    *status = esp_http_client_get_status_code(c->http);
    if (c->out_of_memory) {
        code = VECTUS_ERR_MEMORY;
        goto done;
    }
    /* A HEAD response has no body, whatever its Content-Length says, and reading one would wait for the timeout */
    if (method != HTTP_METHOD_HEAD) {
        char scratch[256];
        int bytes;

        while ((bytes = esp_http_client_read(c->http, scratch, sizeof scratch)) > 0) {}
        if (bytes < 0) c->keep_alive = 0;  /* the body did not arrive whole, so the connection cannot carry another request */
    }

done:
    c->current = NULL;
    /* Headers stay on the handle from one request to the next; esp_http_client_delete_all_headers would take Host too */
    for (i = 0; r->headers[i] != NULL; i += 2) {
        esp_http_client_delete_header(c->http, r->headers[i]);
    }
    if (code != VECTUS_OK || !c->keep_alive) {
        esp_http_client_close(c->http);  /* never reuse a connection a request broke off on */
    }
    free(buffer);
    return code;
}

static void idf_close(void *connection) {
    struct idf_connection *c = connection;

    if (c->http != NULL) esp_http_client_cleanup(c->http);
    free(c);
}

const vectus_transport *vectus_esp_transport(void) {
    static const vectus_transport transport = {idf_open, idf_perform, idf_close};

    return &transport;
}

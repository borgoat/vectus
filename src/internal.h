/* SPDX-License-Identifier: MIT */

/* Shared by the library sources and their tests; not installed */

#ifndef VECTUS_INTERNAL_H
#define VECTUS_INTERNAL_H

#include <stddef.h>

#include <curl/curl.h>
#include <vectus/client.h>

/* Room for an error description, as vectus_client_error and vectus_upload_error return */
#define VECTUS_ERROR_SIZE 256

struct vectus_client_s {

    /**
     * the endpoint of the tus server client will be uploading to
     */
    char *url;

    /**
     * the extensions supported by the server
     */
    unsigned int extensions;

    /**
     * the tus version of the server
     */
    char *version;

    /**
     * the max size supported by the server
     */
    unsigned long long int max_size;

    /**
     * set once the server said which versions and extensions it offers
     */
    int discovered;

    /**
     * seconds without a byte moving before a request gives up, so an upload can resume instead of hanging
     */
    long stall_timeout;

    /**
     * set to send PATCH and DELETE as POST with X-HTTP-Method-Override
     */
    int method_override;

    /**
     * the application's own headers, sent on every request
     */
    struct curl_slist *headers;

    /**
     * called with each request's curl handle before it goes out, NULL for none
     */
    vectus_curl_callback curl_callback;
    void *curl_userdata;

    /**
     * the HTTP status of the last discovery response, 0 if none came
     */
    long status;

    /**
     * why the last discovery failed, empty after one succeeded
     */
    char error[VECTUS_ERROR_SIZE];

    /**
     * set when storing a response header ran out of memory
     */
    int out_of_memory;
};

/* shortcut: every client gets this stall timeout, which only the curl callback can change (CURLOPT_LOW_SPEED_TIME); add a setter when callers need another */
#define VECTUS_DEFAULT_STALL_TIMEOUT 30L

/**
 * Parse one response header line, as curl hands it to a header callback, and pass its name and trimmed value to handle
 *
 * @param buffer the header line, not null-terminated
 * @param bytes the length of buffer
 * @param handle called with the null-terminated name and value, unless the line has no colon (the status line, or the blank line ending the headers)
 * @param target passed to handle
 * @return what the header callback returns to curl: bytes, or 0 to abort when out of memory
 */
size_t vectus_parse_header_line(const char *buffer, size_t bytes, void (*handle)(void *target, const char *name, char *value), void *target);

/**
 * Parse a header value made only of digits, such as Tus-Max-Size
 *
 * @return 1 with the value stored in size, or 0 leaving size alone for anything else, including a sign or a value too large
 */
int vectus_parse_size(const char *value, unsigned long long *size);

/**
 * Send a request set up with its URL, method and callbacks, adding what every request shares: the application's headers
 * after the request's own, the stall timeout, and the curl callback last
 *
 * @param method names the request in error descriptions
 * @param headers the request's own headers, which the application's are appended to; the caller frees the list
 * @param error set to curl's reason when no response came, or to the response's status otherwise
 * @param status set to the response's HTTP status, 0 if none came
 * @return VECTUS_OK when a response arrived, whatever its status, VECTUS_ERR_MEMORY, or VECTUS_ERR_TRANSPORT
 */
vectus_code vectus_request(vectus_client client, CURL *c, const char *method, struct curl_slist **headers, char *error, long *status);

/**
 * vectus_client_discover over c, a fresh or reset handle, so an upload asks over the connection it then uses
 */
vectus_code vectus_discover(vectus_client client, CURL *c);

#endif /* VECTUS_INTERNAL_H */

/* SPDX-License-Identifier: MIT */

/* Shared by the library sources and their tests; not installed */

#ifndef VECTUS_INTERNAL_H
#define VECTUS_INTERNAL_H

#include <stddef.h>

#include <vectus/client.h>


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
     * seconds without a byte moving before a request gives up, so an upload can resume instead of hanging
     */
    long stall_timeout;

    /**
     * set to send PATCH and DELETE as POST with X-HTTP-Method-Override
     */
    int method_override;
};

/* shortcut: every client gets this stall timeout; add a setter when callers need another */
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
 * A curl write callback that drops the response body, which curl would otherwise print to the application's stdout
 */
size_t vectus_discard_body(char *buffer, size_t size, size_t nitems, void *userdata);

/**
 * Parse a header value made only of digits, such as Tus-Max-Size
 *
 * @return 1 with the value stored in size, or 0 leaving size alone for anything else, including a sign or a value too large
 */
int vectus_parse_size(const char *value, unsigned long long *size);

#endif /* VECTUS_INTERNAL_H */

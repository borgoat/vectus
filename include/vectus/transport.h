/* SPDX-License-Identifier: MIT */

/*
 * What a transport implements to carry vectus's HTTP requests, for an HTTP client vectus does not ship one for.
 * vectus/curl.h has the libcurl one. This interface may still change before vectus 1.0
 */

#ifndef VECTUS_TRANSPORT_H
#define VECTUS_TRANSPORT_H

#include <stddef.h>

#include "vectus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One HTTP request, built by vectus; everything it points to stays valid until perform returns
 */
typedef struct vectus_request {
    /**
     * OPTIONS, POST, HEAD, PATCH or DELETE
     */
    const char *method;

    const char *url;

    /**
     * Name and value pairs, ending with NULL. A value may be empty, and goes out as an empty header
     */
    const char *const *headers;

    /**
     * Where the body comes from, NULL for a request without one: write at most size bytes into buffer and return how
     * many. It returns VECTUS_READ_ABORT when the request must fail, and 0 only once the body has ended
     */
    size_t (*read)(void *userdata, char *buffer, size_t size);

    /**
     * The body's length, sent as Content-Length, or VECTUS_LENGTH_UNKNOWN to send it chunked until read returns 0
     */
    unsigned long long length;

    /**
     * Set when the server may refuse the body before it goes out: send Expect: 100-continue and wait for the
     * go-ahead, if the HTTP client can. One that cannot sends the body at once, without that header
     */
    int expect_continue;

    /**
     * Call it with the body bytes sent so far as they go out, and at least once a second while the request runs,
     * if the HTTP client can; NULL for none. A non-zero return means the request must fail
     */
    int (*progress)(void *userdata, unsigned long long sent);

    /**
     * Call it with each response header, the value trimmed of surrounding whitespace, before perform returns
     */
    void (*header)(void *userdata, const char *name, const char *value);

    /**
     * Passed to read, progress and header
     */
    void *userdata;
} vectus_request;

/**
 * The functions of a transport. A client calls them for one upload at a time on each connection, but several threads
 * may open and use connections of their own at once
 */
typedef struct vectus_transport {
    /**
     * Prepare a connection for one upload's requests, or for one vectus_client_discover; it need not connect yet
     *
     * @param userdata what vectus_client_create was given along with the transport
     * @param connection set to the transport's own state for the connection
     * @return VECTUS_OK, or VECTUS_ERR_MEMORY
     */
    vectus_code (*open)(void *userdata, void **connection);

    /**
     * Send one request over the connection and wait for the response's status and headers, dropping its body.
     * A request that no byte moves for a while, 30 seconds unless set otherwise, must fail, so the upload can resume
     * instead of hanging. A request that failed must not leave the connection unusable for the next one
     *
     * @param status set to the response's HTTP status
     * @param error set to why no response came, a NUL-terminated message of at most error_size bytes
     * @return VECTUS_OK when a response arrived, whatever its status, VECTUS_ERR_MEMORY, or VECTUS_ERR_TRANSPORT,
     *         also when read or progress failed the request
     */
    vectus_code (*perform)(void *connection, const vectus_request *request, long *status, char *error, size_t error_size);

    /**
     * Close the connection and free what open made
     */
    void (*close)(void *connection);
} vectus_transport;

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_TRANSPORT_H */

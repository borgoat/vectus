/* SPDX-License-Identifier: MIT */

/*
 * The libcurl transport:
 *
 *   vectus_client_create(&client, "https://tus.example.com/files/", vectus_curl_transport(), NULL);
 *
 * Before starting threads, call curl_global_init(CURL_GLOBAL_DEFAULT) once, unless your libcurl is 7.84 or newer and
 * built thread-safe
 */

#ifndef VECTUS_CURL_H
#define VECTUS_CURL_H

#include <curl/curl.h>

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called with each request's curl handle just before it goes out, to set what vectus has no setting for, such as
 * CURLOPT_CAINFO for a private certificate authority, CURLOPT_PROXY, or CURLOPT_VERBOSE.
 * Leave alone what vectus sets: the URL, method, headers (use vectus_client_set_header), body and callbacks
 */
typedef void (*vectus_curl_callback)(void *userdata, CURL *curl);

/**
 * What to pass vectus_client_create as the transport's userdata, or NULL for none of it
 */
struct vectus_curl_options {
    /**
     * Called with each request's curl handle, see vectus_curl_callback; NULL for none
     */
    vectus_curl_callback callback;

    /**
     * Passed to callback
     */
    void *userdata;
};

/**
 * The transport sending requests through libcurl. Its userdata, a struct vectus_curl_options or NULL, must outlive
 * the client. A request fails after 30 seconds without a byte moving (CURLOPT_LOW_SPEED_TIME)
 */
const vectus_transport *vectus_curl_transport(void);

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_CURL_H */

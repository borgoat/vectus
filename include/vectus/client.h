/* SPDX-License-Identifier: MIT */

#ifndef VECTUS_CLIENT_H
#define VECTUS_CLIENT_H


/**
 * The following flags define all the Tus protocol extensions based on
 * https://tus.io/protocols/resumable-upload.html#protocol-extensions
 */
#define VECTUS_EXT_CREATION             (1 << 0)
#define VECTUS_EXT_CREATION_WITH_UPLOAD (1 << 1)  /* [...] if the Server does not offer the Creation extension, it MUST NOT offer the Creation With Upload extension either. */
#define VECTUS_EXT_EXPIRATION           (1 << 2)
#define VECTUS_EXT_CHECKSUM             (1 << 3)
#define VECTUS_EXT_TERMINATION          (1 << 4)
#define VECTUS_EXT_CONCATENATION        (1 << 5)
#define VECTUS_EXT_CREATION_DEFER_LENGTH (1 << 6)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The result of a vectus call, described by vectus_strerror
 */
typedef enum {
    VECTUS_OK = 0,
    VECTUS_ERR_MEMORY,       /* out of memory */
    VECTUS_ERR_TRANSPORT,    /* the server could not be reached: DNS, connection, TLS or timeout */
    VECTUS_ERR_HTTP_STATUS,  /* the server answered with an unexpected HTTP status, such as 401 for missing credentials */
    VECTUS_ERR_PROTOCOL,     /* the server does not offer tus 1.0.0, or broke the protocol */
    VECTUS_ERR_TOO_LARGE,    /* the upload is larger than the server or curl accept */
    VECTUS_ERR_GONE,         /* the upload no longer exists on the server (404, 410, or 403 as the tus spec allows); forget its URL to start over */
    VECTUS_ERR_SOURCE,       /* the data to upload could not be opened or read, or resuming needed a seek that failed or is not possible */
    VECTUS_ERR_UNSUPPORTED,  /* the server does not offer the tus extension this needs */
    VECTUS_ERR_INVALID,      /* an argument the protocol does not allow, such as a metadata key with a space, or a NULL handle */
    VECTUS_ERR_CANCELLED     /* the progress callback asked to stop */
} vectus_code;

/**
 * Describe a vectus_code
 *
 * @return a static, human-readable message
 */
const char *vectus_strerror(vectus_code code);

/**
 * vectus_client represents a tus client connected to 1 tus server.
 * Every function taking one accepts NULL: getters then return 0 or "", the others VECTUS_ERR_INVALID or nothing.
 *
 * vectus is built on libcurl. Before starting threads, call curl_global_init(CURL_GLOBAL_DEFAULT) once, unless your
 * libcurl is 7.84 or newer and built thread-safe. Several threads may then share one client, each with its own uploads,
 * once vectus_client_discover succeeded and while none of the setters below run.
 */
typedef struct vectus_client_s *vectus_client;

/**
 * Called with each request's curl handle, a CURL *, just before it goes out, to set what vectus has no setting for,
 * such as CURLOPT_CAINFO for a private certificate authority, CURLOPT_PROXY, or CURLOPT_VERBOSE.
 * Leave alone what vectus sets: the URL, method, headers (use vectus_client_set_header), body and callbacks
 */
typedef void (*vectus_curl_callback)(void *userdata, void *curl);

/**
 * Create a vectus_client for the tus server at url, without contacting it yet, so it can be configured first.
 * Once done, release the client with vectus_client_cleanup
 *
 * @param client set to the new vectus_client on success, to NULL otherwise
 * @param url the tus server's upload endpoint, an http or https URL
 * @return VECTUS_OK, VECTUS_ERR_INVALID for anything but an http or https URL, or VECTUS_ERR_MEMORY
 */
vectus_code vectus_client_create(vectus_client *client, const char *url);

/**
 * Send a header on every request to the server, such as Authorization.
 * Setting it again replaces it, so a refreshed token takes effect on the next request
 *
 * @param name the header's name, which must not be one tus or vectus sets: Tus-*, Upload-*, Content-Type,
 *             Content-Length, Transfer-Encoding, Expect or X-HTTP-Method-Override
 * @param value the value, possibly empty, without line breaks; NULL stops sending the header
 * @return VECTUS_OK, VECTUS_ERR_INVALID for a name or value HTTP or vectus does not allow, or VECTUS_ERR_MEMORY
 */
vectus_code vectus_client_set_header(vectus_client client, const char *name, const char *value);

/**
 * Send PATCH and DELETE requests as POST with an X-HTTP-Method-Override header naming the real method,
 * for proxies or firewalls that block those methods. Off by default
 *
 * @param enabled non-zero to turn it on
 */
void vectus_client_set_method_override(vectus_client client, int enabled);

/**
 * Call callback with each request's curl handle before it goes out, see vectus_curl_callback
 *
 * @param callback the function to call, or NULL to stop calling one
 * @param userdata passed to callback
 */
void vectus_client_set_curl_callback(vectus_client client, vectus_curl_callback callback, void *userdata);

/**
 * Ask the server which tus versions and extensions it offers, and the largest upload it accepts.
 * vectus_upload_perform and vectus_upload_terminate do it when they need to, so call it to learn these beforehand,
 * or before sharing the client between threads
 *
 * @return VECTUS_OK, VECTUS_ERR_PROTOCOL when the server does not offer tus 1.0.0, or why the request failed;
 *         vectus_client_error has the details
 */
vectus_code vectus_client_discover(vectus_client client);

/**
 * The tus extensions the server offers
 *
 * @return VECTUS_EXT_* flags, 0 until discovered
 */
unsigned int vectus_client_extensions(vectus_client client);

/**
 * The largest upload the server accepts, from its Tus-Max-Size header
 *
 * @return the size in bytes, 0 when the server gives no limit or until discovered
 */
unsigned long long vectus_client_max_size(vectus_client client);

/**
 * The HTTP status of the last vectus_client_discover response, such as 401 when the server wants credentials
 *
 * @return the status, 0 if no response came
 */
long vectus_client_http_status(vectus_client client);

/**
 * Why the last vectus_client_discover failed, in more detail than vectus_strerror: curl's reason, the HTTP status,
 * or what the server got wrong
 *
 * @return a message valid until the next call on the client, "" when it succeeded
 */
const char *vectus_client_error(vectus_client client);

/**
 * Free up a vectus_client instance
 *
 * @param client the vectus_client to cleanup, or NULL to do nothing
 */
void vectus_client_cleanup(vectus_client client);

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_CLIENT_H */

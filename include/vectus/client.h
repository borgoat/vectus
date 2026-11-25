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
    VECTUS_ERR_HTTP_STATUS,  /* the server answered with an unexpected HTTP status */
    VECTUS_ERR_PROTOCOL      /* the server does not offer tus 1.0.0 */
} vectus_code;

/**
 * Describe a vectus_code
 *
 * @return a static, human-readable message
 */
const char *vectus_strerror(vectus_code code);

/**
 * vectus_client represents a tus client connected to 1 tus server
 */
typedef struct vectus_client_s *vectus_client;

/**
 * Create a vectus_client for the tus server at url, asking it which versions and extensions it offers.
 * Once done, release the client with vectus_client_cleanup
 *
 * @param client set to the new vectus_client on success, to NULL otherwise
 * @param url the tus server's upload endpoint
 * @return VECTUS_OK, or why the client could not be created
 */
vectus_code vectus_client_init(vectus_client *client, const char *url);

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

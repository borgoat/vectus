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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * vectus_client represents a tus client connected to 1 tus server
 */
typedef struct vectus_client_s *vectus_client;

/**
 * Create a vectus_client pointing to the given tus server url.
 * Once done, release the client with vectus_client_cleanup
 *
 * @return a vectus_client
 */
vectus_client vectus_client_init(const char *url);

/**
 * Free up a vectus_client instance
 *
 * @param client the vectus_client to cleanup
 */
void vectus_client_cleanup(vectus_client client);

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_CLIENT_H */

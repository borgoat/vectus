/* SPDX-License-Identifier: MIT */

#ifndef VECTUS_H
#define VECTUS_H

#include <curl/curl.h>
#include <stdbool.h>

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * vectus_upload represents a file to be uploaded via tus
 */
typedef struct vectus_upload_s *vectus_upload;

/**
 * Create a vectus_upload reading its bytes from read_callback.
 * Once done, release it with vectus_upload_cleanup
 *
 * @return a vectus_upload, or NULL if out of memory
 */
vectus_upload vectus_upload_create(curl_read_callback read_callback);

/**
 * Free up a vectus_upload instance
 *
 * @param upload the vectus_upload to cleanup
 */
void vectus_upload_cleanup(vectus_upload upload);

typedef struct vectus_uploader_s *vectus_uploader;

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_H */

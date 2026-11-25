/* SPDX-License-Identifier: MIT */

#ifndef VECTUS_H
#define VECTUS_H

#include <stddef.h>

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called for the next bytes to upload: write at most size * nitems bytes into buffer and return how many were written, 0 at the end.
 * It has the signature of libcurl's CURLOPT_READFUNCTION, without making vectus users include curl.
 */
typedef size_t (*vectus_read_callback)(char *buffer, size_t size, size_t nitems, void *userdata);

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
vectus_upload vectus_upload_create(vectus_read_callback read_callback);

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

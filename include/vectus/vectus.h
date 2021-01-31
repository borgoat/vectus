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

vectus_upload vectus_upload_create(curl_read_callback read_callback);

typedef struct vecuts_uploader_s *vectus_uploader;

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_H */

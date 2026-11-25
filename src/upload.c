/* SPDX-License-Identifier: MIT */

#include <stdlib.h>

#include <vectus/vectus.h>


struct vectus_upload_s {
    /**
     * read_callback will be called when performing the upload to get the byte stream
     */
    curl_read_callback read_callback;
};

vectus_upload vectus_upload_create(curl_read_callback read_callback) {
    vectus_upload upload = calloc(1, sizeof(struct vectus_upload_s));

    if (upload != NULL) upload->read_callback = read_callback;
    return upload;
}

void vectus_upload_cleanup(vectus_upload upload) {
    free(upload);
}

/* SPDX-License-Identifier: MIT */

#include "include/vectus/vectus.h"


struct vectus_upload_s {
    /**
     * read_callback will be called when performing the upload to get the byte stream
     */
    curl_read_callback read_callback;
};

vectus_upload vectus_upload_create(curl_read_callback read_callback) {

}

/* SPDX-License-Identifier: MIT */

/*
 * Upload a file to a tus server, retrying after network and server errors:
 *
 *   vectus-upload https://tus.example.com/files/ video.mp4
 *
 * It prints the upload's URL; pass that as a third argument to resume in a later run.
 * Set VECTUS_AUTHORIZATION to send it as the Authorization header, such as "Bearer <token>".
 */

#define _POSIX_C_SOURCE 200809L  /* sleep */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vectus/curl.h>
#include <vectus/vectus.h>

static int show_progress(void *userdata, unsigned long long offset, unsigned long long length) {
    (void) userdata;
    fprintf(stderr, "\r%llu of %llu bytes", offset, length);
    return 0;  /* anything else would cancel */
}

int main(int argc, char **argv) {
    const char *authorization = getenv("VECTUS_AUTHORIZATION");
    const char *name = argc > 2 && strrchr(argv[2], '/') != NULL ? strrchr(argv[2], '/') + 1 : argv[2];
    vectus_client client = NULL;
    vectus_upload upload = NULL;
    vectus_code code;
    int attempt;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s ENDPOINT FILE [UPLOAD-URL]\n", argv[0]);
        return 2;
    }

    code = vectus_client_create(&client, argv[1], vectus_curl_transport(), NULL);
    if (code == VECTUS_OK && authorization != NULL) code = vectus_client_set_header(client, "Authorization", authorization);
    if (code == VECTUS_OK) code = vectus_upload_create_file(&upload, argv[2]);
    if (code == VECTUS_OK) code = vectus_upload_add_metadata(upload, "filename", name);
    if (code == VECTUS_OK && argc == 4) code = vectus_upload_set_url(upload, argv[3]);
    if (code != VECTUS_OK) {
        fprintf(stderr, "%s\n", code == VECTUS_ERR_SOURCE ? strerror(errno) : vectus_strerror(code));  /* the file could not be opened */
        goto done;
    }
    vectus_upload_set_progress(upload, show_progress, NULL);

    /* Each perform resumes where the server stopped, so errors that may pass are worth another try */
    for (attempt = 1;; attempt++) {
        code = vectus_upload_perform(client, upload);
        if (attempt == 5 || !(code == VECTUS_ERR_TRANSPORT || (code == VECTUS_ERR_HTTP_STATUS && vectus_upload_http_status(upload) >= 500))) {
            break;
        }
        fprintf(stderr, "\n%s; trying again\n", vectus_upload_error(upload));
        sleep((unsigned) attempt);
    }

    fprintf(stderr, "\n");
    if (code == VECTUS_OK) {
        printf("%s\n", vectus_upload_url(upload));
    } else {
        fprintf(stderr, "%s: %s\n", vectus_strerror(code), vectus_upload_error(upload));
        if (vectus_upload_url(upload) != NULL) fprintf(stderr, "resume with: %s %s %s %s\n", argv[0], argv[1], argv[2], vectus_upload_url(upload));
    }

done:
    vectus_upload_cleanup(upload);
    vectus_client_cleanup(client);
    return code == VECTUS_OK ? 0 : 1;
}

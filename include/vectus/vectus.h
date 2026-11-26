/* SPDX-License-Identifier: MIT */

#ifndef VECTUS_H
#define VECTUS_H

#include <stddef.h>
#include <time.h>

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return this from a vectus_read_callback to abort the upload, for example when reading the data failed
 */
#define VECTUS_READ_ABORT ((size_t) -1)

/**
 * Called for the next bytes to upload: write at most size * nitems bytes into buffer and return how many were written.
 * The stream starts at the upload's first byte and must deliver exactly its length; return VECTUS_READ_ABORT to give up.
 */
typedef size_t (*vectus_read_callback)(char *buffer, size_t size, size_t nitems, void *userdata);

/**
 * Called to move the stream to offset bytes from its start, so an interrupted upload resumes where the server stopped
 *
 * @return 0 on success, anything else if the stream cannot move there
 */
typedef int (*vectus_seek_callback)(void *userdata, unsigned long long offset);

/**
 * vectus_upload represents a file to be uploaded via tus
 */
typedef struct vectus_upload_s *vectus_upload;

/**
 * Create a vectus_upload of length bytes, read from read_callback.
 * Once done, release it with vectus_upload_cleanup
 *
 * @param upload set to the new vectus_upload on success, to NULL otherwise
 * @param read_callback delivers the bytes to upload
 * @param seek_callback moves the stream when resuming mid-way, or NULL if it cannot; resuming mid-way then fails with VECTUS_ERR_SOURCE
 * @param userdata passed to both callbacks
 * @param length the number of bytes to upload
 * @return VECTUS_OK, VECTUS_ERR_MEMORY, or VECTUS_ERR_TOO_LARGE if length does not fit curl's signed 64-bit size
 */
vectus_code vectus_upload_create(vectus_upload *upload, vectus_read_callback read_callback, vectus_seek_callback seek_callback,
                                 void *userdata, unsigned long long length);

/**
 * Send the bytes the server does not have yet: create the upload on the server if it has no URL,
 * otherwise ask the server how many bytes it has, then send the rest.
 * Uploads up to 1 MiB go along with the request creating them when the server offers creation-with-upload;
 * if that request fails, the next call starts over.
 * After VECTUS_ERR_TRANSPORT or VECTUS_ERR_HTTP_STATUS, for example when the connection dropped,
 * call it again to resume where the server stopped.
 *
 * @return VECTUS_OK once the server has every byte, VECTUS_ERR_UNSUPPORTED when the upload has no URL and the server
 *         does not offer the creation extension, or why it stopped
 */
vectus_code vectus_upload_perform(vectus_client client, vectus_upload upload);

/**
 * Add a key and value to the metadata sent when vectus_upload_perform creates the upload, such as its filename.
 * Metadata added after the upload was created only reaches the server if it creates the upload again
 *
 * @param key printable ASCII without spaces or commas, not empty and not added before
 * @param value any text, possibly empty; it travels Base64-encoded
 * @return VECTUS_OK, VECTUS_ERR_INVALID for a key the protocol does not allow, or VECTUS_ERR_MEMORY
 */
vectus_code vectus_upload_add_metadata(vectus_upload upload, const char *key, const char *value);

/**
 * Delete the upload from the server and forget its URL, so the next vectus_upload_perform starts over.
 * Does nothing when the upload has no URL
 *
 * @return VECTUS_OK, VECTUS_ERR_UNSUPPORTED when the server does not offer the termination extension,
 *         VECTUS_ERR_GONE when the server no longer has the upload (its URL is kept), or why the request failed
 */
vectus_code vectus_upload_terminate(vectus_client client, vectus_upload upload);

/**
 * The upload's URL on the server, NULL until vectus_upload_perform created it.
 * Store it to resume the upload later, possibly from another process, with vectus_upload_set_url
 */
const char *vectus_upload_url(vectus_upload upload);

/**
 * Point the upload at one created earlier, so vectus_upload_perform resumes it instead of creating a new one
 *
 * @param url a URL vectus_upload_url returned, or NULL to forget it so the next perform creates a new upload
 * @return VECTUS_OK, or VECTUS_ERR_MEMORY
 */
vectus_code vectus_upload_set_url(vectus_upload upload, const char *url);

/**
 * How many bytes the server confirmed it has, as of the last vectus_upload_perform
 */
unsigned long long vectus_upload_offset(vectus_upload upload);

/**
 * When the server will remove the upload if it is still unfinished, from its Upload-Expires header (the expiration extension).
 * Store it along with the URL to skip resuming an upload the server has already removed
 *
 * @return seconds since the epoch, or 0 if no response about the upload's current URL gave a date
 */
time_t vectus_upload_expires(vectus_upload upload);

/**
 * Free up a vectus_upload instance
 *
 * @param upload the vectus_upload to cleanup, or NULL to do nothing
 */
void vectus_upload_cleanup(vectus_upload upload);

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_H */

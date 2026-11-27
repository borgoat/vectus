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
 * Pass as the length to vectus_upload_create when it is not known yet, such as for a pipe; the upload then ends where
 * the read callback returns 0. Needs a server offering creation-defer-length.
 * When resuming, a length the server was already told is trusted, and the stream is read only up to it.
 * A read callback blocking longer than the stall timeout (30 seconds) fails the request with VECTUS_ERR_TRANSPORT
 */
#define VECTUS_LENGTH_UNKNOWN ((unsigned long long) -1)

/**
 * Called for the next bytes to upload: write at most size bytes into buffer and return how many were written.
 * The stream starts at the upload's first byte and must deliver exactly its length, or end by returning 0 when the
 * length is VECTUS_LENGTH_UNKNOWN; return VECTUS_READ_ABORT to give up.
 * Like every vectus callback, it takes its userdata first
 */
typedef size_t (*vectus_read_callback)(void *userdata, char *buffer, size_t size);

/**
 * Called to move the stream to offset bytes from its start, so an interrupted upload resumes where the server stopped
 *
 * @return 0 on success, anything else if the stream cannot move there
 */
typedef int (*vectus_seek_callback)(void *userdata, unsigned long long offset);

/**
 * Called as the data goes out, at least once a second while a request runs, so it can show progress or cancel.
 * offset counts the bytes sent so far, starting from what the server confirmed; resuming can take it back
 *
 * @param length the upload's length, VECTUS_LENGTH_UNKNOWN until a stream of unknown length ends
 * @return 0 to carry on, anything else to stop the request, so vectus_upload_perform returns VECTUS_ERR_CANCELLED
 */
typedef int (*vectus_progress_callback)(void *userdata, unsigned long long offset, unsigned long long length);

/**
 * vectus_upload represents a file to be uploaded via tus.
 * Every function taking one accepts NULL: getters then return 0, NULL or "", the others VECTUS_ERR_INVALID or nothing
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
 * @param length the number of bytes to upload, or VECTUS_LENGTH_UNKNOWN
 * @return VECTUS_OK, VECTUS_ERR_INVALID without a read_callback, VECTUS_ERR_MEMORY,
 *         or VECTUS_ERR_TOO_LARGE if length does not fit curl's signed 64-bit size
 */
vectus_code vectus_upload_create(vectus_upload *upload, vectus_read_callback read_callback, vectus_seek_callback seek_callback,
                                 void *userdata, unsigned long long length);

/**
 * Create a vectus_upload of the file at path, which stays open until vectus_upload_cleanup.
 * It must not change during the upload. For a pipe, use vectus_upload_create with VECTUS_LENGTH_UNKNOWN
 *
 * @param upload set to the new vectus_upload on success, to NULL otherwise
 * @param path a regular file
 * @return VECTUS_OK, VECTUS_ERR_SOURCE when the file cannot be opened or is not a regular file, with errno saying why,
 *         VECTUS_ERR_MEMORY, or VECTUS_ERR_INVALID for a NULL path
 */
vectus_code vectus_upload_create_file(vectus_upload *upload, const char *path);

/**
 * Mark the upload as a partial one, uploaded like any other, then joined with others by vectus_upload_create_final.
 * Call it before vectus_upload_perform creates the upload, which then needs a server offering concatenation
 */
void vectus_upload_set_partial(vectus_upload upload);

/**
 * Create a vectus_upload that vectus_upload_perform turns into the concatenation of finished partial uploads.
 * The server assembles it, so the client has no bytes to send: its length and offset stay 0, and once it has a URL,
 * perform does nothing. The server does not copy the partial uploads' metadata, so add it to this one.
 * Once done, release it with vectus_upload_cleanup
 *
 * @param upload set to the new vectus_upload on success, to NULL otherwise
 * @param partial_urls the partial uploads' URLs, such as from vectus_upload_url, in the order to join them
 * @param count how many partial_urls there are, at least 1
 * @return VECTUS_OK, VECTUS_ERR_INVALID for no URLs, or a URL NULL, empty or with spaces or control characters, or VECTUS_ERR_MEMORY
 */
vectus_code vectus_upload_create_final(vectus_upload *upload, const char *const *partial_urls, size_t count);

/**
 * Send the bytes the server does not have yet: create the upload on the server if it has no URL,
 * otherwise ask the server how many bytes it has, then send the rest.
 * Creating asks the server what it offers first, unless vectus_client_discover already did.
 * Uploads up to 1 MiB go along with the request creating them when the server offers creation-with-upload;
 * if that request fails, the next call starts over.
 * After VECTUS_ERR_TRANSPORT, VECTUS_ERR_HTTP_STATUS or VECTUS_ERR_CANCELLED, for example when the connection dropped,
 * call it again to resume where the server stopped.
 *
 * @return VECTUS_OK once the server has every byte, VECTUS_ERR_UNSUPPORTED when the upload has no URL and the server
 *         does not offer the creation extension (or creation-defer-length, for VECTUS_LENGTH_UNKNOWN, or concatenation,
 *         for partial and final uploads), or why it stopped, with vectus_upload_error saying more
 */
vectus_code vectus_upload_perform(vectus_client client, vectus_upload upload);

/**
 * Call callback as the data goes out, see vectus_progress_callback
 *
 * @param callback the function to call, or NULL to stop calling one
 * @param userdata passed to callback
 */
void vectus_upload_set_progress(vectus_upload upload, vectus_progress_callback callback, void *userdata);

/**
 * Add a key and value to the metadata sent when vectus_upload_perform creates the upload, such as its filename.
 * Metadata added after the upload was created only reaches the server if it creates the upload again
 *
 * @param key printable ASCII without spaces or commas, not empty and not added before
 * @param value any text, possibly empty; it travels Base64-encoded
 * @return VECTUS_OK, VECTUS_ERR_INVALID for a key the protocol does not allow or a NULL value, or VECTUS_ERR_MEMORY
 */
vectus_code vectus_upload_add_metadata(vectus_upload upload, const char *key, const char *value);

/**
 * Delete the upload from the server and forget its URL, so the next vectus_upload_perform starts over.
 * Does nothing when the upload has no URL
 *
 * @return VECTUS_OK, VECTUS_ERR_UNSUPPORTED when the server does not offer the termination extension,
 *         VECTUS_ERR_GONE when the server no longer has the upload (its URL is kept), or why the request failed,
 *         with vectus_upload_error saying more
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
 * The HTTP status of the last response during the last vectus_upload_perform or vectus_upload_terminate,
 * such as 401 when the server wants new credentials, or 403 behind VECTUS_ERR_GONE, which an authorization proxy
 * may answer too: check it before forgetting the URL
 *
 * @return the status, 0 if no response came
 */
long vectus_upload_http_status(vectus_upload upload);

/**
 * Why the last vectus_upload_perform or vectus_upload_terminate failed, in more detail than vectus_strerror:
 * curl's reason, the HTTP status and request, or what the server or the data got wrong
 *
 * @return a message valid until the next call on the upload, "" when it succeeded
 */
const char *vectus_upload_error(vectus_upload upload);

/**
 * Free up a vectus_upload instance, closing its connection to the server and the file vectus_upload_create_file opened
 *
 * @param upload the vectus_upload to cleanup, or NULL to do nothing
 */
void vectus_upload_cleanup(vectus_upload upload);

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_H */

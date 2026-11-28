/* SPDX-License-Identifier: MIT */

/* Shared by the library sources and their tests; not installed */

#ifndef VECTUS_INTERNAL_H
#define VECTUS_INTERNAL_H

#include <stddef.h>

#include <vectus/client.h>
#include <vectus/transport.h>

/* Room for an error description, as vectus_client_error and vectus_upload_error return */
#define VECTUS_ERROR_SIZE 256

struct vectus_client_s {

    /**
     * the endpoint of the tus server client will be uploading to
     */
    char *url;

    /**
     * the extensions supported by the server
     */
    unsigned int extensions;

    /**
     * the tus version of the server
     */
    char *version;

    /**
     * the max size supported by the server
     */
    unsigned long long int max_size;

    /**
     * set once the server said which versions and extensions it offers
     */
    int discovered;

    /**
     * set to send PATCH and DELETE as POST with X-HTTP-Method-Override
     */
    int method_override;

    /**
     * the application's own headers, sent on every request: name and value pairs ending with NULL, NULL for none
     */
    char **headers;

    /**
     * what sends the requests, and the userdata it opens connections with
     */
    const vectus_transport *transport;
    void *transport_userdata;

    /**
     * the HTTP status of the last discovery response, 0 if none came
     */
    long status;

    /**
     * why the last discovery failed, empty after one succeeded
     */
    char error[VECTUS_ERROR_SIZE];

    /**
     * set when storing a response header ran out of memory
     */
    int out_of_memory;
};

/**
 * A transport's connection, kept for an upload's requests, with what opened it
 */
struct vectus_connection {
    void *handle;  /* NULL until the first request */
    const vectus_transport *transport;
    void *userdata;
};

/**
 * Parse a header value made only of digits, such as Tus-Max-Size
 *
 * @return 1 with the value stored in size, or 0 leaving size alone for anything else, including a sign or a value too large
 */
int vectus_parse_size(const char *value, unsigned long long *size);

/**
 * Send a request over connection, opening it first with the client's transport, with the application's headers after
 * the request's own
 *
 * @param name names the request in error descriptions: PATCH even when it goes out as POST
 * @param request the request, whose headers are its own, ending with NULL
 * @param error set to the transport's reason when no response came, or to the response's status otherwise
 * @param status set to the response's HTTP status, 0 if none came
 * @return VECTUS_OK when a response arrived, whatever its status, VECTUS_ERR_MEMORY, or VECTUS_ERR_TRANSPORT
 */
vectus_code vectus_send(vectus_client client, struct vectus_connection *connection, const char *name, vectus_request *request,
                        char *error, long *status);

/**
 * Close the connection, if a request opened it
 */
void vectus_close(struct vectus_connection *connection);

/**
 * vectus_client_discover over connection, so an upload asks over the connection it then uses
 */
vectus_code vectus_discover(vectus_client client, struct vectus_connection *connection);

#endif /* VECTUS_INTERNAL_H */

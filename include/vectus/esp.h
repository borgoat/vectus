/* SPDX-License-Identifier: MIT */

/*
 * The ESP-IDF transport, over esp_http_client:
 *
 *   vectus_client_create(&client, "https://tus.example.com/files/", vectus_esp_transport(), NULL);
 *
 * Built when vectus is an ESP-IDF component; tested on ESP-IDF 5.5
 */

#ifndef VECTUS_ESP_H
#define VECTUS_ESP_H

#include "esp_http_client.h"

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called with the configuration of each esp_http_client handle before vectus makes it, to set what vectus has no
 * setting for, such as cert_pem for a private certificate authority, or timeout_ms.
 * Leave alone what vectus sets: url, method, event_handler, user_data and buffer_size_tx
 */
typedef void (*vectus_esp_callback)(void *userdata, esp_http_client_config_t *config);

/**
 * What to pass vectus_client_create as the transport's userdata, or NULL for none of it
 */
struct vectus_esp_options {
    /**
     * Called with each handle's configuration, see vectus_esp_callback; NULL for none
     */
    vectus_esp_callback callback;

    /**
     * Passed to callback
     */
    void *userdata;
};

/**
 * The transport sending requests through esp_http_client, one handle per upload. Its userdata, a struct
 * vectus_esp_options or NULL, must outlive the client.
 * A request fails after 30 seconds without a byte moving (timeout_ms). https uses ESP-IDF's certificate bundle when
 * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE is set. It cannot wait for 100 Continue, so creation-with-upload sends its data
 * at once. It calls the progress callback as each request starts and after each 4 KiB of the body
 */
const vectus_transport *vectus_esp_transport(void);

#ifdef __cplusplus
}
#endif

#endif /* VECTUS_ESP_H */

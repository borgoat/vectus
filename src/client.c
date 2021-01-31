/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <vectus/client.h>


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
};

/**
 * Convert Tus-Extensions into the extensions flags
 * Something like creation,creation-with-upload,termination,concatenation,creation-defer-length
 *
 * @param extensions
 * @return
 */
unsigned int parse_extensions(char *extensions) {
    unsigned int ext_flag = 0;
    char *current = extensions;
    char *next = NULL;

    for (;;) {
        next = strchr(current, ',');
        if (next != NULL) *next = '\0';

        if (strcmp(current, "creation") == 0) {
            ext_flag |= VECTUS_EXT_CREATION;
        } else if (strcmp(current, "creation-with-upload") == 0) {
            ext_flag |= VECTUS_EXT_CREATION_WITH_UPLOAD;
        } else if (strcmp(current, "termination") == 0) {
            ext_flag |= VECTUS_EXT_TERMINATION;
        } else if (strcmp(current, "concatenation") == 0) {
            ext_flag |= VECTUS_EXT_CONCATENATION;
        } else if (strcmp(current, "expiration") == 0) {
            ext_flag |= VECTUS_EXT_EXPIRATION;
        } else if (strcmp(current, "checksum") == 0) {
            ext_flag |= VECTUS_EXT_CHECKSUM;
        }

        if (next == NULL) break;
        current = next + 1;
    }

    return ext_flag;
}

/**
 * Used to fetch the tus extensions the server implements while initialising our vectus_client
 *
 * @param buffer
 * @param size
 * @param nitems
 * @param userdata
 * @return
 */
size_t client_init_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    vectus_client v = userdata;

    size_t bytes = size * nitems;
    char *buffer_end = buffer + bytes - 1;

    char *sep = memchr(buffer, ':', bytes);
    if (sep == NULL) {  /* Check if the header is not of the Key: Value form (first line usually: GET / [...]) */
        return bytes;
    }

    *sep = '\0';  /* The separator - colon (:) - is set to NULL so that we have a null-terminated string starting at buffer */
    /* Trim newlines from the end of our header */
    while (*buffer_end == '\n' || *buffer_end == '\r') buffer_end--;
    buffer_end++;

    *buffer_end = '\0';
    char *name = buffer; /* At buffer - null-terminated now at sep - starts our key */
    char *value = sep + 1; /* After sep starts our value */

    /* Since the value may have an undefined number of whitespaces on the left, we trim them */
    while (*value == ' ') {
        value++;
    }

    if (strcmp(name, "Tus-Version") == 0) {
        v->version = calloc(strlen(value), sizeof(char));
        strcpy(v->version, value);
    } else if (strcmp(name, "Tus-Resumable") == 0) {
        /* TODO */
    } else if (strcmp(name, "Tus-Extension") == 0) {
        v->extensions = parse_extensions(value);
    }

    return bytes;
}

vectus_client vectus_client_init(const char *url) {
    vectus_client v = malloc(sizeof(struct vectus_client_s));

    v->url = calloc(strlen(url), sizeof(char));
    strcpy(v->url, url);

    CURL *c = curl_easy_init();

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "OPTIONS");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, client_init_header_callback);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *) v);

    CURLcode res = curl_easy_perform(c);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
        /* TODO: handle error */
    }

    curl_easy_cleanup(c);
    return v;
}

void vectus_client_cleanup(vectus_client client) {
    free(client->url);
    free(client->version);
    free(client);
}

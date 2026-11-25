/* SPDX-License-Identifier: MIT */

/*
 * libFuzzer target for the response header parsing in src/client.c.
 * Needs clang with libFuzzer (Apple's clang has none: use Linux or Homebrew llvm):
 *
 *   clang -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -Iinclude fuzz/header.c $(pkg-config --cflags --libs libcurl) -o fuzz-header
 *   ./fuzz-header -dict=fuzz/header.dict -max_total_time=60
 */

#include <stdint.h>

/* client.c and upload.c to reach the static parsers */
#include "../src/client.c"
#include "../src/upload.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct vectus_client_s client = {0};
    struct upload_response response = {0};
    const uint8_t *end = data + size;

    /* Hand the input over line by line like curl does, each line in an exact-size buffer so ASan sees overruns */
    while (data < end) {
        const uint8_t *newline = memchr(data, '\n', (size_t)(end - data));
        size_t len = newline != NULL ? (size_t)(newline - data) + 1 : (size_t)(end - data);
        char *line = malloc(len);

        if (line == NULL) break;
        memcpy(line, data, len);
        client_init_header_callback(line, 1, len, &client);
        upload_header_callback(line, 1, len, &response);
        free(line);
        data += len;
    }

    free(client.version);
    free(response.location);
    return 0;
}

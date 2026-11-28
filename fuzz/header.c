/* SPDX-License-Identifier: MIT */

/*
 * libFuzzer target for the response header parsing in src/client.c, src/upload.c and src/curl.c.
 * Needs clang with libFuzzer (Apple's clang has none: use Linux or Homebrew llvm):
 *
 *   clang -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -Iinclude fuzz/header.c $(pkg-config --cflags --libs libcurl) -o fuzz-header
 *   ./fuzz-header -dict=fuzz/header.dict -max_total_time=60
 */

#include <stdint.h>

/* client.c, upload.c and curl.c to reach the static parsers */
#include "../src/client.c"
#include "../src/upload.c"
#include "../src/curl.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct vectus_client_s client = {0};
    struct upload_response response = {0};
    struct upload_exchange exchange = {NULL, &response};
    const uint8_t *end = data + size;

    /* Hand the input over line by line like curl does, each line in an exact-size buffer so ASan sees overruns */
    while (data < end) {
        const uint8_t *newline = memchr(data, '\n', (size_t)(end - data));
        size_t len = newline != NULL ? (size_t)(newline - data) + 1 : (size_t)(end - data);
        char *line = malloc(len);

        if (line == NULL) break;
        memcpy(line, data, len);
        easy_parse_header_line(line, len, client_handle_header, &client);
        easy_parse_header_line(line, len, upload_handle_header, &exchange);
        free(line);
        data += len;
    }

    free(client.version);
    free(response.location);
    return 0;
}

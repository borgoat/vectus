#include <check.h>

/* client.c to access shadowed fields */
#include "../src/client.c"

/* Hand one header line to the callback the way curl does: not NUL-terminated, in an exact-size buffer so ASan sees overruns */
static void feed_header(vectus_client c, const char *line) {
    size_t len = strlen(line);
    char *buffer = malloc(len);

    memcpy(buffer, line, len);
    ck_assert_uint_eq(client_init_header_callback(buffer, 1, len, c), len);
    free(buffer);
}

START_TEST(test_parse_extensions)
{
    char extensions[] = "creation, termination ,checksum,creation-defer-length";

    ck_assert_uint_eq(parse_extensions(extensions),
                      VECTUS_EXT_CREATION | VECTUS_EXT_TERMINATION | VECTUS_EXT_CHECKSUM | VECTUS_EXT_CREATION_DEFER_LENGTH);
}
END_TEST

START_TEST(test_header_callback)
{
    vectus_client c = calloc(1, sizeof *c);

    feed_header(c, "HTTP/2 204\r\n");
    feed_header(c, "tus-version: 1.0.0,0.2.2\r\n");  /* HTTP/2 sends lowercase header names */
    feed_header(c, "TUS-EXTENSION:\tcreation,expiration \r\n");
    feed_header(c, "\r\n");
    ck_assert_str_eq(c->version, "1.0.0,0.2.2");
    ck_assert_uint_eq(c->extensions, VECTUS_EXT_CREATION | VECTUS_EXT_EXPIRATION);

    feed_header(c, "Tus-Version: 1.0.0");  /* no trailing newline, and replaces the earlier value */
    ck_assert_str_eq(c->version, "1.0.0");

    feed_header(c, "Tus-Max-Size: 18446744073709551615\r\n");
    ck_assert(c->max_size == 18446744073709551615ULL);
    c->max_size = 0;
    feed_header(c, "Tus-Max-Size: -1\r\n");
    feed_header(c, "Tus-Max-Size: 12abc\r\n");
    feed_header(c, "Tus-Max-Size:\r\n");
    ck_assert(c->max_size == 0);
    feed_header(c, "Tus-Max-Size: 18446744073709551616\r\n");  /* one past the maximum */
    ck_assert(c->max_size == 0);

    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_cleanup_without_headers)
{
    vectus_client c = calloc(1, sizeof *c);

    feed_header(c, "Content-Length: 0\r\n");
    ck_assert_ptr_null(c->version);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_vectus_client_init_unreachable)
{
    vectus_client_cleanup(NULL);
    ck_assert_ptr_null(vectus_client_init("http://127.0.0.1:1/"));  /* nothing listens on port 1 */
}
END_TEST

/* Needs a tus server, e.g. VECTUS_TEST_URL=http://localhost:8080/files/ */
START_TEST(test_vectus_client_init)
{
    const char *url = getenv("VECTUS_TEST_URL");
    vectus_client c;
    c = vectus_client_init(url);

    ck_assert_ptr_nonnull(c);
    ck_assert_str_eq(c->url, url);
    ck_assert(c->extensions & VECTUS_EXT_CREATION);

    vectus_client_cleanup(c);
}
END_TEST

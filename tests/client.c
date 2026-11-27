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

START_TEST(test_parse_size)
{
    unsigned long long size = 7;

    ck_assert_int_eq(vectus_parse_size("18446744073709551615", &size), 1);
    ck_assert(size == 18446744073709551615ULL);
    size = 7;
    ck_assert_int_eq(vectus_parse_size("18446744073709551616", &size), 0);  /* one past the maximum */
    ck_assert_int_eq(vectus_parse_size("-1", &size), 0);
    ck_assert_int_eq(vectus_parse_size("+1", &size), 0);
    ck_assert_int_eq(vectus_parse_size("12abc", &size), 0);
    ck_assert_int_eq(vectus_parse_size("", &size), 0);
    ck_assert(size == 7);  /* failures leave it alone */
    ck_assert_int_eq(vectus_parse_size("0", &size), 1);
    ck_assert(size == 0);
}
END_TEST

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

START_TEST(test_client_getters)
{
    vectus_client c = calloc(1, sizeof *c);

    feed_header(c, "Tus-Extension: creation,termination\r\n");
    feed_header(c, "Tus-Max-Size: 1073741824\r\n");
    ck_assert_uint_eq(vectus_client_extensions(c), VECTUS_EXT_CREATION | VECTUS_EXT_TERMINATION);
    ck_assert(vectus_client_max_size(c) == 1073741824ULL);
    ck_assert_uint_eq(vectus_client_extensions(NULL), 0);
    ck_assert(vectus_client_max_size(NULL) == 0);
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

START_TEST(test_offers_tus_1_0_0)
{
    ck_assert(offers_tus_1_0_0("1.0.0"));
    ck_assert(offers_tus_1_0_0("0.2.2, 1.0.0"));
    ck_assert(!offers_tus_1_0_0("1.0.01,0.2.2"));
    ck_assert(!offers_tus_1_0_0("11.0.0"));
    ck_assert(!offers_tus_1_0_0(""));
}
END_TEST

START_TEST(test_strerror)
{
    vectus_code code;

    for (code = VECTUS_OK; code <= VECTUS_ERR_CANCELLED; code++) {
        ck_assert_str_ne(vectus_strerror(code), "unknown error");
    }
    ck_assert_str_eq(vectus_strerror((vectus_code) 99), "unknown error");
}
END_TEST

START_TEST(test_client_discover_unreachable)
{
    vectus_client c;

    ck_assert_int_eq(vectus_client_create(&c, "http://127.0.0.1:1/"), VECTUS_OK);  /* nothing listens on port 1, but nothing asks yet */
    ck_assert_str_eq(vectus_client_error(c), "");
    ck_assert_int_eq(vectus_client_discover(c), VECTUS_ERR_TRANSPORT);
    ck_assert_msg(strncmp(vectus_client_error(c), "OPTIONS: ", 9) == 0 && strlen(vectus_client_error(c)) > 9, "%s", vectus_client_error(c));
    ck_assert_int_eq(vectus_client_http_status(c), 0);
    ck_assert_uint_eq(vectus_client_extensions(c), 0);
    vectus_client_cleanup(c);
}
END_TEST

/* Needs a tus server, e.g. VECTUS_TEST_URL=http://localhost:8080/files/ */
START_TEST(test_client_discover)
{
    const char *url = getenv("VECTUS_TEST_URL");
    vectus_client c;

    ck_assert_int_eq(vectus_client_create(&c, url), VECTUS_OK);
    ck_assert_int_eq(vectus_client_discover(c), VECTUS_OK);
    ck_assert_str_eq(c->url, url);
    ck_assert(c->extensions & VECTUS_EXT_CREATION);
    ck_assert_int_ge(vectus_client_http_status(c), 200);
    ck_assert_str_eq(vectus_client_error(c), "");

    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_client_create)
{
    const char *invalid[] = {"", "not a url", "/files/", "http://", "ftp://h/files/", "file:///tmp/files/"};
    vectus_client c = (vectus_client) &c;  /* any non-NULL value, to check create resets it */
    size_t i;

    ck_assert_int_eq(vectus_client_create(NULL, "http://h/files/"), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_client_create(&c, NULL), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(c);
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        c = (vectus_client) &c;
        ck_assert_msg(vectus_client_create(&c, invalid[i]) == VECTUS_ERR_INVALID, "case %zu", i);
        ck_assert_ptr_null(c);
    }

    ck_assert_int_eq(vectus_client_create(&c, "HTTPS://h:8443/files/"), VECTUS_OK);  /* schemes are case-insensitive */
    ck_assert_str_eq(c->url, "HTTPS://h:8443/files/");
    ck_assert(!c->discovered);  /* nothing asked yet */
    ck_assert_uint_eq(vectus_client_extensions(c), 0);
    ck_assert(vectus_client_max_size(c) == 0);
    ck_assert_int_eq(vectus_client_http_status(c), 0);
    ck_assert_str_eq(vectus_client_error(c), "");
    vectus_client_cleanup(c);
}
END_TEST

/* The client's headers, each followed by a newline */
static const char *client_headers(vectus_client c) {
    static char joined[512];
    struct curl_slist *h;

    joined[0] = '\0';
    for (h = c->headers; h != NULL; h = h->next) {
        strcat(joined, h->data);
        strcat(joined, "\n");
    }
    return joined;
}

START_TEST(test_client_set_header)
{
    const char *invalid_names[] = {
        "", "Bad Name", "Bad:Name", "X\r\nY", "caf\xc3\xa9",
        "Tus-Resumable", "upload-offset", "Upload-Metadata", "content-type", "Content-Length", "Transfer-Encoding", "Expect",
        "X-HTTP-Method-Override",
    };
    const char *invalid_values[] = {"a\r\nX-Injected: 1", "a\nb", "a\rb", "\x01", "a\x7f"};
    vectus_client c;
    size_t i;

    ck_assert_int_eq(vectus_client_create(&c, "http://h/files/"), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, NULL, "1"), VECTUS_ERR_INVALID);
    for (i = 0; i < sizeof invalid_names / sizeof invalid_names[0]; i++) {
        ck_assert_msg(vectus_client_set_header(c, invalid_names[i], "1") == VECTUS_ERR_INVALID, "name %zu", i);
    }
    for (i = 0; i < sizeof invalid_values / sizeof invalid_values[0]; i++) {
        ck_assert_msg(vectus_client_set_header(c, "X-A", invalid_values[i]) == VECTUS_ERR_INVALID, "value %zu", i);
    }
    ck_assert_ptr_null(c->headers);  /* nothing invalid got in */

    ck_assert_int_eq(vectus_client_set_header(c, "Authorization", "Bearer old"), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "X-A", "tab\tand caf\xc3\xa9"), VECTUS_OK);  /* tabs and other bytes are fine */
    ck_assert_int_eq(vectus_client_set_header(c, "X-AB", "kept"), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "X-Empty", ""), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "authorization", "Bearer new"), VECTUS_OK);  /* replaces, whatever the case */
    ck_assert_str_eq(client_headers(c), "X-A: tab\tand caf\xc3\xa9\nX-AB: kept\nX-Empty;\nauthorization: Bearer new\n");

    ck_assert_int_eq(vectus_client_set_header(c, "X-A", NULL), VECTUS_OK);  /* not X-AB, which starts the same */
    ck_assert_int_eq(vectus_client_set_header(c, "X-Missing", NULL), VECTUS_OK);
    ck_assert_str_eq(client_headers(c), "X-AB: kept\nX-Empty;\nauthorization: Bearer new\n");
    vectus_client_cleanup(c);  /* and its headers */
}
END_TEST

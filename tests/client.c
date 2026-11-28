#include <check.h>

/* client.c and curl.c to access shadowed fields */
#include "../src/client.c"
#include "../src/curl.c"

/* Hand one header line to the client the way curl does: not NUL-terminated, in an exact-size buffer so ASan sees overruns */
static void feed_header(vectus_client c, const char *line) {
    size_t len = strlen(line);
    char *buffer = malloc(len);

    memcpy(buffer, line, len);
    ck_assert_uint_eq(easy_parse_header_line(buffer, len, client_handle_header, c), len);
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

START_TEST(test_resolve_url)
{
    /* RFC 3986 section 5.4, which resolves these against http://a/b/c/d;p?q, minus fragments, which vectus drops */
    const char *cases[][2] = {
        {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"}, {"/g", "http://a/g"},
        {"//g", "http://g"}, {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"}, {"g#s", "http://a/b/c/g"},
        {";x", "http://a/b/c/;x"}, {"g;x", "http://a/b/c/g;x"}, {".", "http://a/b/c/"}, {"./", "http://a/b/c/"},
        {"..", "http://a/b/"}, {"../", "http://a/b/"}, {"../g", "http://a/b/g"}, {"../..", "http://a/"},
        {"../../", "http://a/"}, {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"}, {"/./g", "http://a/g"}, {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."}, {".g", "http://a/b/c/.g"}, {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"}, {"./g/.", "http://a/b/c/g/"}, {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"}, {"g;x=1/../y", "http://a/b/c/y"}, {"g?y/./x", "http://a/b/c/g?y/./x"},
        {"g?y/../x", "http://a/b/c/g?y/../x"},
        /* absolute ones, as tusd answers */
        {"https://other:8443/files/x/../y?z#f", "https://other:8443/files/y?z"}, {"HTTP://h", "HTTP://h"},
        {"http://user@[::1]:8080/f", "http://user@[::1]:8080/f"},
    };
    const char *invalid[] = {
        "", "#s", "g:h", "ftp://h/x", "http:g", "http://", "http:///x", "http://user@/x", "http://:80/x", "http://[::1",
        "http://[]/x", "http://h]/x", "g h", "g\r\nX-Injected: 1", "g\x7f", "caf\xc3\xa9",
    };
    char *resolved;
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ck_assert_msg(vectus_resolve_url("http://a/b/c/d;p?q", cases[i][0], &resolved) == VECTUS_OK, "%s", cases[i][0]);
        ck_assert_msg(strcmp(resolved, cases[i][1]) == 0, "%s resolved to %s", cases[i][0], resolved);
        free(resolved);
    }
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        resolved = (char *) invalid;
        ck_assert_msg(vectus_resolve_url("http://a/b/c/d;p?q", invalid[i], &resolved) == VECTUS_ERR_INVALID, "case %zu", i);
        ck_assert_ptr_eq(resolved, (char *) invalid);  /* untouched */
    }

    /* A base without a path, and a relative reference against one */
    ck_assert_int_eq(vectus_resolve_url("http://h:1", "abc", &resolved), VECTUS_OK);
    ck_assert_str_eq(resolved, "http://h:1/abc");
    free(resolved);
}
END_TEST

START_TEST(test_parse_http_date)
{
    const char *invalid[] = {
        "", "soon", "Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994", "Sun, 06 Nov 1994 08:49:37 UTC",
        "Sun, 06 Nov 1994 08:49:37 GMT ", "Sun, 06 Nov 1994 08:49:37", "Sun, 06 Nob 1994 08:49:37 GMT",
        "Sun, 06 ovN 1994 08:49:37 GMT", "Sun, 32 Nov 1994 08:49:37 GMT", "Sun, 00 Nov 1994 08:49:37 GMT",
        "Sun, 06 Nov 1994 24:00:00 GMT", "Sun, 06 Nov 1994 08:60:00 GMT", "Sun, 06 Nov 1969 08:49:37 GMT",
        "Sun, 06 Nov 1994 -8:49:37 GMT",
    };
    size_t i;

    ck_assert(vectus_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777LL);  /* RFC 9110's example */
    ck_assert(vectus_parse_http_date("Wed, 25 Jun 2014 16:00:00 GMT") == 1403712000LL);  /* the tus spec's */
    ck_assert(vectus_parse_http_date("Fri, 31 Dec 1999 23:59:59 GMT") == 946684799LL);
    ck_assert(vectus_parse_http_date("Tue, 29 Feb 2028 12:00:00 GMT") == 1835438400LL);
    ck_assert(vectus_parse_http_date("Thu, 01 Jan 2071 00:00:00 GMT") == 3187296000LL);  /* past 32-bit time_t */
    ck_assert(vectus_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        ck_assert_msg(vectus_parse_http_date(invalid[i]) == -1, "%s", invalid[i]);
    }
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

    ck_assert_int_eq(vectus_client_create(&c, "http://127.0.0.1:1/", vectus_curl_transport(), NULL), VECTUS_OK);  /* nothing listens on port 1, but nothing asks yet */
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

    ck_assert_int_eq(vectus_client_create(&c, url, vectus_curl_transport(), NULL), VECTUS_OK);
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
    const char *invalid[] = {"", "not a url", "/files/", "http://", "ftp://h/files/", "file:///tmp/files/", "http://h/files/\r\nX: 1"};
    vectus_client c = (vectus_client) &c;  /* any non-NULL value, to check create resets it */
    size_t i;

    ck_assert_int_eq(vectus_client_create(NULL, "http://h/files/", vectus_curl_transport(), NULL), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_client_create(&c, NULL, vectus_curl_transport(), NULL), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(c);
    c = (vectus_client) &c;
    ck_assert_int_eq(vectus_client_create(&c, "http://h/files/", NULL, NULL), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(c);
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        c = (vectus_client) &c;
        ck_assert_msg(vectus_client_create(&c, invalid[i], vectus_curl_transport(), NULL) == VECTUS_ERR_INVALID, "case %zu", i);
        ck_assert_ptr_null(c);
    }

    ck_assert_int_eq(vectus_client_create(&c, "HTTPS://h:8443/files/#top", vectus_curl_transport(), NULL), VECTUS_OK);  /* schemes are case-insensitive */
    ck_assert_str_eq(c->url, "HTTPS://h:8443/files/");  /* without the fragment, which stays with the client */
    ck_assert(!c->discovered);  /* nothing asked yet */
    ck_assert_uint_eq(vectus_client_extensions(c), 0);
    ck_assert(vectus_client_max_size(c) == 0);
    ck_assert_int_eq(vectus_client_http_status(c), 0);
    ck_assert_str_eq(vectus_client_error(c), "");
    vectus_client_cleanup(c);
}
END_TEST

/* The client's headers as "Name: value", each followed by a newline */
static const char *client_headers(vectus_client c) {
    static char joined[512];
    size_t i;

    joined[0] = '\0';
    for (i = 0; c->headers != NULL && c->headers[i] != NULL; i += 2) {
        strcat(joined, c->headers[i]);
        strcat(joined, ": ");
        strcat(joined, c->headers[i + 1]);
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

    ck_assert_int_eq(vectus_client_create(&c, "http://h/files/", vectus_curl_transport(), NULL), VECTUS_OK);
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
    ck_assert_str_eq(client_headers(c), "X-A: tab\tand caf\xc3\xa9\nX-AB: kept\nX-Empty: \nauthorization: Bearer new\n");

    ck_assert_int_eq(vectus_client_set_header(c, "X-A", NULL), VECTUS_OK);  /* not X-AB, which starts the same */
    ck_assert_int_eq(vectus_client_set_header(c, "X-Missing", NULL), VECTUS_OK);
    ck_assert_str_eq(client_headers(c), "X-AB: kept\nX-Empty: \nauthorization: Bearer new\n");
    vectus_client_cleanup(c);  /* and its headers */
}
END_TEST

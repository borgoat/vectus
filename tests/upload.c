#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <check.h>

/* upload.c to access shadowed fields */
#include "../src/upload.c"

#define MIB (1024ULL * 1024ULL)

/* Byte i of the test data depends on i, so sending the wrong range cannot go unnoticed */
static unsigned char source_byte(unsigned long long i) {
    return (unsigned char) ((i * 2654435761ULL) >> 24);
}

enum source_failure { SOURCE_FINE, SOURCE_ABORTS, SOURCE_ENDS_EARLY };

/* The data to upload: source_byte, or text when set */
struct source {
    const char *text;
    unsigned long long position;
    enum source_failure failure;
    unsigned long long fail_at;     /* where the failure kicks in */
    void (*hook)(void);             /* called once, when position first reaches hook_at */
    unsigned long long hook_at;
    int reads;
    int seeks;
    unsigned long long first_seek;
};

static size_t source_read(char *buffer, size_t size, size_t nitems, void *userdata) {
    struct source *s = userdata;
    size_t n = size * nitems;
    size_t i;

    s->reads++;
    if (s->failure != SOURCE_FINE && s->position >= s->fail_at) {
        return s->failure == SOURCE_ABORTS ? VECTUS_READ_ABORT : 0;
    }
    if (s->hook != NULL && s->position >= s->hook_at) {
        void (*hook)(void) = s->hook;

        s->hook = NULL;
        hook();
    }
    for (i = 0; i < n; i++) {
        buffer[i] = s->text != NULL ? s->text[s->position + i] : (char) source_byte(s->position + i);
    }
    s->position += n;
    return n;
}

static int source_seek(void *userdata, unsigned long long offset) {
    struct source *s = userdata;

    if (s->seeks++ == 0) s->first_seek = offset;
    s->position = offset;
    return 0;
}

/* A client for a server the test controls, without the OPTIONS handshake */
static vectus_client test_client(const char *url) {
    vectus_client c = calloc(1, sizeof *c);

    c->url = malloc(strlen(url) + 1);
    strcpy(c->url, url);
    c->stall_timeout = 5;
    c->extensions = VECTUS_EXT_CREATION | VECTUS_EXT_TERMINATION;  /* as tusd offers by default */
    return c;
}

static int count(const char *haystack, const char *needle) {
    int n = 0;

    while ((haystack = strstr(haystack, needle)) != NULL) {
        n++;
        haystack++;
    }
    return n;
}

/* ---- A canned-response HTTP server in a child process, reporting the requests it got ---- */

struct fake_server {
    pid_t pid;
    int port;
    int requests;  /* read end of the pipe the child copies every request to */
    char url[64];
};

static void write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);

        if (n <= 0) return;
        data += n;
        len -= (size_t) n;
    }
}

/* Read one request off conn, body included, copying it to out */
static void fake_server_read_request(int conn, int out) {
    static char buffer[1 << 16];
    size_t used = 0;
    char *end = NULL;
    char *p;
    unsigned long long body = 0;
    unsigned long long have;

    while (end == NULL && used < sizeof buffer - 1) {
        ssize_t n = read(conn, buffer + used, sizeof buffer - 1 - used);

        if (n <= 0) break;
        used += (size_t) n;
        buffer[used] = '\0';
        end = strstr(buffer, "\r\n\r\n");
    }
    write_all(out, buffer, used);
    if (end == NULL) return;

    for (p = buffer; p < end; p++) *p = (char) tolower((unsigned char) *p);
    p = strstr(buffer, "\ncontent-length:");
    if (p != NULL) body = strtoull(p + strlen("\ncontent-length:"), NULL, 10);
    for (have = used - (size_t) (end + 4 - buffer); have < body;) {
        ssize_t n = read(conn, buffer, sizeof buffer);

        if (n <= 0) break;
        write_all(out, buffer, (size_t) n);
        have += (size_t) n;
    }
}

/* Answer each connection with the next of the NULL-terminated responses */
static void fake_server_start(struct fake_server *server, const char *const *responses) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int fds[2];
    int listener = socket(AF_INET, SOCK_STREAM, 0);

    ck_assert_int_ge(listener, 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);  /* 127.0.0.1; INADDR_LOOPBACK is not POSIX */
    ck_assert_int_eq(bind(listener, (struct sockaddr *) &addr, sizeof addr), 0);
    ck_assert_int_eq(listen(listener, 8), 0);
    ck_assert_int_eq(getsockname(listener, (struct sockaddr *) &addr, &len), 0);
    server->port = ntohs(addr.sin_port);
    snprintf(server->url, sizeof server->url, "http://127.0.0.1:%d/files/", server->port);
    ck_assert_int_eq(pipe(fds), 0);

    server->pid = fork();
    ck_assert_int_ge(server->pid, 0);
    if (server->pid == 0) {
        alarm(30);  /* never outlive a test that failed before stopping us */
        signal(SIGPIPE, SIG_IGN);
        close(fds[0]);
        for (; *responses != NULL; responses++) {
            int conn = accept(listener, NULL, NULL);

            if (conn < 0) _exit(1);
            fake_server_read_request(conn, fds[1]);
            write_all(conn, *responses, strlen(*responses));
            close(conn);
        }
        _exit(0);
    }
    close(listener);
    close(fds[1]);
    server->requests = fds[0];
}

/* Stop the server, returning everything the client sent it */
static const char *fake_server_stop(struct fake_server *server) {
    static char seen[1 << 16];
    size_t used = 0;
    ssize_t n;

    kill(server->pid, SIGKILL);
    waitpid(server->pid, NULL, 0);
    while (used < sizeof seen - 1 && (n = read(server->requests, seen + used, sizeof seen - 1 - used)) > 0) {
        used += (size_t) n;
    }
    seen[used] = '\0';
    close(server->requests);
    return seen;
}

#define RESPONSE(status, headers) "HTTP/1.1 " status "\r\n" headers "Content-Length: 0\r\nConnection: close\r\n\r\n"
#define CREATED RESPONSE("201 Created", "Location: /files/abc\r\n")

struct fake_run {
    struct fake_server server;
    vectus_client client;
    vectus_upload upload;
    const char *seen;
    vectus_code code;
};

/* Perform an upload of "hello" against canned responses; with resume set, the upload already exists at /files/abc */
static void fake_run(struct fake_run *run, const char *const *responses, struct source *source, vectus_seek_callback seek, int resume) {
    char url[96];

    source->text = "hello";
    fake_server_start(&run->server, responses);
    run->client = test_client(run->server.url);
    ck_assert_int_eq(vectus_upload_create(&run->upload, source_read, seek, source, 5), VECTUS_OK);
    if (resume) {
        snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", run->server.port);
        ck_assert_int_eq(vectus_upload_set_url(run->upload, url), VECTUS_OK);
    }
    run->code = vectus_upload_perform(run->client, run->upload);
    run->seen = fake_server_stop(&run->server);
}

static void fake_run_cleanup(struct fake_run *run) {
    vectus_upload_cleanup(run->upload);
    vectus_client_cleanup(run->client);
}

struct expectation {
    const char *response;
    vectus_code code;
};

/* ---- Unit tests ---- */

static void feed_upload_header(struct upload_response *response, const char *line) {
    size_t len = strlen(line);
    char *buffer = malloc(len);  /* exact size, like curl's unterminated buffer */

    memcpy(buffer, line, len);
    ck_assert_uint_eq(upload_header_callback(buffer, 1, len, response), len);
    free(buffer);
}

START_TEST(test_upload_headers)
{
    struct upload_response response = {0};

    feed_upload_header(&response, "HTTP/1.1 201 Created\r\n");
    feed_upload_header(&response, "location: /files/abc\r\n");
    feed_upload_header(&response, "UPLOAD-OFFSET:\t12 \r\n");
    feed_upload_header(&response, "Upload-Length: 34");
    ck_assert_str_eq(response.location, "/files/abc");
    ck_assert(response.has_offset && response.offset == 12);
    ck_assert(response.has_length && response.length == 34);

    feed_upload_header(&response, "Upload-Offset: -1\r\n");
    ck_assert(!response.has_offset);
    free(response.location);

    memset(&response, 0, sizeof response);
    feed_upload_header(&response, "Upload-Expires: soon\r\n");
    ck_assert(response.expires == 0);
    feed_upload_header(&response, "upload-expires: Wed, 25 Jun 2014 16:00:00 GMT\r\n");  /* the spec's example */
    ck_assert(response.expires == 1403712000);
}
END_TEST

START_TEST(test_upload_resolve_url)
{
    const char *cases[][2] = {
        {"/files/abc", "http://h:1/files/abc"},
        {"abc", "http://h:1/files/abc"},
        {"https://other/x", "https://other/x"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *resolved = upload_resolve_url("http://h:1/files/", cases[i][0]);

        ck_assert_str_eq(resolved, cases[i][1]);
        free(resolved);
    }
}
END_TEST

START_TEST(test_upload_read_wrapper)
{
    struct source source = {0};
    vectus_upload u;
    char buffer[100];

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 10), VECTUS_OK);
    u->position = source.position = 8;
    ck_assert_uint_eq(upload_read_callback(buffer, 1, sizeof buffer, u), 2);  /* never past the length */
    ck_assert((unsigned char) buffer[1] == source_byte(9));
    ck_assert(u->position == 10);
    ck_assert_uint_eq(upload_read_callback(buffer, 1, sizeof buffer, u), 0);
    ck_assert_int_eq(source.reads, 1);  /* the end needs no read */

    u->position = source.position = 0;
    source.failure = SOURCE_ENDS_EARLY;
    ck_assert_uint_eq(upload_read_callback(buffer, 1, sizeof buffer, u), CURL_READFUNC_ABORT);
    ck_assert(u->source_failed);

    u->source_failed = 0;
    source.failure = SOURCE_ABORTS;
    ck_assert_uint_eq(upload_read_callback(buffer, 1, sizeof buffer, u), CURL_READFUNC_ABORT);
    ck_assert(u->source_failed);
    ck_assert(u->position == 0);
    vectus_upload_cleanup(u);
}
END_TEST

START_TEST(test_upload_create_and_urls)
{
    struct source source = {0};
    vectus_upload u = (vectus_upload) &u;  /* any non-NULL value, to check create resets it */

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, UPLOAD_MAX_LENGTH + 1), VECTUS_ERR_TOO_LARGE);
    ck_assert_ptr_null(u);

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, UPLOAD_MAX_LENGTH), VECTUS_OK);
    ck_assert_ptr_null(vectus_upload_url(u));
    ck_assert(vectus_upload_offset(u) == 0);
    ck_assert_int_eq(vectus_upload_set_url(u, "http://h/files/abc"), VECTUS_OK);
    ck_assert_str_eq(vectus_upload_url(u), "http://h/files/abc");
    ck_assert_int_eq(vectus_upload_set_url(u, NULL), VECTUS_OK);
    ck_assert_ptr_null(vectus_upload_url(u));
    vectus_upload_cleanup(u);
    vectus_upload_cleanup(NULL);
}
END_TEST

START_TEST(test_upload_too_large_for_server)
{
    struct source source = {0};
    vectus_client c = test_client("http://127.0.0.1:1/files/");  /* any request would fail with VECTUS_ERR_TRANSPORT */
    vectus_upload u;

    c->max_size = 10;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 11), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TOO_LARGE);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_base64)
{
    /* RFC 4648's test vectors */
    const char *cases[][2] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    char out[16];
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        upload_base64((const unsigned char *) cases[i][0], strlen(cases[i][0]), out);
        ck_assert_str_eq(out, cases[i][1]);
    }
    upload_base64((const unsigned char *) "\xff\xfe\x00", 3, out);  /* high bits and a NUL */
    ck_assert_str_eq(out, "//4A");
}
END_TEST

START_TEST(test_upload_metadata)
{
    /* The last two are already added: one followed by a value, one by the end */
    const char *invalid[] = {"", "file name", "a,b", "a\r\nX-Injected: 1", "caf\xc3\xa9", "filename", "is_confidential"};
    struct source source = {0};
    vectus_upload u;
    size_t i;

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    ck_assert_ptr_null(u->metadata);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "filename", "world_domination_plan.pdf"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "is_confidential", ""), VECTUS_OK);
    ck_assert_str_eq(u->metadata, "Upload-Metadata: filename d29ybGRfZG9taW5hdGlvbl9wbGFuLnBkZg==,is_confidential");  /* the spec's example */

    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        ck_assert_msg(vectus_upload_add_metadata(u, invalid[i], "x") == VECTUS_ERR_INVALID, "case %zu", i);
    }
    ck_assert_int_eq(vectus_upload_add_metadata(u, "file", "x"), VECTUS_OK);  /* a prefix of a key is another key */
    ck_assert_int_eq(vectus_upload_add_metadata(u, "is_confidential", "x"), VECTUS_ERR_INVALID);  /* now followed by a comma */
    ck_assert_str_eq(u->metadata, "Upload-Metadata: filename d29ybGRfZG9taW5hdGlvbl9wbGFuLnBkZg==,is_confidential,file eA==");
    vectus_upload_cleanup(u);
}
END_TEST

START_TEST(test_upload_extensions_required)
{
    struct source source = {0};
    vectus_client c = test_client("http://127.0.0.1:1/files/");  /* any request would fail with VECTUS_ERR_TRANSPORT */
    vectus_upload u;

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_OK);  /* no URL, nothing to delete */

    c->extensions = VECTUS_EXT_TERMINATION;
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_UNSUPPORTED);
    ck_assert_ptr_null(vectus_upload_url(u));

    c->extensions = VECTUS_EXT_CREATION;
    ck_assert_int_eq(vectus_upload_set_url(u, "http://127.0.0.1:1/files/abc"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_ERR_UNSUPPORTED);
    ck_assert_str_eq(vectus_upload_url(u), "http://127.0.0.1:1/files/abc");
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_unreachable)
{
    struct source source = {0};
    vectus_client c = test_client("http://127.0.0.1:1/files/");  /* nothing listens on port 1 */
    vectus_upload u;

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_stall)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    struct source source = {0};
    char url[64];
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    vectus_client c;
    vectus_upload u;
    time_t start;

    /* The kernel completes connections to a listening socket, but nothing ever answers them */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);  /* 127.0.0.1; INADDR_LOOPBACK is not POSIX */
    ck_assert_int_eq(bind(listener, (struct sockaddr *) &addr, sizeof addr), 0);
    ck_assert_int_eq(listen(listener, 8), 0);
    ck_assert_int_eq(getsockname(listener, (struct sockaddr *) &addr, &len), 0);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/files/", ntohs(addr.sin_port));

    c = test_client(url);
    c->stall_timeout = 1;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    start = time(NULL);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);
    ck_assert_int_lt(time(NULL) - start, 10);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
    close(listener);
}
END_TEST

/* ---- Against canned responses ---- */

START_TEST(test_upload_fake_create_and_send)
{
    const char *responses[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    struct fake_run run;
    char url[96];

    fake_run(&run, responses, &source, source_seek, 0);
    ck_assert_int_eq(run.code, VECTUS_OK);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", run.server.port);
    ck_assert_str_eq(vectus_upload_url(run.upload), url);  /* the relative Location, resolved */
    ck_assert(vectus_upload_offset(run.upload) == 5);
    ck_assert_int_eq(source.seeks, 0);

    ck_assert_int_eq(count(run.seen, "POST /files/ HTTP/1.1\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "Upload-Length: 5\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "x-www-form-urlencoded"), 0);
    ck_assert_int_eq(count(run.seen, "PATCH /files/abc HTTP/1.1\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "Upload-Offset: 0\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "Content-Type: application/offset+octet-stream\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "Expect:"), 0);
    ck_assert_int_eq(count(run.seen, "\r\n\r\nhello"), 1);
    ck_assert_int_eq(count(run.seen, "Tus-Resumable: 1.0.0\r\n"), 2);  /* on every request */
    fake_run_cleanup(&run);
}
END_TEST

START_TEST(test_upload_fake_zero_length)
{
    const char *responses[] = {CREATED, NULL};
    struct source source = {0};
    vectus_upload u;
    struct fake_server server;
    vectus_client c;
    const char *seen;

    fake_server_start(&server, responses);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 0), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);
    ck_assert_int_eq(count(seen, "Upload-Length: 0\r\n"), 1);
    ck_assert_int_eq(count(seen, "PATCH"), 0);
    ck_assert_int_eq(source.reads, 0);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_metadata)
{
    const char *responses[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    vectus_upload u;
    struct fake_server server;
    vectus_client c;
    const char *seen;

    source.text = "hello";
    fake_server_start(&server, responses);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "filename", "world_domination_plan.pdf"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "is_confidential", ""), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);

    /* Once, on the POST before the PATCH */
    ck_assert_int_eq(count(seen, "\r\nUpload-Metadata: filename d29ybGRfZG9taW5hdGlvbl9wbGFuLnBkZg==,is_confidential\r\n"), 1);
    ck_assert_int_eq(count(seen, "Upload-Metadata"), 1);
    ck_assert(strstr(seen, "Upload-Metadata") < strstr(seen, "PATCH"));
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_expires)
{
#define EXPIRES_16 "Upload-Expires: Wed, 25 Jun 2014 16:00:00 GMT\r\n"
#define EXPIRES_17 "Upload-Expires: Wed, 25 Jun 2014 17:00:00 GMT\r\n"
    const char *created[] = {
        RESPONSE("201 Created", "Location: /files/abc\r\n" EXPIRES_16),
        RESPONSE("204 No Content", "Upload-Offset: 3\r\n" EXPIRES_17),
        RESPONSE("204 No Content", "Upload-Offset: 5\r\n"),  /* no date, so the last one holds */
        NULL,
    };
    const char *resumed[] = {RESPONSE("200 OK", "Upload-Offset: 5\r\nUpload-Length: 5\r\n" EXPIRES_16), NULL};
#undef EXPIRES_16
#undef EXPIRES_17
    struct source source = {0};
    struct fake_run run;

    fake_run(&run, created, &source, source_seek, 0);
    ck_assert_int_eq(run.code, VECTUS_OK);
    ck_assert(vectus_upload_expires(run.upload) == 1403715600);
    ck_assert_int_eq(vectus_upload_set_url(run.upload, NULL), VECTUS_OK);
    ck_assert(vectus_upload_expires(run.upload) == 0);  /* another URL, another upload */
    fake_run_cleanup(&run);

    memset(&source, 0, sizeof source);
    fake_run(&run, resumed, &source, source_seek, 1);
    ck_assert_int_eq(run.code, VECTUS_OK);
    ck_assert(vectus_upload_expires(run.upload) == 1403712000);
    fake_run_cleanup(&run);
}
END_TEST

START_TEST(test_upload_fake_terminate)
{
    const struct expectation cases[] = {
        {RESPONSE("204 No Content", ""), VECTUS_OK},
        {RESPONSE("404 Not Found", ""), VECTUS_ERR_GONE},
        {RESPONSE("500 Internal Server Error", ""), VECTUS_ERR_HTTP_STATUS},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {cases[i].response, NULL};
        struct source source = {0};
        struct fake_server server;
        vectus_client c;
        vectus_upload u;
        vectus_code code;
        const char *seen;
        char url[96];

        fake_server_start(&server, responses);
        c = test_client(server.url);
        snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", server.port);
        ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
        ck_assert_int_eq(vectus_upload_set_url(u, url), VECTUS_OK);
        code = vectus_upload_terminate(c, u);
        seen = fake_server_stop(&server);

        ck_assert_msg(code == cases[i].code, "case %zu: %s", i, vectus_strerror(code));
        ck_assert_int_eq(count(seen, "DELETE /files/abc HTTP/1.1\r\n"), 1);
        ck_assert_int_eq(count(seen, "Tus-Resumable: 1.0.0\r\n"), 1);
        if (code == VECTUS_OK) {
            ck_assert_ptr_null(vectus_upload_url(u));  /* forgotten, so the next perform starts over */
        } else {
            ck_assert_str_eq(vectus_upload_url(u), url);
        }
        vectus_upload_cleanup(u);
        vectus_client_cleanup(c);
    }
}
END_TEST

START_TEST(test_upload_fake_bodies_discarded)
{
    /* Servers may answer with a body, such as tusd's error text, which must not end up on the application's stdout */
#define WITH_BODY(status, headers) "HTTP/1.1 " status "\r\n" headers "Connection: close\r\n\r\nbody on stdout"
    const char *responses[] = {
        WITH_BODY("200 OK", "Tus-Version: 1.0.0\r\nTus-Extension: creation\r\n"),
        WITH_BODY("201 Created", "Location: /files/abc\r\n"),
        WITH_BODY("404 Not Found", ""),
        NULL,
    };
#undef WITH_BODY
    FILE *captured = tmpfile();
    struct source source = {0};
    struct fake_server server;
    vectus_client c = NULL;
    vectus_upload u = NULL;
    vectus_code init_code;
    vectus_code code = VECTUS_OK;
    int saved_stdout;

    ck_assert_ptr_nonnull(captured);
    fake_server_start(&server, responses);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(fileno(captured), STDOUT_FILENO);
    init_code = vectus_client_init(&c, server.url);
    if (init_code == VECTUS_OK && vectus_upload_create(&u, source_read, NULL, &source, 5) == VECTUS_OK) {
        code = vectus_upload_perform(c, u);
    }
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);  /* back before any assertion, so failures stay visible */
    close(saved_stdout);
    fake_server_stop(&server);

    ck_assert_int_eq(init_code, VECTUS_OK);
    ck_assert_int_eq(code, VECTUS_ERR_GONE);
    ck_assert_int_eq((int) lseek(fileno(captured), 0, SEEK_END), 0);
    fclose(captured);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_create_errors)
{
    const struct expectation cases[] = {
        {RESPONSE("201 Created", ""), VECTUS_ERR_PROTOCOL},  /* no Location */
        {RESPONSE("413 Request Entity Too Large", ""), VECTUS_ERR_TOO_LARGE},
        {RESPONSE("500 Internal Server Error", ""), VECTUS_ERR_HTTP_STATUS},
        {RESPONSE("200 OK", "Location: /files/abc\r\n"), VECTUS_ERR_HTTP_STATUS},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {cases[i].response, NULL};
        struct source source = {0};
        struct fake_run run;

        fake_run(&run, responses, &source, source_seek, 0);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        ck_assert_int_eq(count(run.seen, "PATCH"), 0);
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_upload_fake_resume_errors)
{
    const struct expectation cases[] = {
        {RESPONSE("404 Not Found", ""), VECTUS_ERR_GONE},
        {RESPONSE("410 Gone", ""), VECTUS_ERR_GONE},
        {RESPONSE("403 Forbidden", ""), VECTUS_ERR_GONE},
        {RESPONSE("500 Internal Server Error", ""), VECTUS_ERR_HTTP_STATUS},
        {RESPONSE("200 OK", "Upload-Length: 5\r\n"), VECTUS_ERR_PROTOCOL},                       /* no Upload-Offset */
        {RESPONSE("200 OK", "Upload-Offset: 6\r\nUpload-Length: 5\r\n"), VECTUS_ERR_PROTOCOL},  /* past the end */
        {RESPONSE("200 OK", "Upload-Offset: 0\r\nUpload-Length: 9\r\n"), VECTUS_ERR_PROTOCOL},  /* another upload's length */
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {cases[i].response, NULL};
        struct source source = {0};
        struct fake_run run;

        fake_run(&run, responses, &source, source_seek, 1);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        ck_assert_int_eq(count(run.seen, "HEAD /files/abc HTTP/1.1\r\n"), 1);
        ck_assert_int_eq(count(run.seen, "PATCH"), 0);
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_upload_fake_send_errors)
{
    const struct expectation cases[] = {
        {RESPONSE("204 No Content", "Upload-Offset: 0\r\n"), VECTUS_ERR_PROTOCOL},  /* no progress, which would loop forever */
        {RESPONSE("204 No Content", ""), VECTUS_ERR_PROTOCOL},
        {RESPONSE("204 No Content", "Upload-Offset: 6\r\n"), VECTUS_ERR_PROTOCOL},
        {RESPONSE("409 Conflict", ""), VECTUS_ERR_HTTP_STATUS},
        {RESPONSE("404 Not Found", ""), VECTUS_ERR_GONE},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {CREATED, cases[i].response, NULL};
        struct source source = {0};
        struct fake_run run;

        fake_run(&run, responses, &source, source_seek, 0);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        ck_assert_int_eq(count(run.seen, "PATCH"), 1);
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_upload_fake_resume_needs_seek)
{
    const char *head = RESPONSE("200 OK", "Upload-Offset: 2\r\nUpload-Length: 5\r\n");
    const char *without_seek[] = {head, NULL};
    const char *with_seek[] = {head, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    struct fake_run run;

    fake_run(&run, without_seek, &source, NULL, 1);
    ck_assert_int_eq(run.code, VECTUS_ERR_SOURCE);
    ck_assert(vectus_upload_offset(run.upload) == 2);
    ck_assert_int_eq(count(run.seen, "PATCH"), 0);
    fake_run_cleanup(&run);

    memset(&source, 0, sizeof source);
    fake_run(&run, with_seek, &source, source_seek, 1);
    ck_assert_int_eq(run.code, VECTUS_OK);
    ck_assert_int_eq(source.seeks, 1);
    ck_assert(source.first_seek == 2);
    ck_assert_int_eq(count(run.seen, "Upload-Offset: 2\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "Content-Length: 3\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "\r\n\r\nllo"), 1);
    fake_run_cleanup(&run);
}
END_TEST

START_TEST(test_upload_fake_partial_accept)
{
    /* The server keeps only 3 of the 5 bytes, so the rest goes in a second PATCH after seeking back */
    const char *responses[] = {
        CREATED,
        RESPONSE("204 No Content", "Upload-Offset: 3\r\n"),
        RESPONSE("204 No Content", "Upload-Offset: 5\r\n"),
        NULL,
    };
    struct source source = {0};
    struct fake_run run;

    fake_run(&run, responses, &source, source_seek, 0);
    ck_assert_int_eq(run.code, VECTUS_OK);
    ck_assert_int_eq(count(run.seen, "PATCH /files/abc"), 2);
    ck_assert_int_eq(source.seeks, 1);
    ck_assert(source.first_seek == 3);
    ck_assert_int_eq(count(run.seen, "Upload-Offset: 3\r\n"), 1);
    ck_assert_int_eq(count(run.seen, "\r\n\r\nlo"), 1);
    fake_run_cleanup(&run);
}
END_TEST

START_TEST(test_upload_fake_source_fails)
{
    const enum source_failure failures[] = {SOURCE_ABORTS, SOURCE_ENDS_EARLY};
    size_t i;

    for (i = 0; i < sizeof failures / sizeof failures[0]; i++) {
        const char *responses[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
        struct source source = {0};
        struct fake_run run;

        source.failure = failures[i];
        source.fail_at = 0;
        fake_run(&run, responses, &source, source_seek, 0);
        ck_assert_msg(run.code == VECTUS_ERR_SOURCE, "case %zu: %s", i, vectus_strerror(run.code));
        fake_run_cleanup(&run);
    }
}
END_TEST

/* ---- Against a real tus server: VECTUS_TEST_URL ---- */

struct body {
    unsigned char *data;
    size_t len;
};

static size_t body_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct body *body = userdata;
    size_t n = size * nmemb;
    unsigned char *grown = realloc(body->data, body->len + n + 1);

    if (grown == NULL) return 0;
    memcpy(grown + body->len, ptr, n);
    body->data = grown;
    body->len += n;
    return n;
}

/* A plain request for test setup and checks, independent of the code under test */
static long http(const char *method, const char *url, const char *json, struct body *out) {
    struct body ignored = {0};
    CURL *c = curl_easy_init();
    struct curl_slist *headers = curl_slist_append(NULL, "Tus-Resumable: 1.0.0");
    long status = 0;

    if (json != NULL) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (json != NULL) curl_easy_setopt(c, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out != NULL ? out : &ignored);
    ck_assert_int_eq(curl_easy_perform(c), CURLE_OK);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    free(ignored.data);
    return status;
}

/* Download the upload from the server and compare it with the test data */
static void assert_uploaded(const char *url, unsigned long long length) {
    struct body body = {0};
    size_t i;

    ck_assert_int_eq(http("GET", url, NULL, &body), 200);
    ck_assert_uint_eq(body.len, length);
    for (i = 0; i < body.len; i++) {
        if (body.data[i] != source_byte(i)) ck_abort_msg("byte %zu of the upload differs", i);
    }
    free(body.data);
}

static vectus_client live_client(const char *variable) {
    vectus_client c;

    ck_assert_int_eq(vectus_client_init(&c, getenv(variable)), VECTUS_OK);
    return c;
}

/* Upload length bytes of test data in one go, returning the upload */
static vectus_upload live_upload(vectus_client c, struct source *source, unsigned long long length) {
    vectus_upload u;

    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, source, length), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    return u;
}

START_TEST(test_upload_live)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u = live_upload(c, &source, 4 * MIB);

    ck_assert(vectus_upload_offset(u) == 4 * MIB);
    ck_assert_int_eq(source.seeks, 0);
    assert_uploaded(vectus_upload_url(u), 4 * MIB);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_empty)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u = live_upload(c, &source, 0);
    vectus_upload again;

    ck_assert_int_eq(source.reads, 0);
    /* Resuming asks the server, which must agree there is nothing left to send */
    ck_assert_int_eq(vectus_upload_create(&again, source_read, NULL, &source, 0), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_set_url(again, vectus_upload_url(u)), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, again), VECTUS_OK);
    ck_assert_int_eq(source.reads, 0);
    vectus_upload_cleanup(again);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_resume)
{
    struct source first = {0};
    struct source second = {0};
    struct source third = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u;
    unsigned long long stopped;

    /* The first process dies half-way through, leaving the server with part of the data */
    first.failure = SOURCE_ABORTS;
    first.fail_at = 2 * MIB;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &first, 4 * MIB), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_SOURCE);
    ck_assert_ptr_nonnull(vectus_upload_url(u));

    /* A second process resumes from the stored URL, but its data cannot seek */
    {
        vectus_upload resumed;

        ck_assert_int_eq(vectus_upload_create(&resumed, source_read, NULL, &second, 4 * MIB), VECTUS_OK);
        ck_assert_int_eq(vectus_upload_set_url(resumed, vectus_upload_url(u)), VECTUS_OK);
        ck_assert_int_eq(vectus_upload_perform(c, resumed), VECTUS_ERR_SOURCE);
        stopped = vectus_upload_offset(resumed);
        ck_assert_msg(stopped > 0 && stopped < 4 * MIB, "the server stopped at %llu", stopped);
        vectus_upload_cleanup(resumed);
    }

    /* A third process can seek, and finishes from where the server stopped */
    {
        vectus_upload resumed;

        ck_assert_int_eq(vectus_upload_create(&resumed, source_read, source_seek, &third, 4 * MIB), VECTUS_OK);
        ck_assert_int_eq(vectus_upload_set_url(resumed, vectus_upload_url(u)), VECTUS_OK);
        ck_assert_int_eq(vectus_upload_perform(c, resumed), VECTUS_OK);
        ck_assert_int_eq(third.seeks, 1);
        ck_assert(third.first_seek == stopped);
        vectus_upload_cleanup(resumed);
    }

    assert_uploaded(vectus_upload_url(u), 4 * MIB);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_length_mismatch)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u = live_upload(c, &source, 1000);
    vectus_upload other;

    ck_assert_int_eq(vectus_upload_create(&other, source_read, source_seek, &source, 999), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_set_url(other, vectus_upload_url(u)), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, other), VECTUS_ERR_PROTOCOL);
    vectus_upload_cleanup(other);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_gone)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u = live_upload(c, &source, 1000);
    char terminated[512];
    char missing[512];

    /* Terminated through the termination extension, which forgets the URL */
    snprintf(terminated, sizeof terminated, "%s", vectus_upload_url(u));
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_OK);
    ck_assert_ptr_null(vectus_upload_url(u));
    ck_assert_int_eq(vectus_upload_set_url(u, terminated), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_GONE);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_ERR_GONE);

    /* Never existed */
    snprintf(missing, sizeof missing, "%sdoes-not-exist", getenv("VECTUS_TEST_URL"));
    ck_assert_int_eq(vectus_upload_set_url(u, missing), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_GONE);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_too_large)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    unsigned long long max = vectus_client_max_size(c);
    vectus_upload u;

    if (max == 0) {  /* the server has no limit to test */
        vectus_client_cleanup(c);
        return;
    }
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, max + 1), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TOO_LARGE);  /* refused before asking */
    c->max_size = 0;
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TOO_LARGE);  /* the server's 413 */
    ck_assert_ptr_null(vectus_upload_url(u));
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

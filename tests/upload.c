#include <arpa/inet.h>
#include <pthread.h>
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
    unsigned long long base;        /* where in source_byte the stream starts, for a part of a larger upload */
    unsigned long long position;
    enum source_failure failure;
    unsigned long long fail_at;     /* where the failure kicks in */
    void (*hook)(void);             /* called once, when position first reaches hook_at */
    unsigned long long hook_at;
    int reads;
    int seeks;
    unsigned long long first_seek;
};

static size_t source_read(void *userdata, char *buffer, size_t n) {
    struct source *s = userdata;
    size_t i;

    s->reads++;
    if (s->failure != SOURCE_FINE && s->position >= s->fail_at) {
        return s->failure == SOURCE_ABORTS ? VECTUS_READ_ABORT : 0;
    }
    if (s->failure == SOURCE_ENDS_EARLY && n > s->fail_at - s->position) {
        n = (size_t) (s->fail_at - s->position);  /* a stream exactly fail_at bytes long */
    }
    if (s->hook != NULL && s->position >= s->hook_at) {
        void (*hook)(void) = s->hook;

        s->hook = NULL;
        hook();
    }
    for (i = 0; i < n; i++) {
        buffer[i] = s->text != NULL ? s->text[s->position + i] : (char) source_byte(s->base + s->position + i);
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

/* Write length bytes of test data to a new temporary file, whose path goes into path; the caller removes it */
static void temp_file(char *path, size_t size, unsigned long long length) {
    const char *dir = getenv("TMPDIR");
    unsigned long long i;
    FILE *f;
    int fd;

    snprintf(path, size, "%s/vectus-test-XXXXXX", dir != NULL ? dir : "/tmp");
    fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    f = fdopen(fd, "wb");
    ck_assert_ptr_nonnull(f);
    for (i = 0; i < length; i++) fputc(source_byte(i), f);
    ck_assert_int_eq(fclose(f), 0);
}

/* What a progress callback saw; it cancels once the offset reaches cancel_at, if cancel is set */
struct progress {
    int calls;
    unsigned long long first_length;
    unsigned long long last_offset;
    unsigned long long last_length;
    unsigned long long max_offset;
    int cancel;
    unsigned long long cancel_at;
};

static int progress_record(void *userdata, unsigned long long offset, unsigned long long length) {
    struct progress *p = userdata;

    if (p->calls++ == 0) p->first_length = length;
    p->last_offset = offset;
    p->last_length = length;
    if (offset > p->max_offset) p->max_offset = offset;
    if (p->cancel && offset >= p->cancel_at) {
        p->cancel = 0;  /* once */
        return 1;
    }
    return 0;
}

/* A curl callback counting its calls, and setting an option the fake server can see */
static void set_user_agent(void *userdata, CURL *curl) {
    int *calls = userdata;

    (*calls)++;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "vectus-test/1");
}

/* curl options giving up on a request after fewer seconds without a byte moving than the 30 it waits otherwise */
static void set_stall_timeout(void *userdata, CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, *(long *) userdata);
}
static long one_second = 1;
static long two_seconds = 2;
static long five_seconds = 5;
static struct vectus_curl_options stall_1s = {set_stall_timeout, &one_second};
static struct vectus_curl_options stall_2s = {set_stall_timeout, &two_seconds};
static struct vectus_curl_options stall_5s = {set_stall_timeout, &five_seconds};

/* A client for a server the test controls, as if it had already asked the server what it offers */
static vectus_client test_client(const char *url) {
    vectus_client c;

    ck_assert_int_eq(vectus_client_create(&c, url, vectus_curl_transport(), &stall_5s), VECTUS_OK);
    c->extensions = VECTUS_EXT_CREATION | VECTUS_EXT_TERMINATION;  /* as tusd offers by default */
    c->discovered = 1;
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

/* Read one request off conn, body included, copying it to out; 0 if the client closed conn before sending one */
static int fake_server_read_request(int conn, int out) {
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
    if (end == NULL) return used > 0;

    for (p = buffer; p < end; p++) *p = (char) tolower((unsigned char) *p);
    have = used - (size_t) (end + 4 - buffer);
    if (strstr(buffer, "\ntransfer-encoding: chunked") != NULL) {
        /* shortcut: done once what arrived ends with the last chunk, which holds for the small bodies the tests send */
        memmove(buffer, end + 4, have);
        while (!(have >= 5 && memcmp(buffer + have - 5, "0\r\n\r\n", 5) == 0) && have < sizeof buffer) {
            ssize_t n = read(conn, buffer + have, sizeof buffer - have);

            if (n <= 0) break;
            write_all(out, buffer + have, (size_t) n);
            have += (size_t) n;
        }
        return 1;
    }
    p = strstr(buffer, "\ncontent-length:");
    if (p != NULL) body = strtoull(p + strlen("\ncontent-length:"), NULL, 10);
    while (have < body) {
        ssize_t n = read(conn, buffer, sizeof buffer);

        if (n <= 0) break;
        write_all(out, buffer, (size_t) n);
        have += (size_t) n;
    }
    return 1;
}

/*
 * Answer each request with the next of the NULL-terminated responses, closing the connection after those that say so
 * (and after an empty one, which answers nothing), and noting each new connection as "[accept]" among the requests
 */
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
        int conn = -1;

        for (; *responses != NULL; responses++) {
            for (;;) {
                if (conn < 0) {
                    conn = accept(listener, NULL, NULL);
                    if (conn < 0) _exit(1);
                    write_all(fds[1], "[accept]\n", 9);
                }
                if (fake_server_read_request(conn, fds[1])) break;
                close(conn);  /* the client closed it instead of sending another request */
                conn = -1;
            }
            write_all(conn, *responses, strlen(*responses));
            if (**responses == '\0' || strstr(*responses, "Connection: close") != NULL) {
                close(conn);
                conn = -1;
            }
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
#define KEEP_ALIVE(status, headers) "HTTP/1.1 " status "\r\n" headers "Content-Length: 0\r\n\r\n"
#define OPTIONS_OK RESPONSE("204 No Content", "Tus-Resumable: 1.0.0\r\nTus-Version: 1.0.0\r\nTus-Extension: creation,termination\r\n")
#define CREATED RESPONSE("201 Created", "Location: /files/abc\r\n")

struct fake_run {
    struct fake_server server;
    vectus_client client;
    vectus_upload upload;
    const char *seen;
    vectus_code code;
};

/*
 * Perform an upload of "hello" against canned responses, announced as length bytes, with the server offering extensions
 * on top of test_client's; with resume set, the upload already exists at /files/abc
 */
static void fake_run_with(struct fake_run *run, const char *const *responses, struct source *source, vectus_seek_callback seek, int resume,
                          unsigned long long length, unsigned int extensions) {
    char url[96];

    source->text = "hello";
    fake_server_start(&run->server, responses);
    run->client = test_client(run->server.url);
    run->client->extensions |= extensions;
    ck_assert_int_eq(vectus_upload_create(&run->upload, source_read, seek, source, length), VECTUS_OK);
    if (resume) {
        snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", run->server.port);
        ck_assert_int_eq(vectus_upload_set_url(run->upload, url), VECTUS_OK);
    }
    run->code = vectus_upload_perform(run->client, run->upload);
    run->seen = fake_server_stop(&run->server);
}

static void fake_run(struct fake_run *run, const char *const *responses, struct source *source, vectus_seek_callback seek, int resume) {
    fake_run_with(run, responses, source, seek, resume, 5, 0);
}

static void fake_run_cleanup(struct fake_run *run) {
    vectus_upload_cleanup(run->upload);
    vectus_client_cleanup(run->client);
}

struct expectation {
    const char *response;
    vectus_code code;
    long status;        /* vectus_upload_http_status after */
    const char *error;  /* in vectus_upload_error after, "" for none */
};

/* The upload's status and error say what the case expects */
static void assert_detail(vectus_upload u, const struct expectation *expected, size_t i) {
    ck_assert_msg(vectus_upload_http_status(u) == expected->status, "case %zu: status %ld", i, vectus_upload_http_status(u));
    if (*expected->error == '\0') {
        ck_assert_msg(*vectus_upload_error(u) == '\0', "case %zu: error \"%s\"", i, vectus_upload_error(u));
    } else {
        ck_assert_msg(strstr(vectus_upload_error(u), expected->error) != NULL, "case %zu: error \"%s\"", i, vectus_upload_error(u));
    }
}

/* ---- Unit tests ---- */

static void feed_upload_header(struct upload_response *response, const char *line) {
    struct upload_exchange exchange = {NULL, response};
    size_t len = strlen(line);
    char *buffer = malloc(len);  /* exact size, like curl's unterminated buffer */

    memcpy(buffer, line, len);
    ck_assert_uint_eq(easy_parse_header_line(buffer, len, upload_handle_header, &exchange), len);
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

/* Read through the wrapper the transport calls */
static size_t read_through(vectus_upload u, char *buffer, size_t size) {
    struct upload_exchange exchange = {u, NULL};

    return upload_read(&exchange, buffer, size);
}

START_TEST(test_upload_read_wrapper)
{
    struct source source = {0};
    vectus_upload u;
    char buffer[100];

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 10), VECTUS_OK);
    u->position = source.position = 8;
    ck_assert_uint_eq(read_through(u, buffer, sizeof buffer), 2);  /* never past the length */
    ck_assert((unsigned char) buffer[1] == source_byte(9));
    ck_assert(u->position == 10);
    ck_assert_uint_eq(read_through(u, buffer, sizeof buffer), 0);
    ck_assert_int_eq(source.reads, 1);  /* the end needs no read */

    u->position = source.position = 0;
    source.failure = SOURCE_ENDS_EARLY;
    ck_assert_uint_eq(read_through(u, buffer, sizeof buffer), VECTUS_READ_ABORT);
    ck_assert(u->source_failed);

    u->source_failed = 0;
    source.failure = SOURCE_ABORTS;
    ck_assert_uint_eq(read_through(u, buffer, sizeof buffer), VECTUS_READ_ABORT);
    ck_assert(u->source_failed);
    ck_assert(u->position == 0);
    vectus_upload_cleanup(u);

    /* Without a length, the stream ending is the end of the upload */
    memset(&source, 0, sizeof source);
    source.failure = SOURCE_ENDS_EARLY;
    source.fail_at = 7;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);
    ck_assert_uint_eq(read_through(u, buffer, sizeof buffer), 7);
    ck_assert_uint_eq(read_through(u, buffer, sizeof buffer), 0);
    ck_assert(!u->source_failed);
    ck_assert(u->length == 7);
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
    ck_assert(u->length == UPLOAD_MAX_LENGTH);
    vectus_upload_cleanup(u);
    vectus_upload_cleanup(NULL);

    /* A deferred upload forgets a length it learnt about another URL */
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);
    ck_assert(!u->length_declared);
    u->length = 5;
    ck_assert_int_eq(vectus_upload_set_url(u, "http://h/files/abc"), VECTUS_OK);
    ck_assert(u->length == VECTUS_LENGTH_UNKNOWN);
    vectus_upload_cleanup(u);
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
    ck_assert_str_eq(vectus_upload_error(u), "the upload's 11 bytes are over the server's Tus-Max-Size of 10");
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
    ck_assert_str_eq(u->metadata, "filename d29ybGRfZG9taW5hdGlvbl9wbGFuLnBkZg==,is_confidential");  /* the spec's example */

    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        ck_assert_msg(vectus_upload_add_metadata(u, invalid[i], "x") == VECTUS_ERR_INVALID, "case %zu", i);
    }
    ck_assert_int_eq(vectus_upload_add_metadata(u, "file", "x"), VECTUS_OK);  /* a prefix of a key is another key */
    ck_assert_int_eq(vectus_upload_add_metadata(u, "is_confidential", "x"), VECTUS_ERR_INVALID);  /* now followed by a comma */
    ck_assert_str_eq(u->metadata, "filename d29ybGRfZG9taW5hdGlvbl9wbGFuLnBkZg==,is_confidential,file eA==");
    vectus_upload_cleanup(u);
}
END_TEST

START_TEST(test_upload_create_final)
{
    const char *urls[] = {"/files/a", "http://h/files/b"};
    const char *invalid[] = {"", "/files/a b", "/files/a\r\nX-Injected: 1", "/files/caf\xc3\xa9"};
    vectus_upload u = (vectus_upload) &u;  /* any non-NULL value, to check create resets it */
    size_t i;

    ck_assert_int_eq(vectus_upload_create_final(&u, urls, 0), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(u);
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        const char *with_invalid[] = {"/files/ok", invalid[i]};

        ck_assert_msg(vectus_upload_create_final(&u, with_invalid, 2) == VECTUS_ERR_INVALID, "case %zu", i);
        ck_assert_ptr_null(u);
    }

    ck_assert_int_eq(vectus_upload_create_final(&u, urls, 1), VECTUS_OK);
    ck_assert_str_eq(u->final, "final;/files/a");
    vectus_upload_cleanup(u);
    ck_assert_int_eq(vectus_upload_create_final(&u, urls, 2), VECTUS_OK);
    ck_assert_str_eq(u->final, "final;/files/a http://h/files/b");  /* as the spec's example */
    ck_assert(u->length == 0);
    vectus_upload_cleanup(u);
}
END_TEST

START_TEST(test_upload_creates_with_data)
{
    struct source source = {0};
    vectus_client c = test_client("http://127.0.0.1:1/files/");
    vectus_upload u;

    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, UPLOAD_CREATION_DATA_MAX), VECTUS_OK);
    ck_assert(!upload_creates_with_data(c, u));  /* the server does not offer it */
    c->extensions |= VECTUS_EXT_CREATION_WITH_UPLOAD;
    ck_assert(upload_creates_with_data(c, u));
    u->length = UPLOAD_CREATION_DATA_MAX + 1;
    ck_assert(!upload_creates_with_data(c, u));  /* too much to send again if the POST fails */
    u->length = 0;
    ck_assert(!upload_creates_with_data(c, u));
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
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
    ck_assert_str_eq(vectus_upload_error(u), "the server does not offer the creation extension");
    ck_assert_ptr_null(vectus_upload_url(u));

    c->extensions = VECTUS_EXT_CREATION;
    ck_assert_int_eq(vectus_upload_set_url(u, "http://127.0.0.1:1/files/abc"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_ERR_UNSUPPORTED);
    ck_assert_str_eq(vectus_upload_error(u), "the server does not offer the termination extension");
    ck_assert_str_eq(vectus_upload_url(u), "http://127.0.0.1:1/files/abc");
    vectus_upload_cleanup(u);

    c->max_size = 10;  /* an unknown length is not over it */
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_UNSUPPORTED);  /* no creation-defer-length */
    ck_assert_ptr_nonnull(strstr(vectus_upload_error(u), "creation-defer-length"));
    c->extensions |= VECTUS_EXT_CREATION_DEFER_LENGTH;
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);  /* gets as far as asking */
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
    ck_assert_msg(strncmp(vectus_upload_error(u), "POST: ", 6) == 0 && strlen(vectus_upload_error(u)) > 6, "%s", vectus_upload_error(u));
    ck_assert_int_eq(vectus_upload_http_status(u), 0);
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
    c->transport_userdata = &stall_1s;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    start = time(NULL);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);
    ck_assert_int_lt(time(NULL) - start, 10);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
    close(listener);
}
END_TEST

START_TEST(test_null_handles)
{
    const char *urls[] = {"/files/a", NULL};
    struct source source = {0};
    vectus_client c;
    vectus_upload u = (vectus_upload) &u;

    ck_assert_int_eq(vectus_client_set_header(NULL, "X-A", "1"), VECTUS_ERR_INVALID);
    vectus_client_set_method_override(NULL, 1);
    ck_assert_int_eq(vectus_client_discover(NULL), VECTUS_ERR_INVALID);
    ck_assert_uint_eq(vectus_client_extensions(NULL), 0);
    ck_assert(vectus_client_max_size(NULL) == 0);
    ck_assert_int_eq(vectus_client_http_status(NULL), 0);
    ck_assert_str_eq(vectus_client_error(NULL), "");
    vectus_client_cleanup(NULL);

    ck_assert_int_eq(vectus_upload_create(NULL, source_read, NULL, &source, 5), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_create(&u, NULL, NULL, &source, 5), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(u);
    ck_assert_int_eq(vectus_upload_create_file(NULL, "/dev/null"), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_create_file(&u, NULL), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_create_final(NULL, urls, 1), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_create_final(&u, NULL, 1), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_create_final(&u, urls, 2), VECTUS_ERR_INVALID);  /* a NULL URL */
    ck_assert_ptr_null(u);
    vectus_upload_set_partial(NULL);
    vectus_upload_set_progress(NULL, progress_record, NULL);
    ck_assert_int_eq(vectus_upload_add_metadata(NULL, "k", "v"), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_perform(NULL, NULL), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_terminate(NULL, NULL), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_set_url(NULL, "http://h/files/abc"), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(vectus_upload_url(NULL));
    ck_assert(vectus_upload_offset(NULL) == 0);
    ck_assert(vectus_upload_expires(NULL) == 0);
    ck_assert_int_eq(vectus_upload_http_status(NULL), 0);
    ck_assert_str_eq(vectus_upload_error(NULL), "");
    vectus_upload_cleanup(NULL);

    /* One handle without the other, and NULL arguments next to a real handle */
    c = test_client("http://127.0.0.1:1/files/");
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, NULL), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_perform(NULL, u), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_terminate(c, NULL), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_terminate(NULL, u), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_add_metadata(u, NULL, "v"), VECTUS_ERR_INVALID);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "k", NULL), VECTUS_ERR_INVALID);
    ck_assert_ptr_null(u->metadata);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_create_file)
{
    char path[512];
    vectus_upload u = (vectus_upload) &u;  /* any non-NULL value, to check create resets it */

    errno = 0;
    ck_assert_int_eq(vectus_upload_create_file(&u, "/nonexistent/vectus-test"), VECTUS_ERR_SOURCE);
    ck_assert_int_eq(errno, ENOENT);  /* left for the caller to tell why */
    ck_assert_ptr_null(u);
    ck_assert_int_eq(vectus_upload_create_file(&u, "."), VECTUS_ERR_SOURCE);  /* opens, but has no length to upload */
    ck_assert_int_eq(errno, EISDIR);
    ck_assert_ptr_null(u);

    temp_file(path, sizeof path, 3000);
    ck_assert_int_eq(vectus_upload_create_file(&u, path), VECTUS_OK);
    ck_assert(u->length == 3000);
    ck_assert(u->seek_callback != NULL);
    vectus_upload_cleanup(u);  /* closes the file, which the sanitizers would report as a leak otherwise */

    /* Past 4 GiB, so the size cannot have gone through 32 bits; sparse, so it takes no space */
    ck_assert_int_eq(truncate(path, (off_t) (5 * 1024 * MIB)), 0);
    ck_assert_int_eq(vectus_upload_create_file(&u, path), VECTUS_OK);
    ck_assert(u->length == 5 * 1024 * MIB);
    vectus_upload_cleanup(u);
    unlink(path);
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

START_TEST(test_upload_fake_create_with_upload)
{
    const struct {
        const char *responses[3];
        vectus_code code;
        int patches;
    } cases[] = {
        {{RESPONSE("201 Created", "Location: /files/abc\r\nUpload-Offset: 5\r\n"), NULL}, VECTUS_OK, 0},
        {{RESPONSE("201 Created", "Location: /files/abc\r\nUpload-Offset: 3\r\n"),  /* kept part, the rest goes in a PATCH */
          RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL}, VECTUS_OK, 1},
        {{RESPONSE("201 Created", "Location: /files/abc\r\n"), NULL}, VECTUS_ERR_PROTOCOL, 0},
        {{RESPONSE("201 Created", "Location: /files/abc\r\nUpload-Offset: 6\r\n"), NULL}, VECTUS_ERR_PROTOCOL, 0},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct source source = {0};
        struct fake_server server;
        vectus_client c;
        vectus_upload u;
        vectus_code code;
        const char *seen;

        source.text = "hello";
        fake_server_start(&server, cases[i].responses);
        c = test_client(server.url);
        c->extensions |= VECTUS_EXT_CREATION_WITH_UPLOAD;
        ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
        code = vectus_upload_perform(c, u);
        seen = fake_server_stop(&server);

        ck_assert_msg(code == cases[i].code, "case %zu: %s", i, vectus_strerror(code));
        ck_assert_ptr_nonnull(vectus_upload_url(u));  /* kept even after a protocol error, so the next perform asks the server */
        ck_assert_int_eq(count(seen, "POST /files/ HTTP/1.1\r\n"), 1);
        ck_assert_int_eq(count(seen, "Upload-Length: 5\r\n"), 1);
        ck_assert_int_eq(count(seen, "Expect: 100-continue\r\n"), 1);
        ck_assert_int_eq(count(seen, "Content-Type: application/offset+octet-stream\r\n"), 1 + cases[i].patches);
        ck_assert_int_eq(count(seen, "x-www-form-urlencoded"), 0);
        ck_assert_int_eq(count(seen, "\r\n\r\nhello"), 1);  /* the data, in the POST */
        ck_assert_int_eq(count(seen, "PATCH"), cases[i].patches);
        if (cases[i].patches > 0) {
            ck_assert_int_eq(count(seen, "Upload-Offset: 3\r\n"), 1);
            ck_assert_int_eq(count(seen, "\r\n\r\nlo"), 1);
        }
        if (code == VECTUS_OK) ck_assert(vectus_upload_offset(u) == 5);
        vectus_upload_cleanup(u);
        vectus_client_cleanup(c);
    }
}
END_TEST

START_TEST(test_upload_fake_create_with_upload_retry)
{
    /* The first POST gets no answer, so its data is lost with the URL, and the next perform sends it all again */
    const char *responses[] = {"", RESPONSE("201 Created", "Location: /files/abc\r\nUpload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;

    source.text = "hello";
    fake_server_start(&server, responses);
    c = test_client(server.url);
    c->extensions |= VECTUS_EXT_CREATION_WITH_UPLOAD;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);
    ck_assert_ptr_null(vectus_upload_url(u));
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);

    ck_assert_int_eq(source.seeks, 1);
    ck_assert(source.first_seek == 0);
    ck_assert_int_eq(count(seen, "\r\n\r\nhello"), 2);
    ck_assert(vectus_upload_offset(u) == 5);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_deferred)
{
#define DEFER_HEAD(headers) RESPONSE("200 OK", "Upload-Offset: " headers)
#define OFFSET(n) RESPONSE("204 No Content", "Upload-Offset: " #n "\r\n")
    const struct {
        const char *responses[4];
        int resume;
        unsigned long long stream;  /* how much of "hello" the stream delivers before it ends */
        vectus_code code;
        int patches;                /* -1 for a PATCH that may abort before reaching the server */
        int declares;               /* how many requests carry Upload-Length */
        const char *expect[3];      /* each sent exactly once */
    } cases[] = {
        /* The stream goes chunked, then an empty PATCH declares its length */
        {{CREATED, OFFSET(5), OFFSET(5), NULL}, 0, 5, VECTUS_OK, 2, 1,
         {"5\r\nhello\r\n0\r\n\r\n", "Upload-Length: 5\r\n", "Upload-Offset: 5\r\n"}},
        /* The server kept part of the stream, so the PATCH with the rest declares the length too */
        {{CREATED, OFFSET(3), OFFSET(5), NULL}, 0, 5, VECTUS_OK, 2, 1,
         {"Upload-Offset: 3\r\n", "Content-Length: 2\r\n", "\r\n\r\nlo"}},
        {{CREATED, OFFSET(0), OFFSET(0), NULL}, 0, 0, VECTUS_OK, 2, 1,
         {"\r\n\r\n0\r\n\r\n", "Upload-Length: 0\r\n", NULL}},
        /* Resuming while the server still waits for the length */
        {{DEFER_HEAD("2\r\nUpload-Defer-Length: 1\r\n"), OFFSET(5), OFFSET(5), NULL}, 1, 5, VECTUS_OK, 2, 1,
         {"3\r\nllo\r\n0\r\n\r\n", "Upload-Offset: 5\r\n", NULL}},
        /* The length was declared, but its response lost: nothing left to send or declare */
        {{DEFER_HEAD("5\r\nUpload-Length: 5\r\n"), NULL}, 1, 5, VECTUS_OK, 0, 0, {NULL}},
        /* Resuming with the length declared earlier: a plain PATCH of the rest */
        {{DEFER_HEAD("2\r\nUpload-Length: 5\r\n"), OFFSET(5), NULL}, 1, 5, VECTUS_OK, 1, 0,
         {"Content-Length: 3\r\n", "\r\n\r\nllo", NULL}},
        /* The server was told more than the stream holds; curl may find out before it sends anything */
        {{DEFER_HEAD("0\r\nUpload-Length: 9\r\n"), OFFSET(9), NULL}, 1, 5, VECTUS_ERR_SOURCE, -1, 0, {NULL}},
    };
#undef DEFER_HEAD
#undef OFFSET
    size_t i;
    size_t j;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct source source = {0};
        struct fake_run run;

        source.failure = SOURCE_ENDS_EARLY;
        source.fail_at = cases[i].stream;
        fake_run_with(&run, cases[i].responses, &source, source_seek, cases[i].resume, VECTUS_LENGTH_UNKNOWN,
                      VECTUS_EXT_CREATION_DEFER_LENGTH);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        ck_assert_msg(cases[i].patches < 0 ? count(run.seen, "PATCH") <= 1 : count(run.seen, "PATCH") == cases[i].patches,
                      "case %zu: %d PATCH", i, count(run.seen, "PATCH"));
        ck_assert_msg(count(run.seen, "Upload-Length:") == cases[i].declares, "case %zu: %d Upload-Length", i,
                      count(run.seen, "Upload-Length:"));
        ck_assert_int_eq(count(run.seen, "Upload-Defer-Length: 1\r\n"), cases[i].resume ? 0 : 1);
        for (j = 0; j < sizeof cases[i].expect / sizeof cases[i].expect[0] && cases[i].expect[j] != NULL; j++) {
            ck_assert_msg(count(run.seen, cases[i].expect[j]) == 1, "case %zu: %s", i, cases[i].expect[j]);
        }
        if (run.code == VECTUS_OK) ck_assert(vectus_upload_offset(run.upload) == cases[i].stream);
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_upload_fake_concatenation)
{
    const char *partial_responses[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    const char *final_responses[] = {RESPONSE("201 Created", "Location: /files/final\r\n"), NULL};
    const char *urls[] = {"/files/a", "/files/b"};
    struct source source = {0};
    struct fake_server server;
    struct fake_run run;
    vectus_client c;
    vectus_upload u;
    const char *seen;
    char url[96];

    /* A partial upload is created as one, then sent as usual */
    fake_run_with(&run, partial_responses, &source, source_seek, 0, 5, VECTUS_EXT_CONCATENATION);
    ck_assert_int_eq(run.code, VECTUS_OK);  /* fake_run_with marks nothing partial: check the header is opt-in */
    ck_assert_int_eq(count(run.seen, "Upload-Concat"), 0);
    fake_run_cleanup(&run);

    memset(&source, 0, sizeof source);
    source.text = "hello";
    fake_server_start(&server, partial_responses);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    vectus_upload_set_partial(u);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_UNSUPPORTED);  /* no concatenation */
    ck_assert_ptr_nonnull(strstr(vectus_upload_error(u), "concatenation"));
    c->extensions |= VECTUS_EXT_CONCATENATION;
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);
    ck_assert_int_eq(count(seen, "Upload-Concat: partial\r\n"), 1);
    ck_assert_int_eq(count(seen, "Upload-Length: 5\r\n"), 1);
    ck_assert_int_eq(count(seen, "PATCH /files/abc"), 1);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);

    /* The final upload is only created, with its own metadata, and never sent to */
    fake_server_start(&server, final_responses);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create_final(&u, urls, 2), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "filename", "world_domination_plan.pdf"), VECTUS_OK);
    vectus_upload_set_partial(u);  /* a mistake, which must not make it both */
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_UNSUPPORTED);
    c->extensions |= VECTUS_EXT_CONCATENATION;
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);  /* already created: no request */
    seen = fake_server_stop(&server);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/files/final", server.port);
    ck_assert_str_eq(vectus_upload_url(u), url);
    ck_assert(vectus_upload_offset(u) == 0);
    ck_assert_int_eq(count(seen, "POST /files/ HTTP/1.1\r\n"), 1);
    ck_assert_int_eq(count(seen, "final;/files/a /files/b\r\n"), 1);
    ck_assert_int_eq(count(seen, "Upload-Concat"), 1);
    ck_assert_int_eq(count(seen, "filename d29ybGRfZG9taW5hdGlvbl9wbGFuLnBkZg==\r\n"), 1);
    ck_assert_int_eq(count(seen, "Upload-Length"), 0);  /* the server sums it up */
    ck_assert_int_eq(count(seen, "Upload-Defer-Length"), 0);
    ck_assert_int_eq(count(seen, "PATCH"), 0);
    ck_assert_int_eq(count(seen, "HEAD"), 0);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_method_override)
{
    const char *responses[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), RESPONSE("204 No Content", ""), NULL};
    struct source source = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;

    source.text = "hello";
    fake_server_start(&server, responses);
    c = test_client(server.url);
    ck_assert_int_eq(c->method_override, 0);  /* off by default */
    vectus_client_set_method_override(c, 2);
    ck_assert_int_eq(c->method_override, 1);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, NULL, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);

    ck_assert_int_eq(count(seen, "POST /files/abc HTTP/1.1\r\n"), 2);
    ck_assert_int_eq(count(seen, "X-HTTP-Method-Override: PATCH\r\n"), 1);
    ck_assert_int_eq(count(seen, "X-HTTP-Method-Override: DELETE\r\n"), 1);
    ck_assert_int_eq(count(seen, "PATCH /"), 0);
    ck_assert_int_eq(count(seen, "DELETE /"), 0);
    ck_assert_int_eq(count(seen, "Upload-Offset: 0\r\n"), 1);
    ck_assert_int_eq(count(seen, "\r\n\r\nhello"), 1);
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
        {RESPONSE("204 No Content", ""), VECTUS_OK, 204, ""},
        {RESPONSE("404 Not Found", ""), VECTUS_ERR_GONE, 404, "DELETE answered HTTP 404"},
        {RESPONSE("500 Internal Server Error", ""), VECTUS_ERR_HTTP_STATUS, 500, "DELETE answered HTTP 500"},
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
        assert_detail(u, &cases[i], i);
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
    init_code = vectus_client_create(&c, server.url, vectus_curl_transport(), NULL);
    if (init_code == VECTUS_OK && vectus_upload_create(&u, source_read, NULL, &source, 5) == VECTUS_OK) {  /* asks OPTIONS first */
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
        {RESPONSE("201 Created", ""), VECTUS_ERR_PROTOCOL, 201, "POST answered 201 without a Location header"},
        {RESPONSE("201 Created", "Location: http://[::1\r\n"), VECTUS_ERR_PROTOCOL, 201, "not a URL"},
        {RESPONSE("413 Request Entity Too Large", ""), VECTUS_ERR_TOO_LARGE, 413, "POST answered HTTP 413"},
        {RESPONSE("500 Internal Server Error", ""), VECTUS_ERR_HTTP_STATUS, 500, "POST answered HTTP 500"},
        {RESPONSE("200 OK", "Location: /files/abc\r\n"), VECTUS_ERR_HTTP_STATUS, 200, "POST answered HTTP 200"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {cases[i].response, NULL};
        struct source source = {0};
        struct fake_run run;

        fake_run(&run, responses, &source, source_seek, 0);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        assert_detail(run.upload, &cases[i], i);
        ck_assert_int_eq(count(run.seen, "PATCH"), 0);
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_upload_fake_resume_errors)
{
    const struct expectation cases[] = {
        {RESPONSE("404 Not Found", ""), VECTUS_ERR_GONE, 404, "HEAD answered HTTP 404"},
        {RESPONSE("410 Gone", ""), VECTUS_ERR_GONE, 410, "HEAD answered HTTP 410"},
        {RESPONSE("403 Forbidden", ""), VECTUS_ERR_GONE, 403, "HEAD answered HTTP 403"},  /* the status tells it from a proxy refusing */
        {RESPONSE("500 Internal Server Error", ""), VECTUS_ERR_HTTP_STATUS, 500, "HEAD answered HTTP 500"},
        {RESPONSE("200 OK", "Upload-Length: 5\r\n"), VECTUS_ERR_PROTOCOL, 200, "HEAD answered without Upload-Offset"},
        {RESPONSE("200 OK", "Upload-Offset: 6\r\nUpload-Length: 5\r\n"), VECTUS_ERR_PROTOCOL, 200, "past the upload's length"},
        {RESPONSE("200 OK", "Upload-Offset: 0\r\nUpload-Length: 9\r\n"), VECTUS_ERR_PROTOCOL, 200, "as 9 bytes, not 5"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {cases[i].response, NULL};
        struct source source = {0};
        struct fake_run run;

        fake_run(&run, responses, &source, source_seek, 1);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        assert_detail(run.upload, &cases[i], i);
        ck_assert_int_eq(count(run.seen, "HEAD /files/abc HTTP/1.1\r\n"), 1);
        ck_assert_int_eq(count(run.seen, "PATCH"), 0);
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_upload_fake_send_errors)
{
    const struct expectation cases[] = {
        /* no progress, which would loop forever */
        {RESPONSE("204 No Content", "Upload-Offset: 0\r\n"), VECTUS_ERR_PROTOCOL, 204, "PATCH from byte 0 answered an impossible Upload-Offset of 0"},
        {RESPONSE("204 No Content", ""), VECTUS_ERR_PROTOCOL, 204, "PATCH answered without Upload-Offset"},
        {RESPONSE("204 No Content", "Upload-Offset: 6\r\n"), VECTUS_ERR_PROTOCOL, 204, "impossible Upload-Offset of 6"},
        {RESPONSE("409 Conflict", ""), VECTUS_ERR_HTTP_STATUS, 409, "PATCH answered HTTP 409"},
        {RESPONSE("404 Not Found", ""), VECTUS_ERR_GONE, 404, "PATCH answered HTTP 404"},
        {RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), VECTUS_OK, 204, ""},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *responses[] = {CREATED, cases[i].response, NULL};
        struct source source = {0};
        struct fake_run run;

        fake_run(&run, responses, &source, source_seek, 0);
        ck_assert_msg(run.code == cases[i].code, "case %zu: %s", i, vectus_strerror(run.code));
        assert_detail(run.upload, &cases[i], i);
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
    ck_assert_str_eq(vectus_upload_error(run.upload), "could not seek the data to byte 2: the upload has no seek callback");
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
        source.fail_at = i == 0 ? 0 : 2;  /* an aborting source reads whole until it aborts, one ending early gives what it has */
        fake_run(&run, responses, &source, source_seek, 0);
        ck_assert_msg(run.code == VECTUS_ERR_SOURCE, "case %zu: %s", i, vectus_strerror(run.code));
        ck_assert_str_eq(vectus_upload_error(run.upload), i == 0 ? "the read callback aborted, at byte 0"
                                                                 : "the data ended before the upload's length, at byte 2");
        fake_run_cleanup(&run);
    }
}
END_TEST

START_TEST(test_client_fake_discover)
{
    const char *responses[] = {
        RESPONSE("200 OK", "Tus-Version: 1.0.0,0.2.2\r\nTus-Extension: creation,expiration\r\nTus-Max-Size: 1000\r\n"),
        RESPONSE("401 Unauthorized", ""),
        RESPONSE("200 OK", "Tus-Extension: creation\r\n"),
        RESPONSE("200 OK", "Tus-Version: 0.2.2\r\n"),
        NULL,
    };
    struct fake_server server;
    vectus_client c;
    const char *seen;

    fake_server_start(&server, responses);
    ck_assert_int_eq(vectus_client_create(&c, server.url, vectus_curl_transport(), NULL), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "Authorization", "Bearer t"), VECTUS_OK);

    ck_assert_int_eq(vectus_client_discover(c), VECTUS_OK);
    ck_assert_uint_eq(vectus_client_extensions(c), VECTUS_EXT_CREATION | VECTUS_EXT_EXPIRATION);
    ck_assert(vectus_client_max_size(c) == 1000);
    ck_assert_int_eq(vectus_client_http_status(c), 200);
    ck_assert_str_eq(vectus_client_error(c), "");

    /* Asking again forgets the earlier answer */
    ck_assert_int_eq(vectus_client_discover(c), VECTUS_ERR_HTTP_STATUS);
    ck_assert_int_eq(vectus_client_http_status(c), 401);
    ck_assert_str_eq(vectus_client_error(c), "OPTIONS answered HTTP 401");
    ck_assert_uint_eq(vectus_client_extensions(c), 0);
    ck_assert(vectus_client_max_size(c) == 0);
    ck_assert(!c->discovered);

    ck_assert_int_eq(vectus_client_discover(c), VECTUS_ERR_PROTOCOL);
    ck_assert_str_eq(vectus_client_error(c), "OPTIONS answered without Tus-Version: is this a tus endpoint?");
    ck_assert_int_eq(vectus_client_discover(c), VECTUS_ERR_PROTOCOL);
    ck_assert_str_eq(vectus_client_error(c), "the server offers tus 0.2.2, not 1.0.0");
    seen = fake_server_stop(&server);

    ck_assert_int_eq(count(seen, "OPTIONS /files/ HTTP/1.1\r\n"), 4);
    ck_assert_int_eq(count(seen, "Authorization: Bearer t\r\n"), 4);
    ck_assert_int_eq(count(seen, "Tus-Resumable"), 0);  /* OPTIONS needs none */
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_auth_refresh)
{
    /* The server wants credentials even to say what it offers, so the first perform fails asking it */
    const char *responses[] = {RESPONSE("401 Unauthorized", ""), OPTIONS_OK, CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;

    source.text = "hello";
    fake_server_start(&server, responses);
    ck_assert_int_eq(vectus_client_create(&c, server.url, vectus_curl_transport(), NULL), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "Authorization", "Bearer expired"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);

    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_HTTP_STATUS);
    ck_assert_int_eq(vectus_upload_http_status(u), 401);  /* from the OPTIONS asked on the way */
    ck_assert_str_eq(vectus_upload_error(u), "OPTIONS answered HTTP 401");
    ck_assert_ptr_null(vectus_upload_url(u));

    ck_assert_int_eq(vectus_client_set_header(c, "Authorization", "Bearer fresh"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_http_status(u), 204);
    ck_assert_str_eq(vectus_upload_error(u), "");
    seen = fake_server_stop(&server);

    ck_assert_int_eq(count(seen, "OPTIONS /files/ HTTP/1.1\r\n"), 2);
    ck_assert_int_eq(count(seen, "Authorization: Bearer expired\r\n"), 1);
    ck_assert_int_eq(count(seen, "Authorization: Bearer fresh\r\n"), 3);  /* OPTIONS, POST and PATCH */
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_headers_everywhere)
{
    const char *responses[] = {
        OPTIONS_OK,
        CREATED,
        RESPONSE("204 No Content", "Upload-Offset: 5\r\n"),
        RESPONSE("200 OK", "Upload-Offset: 5\r\nUpload-Length: 5\r\n"),
        RESPONSE("204 No Content", ""),
        NULL,
    };
    const char *requests[] = {"OPTIONS /files/ ", "POST /files/ ", "PATCH /files/abc ", "HEAD /files/abc ", "DELETE /files/abc "};
    struct source source = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;
    int calls = 0;
    struct vectus_curl_options options = {set_user_agent, &calls};
    size_t i;

    source.text = "hello";
    fake_server_start(&server, responses);
    ck_assert_int_eq(vectus_client_create(&c, server.url, vectus_curl_transport(), &options), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "X-Custom", "1"), VECTUS_OK);
    ck_assert_int_eq(vectus_client_set_header(c, "X-Empty", ""), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);                      /* OPTIONS, POST, PATCH */
    ck_assert_int_eq(vectus_upload_set_url(u, vectus_upload_url(u)), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);                      /* HEAD, which finds it done */
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_OK);                    /* DELETE */
    seen = fake_server_stop(&server);

    for (i = 0; i < sizeof requests / sizeof requests[0]; i++) {
        ck_assert_msg(count(seen, requests[i]) == 1, "%s", requests[i]);
    }
    ck_assert_int_eq(count(seen, "X-Custom: 1\r\n"), 5);
    ck_assert_int_eq(count(seen, "X-Empty:\r\n"), 5);
    ck_assert_int_eq(count(seen, "User-Agent: vectus-test/1\r\n"), 5);
    ck_assert_int_eq(calls, 5);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_resume_without_discovery)
{
    /* Resuming needs nothing the server offers, so it goes straight to HEAD */
    const char *responses[] = {RESPONSE("200 OK", "Upload-Offset: 2\r\nUpload-Length: 5\r\n"), RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;
    char url[96];

    source.text = "hello";
    fake_server_start(&server, responses);
    ck_assert_int_eq(vectus_client_create(&c, server.url, vectus_curl_transport(), NULL), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", server.port);
    ck_assert_int_eq(vectus_upload_set_url(u, url), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);

    ck_assert_int_eq(count(seen, "OPTIONS"), 0);
    ck_assert_int_eq(count(seen, "\r\n\r\nllo"), 1);
    ck_assert(!c->discovered);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_connection_reuse)
{
    const char *responses[] = {
        KEEP_ALIVE("204 No Content", "Tus-Resumable: 1.0.0\r\nTus-Version: 1.0.0\r\nTus-Extension: creation,termination\r\n"),
        KEEP_ALIVE("201 Created", "Location: /files/abc\r\n"),
        KEEP_ALIVE("204 No Content", "Upload-Offset: 5\r\n"),
        KEEP_ALIVE("200 OK", "Upload-Offset: 5\r\nUpload-Length: 5\r\n"),
        KEEP_ALIVE("204 No Content", ""),
        NULL,
    };
    struct source source = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;

    source.text = "hello";
    fake_server_start(&server, responses);
    c = test_client(server.url);
    c->discovered = 0;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_set_url(u, vectus_upload_url(u)), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);

    ck_assert_int_eq(count(seen, " /files/"), 5);
    ck_assert_int_eq(count(seen, "[accept]"), 1);  /* OPTIONS, POST, PATCH, HEAD and DELETE over one connection */
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_progress)
{
    /* The server keeps 3 bytes of the first PATCH, so the second one reports from 3 again */
    const char *accepted_in_two[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 3\r\n"), RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    const char *deferred[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), RESPONSE("204 No Content", "Upload-Offset: 5\r\n"), NULL};
    struct source source = {0};
    struct progress progress = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;

    source.text = "hello";
    fake_server_start(&server, accepted_in_two);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
    vectus_upload_set_progress(u, progress_record, &progress);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    fake_server_stop(&server);
    ck_assert_int_ge(progress.calls, 3);  /* at least once per request */
    ck_assert(progress.first_length == 5 && progress.last_length == 5);
    ck_assert(progress.last_offset == 5);
    ck_assert(progress.max_offset == 5);  /* never past the data */
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);

    /* A stream of unknown length learns it when the stream ends */
    memset(&source, 0, sizeof source);
    memset(&progress, 0, sizeof progress);
    source.text = "hello";
    source.failure = SOURCE_ENDS_EARLY;
    source.fail_at = 5;
    fake_server_start(&server, deferred);
    c = test_client(server.url);
    c->extensions |= VECTUS_EXT_CREATION_DEFER_LENGTH;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);
    vectus_upload_set_progress(u, progress_record, &progress);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    fake_server_stop(&server);
    ck_assert(progress.first_length == VECTUS_LENGTH_UNKNOWN);
    ck_assert(progress.last_length == 5);
    ck_assert(progress.last_offset == 5);
    ck_assert(progress.max_offset == 5);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_fake_cancel)
{
    const char *responses[] = {RESPONSE("200 OK", "Upload-Offset: 0\r\nUpload-Length: 5\r\n"), NULL};
    struct source source = {0};
    struct progress progress = {0};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    char url[96];

    progress.cancel = 1;  /* at the first call */
    fake_server_start(&server, responses);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 5), VECTUS_OK);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", server.port);
    ck_assert_int_eq(vectus_upload_set_url(u, url), VECTUS_OK);
    vectus_upload_set_progress(u, progress_record, &progress);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_CANCELLED);
    fake_server_stop(&server);

    ck_assert_str_eq(vectus_upload_error(u), "HEAD: cancelled by the progress callback");
    ck_assert_int_eq(vectus_upload_http_status(u), 0);
    ck_assert_str_eq(vectus_upload_url(u), url);  /* kept, to resume later */
    ck_assert_int_eq(progress.calls, 1);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

/* Where the body of the first request starting with request begins, in what the fake server saw */
static const char *body_of(const char *seen, const char *request) {
    const char *at = strstr(seen, request);

    ck_assert_ptr_nonnull(at);
    at = strstr(at, "\r\n\r\n");
    ck_assert_ptr_nonnull(at);
    return at + 4;
}

/* data holds the test data from byte from to byte to */
static void assert_test_data(const char *data, unsigned long long from, unsigned long long to) {
    unsigned long long i;

    for (i = from; i < to; i++) {
        if ((unsigned char) data[i - from] != source_byte(i)) ck_abort_msg("byte %llu differs", i);
    }
}

START_TEST(test_upload_fake_file)
{
    const char *created[] = {CREATED, RESPONSE("204 No Content", "Upload-Offset: 3000\r\n"), NULL};
    const char *resumed[] = {RESPONSE("200 OK", "Upload-Offset: 1000\r\nUpload-Length: 3000\r\n"), RESPONSE("204 No Content", "Upload-Offset: 3000\r\n"), NULL};
    struct fake_server server;
    vectus_client c;
    vectus_upload u;
    const char *seen;
    char path[512];
    char url[96];

    temp_file(path, sizeof path, 3000);
    fake_server_start(&server, created);
    c = test_client(server.url);
    ck_assert_int_eq(vectus_upload_create_file(&u, path), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);
    ck_assert_int_eq(count(seen, "Upload-Length: 3000\r\n"), 1);
    ck_assert_int_eq(count(seen, "Content-Length: 3000\r\n"), 1);
    assert_test_data(body_of(seen, "PATCH /files/abc"), 0, 3000);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);

    /* Another process resumes it, reading the file from where the server stopped */
    fake_server_start(&server, resumed);
    c = test_client(server.url);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/files/abc", server.port);
    ck_assert_int_eq(vectus_upload_create_file(&u, path), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_set_url(u, url), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    seen = fake_server_stop(&server);
    ck_assert_int_eq(count(seen, "Upload-Offset: 1000\r\n"), 1);
    ck_assert_int_eq(count(seen, "Content-Length: 2000\r\n"), 1);
    assert_test_data(body_of(seen, "PATCH /files/abc"), 1000, 3000);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
    unlink(path);
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

    ck_assert_int_eq(vectus_client_create(&c, getenv(variable), vectus_curl_transport(), NULL), VECTUS_OK);
    ck_assert_msg(vectus_client_discover(c) == VECTUS_OK, "%s", vectus_client_error(c));
    return c;
}

/* Upload length bytes of test data in one go, returning the upload */
static vectus_upload live_upload(vectus_client c, struct source *source, unsigned long long length) {
    vectus_upload u;

    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, source, length), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    return u;
}

/*
 * Call perform until the upload is done, as an application would, returning how many calls it took.
 * Only connection and HTTP errors may happen on the way: tusd can briefly hold the interrupted request.
 */
static int perform_until_done(vectus_client c, vectus_upload u, int attempts) {
    int i;

    for (i = 1; i <= attempts; i++) {
        vectus_code code = vectus_upload_perform(c, u);

        if (code == VECTUS_OK) return i;
        ck_assert_msg(code == VECTUS_ERR_TRANSPORT || code == VECTUS_ERR_HTTP_STATUS, "attempt %d: %s: %s", i, vectus_strerror(code),
                      vectus_upload_error(u));
        sleep(1);
    }
    ck_abort_msg("the upload was not done after %d attempts", attempts);
    return 0;
}

/* The upload resumed from the middle: a seek to an offset the server reported, strictly inside the data */
static void assert_resumed_mid_way(const struct source *source) {
    ck_assert_int_ge(source->seeks, 1);
    ck_assert_msg(source->first_seek > 0 && source->first_seek < 4 * MIB, "resumed from %llu", source->first_seek);
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

START_TEST(test_upload_live_small)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u;

    ck_assert(vectus_client_extensions(c) & VECTUS_EXT_CREATION_WITH_UPLOAD);  /* so the data goes in the POST */
    u = live_upload(c, &source, 1000);
    ck_assert(vectus_upload_offset(u) == 1000);
    ck_assert_int_eq(source.seeks, 0);
    assert_uploaded(vectus_upload_url(u), 1000);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_deferred)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u;

    ck_assert(vectus_client_extensions(c) & VECTUS_EXT_CREATION_DEFER_LENGTH);
    source.failure = SOURCE_ENDS_EARLY;  /* announced without a length, so this is where the upload ends */
    source.fail_at = 4 * MIB + 7;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_OK);
    ck_assert(vectus_upload_offset(u) == 4 * MIB + 7);
    assert_uploaded(vectus_upload_url(u), 4 * MIB + 7);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_concatenation)
{
    struct source first = {0};
    struct source second = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload parts[2];
    vectus_upload final;
    const char *urls[2];

    ck_assert(vectus_client_extensions(c) & VECTUS_EXT_CONCATENATION);

    /* A small part, which rides in its creation POST, then a stream of unknown length carrying on from it */
    ck_assert_int_eq(vectus_upload_create(&parts[0], source_read, source_seek, &first, 1000), VECTUS_OK);
    second.base = 1000;
    second.failure = SOURCE_ENDS_EARLY;
    second.fail_at = 4 * MIB + 7;
    ck_assert_int_eq(vectus_upload_create(&parts[1], source_read, source_seek, &second, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);
    vectus_upload_set_partial(parts[0]);
    vectus_upload_set_partial(parts[1]);
    ck_assert_int_eq(vectus_upload_perform(c, parts[0]), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, parts[1]), VECTUS_OK);

    urls[0] = vectus_upload_url(parts[0]);
    urls[1] = vectus_upload_url(parts[1]);
    ck_assert_int_eq(vectus_upload_create_final(&final, urls, 2), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_add_metadata(final, "filename", "joined.bin"), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, final), VECTUS_OK);
    assert_uploaded(vectus_upload_url(final), 1000 + 4 * MIB + 7);

    ck_assert_int_eq(vectus_upload_terminate(c, final), VECTUS_OK);
    vectus_upload_cleanup(final);
    vectus_upload_cleanup(parts[0]);
    vectus_upload_cleanup(parts[1]);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_upload_live_method_override)
{
    struct source source = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u;

    vectus_client_set_method_override(c, 1);
    u = live_upload(c, &source, 2 * MIB);  /* over the creation-with-upload limit, so it takes a PATCH */
    assert_uploaded(vectus_upload_url(u), 2 * MIB);
    ck_assert_int_eq(vectus_upload_terminate(c, u), VECTUS_OK);  /* a DELETE */
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

START_TEST(test_upload_live_file)
{
    char path[512];
    vectus_client c;
    vectus_upload u;

    /* Not discovered beforehand: creating asks the server on the way */
    ck_assert_int_eq(vectus_client_create(&c, getenv("VECTUS_TEST_URL"), vectus_curl_transport(), NULL), VECTUS_OK);
    temp_file(path, sizeof path, 4 * MIB + 3);
    ck_assert_int_eq(vectus_upload_create_file(&u, path), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_add_metadata(u, "filename", "test.bin"), VECTUS_OK);
    ck_assert_msg(vectus_upload_perform(c, u) == VECTUS_OK, "%s", vectus_upload_error(u));
    ck_assert(c->discovered);
    assert_uploaded(vectus_upload_url(u), 4 * MIB + 3);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
    unlink(path);
}
END_TEST

START_TEST(test_upload_live_progress_cancel)
{
    struct source source = {0};
    struct progress progress = {0};
    vectus_client c = live_client("VECTUS_TEST_URL");
    vectus_upload u;

    progress.cancel = 1;
    progress.cancel_at = 1 * MIB;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 4 * MIB), VECTUS_OK);
    vectus_upload_set_progress(u, progress_record, &progress);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_CANCELLED);
    ck_assert_str_eq(vectus_upload_error(u), "PATCH: cancelled by the progress callback");
    ck_assert_ptr_nonnull(vectus_upload_url(u));

    perform_until_done(c, u, 5);  /* the progress callback carries on, without cancelling again */
    /* Resumed, not started over: no seek when the server kept all that was read, otherwise one into the middle */
    ck_assert_msg(source.seeks == 0 || (source.first_seek > 0 && source.first_seek < 4 * MIB), "resumed from %llu", source.first_seek);
    ck_assert(progress.last_offset == 4 * MIB && progress.last_length == 4 * MIB);
    assert_uploaded(vectus_upload_url(u), 4 * MIB);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

/* One partial upload performed on its own thread, through a client the threads share */
struct parallel_part {
    vectus_client client;
    vectus_upload upload;
    struct source source;
    vectus_code code;
};

static void *perform_part(void *arg) {
    struct parallel_part *part = arg;

    part->code = vectus_upload_perform(part->client, part->upload);  /* assertions stay on the main thread */
    return NULL;
}

START_TEST(test_upload_live_parallel)
{
    struct parallel_part parts[2];
    pthread_t threads[2];
    const char *urls[2];
    vectus_client c = live_client("VECTUS_TEST_URL");  /* discovered, so the threads only read it */
    vectus_upload final;
    int i;

    memset(parts, 0, sizeof parts);
    for (i = 0; i < 2; i++) {
        parts[i].client = c;
        parts[i].source.base = (unsigned long long) i * 2 * MIB;  /* each its half of the test data */
        ck_assert_int_eq(vectus_upload_create(&parts[i].upload, source_read, source_seek, &parts[i].source, 2 * MIB), VECTUS_OK);
        vectus_upload_set_partial(parts[i].upload);
    }
    for (i = 0; i < 2; i++) ck_assert_int_eq(pthread_create(&threads[i], NULL, perform_part, &parts[i]), 0);
    for (i = 0; i < 2; i++) ck_assert_int_eq(pthread_join(threads[i], NULL), 0);
    for (i = 0; i < 2; i++) {
        ck_assert_msg(parts[i].code == VECTUS_OK, "part %d: %s", i, vectus_upload_error(parts[i].upload));
        urls[i] = vectus_upload_url(parts[i].upload);
    }

    ck_assert_int_eq(vectus_upload_create_final(&final, urls, 2), VECTUS_OK);
    ck_assert_int_eq(vectus_upload_perform(c, final), VECTUS_OK);
    assert_uploaded(vectus_upload_url(final), 4 * MIB);
    vectus_upload_cleanup(final);
    for (i = 0; i < 2; i++) vectus_upload_cleanup(parts[i].upload);
    vectus_client_cleanup(c);
}
END_TEST

/* SPDX-License-Identifier: MIT */

/*
 * vectus over esp_http_client, against tusd and Toxiproxy. Each check runs one risky case through vectus's API and
 * prints "ok" or "FAIL" lines, which count, and "note" lines, which are observations. run.sh runs it on ESP-IDF's
 * linux target, then builds it for the ESP32; on a board, set Wi-Fi and the URLs with `idf.py menuconfig`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_LINUX
#include <signal.h>
#else
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#endif

#include <vectus/esp.h>
#include <vectus/vectus.h>

#define KIB 1024ULL
#define MIB (1024ULL * 1024ULL)

static const char *tus_url;        /* tusd's creation endpoint */
static const char *proxied_url;    /* the same, through Toxiproxy */
static const char *toxiproxy_api;
static int failures;

#define CHECK(cond, ...) do { int ok_ = (cond); printf(ok_ ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
                              if (!ok_) failures++; } while (0)
#define NOTE(...) do { printf("  note  "); printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *setting(const char *name, const char *fallback) {
    const char *value = getenv(name);  /* the linux target's environment; the board has none */

    return value != NULL && *value != '\0' ? value : fallback;
}

static long long now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* The bytes every upload sends, so a download can be checked without keeping a copy */
static char pattern(unsigned long long i) {
    return (char) (i * 7 + (i >> 11));
}

/* The data to upload: length bytes of the pattern, ending there */
struct source {
    unsigned long long position;
    unsigned long long length;
    unsigned long long hook_at;  /* call hook once the stream gets there */
    void (*hook)(void);
    int seeks;
    unsigned long long first_seek;
};

static size_t source_read(void *userdata, char *buffer, size_t size) {
    struct source *s = userdata;
    size_t i;

    if (s->hook != NULL && s->position >= s->hook_at) {
        void (*hook)(void) = s->hook;

        s->hook = NULL;
        hook();
    }
    if (size > s->length - s->position) size = (size_t) (s->length - s->position);
    for (i = 0; i < size; i++) buffer[i] = pattern(s->position + i);
    s->position += size;
    return size;
}

static int source_seek(void *userdata, unsigned long long offset) {
    struct source *s = userdata;

    if (s->seeks++ == 0) s->first_seek = offset;
    s->position = offset;
    return 0;
}

/* esp_http_client options giving up after *(int *) userdata ms without a byte moving, instead of 30 s */
static void set_timeout(void *userdata, esp_http_client_config_t *config) {
    config->timeout_ms = *(int *) userdata;
}

static vectus_client client_for(const char *url, struct vectus_esp_options *options) {
    vectus_client c = NULL;
    vectus_code code = vectus_client_create(&c, url, vectus_esp_transport(), options);

    CHECK(code == VECTUS_OK, "vectus_client_create: %s", vectus_strerror(code));
    return c;
}

static vectus_upload upload_of(struct source *s, unsigned long long length) {
    vectus_upload u = NULL;

    vectus_upload_create(&u, source_read, source_seek, s, length);
    return u;
}

/* Perform, printing a line about how it went */
static vectus_code perform(vectus_client c, vectus_upload u) {
    long long start = now_ms();
    vectus_code code = vectus_upload_perform(c, u);

    printf("          perform: %s%s%s, after %lld ms, at byte %llu\n", vectus_strerror(code), code == VECTUS_OK ? "" : ", ",
           vectus_upload_error(u), now_ms() - start, vectus_upload_offset(u));
    return code;
}

/* Perform until done, a second apart, as an application would after errors worth another try; returns the performs it
   took, 0 if it never finished */
static int perform_until_done(vectus_client c, vectus_upload u) {
    int attempt;

    for (attempt = 1; attempt <= 10; attempt++) {
        vectus_code code = perform(c, u);

        if (code == VECTUS_OK) return attempt;
        if (code != VECTUS_ERR_TRANSPORT && code != VECTUS_ERR_HTTP_STATUS) return 0;
        vTaskDelay(pdMS_TO_TICKS(1000));  /* tusd may still hold the upload's lock for the broken request */
    }
    return 0;
}

/* GET the upload back and compare it with the pattern */
static int download_matches(vectus_upload u, unsigned long long length) {
    esp_http_client_config_t config = {.url = vectus_upload_url(u), .timeout_ms = 10000};
    esp_http_client_handle_t http;
    unsigned long long at = 0;
    int same = 1;
    char buffer[1024];
    int bytes;
    int i;

    if (config.url == NULL || (http = esp_http_client_init(&config)) == NULL) {
        return 0;
    }
    if (esp_http_client_open(http, 0) != ESP_OK || esp_http_client_fetch_headers(http) < 0
        || esp_http_client_get_status_code(http) != 200) {
        same = 0;
    }
    while (same && (bytes = esp_http_client_read(http, buffer, sizeof buffer)) > 0) {
        for (i = 0; i < bytes; i++) {
            if (buffer[i] != pattern(at + (unsigned long long) i)) same = 0;
        }
        at += (unsigned long long) bytes;
    }
    esp_http_client_cleanup(http);
    return same && at == length;
}

/* Call Toxiproxy's API */
static long toxiproxy(esp_http_client_method_t method, const char *path, const char *json) {
    char url[256];
    esp_http_client_config_t config = {.url = url, .timeout_ms = 5000};
    esp_http_client_handle_t http;
    long status = 0;

    snprintf(url, sizeof url, "%s%s", toxiproxy_api, path);
    http = esp_http_client_init(&config);
    if (http == NULL) {
        return 0;
    }
    esp_http_client_set_method(http, method);
    if (json != NULL) {
        esp_http_client_set_header(http, "Content-Type", "application/json");
        esp_http_client_set_post_field(http, json, (int) strlen(json));
    }
    if (esp_http_client_perform(http) == ESP_OK) status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    return status;
}

static void toxic_add(const char *name, const char *type, const char *attributes) {
    char json[256];

    snprintf(json, sizeof json, "{\"name\":\"%s\",\"type\":\"%s\",\"stream\":\"upstream\",\"attributes\":%s}", name, type, attributes);
    if (toxiproxy(HTTP_METHOD_POST, "/proxies/tusd/toxics", json) != 200) NOTE("could not add the %s toxic", name);
}

static void toxic_remove(const char *name) {
    char path[64];

    snprintf(path, sizeof path, "/proxies/tusd/toxics/%s", name);
    toxiproxy(HTTP_METHOD_DELETE, path, NULL);
}

/* Hold every byte towards tusd from now on, without closing the connection */
static void stall_now(void) {
    toxic_add("stall", "timeout", "{\"timeout\":0}");
}

static void check_discover(void) {
    vectus_client c = client_for(tus_url, NULL);
    unsigned int wanted = VECTUS_EXT_CREATION | VECTUS_EXT_CREATION_WITH_UPLOAD | VECTUS_EXT_CREATION_DEFER_LENGTH
                          | VECTUS_EXT_TERMINATION | VECTUS_EXT_CONCATENATION;
    vectus_code code;

    printf("\nDiscovering the server, its headers arriving as esp_http_client events\n");
    code = vectus_client_discover(c);
    CHECK(code == VECTUS_OK, "vectus_client_discover: %s %s", vectus_strerror(code), vectus_client_error(c));
    CHECK((vectus_client_extensions(c) & wanted) == wanted, "extensions 0x%x", vectus_client_extensions(c));
    vectus_client_cleanup(c);
}

static void check_upload_and_resume(void) {
    vectus_client c = client_for(tus_url, NULL);
    struct source s = {.length = 2 * MIB};  /* over 1 MiB, so the data goes in a PATCH, not with the POST */
    vectus_upload u = upload_of(&s, s.length);
    long long start;

    printf("\nCreating and sending 2 MiB, then resuming the finished upload, which only asks with HEAD\n");
    CHECK(perform(c, u) == VECTUS_OK && vectus_upload_offset(u) == s.length, "POST and PATCH");
    CHECK(download_matches(u, s.length), "the upload downloads back byte for byte");
    vectus_upload_set_url(u, vectus_upload_url(u));
    start = now_ms();
    CHECK(perform(c, u) == VECTUS_OK && s.seeks == 0, "HEAD found it done, in %lld ms: it did not wait for a body", now_ms() - start);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_creation_with_upload(void) {
    vectus_client c = client_for(tus_url, NULL);
    struct source s = {.length = 64 * KIB};
    vectus_upload u = upload_of(&s, s.length);

    printf("\nCreation-with-upload: 64 KiB in the POST, without Expect: 100-continue\n");
    CHECK(perform(c, u) == VECTUS_OK && vectus_upload_offset(u) == s.length, "POST carrying the data");
    CHECK(download_matches(u, s.length), "the upload downloads back byte for byte");
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_deferred(void) {
    vectus_client c = client_for(tus_url, NULL);
    struct source s = {.length = 300 * KIB + 7};
    vectus_upload u = upload_of(&s, VECTUS_LENGTH_UNKNOWN);

    printf("\nA stream of unknown length: a chunked PATCH, then one declaring the length\n");
    CHECK(perform(c, u) == VECTUS_OK && vectus_upload_offset(u) == s.length, "the upload took the whole stream");
    CHECK(download_matches(u, s.length), "the upload downloads back byte for byte");
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_long_header(void) {
    vectus_client c = client_for(tus_url, NULL);
    struct source s = {.length = 2 * MIB};
    vectus_upload u = upload_of(&s, s.length);
    char *token = malloc(3001);

    printf("\nA 3000-byte Authorization header, past esp_http_client's default 512-byte buffer\n");
    memset(token, 'a', 3000);
    memcpy(token, "Bearer ", 7);
    token[3000] = '\0';
    vectus_client_set_header(c, "Authorization", token);
    CHECK(perform(c, u) == VECTUS_OK, "POST and PATCH");
    free(token);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_stale_connection(void) {
    vectus_client c = client_for(proxied_url, NULL);
    struct source s = {.length = 2 * MIB};
    vectus_upload u = upload_of(&s, s.length);
    int attempts;

    printf("\nA kept-alive connection the server dropped meanwhile (Toxiproxy disabled and enabled again)\n");
    CHECK(perform(c, u) == VECTUS_OK, "the upload went through the proxy");
    toxiproxy(HTTP_METHOD_POST, "/proxies/tusd", "{\"enabled\":false}");
    toxiproxy(HTTP_METHOD_POST, "/proxies/tusd", "{\"enabled\":true}");
    vectus_upload_set_url(u, vectus_upload_url(u));
    attempts = perform_until_done(c, u);
    CHECK(attempts > 0 && attempts <= 2, "resuming over the dropped connection took %d perform(s)", attempts);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_resume_after_cut(void) {
    vectus_client c = client_for(proxied_url, NULL);
    struct source s = {.length = 4 * MIB};
    vectus_upload u = upload_of(&s, s.length);
    int attempts;

    printf("\nResuming after the connection is cut 1 MiB in (Toxiproxy limit_data)\n");
    toxic_remove("cut");
    toxic_remove("stall");
    toxic_add("cut", "limit_data", "{\"bytes\":1048576}");
    CHECK(perform(c, u) == VECTUS_ERR_TRANSPORT, "the cut perform failed");
    toxic_remove("cut");
    attempts = perform_until_done(c, u);
    CHECK(attempts > 0 && s.first_seek > 0 && s.first_seek < s.length, "resumed from byte %llu, done after %d perform(s)", s.first_seek, attempts);
    CHECK(download_matches(u, s.length), "the upload downloads back byte for byte");
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_resume_after_stall(void) {
    static int two_seconds = 2000;
    static struct vectus_esp_options options = {set_timeout, &two_seconds};
    vectus_client c = client_for(proxied_url, &options);
    struct source s = {.length = 4 * MIB, .hook_at = 1 * MIB, .hook = stall_now};
    vectus_upload u = upload_of(&s, s.length);
    long long start = now_ms();
    vectus_code code;
    int attempts;

    printf("\nResuming after the connection goes silent 1 MiB in (Toxiproxy timeout), with a 2 s timeout\n");
    code = perform(c, u);
    CHECK(code == VECTUS_ERR_TRANSPORT && now_ms() - start < 15000, "the stalled perform gave up after %lld ms", now_ms() - start);
    toxic_remove("stall");
    attempts = perform_until_done(c, u);
    CHECK(attempts > 0 && s.first_seek > 0 && s.first_seek < s.length, "resumed from byte %llu, done after %d perform(s)", s.first_seek, attempts);
    CHECK(download_matches(u, s.length), "the upload downloads back byte for byte");
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static int cancel_past_256_kib(void *userdata, unsigned long long offset, unsigned long long length) {
    int *cancel = userdata;

    (void) length;
    return *cancel && offset > 256 * KIB;
}

static void check_cancel_and_terminate(void) {
    vectus_client c = client_for(tus_url, NULL);
    struct source s = {.length = 2 * MIB};
    vectus_upload u = upload_of(&s, s.length);
    int cancel = 1;
    vectus_code code;

    printf("\nCancelling from the progress callback, resuming, then terminating\n");
    vectus_upload_set_progress(u, cancel_past_256_kib, &cancel);
    code = perform(c, u);
    CHECK(code == VECTUS_ERR_CANCELLED && vectus_upload_url(u) != NULL, "the progress callback cancelled the PATCH");
    cancel = 0;
    /* Resumed, not started over: no seek when the server kept all that was read, otherwise one into the middle */
    CHECK(perform_until_done(c, u) > 0 && (s.seeks == 0 || (s.first_seek > 0 && s.first_seek < s.length)), "resumed, %s",
          s.seeks == 0 ? "the server having kept all that was read" : "seeking into the middle");
    CHECK(download_matches(u, s.length), "the upload downloads back byte for byte");
    code = vectus_upload_terminate(c, u);
    CHECK(code == VECTUS_OK && vectus_upload_url(u) == NULL, "DELETE: %s", vectus_strerror(code));
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

static void check_gone(void) {
    vectus_client c = client_for(tus_url, NULL);
    struct source s = {.length = 2 * MIB};
    vectus_upload u = upload_of(&s, s.length);
    char url[300];
    long long start = now_ms();
    vectus_code code;

    printf("\nResuming an upload tusd does not have\n");
    snprintf(url, sizeof url, "%s%s", tus_url, "0123456789abcdef0123456789abcdef");
    vectus_upload_set_url(u, url);
    code = perform(c, u);
    CHECK(code == VECTUS_ERR_GONE && vectus_upload_http_status(u) == 404 && now_ms() - start < 10000, "HEAD answered %ld",
          vectus_upload_http_status(u));
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}

void app_main(void) {
    /* esp_http_client posts every event to the default loop too, and logs an error each time there is none */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_IDF_TARGET_LINUX
    signal(SIGPIPE, SIG_IGN);  /* a write to a connection the server closed would kill the process; lwIP has no SIGPIPE */
#else
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(example_connect());  /* Wi-Fi, set under "Example Connection Configuration" in menuconfig */
#endif
    tus_url = setting("VECTUS_TEST_URL", CONFIG_VECTUS_TEST_URL);
    proxied_url = setting("VECTUS_TEST_PROXIED_URL", CONFIG_VECTUS_TEST_PROXIED_URL);
    toxiproxy_api = setting("VECTUS_TEST_TOXIPROXY", CONFIG_VECTUS_TEST_TOXIPROXY);
    printf("vectus over esp_http_client on ESP-IDF %d.%d.%d, against %s\n", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,
           ESP_IDF_VERSION_PATCH, tus_url);

    check_discover();
    check_upload_and_resume();
    check_creation_with_upload();
    check_deferred();
    check_long_header();
    check_stale_connection();
    check_resume_after_cut();
    check_resume_after_stall();
    check_cancel_and_terminate();
    check_gone();

    printf("\n%d check(s) failed\n", failures);
#if CONFIG_IDF_TARGET_LINUX
    exit(failures == 0 ? 0 : 1);
#else
    printf("lowest free heap: %u bytes internal, %u in all; main task stack never had less than %u bytes left\n",
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned) esp_get_minimum_free_heap_size(),
           (unsigned) uxTaskGetStackHighWaterMark(NULL));
#endif
}

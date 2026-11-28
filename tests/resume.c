/*
 * Resuming through Toxiproxy, which breaks the connection to tusd on purpose.
 * VECTUS_TEST_TOXIPROXY is its API, and VECTUS_TEST_PROXIED_URL the tus endpoint behind its proxy named "tusd";
 * tests/e2e-env.sh sets both up. Uses the helpers in upload.c, such as perform_until_done.
 */

static void toxic_add(const char *name, const char *type, const char *attributes) {
    char url[256];
    char json[256];

    snprintf(url, sizeof url, "%s/proxies/tusd/toxics", getenv("VECTUS_TEST_TOXIPROXY"));
    snprintf(json, sizeof json, "{\"name\":\"%s\",\"type\":\"%s\",\"stream\":\"upstream\",\"attributes\":%s}", name, type, attributes);
    ck_assert_int_eq(http("POST", url, json, NULL), 200);
}

static void toxic_remove(const char *name) {
    char url[256];
    long status;

    snprintf(url, sizeof url, "%s/proxies/tusd/toxics/%s", getenv("VECTUS_TEST_TOXIPROXY"), name);
    status = http("DELETE", url, NULL, NULL);
    ck_assert_msg(status == 204 || status == 404, "removing toxic %s answered %ld", name, status);
}

/* Check forks every test, so a failed one leaves its toxics behind: start clean */
static void toxics_clear(void) {
    toxic_remove("cut");
    toxic_remove("stall");
}

/* Hold every byte towards tusd from now on, without closing the connection */
static void stall_now(void) {
    toxic_add("stall", "timeout", "{\"timeout\":0}");
}

START_TEST(test_resume_after_cut)
{
    struct source source = {0};
    vectus_client c;
    vectus_upload u;

    toxics_clear();
    c = live_client("VECTUS_TEST_PROXIED_URL");
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 4 * MIB), VECTUS_OK);

    toxic_add("cut", "limit_data", "{\"bytes\":1048576}");  /* each connection closes after 1 MiB towards tusd */
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);
    toxic_remove("cut");

    perform_until_done(c, u, 5);
    assert_resumed_mid_way(&source);
    assert_uploaded(vectus_upload_url(u), 4 * MIB);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_resume_deferred_after_cut)
{
    struct source source = {0};
    vectus_client c;
    vectus_upload u;

    toxics_clear();
    c = live_client("VECTUS_TEST_PROXIED_URL");
    source.failure = SOURCE_ENDS_EARLY;  /* a stream of unknown length, cut while sent chunked */
    source.fail_at = 4 * MIB + 7;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, VECTUS_LENGTH_UNKNOWN), VECTUS_OK);

    toxic_add("cut", "limit_data", "{\"bytes\":1048576}");
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);
    toxic_remove("cut");

    perform_until_done(c, u, 5);
    assert_resumed_mid_way(&source);
    assert_uploaded(vectus_upload_url(u), 4 * MIB + 7);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_resume_after_stall)
{
    struct source source = {0};
    vectus_client c;
    vectus_upload u;
    time_t start;

    toxics_clear();
    c = live_client("VECTUS_TEST_PROXIED_URL");
    c->transport_userdata = &stall_2s;
    source.hook = stall_now;  /* the connection goes silent once 1 MiB has been read */
    source.hook_at = 1 * MIB;
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 4 * MIB), VECTUS_OK);

    start = time(NULL);
    ck_assert_int_eq(vectus_upload_perform(c, u), VECTUS_ERR_TRANSPORT);  /* nothing would end the request but vectus */
    ck_assert_int_lt(time(NULL) - start, 30);
    toxic_remove("stall");

    perform_until_done(c, u, 10);
    assert_resumed_mid_way(&source);
    assert_uploaded(vectus_upload_url(u), 4 * MIB);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

START_TEST(test_resume_repeatedly)
{
    struct source source = {0};
    vectus_client c;
    vectus_upload u;
    int attempts;

    toxics_clear();
    c = live_client("VECTUS_TEST_PROXIED_URL");
    ck_assert_int_eq(vectus_upload_create(&u, source_read, source_seek, &source, 4 * MIB), VECTUS_OK);

    /* Every connection dies after 1 MiB, so the upload only gets through a piece at a time */
    toxic_add("cut", "limit_data", "{\"bytes\":1048576}");
    attempts = perform_until_done(c, u, 20);
    toxic_remove("cut");

    ck_assert_int_ge(attempts, 4);
    ck_assert_int_ge(source.seeks, attempts - 1);
    assert_resumed_mid_way(&source);
    assert_uploaded(vectus_upload_url(u), 4 * MIB);
    vectus_upload_cleanup(u);
    vectus_client_cleanup(c);
}
END_TEST

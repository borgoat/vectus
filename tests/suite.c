#define _POSIX_C_SOURCE 200809L  /* sockets, fork and pipes for the fake servers in upload.c */

#include <check.h>

#include "client.c"
#include "upload.c"
#include "resume.c"

Suite *vectus_suite(void) {
    Suite *s;
    TCase *tc_client;
    TCase *tc_upload;
    TCase *tc_live;
    TCase *tc_resume;

    s = suite_create("vectus");
    tc_client = tcase_create("client");

    tcase_add_test(tc_client, test_parse_size);
    tcase_add_test(tc_client, test_parse_extensions);
    tcase_add_test(tc_client, test_header_callback);
    tcase_add_test(tc_client, test_client_getters);
    tcase_add_test(tc_client, test_cleanup_without_headers);
    tcase_add_test(tc_client, test_offers_tus_1_0_0);
    tcase_add_test(tc_client, test_strerror);
    tcase_add_test(tc_client, test_client_create);
    tcase_add_test(tc_client, test_client_set_header);
    tcase_add_test(tc_client, test_client_discover_unreachable);
    if (getenv("VECTUS_TEST_URL") != NULL) {
        tcase_add_test(tc_client, test_client_discover);
    }
    suite_add_tcase(s, tc_client);

    tc_upload = tcase_create("upload");
    tcase_set_timeout(tc_upload, 30);  /* Check's default is 4 seconds */
    tcase_add_test(tc_upload, test_null_handles);
    tcase_add_test(tc_upload, test_upload_create_file);
    tcase_add_test(tc_upload, test_upload_headers);
    tcase_add_test(tc_upload, test_upload_resolve_url);
    tcase_add_test(tc_upload, test_upload_read_wrapper);
    tcase_add_test(tc_upload, test_upload_create_and_urls);
    tcase_add_test(tc_upload, test_upload_too_large_for_server);
    tcase_add_test(tc_upload, test_upload_base64);
    tcase_add_test(tc_upload, test_upload_metadata);
    tcase_add_test(tc_upload, test_upload_create_final);
    tcase_add_test(tc_upload, test_upload_creates_with_data);
    tcase_add_test(tc_upload, test_upload_extensions_required);
    tcase_add_test(tc_upload, test_upload_unreachable);
    tcase_add_test(tc_upload, test_upload_stall);
    tcase_add_test(tc_upload, test_upload_fake_create_and_send);
    tcase_add_test(tc_upload, test_upload_fake_zero_length);
    tcase_add_test(tc_upload, test_upload_fake_metadata);
    tcase_add_test(tc_upload, test_upload_fake_create_with_upload);
    tcase_add_test(tc_upload, test_upload_fake_create_with_upload_retry);
    tcase_add_test(tc_upload, test_upload_fake_deferred);
    tcase_add_test(tc_upload, test_upload_fake_concatenation);
    tcase_add_test(tc_upload, test_upload_fake_method_override);
    tcase_add_test(tc_upload, test_upload_fake_expires);
    tcase_add_test(tc_upload, test_upload_fake_terminate);
    tcase_add_test(tc_upload, test_upload_fake_bodies_discarded);
    tcase_add_test(tc_upload, test_upload_fake_create_errors);
    tcase_add_test(tc_upload, test_upload_fake_resume_errors);
    tcase_add_test(tc_upload, test_upload_fake_send_errors);
    tcase_add_test(tc_upload, test_upload_fake_resume_needs_seek);
    tcase_add_test(tc_upload, test_upload_fake_partial_accept);
    tcase_add_test(tc_upload, test_upload_fake_source_fails);
    tcase_add_test(tc_upload, test_client_fake_discover);
    tcase_add_test(tc_upload, test_upload_fake_auth_refresh);
    tcase_add_test(tc_upload, test_upload_fake_headers_everywhere);
    tcase_add_test(tc_upload, test_upload_fake_resume_without_discovery);
    tcase_add_test(tc_upload, test_upload_fake_connection_reuse);
    tcase_add_test(tc_upload, test_upload_fake_progress);
    tcase_add_test(tc_upload, test_upload_fake_cancel);
    tcase_add_test(tc_upload, test_upload_fake_file);
    suite_add_tcase(s, tc_upload);

    if (getenv("VECTUS_TEST_URL") != NULL) {
        tc_live = tcase_create("live");
        tcase_set_timeout(tc_live, 60);
        tcase_add_test(tc_live, test_upload_live);
        tcase_add_test(tc_live, test_upload_live_small);
        tcase_add_test(tc_live, test_upload_live_deferred);
        tcase_add_test(tc_live, test_upload_live_concatenation);
        tcase_add_test(tc_live, test_upload_live_method_override);
        tcase_add_test(tc_live, test_upload_live_empty);
        tcase_add_test(tc_live, test_upload_live_resume);
        tcase_add_test(tc_live, test_upload_live_length_mismatch);
        tcase_add_test(tc_live, test_upload_live_gone);
        tcase_add_test(tc_live, test_upload_live_too_large);
        tcase_add_test(tc_live, test_upload_live_file);
        tcase_add_test(tc_live, test_upload_live_progress_cancel);
        tcase_add_test(tc_live, test_upload_live_parallel);
        suite_add_tcase(s, tc_live);
    }

    if (getenv("VECTUS_TEST_TOXIPROXY") != NULL && getenv("VECTUS_TEST_PROXIED_URL") != NULL) {
        tc_resume = tcase_create("resume");
        tcase_set_timeout(tc_resume, 120);
        tcase_add_test(tc_resume, test_resume_after_cut);
        tcase_add_test(tc_resume, test_resume_deferred_after_cut);
        tcase_add_test(tc_resume, test_resume_after_stall);
        tcase_add_test(tc_resume, test_resume_repeatedly);
        suite_add_tcase(s, tc_resume);
    }

    return s;
}

int main(void) {
    int number_failed = 0;
    Suite *s;
    SRunner *sr;

    s = vectus_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_ENV);  /* CK_VERBOSITY=verbose lists each test */
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include <check.h>

#include "client.c"

Suite *vectus_suite() {
    Suite *s;
    TCase *tc_client;

    s = suite_create("vectus");
    tc_client = tcase_create("client");

    tcase_add_test(tc_client, test_vectus_client_init);
    suite_add_tcase(s, tc_client);

    return s;
}

int main(void) {
    int number_failed = 0;
    Suite *s;
    SRunner *sr;

    s = vectus_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

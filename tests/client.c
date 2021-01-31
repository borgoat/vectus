#include <check.h>

/* client.c to access shadowed fields */
#include "../src/client.c"

START_TEST(test_vectus_client_init)
{
    vectus_client c;
    c = vectus_client_init("http://localhost:1080/files");

    ck_assert_str_eq(c->url, "http://localhost:1080/files");
    ck_assert(c->extensions & VECTUS_EXT_CREATION);

    vectus_client_cleanup(c);
}
END_TEST

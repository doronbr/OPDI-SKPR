#include "unity.h"
#include "opdi_net.h"

// NOTE: These tests are skeletons; real tests need accessors for internal MRU state or to infer via behavior.

void setUp(void) {}
void tearDown(void) {}

extern size_t opdi_net_profiles_serialize(char *out, size_t out_cap);

static void add_profile(const char *ssid){
    opdi_net_profile_t p = {0};
    snprintf(p.ssid, sizeof(p.ssid), "%s", ssid);
    p.auth = 0; p.hidden=false; p.psk[0]='\0';
    TEST_ASSERT_EQUAL(ESP_OK, opdi_net_add_profile(&p));
}

void test_mru_add_promote_order(void){
    TEST_ASSERT_EQUAL(ESP_OK, opdi_net_init());
    add_profile("A");
    add_profile("B");
    add_profile("C");
    // Serialize profiles list; newest added should be MRU[0]
    char buf[512]; size_t n = opdi_net_profiles_serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n>2);
    // Expect order C,B,A by insertion MRU promotion semantics
    // We only have masked IDs & lengths; cannot assert SSID directly without deeper hooks.
    // For now ensure length field counts match (all 1) and there are three objects.
    int count_len1=0; for (char *p=buf; *p; ++p) { if (strncmp(p, "\"ssid_len\":1", 12)==0) count_len1++; }
    TEST_ASSERT_EQUAL(3, count_len1);
    TEST_ASSERT_EQUAL(NET_STA_CONNECT, opdi_net_get_state());
}

// Additional test: when no profiles present, state becomes AP_ACTIVE already covered elsewhere.

int run_unity_tests(void){
    UNITY_BEGIN();
    RUN_TEST(test_mru_add_promote_order);
    return UNITY_END();
}

#ifdef CONFIG_IDF_TARGET
void app_main(void){
    run_unity_tests();
}
#endif

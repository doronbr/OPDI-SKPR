#include "unity.h"
#include "opdi_net.h"

void setUp(void) {}
void tearDown(void) {}

void test_retry_to_ap_transition(void){
    TEST_ASSERT_EQUAL(ESP_OK, opdi_net_init());
    // Without profiles should go directly to AP mode
    // Since attempt_next_profile sets AP when no profiles
    TEST_ASSERT_EQUAL(NET_AP_ACTIVE, opdi_net_get_state());
}

int run_unity_tests(void){
    UNITY_BEGIN();
    RUN_TEST(test_retry_to_ap_transition);
    return UNITY_END();
}

#ifdef CONFIG_IDF_TARGET
void app_main(void){
    run_unity_tests();
}
#endif

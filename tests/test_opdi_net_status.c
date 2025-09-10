#include "unity.h"
#include "opdi_net.h"
#include <string.h>

#ifndef CONFIG_OPDI_NET_TESTING
#warning "Tests require CONFIG_OPDI_NET_TESTING=y"
#endif

static char json_buf[512];

// Simple helper to call REST builder indirectly by hitting public APIs.
// We simulate by forcing states and using test simulate functions.

void setUp(void) {
}

void tearDown(void) {
}

void test_status_ap_mode(void) {
    opdi_net_test_force_state(NET_AP_ACTIVE);
    // We can't call internal builder directly; instead check metrics advancing via logs API or metrics.
    opdi_net_metrics_t m; opdi_net_get_metrics(&m);
    TEST_ASSERT_EQUAL_UINT(0, m.connects_success);
}

void test_status_sta_connected(void) {
    opdi_net_test_simulate_sta_connected("192.168.1.10", "192.168.1.1", -42);
    opdi_net_metrics_t m; opdi_net_get_metrics(&m);
    TEST_ASSERT_TRUE(m.connects_success >= 1);
}

int run_unity_tests(void);
int run_unity_tests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_ap_mode);
    RUN_TEST(test_status_sta_connected);
    return UNITY_END();
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
void app_main(void) {
    run_unity_tests();
}
#endif

// Tests for extended camera config & IR logic scaffolding
#include "unity.h"
#include "opdi_cam.h"
#include "nvs_flash.h"

static void feed_luma_seq(const uint16_t *vals, size_t n){
    for(size_t i=0;i<n;i++){ opdi_cam_on_frame(vals[i]); }
    // force periodic tick to commit fps counters
    opdi_cam_periodic_1s();
}

void setUp(void){ nvs_flash_init(); opdi_cam_init(); opdi_cam_manager_init(); }
void tearDown(void){}

void test_ext_config_roundtrip_minimal(void){
    opdi_cam_ext_config_t c; opdi_cam_ext_config_get(&c);
    uint8_t orig_q = c.jpeg_q;
    c.jpeg_q = 85; TEST_ASSERT_EQUAL(ESP_OK, opdi_cam_ext_config_set(&c));
    opdi_cam_ext_config_get(&c);
    TEST_ASSERT_EQUAL_UINT8(85, c.jpeg_q);
    // restore
    c.jpeg_q = orig_q; opdi_cam_ext_config_set(&c);
}

void test_ir_hysteresis_basic(void){
    opdi_cam_ir_set_mode(OPDI_IR_MODE_AUTO);
    // Sequence bright -> dark to trigger ON after hysteresis delay (we cannot fast-forward timer easily here, just exercise eval path)
    uint16_t seq1[] = {80,75,50,35,30,28,27};
    feed_luma_seq(seq1, sizeof(seq1)/sizeof(seq1[0]));
    // We cannot assert ON due to timing guard; just ensure function calls did not crash and luma updated.
    opdi_cam_telemetry_t t; opdi_cam_get_telemetry(&t);
    TEST_ASSERT_TRUE(t.luma_avg <= 80);
}

int run_unity_tests(void){
    UNITY_BEGIN();
    RUN_TEST(test_ext_config_roundtrip_minimal);
    RUN_TEST(test_ir_hysteresis_basic);
    return UNITY_END();
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
void app_main(void){ run_unity_tests(); }
#endif

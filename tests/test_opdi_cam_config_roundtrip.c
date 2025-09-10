// Unity test for opdi_cam config persistence & clamping
#include "unity.h"
#include "opdi_cam.h"
#include "nvs_flash.h"

static opdi_cam_config_t cfg;

void setUp(void) {
	// Ensure NVS initialized (idempotent if already)
	nvs_flash_init();
	opdi_cam_init();
}

void tearDown(void) {
}

static void reload_module(void) {
	// Simulate reboot by re-calling init (state is static inside module)
	opdi_cam_init();
}

void test_cam_config_defaults(void) {
	opdi_cam_get_config(&cfg);
	TEST_ASSERT_EQUAL_INT(0, cfg.brightness);
	TEST_ASSERT_EQUAL_INT(0, cfg.contrast);
	TEST_ASSERT_EQUAL_INT(0, cfg.saturation);
	TEST_ASSERT_TRUE(cfg.auto_exposure);
}

void test_cam_config_roundtrip_and_clamp(void) {
	opdi_cam_get_config(&cfg);
	cfg.brightness = 15; // should clamp to 10
	cfg.contrast = -15;  // should clamp to -10
	cfg.saturation = 7;  // within range
	cfg.auto_exposure = false;
	TEST_ASSERT_EQUAL(ESP_OK, opdi_cam_set_config(&cfg));

	// Reload to force NVS read
	reload_module();
	opdi_cam_get_config(&cfg);
	TEST_ASSERT_EQUAL_INT(10, cfg.brightness);
	TEST_ASSERT_EQUAL_INT(-10, cfg.contrast);
	TEST_ASSERT_EQUAL_INT(7, cfg.saturation);
	TEST_ASSERT_FALSE(cfg.auto_exposure);
}

int run_unity_tests(void);
int run_unity_tests(void) {
	UNITY_BEGIN();
	RUN_TEST(test_cam_config_defaults);
	RUN_TEST(test_cam_config_roundtrip_and_clamp);
	return UNITY_END();
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
void app_main(void) {
	run_unity_tests();
}
#endif

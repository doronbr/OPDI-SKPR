// Unity test for opdi_cam snapshot API contract
#include "unity.h"
#include "opdi_cam.h"
#include <string.h>

static unsigned char buf[4096];

void setUp(void) {
	opdi_cam_init();
}

void tearDown(void) {
}

void test_cam_snapshot_contract(void) {
	int need = opdi_cam_snapshot(NULL, 0);
	TEST_ASSERT_MESSAGE(need > 0, "Snapshot reported non-positive size");
	TEST_ASSERT_LESS_OR_EQUAL(4096, need); // ensure declared size not exceeding test buffer
	int written = opdi_cam_snapshot(buf, sizeof(buf));
	TEST_ASSERT_EQUAL(need, written);
	TEST_ASSERT_TRUE_MESSAGE(written >= 4, "Too small to contain JPEG markers");
	TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);
	TEST_ASSERT_EQUAL_HEX8(0xD8, buf[1]);
	TEST_ASSERT_EQUAL_HEX8(0xFF, buf[written-2]);
	TEST_ASSERT_EQUAL_HEX8(0xD9, buf[written-1]);
}

int run_unity_tests(void);
int run_unity_tests(void) {
	UNITY_BEGIN();
	RUN_TEST(test_cam_snapshot_contract);
	return UNITY_END();
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
void app_main(void) {
	run_unity_tests();
}
#endif

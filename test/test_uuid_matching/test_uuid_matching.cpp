// Unit tests for Raven UUID matching and firmware version estimation.

#include <unity.h>
#include "../../src/fy_detect.h"

// --- Raven UUID matching tests ---

void test_raven_uuid_known_service(void) {
    const char* uuids[] = { RAVEN_DEVICE_INFO_SERVICE };
    char out[41] = {0};
    TEST_ASSERT_TRUE(checkRavenUUIDFromStrings(uuids, 1, out));
    TEST_ASSERT_EQUAL_STRING(RAVEN_DEVICE_INFO_SERVICE, out);
}

void test_raven_uuid_all_known(void) {
    size_t count = sizeof(raven_service_uuids) / sizeof(raven_service_uuids[0]);
    for (size_t i = 0; i < count; i++) {
        const char* uuids[] = { raven_service_uuids[i] };
        TEST_ASSERT_TRUE_MESSAGE(
            checkRavenUUIDFromStrings(uuids, 1, NULL),
            raven_service_uuids[i]);
    }
}

void test_raven_uuid_case_insensitive(void) {
    const char* uuids[] = { "0000180A-0000-1000-8000-00805F9B34FB" };
    TEST_ASSERT_TRUE(checkRavenUUIDFromStrings(uuids, 1, NULL));
}

void test_raven_uuid_no_match(void) {
    const char* uuids[] = { "12345678-1234-1234-1234-123456789abc" };
    TEST_ASSERT_FALSE(checkRavenUUIDFromStrings(uuids, 1, NULL));
}

void test_raven_uuid_empty_list(void) {
    TEST_ASSERT_FALSE(checkRavenUUIDFromStrings(NULL, 0, NULL));
}

void test_raven_uuid_mixed_list(void) {
    // One unknown, one known — should still match
    const char* uuids[] = {
        "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        RAVEN_GPS_SERVICE
    };
    char out[41] = {0};
    TEST_ASSERT_TRUE(checkRavenUUIDFromStrings(uuids, 2, out));
    TEST_ASSERT_EQUAL_STRING(RAVEN_GPS_SERVICE, out);
}

void test_raven_uuid_null_out_buffer(void) {
    const char* uuids[] = { RAVEN_POWER_SERVICE };
    TEST_ASSERT_TRUE(checkRavenUUIDFromStrings(uuids, 1, NULL));
}

// --- Firmware version estimation tests ---

void test_fw_v11x(void) {
    // Old location service present, no new GPS
    TEST_ASSERT_EQUAL_STRING("1.1.x",
        estimateRavenFWFromFlags(false, true, false));
}

void test_fw_v12x(void) {
    // New GPS present, no power service
    TEST_ASSERT_EQUAL_STRING("1.2.x",
        estimateRavenFWFromFlags(true, false, false));
}

void test_fw_v13x(void) {
    // New GPS and power both present
    TEST_ASSERT_EQUAL_STRING("1.3.x",
        estimateRavenFWFromFlags(true, false, true));
}

void test_fw_unknown(void) {
    // No relevant services
    TEST_ASSERT_EQUAL_STRING("?",
        estimateRavenFWFromFlags(false, false, false));
}

void test_fw_v13x_all_flags(void) {
    // All flags true — new GPS + power wins
    TEST_ASSERT_EQUAL_STRING("1.3.x",
        estimateRavenFWFromFlags(true, true, true));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_raven_uuid_known_service);
    RUN_TEST(test_raven_uuid_all_known);
    RUN_TEST(test_raven_uuid_case_insensitive);
    RUN_TEST(test_raven_uuid_no_match);
    RUN_TEST(test_raven_uuid_empty_list);
    RUN_TEST(test_raven_uuid_mixed_list);
    RUN_TEST(test_raven_uuid_null_out_buffer);

    RUN_TEST(test_fw_v11x);
    RUN_TEST(test_fw_v12x);
    RUN_TEST(test_fw_v13x);
    RUN_TEST(test_fw_unknown);
    RUN_TEST(test_fw_v13x_all_flags);

    return UNITY_END();
}

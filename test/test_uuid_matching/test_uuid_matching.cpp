// Unit tests for Raven UUID matching and firmware version estimation.
// Runs on the host with: pio test -e native
// No hardware or ESP32 toolchain required.

#include <unity.h>
#include "../../fy_detect.h"

// ── Raven UUID matching tests ─────────────────────────────────────────────────

void test_raven_uuid_known_device_info(void) {
    const char* uuids[] = { FY_RAVEN_DEVICE_INFO };
    char out[41] = {0};
    TEST_ASSERT_TRUE(fyCheckRavenUUIDFromStrings(uuids, 1, out));
    TEST_ASSERT_EQUAL_STRING(FY_RAVEN_DEVICE_INFO, out);
}

void test_raven_uuid_all_known(void) {
    for (size_t i = 0; i < FY_RAVEN_UUID_COUNT; i++) {
        const char* uuids[] = { fy_raven_uuids[i] };
        TEST_ASSERT_TRUE_MESSAGE(
            fyCheckRavenUUIDFromStrings(uuids, 1, nullptr),
            fy_raven_uuids[i]);
    }
}

void test_raven_uuid_case_insensitive(void) {
    const char* uuids[] = { "0000180A-0000-1000-8000-00805F9B34FB" };
    TEST_ASSERT_TRUE(fyCheckRavenUUIDFromStrings(uuids, 1, nullptr));
}

void test_raven_uuid_no_match(void) {
    const char* uuids[] = { "12345678-1234-1234-1234-123456789abc" };
    TEST_ASSERT_FALSE(fyCheckRavenUUIDFromStrings(uuids, 1, nullptr));
}

void test_raven_uuid_null_list(void) {
    TEST_ASSERT_FALSE(fyCheckRavenUUIDFromStrings(nullptr, 0, nullptr));
}

void test_raven_uuid_empty_list(void) {
    const char* uuids[] = { nullptr };
    TEST_ASSERT_FALSE(fyCheckRavenUUIDFromStrings(uuids, 0, nullptr));
}

void test_raven_uuid_mixed_list_finds_known(void) {
    // First entry unknown, second known — should still match
    const char* uuids[] = {
        "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        FY_RAVEN_GPS
    };
    char out[41] = {0};
    TEST_ASSERT_TRUE(fyCheckRavenUUIDFromStrings(uuids, 2, out));
    TEST_ASSERT_EQUAL_STRING(FY_RAVEN_GPS, out);
}

void test_raven_uuid_null_out_buffer_ok(void) {
    const char* uuids[] = { FY_RAVEN_POWER };
    // Passing nullptr for out_uuid must not crash
    TEST_ASSERT_TRUE(fyCheckRavenUUIDFromStrings(uuids, 1, nullptr));
}

void test_raven_old_style_short_uuids_do_not_match(void) {
    // Verify the old short-form UUIDs (used in pre-PR#39 firmware) do NOT
    // match the full 128-bit UUID strings — they are different representations.
    const char* old_short[] = {
        "1b7e",  // was RAVEN_SVC_UUID_PRIMARY
        "fd60"   // was RAVEN_SVC_UUID_TELEM
    };
    TEST_ASSERT_FALSE(fyCheckRavenUUIDFromStrings(old_short, 2, nullptr));
}

// ── Raven firmware version estimation tests ───────────────────────────────────

void test_fw_v11x(void) {
    // Old location service only — firmware 1.1.x
    TEST_ASSERT_EQUAL_STRING("1.1.x", fyEstimateRavenFW(false, true, false));
}

void test_fw_v12x(void) {
    // New GPS present, no power service — firmware 1.2.x
    TEST_ASSERT_EQUAL_STRING("1.2.x", fyEstimateRavenFW(true, false, false));
}

void test_fw_v13x(void) {
    // New GPS + power present — firmware 1.3.x
    TEST_ASSERT_EQUAL_STRING("1.3.x", fyEstimateRavenFW(true, false, true));
}

void test_fw_v13x_all_flags(void) {
    // All flags set — new GPS+power wins over old location
    TEST_ASSERT_EQUAL_STRING("1.3.x", fyEstimateRavenFW(true, true, true));
}

void test_fw_unknown(void) {
    // No identifying service categories
    TEST_ASSERT_EQUAL_STRING("?", fyEstimateRavenFW(false, false, false));
}

void test_fw_only_old_health(void) {
    // Only old health service — no GPS or power — unknown version
    TEST_ASSERT_EQUAL_STRING("?", fyEstimateRavenFW(false, false, false));
}

// ── UUID count sanity ─────────────────────────────────────────────────────────

void test_raven_uuid_count(void) {
    // Must have exactly 8 known Raven service UUIDs
    TEST_ASSERT_EQUAL(8u, (unsigned)FY_RAVEN_UUID_COUNT);
}

// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_raven_uuid_known_device_info);
    RUN_TEST(test_raven_uuid_all_known);
    RUN_TEST(test_raven_uuid_case_insensitive);
    RUN_TEST(test_raven_uuid_no_match);
    RUN_TEST(test_raven_uuid_null_list);
    RUN_TEST(test_raven_uuid_empty_list);
    RUN_TEST(test_raven_uuid_mixed_list_finds_known);
    RUN_TEST(test_raven_uuid_null_out_buffer_ok);
    RUN_TEST(test_raven_old_style_short_uuids_do_not_match);

    RUN_TEST(test_fw_v11x);
    RUN_TEST(test_fw_v12x);
    RUN_TEST(test_fw_v13x);
    RUN_TEST(test_fw_v13x_all_flags);
    RUN_TEST(test_fw_unknown);
    RUN_TEST(test_fw_only_old_health);

    RUN_TEST(test_raven_uuid_count);

    return UNITY_END();
}

// Unit tests for MAC, device name, and manufacturer ID detection logic.

#include <unity.h>
#include "../../src/fy_detect.h"

// --- Flock MAC prefix tests ---

void test_flock_mac_known_prefix(void) {
    TEST_ASSERT_TRUE(checkFlockMAC("58:8e:81:aa:bb:cc"));
    TEST_ASSERT_TRUE(checkFlockMAC("b4:1e:52:00:11:22"));
    TEST_ASSERT_TRUE(checkFlockMAC("70:c9:4e:de:ad:01"));
}

void test_flock_mac_case_insensitive(void) {
    TEST_ASSERT_TRUE(checkFlockMAC("58:8E:81:AA:BB:CC"));
    TEST_ASSERT_TRUE(checkFlockMAC("B4:1E:52:00:11:22"));
}

void test_flock_mac_all_prefixes(void) {
    size_t count = sizeof(flock_mac_prefixes) / sizeof(flock_mac_prefixes[0]);
    for (size_t i = 0; i < count; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%s:00:00:00", flock_mac_prefixes[i]);
        TEST_ASSERT_TRUE_MESSAGE(checkFlockMAC(mac), flock_mac_prefixes[i]);
    }
}

void test_flock_mac_no_match(void) {
    TEST_ASSERT_FALSE(checkFlockMAC("aa:bb:cc:dd:ee:ff"));
    TEST_ASSERT_FALSE(checkFlockMAC("00:00:00:00:00:00"));
}

// --- Contract manufacturer MAC tests ---

void test_mfr_mac_known(void) {
    TEST_ASSERT_TRUE(checkFlockMfrMAC("f4:6a:dd:11:22:33"));
    TEST_ASSERT_TRUE(checkFlockMfrMAC("e8:d0:fc:aa:bb:cc"));
}

void test_mfr_mac_no_match(void) {
    TEST_ASSERT_FALSE(checkFlockMfrMAC("aa:bb:cc:dd:ee:ff"));
}

void test_mfr_mac_not_in_flock_list(void) {
    // Contract mfr MACs should NOT match the high-confidence Flock list
    TEST_ASSERT_FALSE(checkFlockMAC("f4:6a:dd:11:22:33"));
}

// --- SoundThinking MAC tests ---

void test_soundthinking_mac_known(void) {
    TEST_ASSERT_TRUE(checkSoundThinkingMAC("d4:11:d6:aa:bb:cc"));
}

void test_soundthinking_mac_case_insensitive(void) {
    TEST_ASSERT_TRUE(checkSoundThinkingMAC("D4:11:D6:AA:BB:CC"));
}

void test_soundthinking_mac_no_match(void) {
    TEST_ASSERT_FALSE(checkSoundThinkingMAC("aa:bb:cc:dd:ee:ff"));
}

// --- Device name tests ---

void test_device_name_exact(void) {
    TEST_ASSERT_TRUE(checkDeviceName("FS Ext Battery"));
    TEST_ASSERT_TRUE(checkDeviceName("Penguin"));
    TEST_ASSERT_TRUE(checkDeviceName("Flock"));
    TEST_ASSERT_TRUE(checkDeviceName("Pigvision"));
}

void test_device_name_substring(void) {
    TEST_ASSERT_TRUE(checkDeviceName("My Flock Device"));
    TEST_ASSERT_TRUE(checkDeviceName("Pigvision Controller v2"));
}

void test_device_name_case_insensitive(void) {
    TEST_ASSERT_TRUE(checkDeviceName("penguin"));
    TEST_ASSERT_TRUE(checkDeviceName("FLOCK"));
    TEST_ASSERT_TRUE(checkDeviceName("fs ext battery"));
}

void test_device_name_no_match(void) {
    TEST_ASSERT_FALSE(checkDeviceName("Random BLE Device"));
    TEST_ASSERT_FALSE(checkDeviceName("iPhone"));
}

void test_device_name_null_and_empty(void) {
    TEST_ASSERT_FALSE(checkDeviceName(NULL));
    TEST_ASSERT_FALSE(checkDeviceName(""));
}

// --- Manufacturer ID tests ---

void test_manufacturer_id_known(void) {
    TEST_ASSERT_TRUE(checkManufacturerID(0x09C8));
}

void test_manufacturer_id_no_match(void) {
    TEST_ASSERT_FALSE(checkManufacturerID(0x0000));
    TEST_ASSERT_FALSE(checkManufacturerID(0xFFFF));
    TEST_ASSERT_FALSE(checkManufacturerID(0x004C));  // Apple
}

// --- List isolation tests ---

void test_lists_are_independent(void) {
    // SoundThinking MAC should not match Flock or contract mfr lists
    TEST_ASSERT_FALSE(checkFlockMAC("d4:11:d6:aa:bb:cc"));
    TEST_ASSERT_FALSE(checkFlockMfrMAC("d4:11:d6:aa:bb:cc"));
    // Flock MAC should not match SoundThinking
    TEST_ASSERT_FALSE(checkSoundThinkingMAC("58:8e:81:aa:bb:cc"));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_flock_mac_known_prefix);
    RUN_TEST(test_flock_mac_case_insensitive);
    RUN_TEST(test_flock_mac_all_prefixes);
    RUN_TEST(test_flock_mac_no_match);

    RUN_TEST(test_mfr_mac_known);
    RUN_TEST(test_mfr_mac_no_match);
    RUN_TEST(test_mfr_mac_not_in_flock_list);

    RUN_TEST(test_soundthinking_mac_known);
    RUN_TEST(test_soundthinking_mac_case_insensitive);
    RUN_TEST(test_soundthinking_mac_no_match);

    RUN_TEST(test_device_name_exact);
    RUN_TEST(test_device_name_substring);
    RUN_TEST(test_device_name_case_insensitive);
    RUN_TEST(test_device_name_no_match);
    RUN_TEST(test_device_name_null_and_empty);

    RUN_TEST(test_manufacturer_id_known);
    RUN_TEST(test_manufacturer_id_no_match);

    RUN_TEST(test_lists_are_independent);

    return UNITY_END();
}

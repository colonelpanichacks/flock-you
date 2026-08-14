// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// ble_selftest.h — optional on-device BLE self-test (BLE_SELF_TEST=1 only).
//
// WHY THIS EXISTS:
// Validating the BLE-only detection pipeline end-to-end (real NimBLE
// advertisement parsing -> fyProcessBLEAdvertisedDevice() matching ->
// enqueueAlert() -> drainAlertQueue() -> LED/chirp/JSON) normally requires a
// SECOND physical BLE transmitter (e.g. beacon_test.cpp on a separate board)
// broadcasting one of the three Flock BLE signatures. When only one board
// is available, this lets that SAME board briefly advertise each signature
// itself; its own continuous NimBLE scan (BLE_COEX_MODE) picks the
// advertisement back up, exercising the exact real parsing/matching code a
// genuine external Flock BLE signal would.
//
// ROOT CAUSE OF THE FREEZE (found via on-device testing, v1 + v2 both hung):
// bleSelfTestFire() used to call `g_pBLEScan->start(N, false)` to run a
// short dedicated scan window overlapping the advertising burst. With an
// int + bool argument list, that call resolves to NimBLEScan's *blocking*
// overload — `NimBLEScanResults start(uint32_t duration, bool is_continue)`
// — NOT the async duration-limited one. The blocking overload parks the
// CALLING task (main loop!) in `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` and
// only returns if something calls `stop()` (which gives the task
// notification) or the internal duration-complete GAP event does so. Our
// own later `stop()` call was racing the scan's own duration timer, and in
// practice the notify was never delivered — main loop hung forever, with
// zero further serial output, exactly matching what was observed on
// hardware (one "BLE-Flock rssi=..." line from the BLE host task, then
// silence).
//
// THE FIX: don't touch the scanner at all. BLE_COEX_MODE already keeps a
// continuous (duration=0, non-blocking) scan running at all times via
// bleCoexStart()/bleScanTick() in main.cpp — it already proved (in the very
// first test) that it catches our own self-advertised packet perfectly
// fine while running concurrently. bleSelfTestFire() now ONLY starts/stops
// *advertising*; the always-on scan does the rest, identically to how it
// would pick up a real external Flock BLE device.
//
// MAC ROTATION: real Flock BLE beacons aren't expected to rotate their
// address, but a fixed self-test address made every capture look identical
// and made it hard to tell scenarios apart in logs. Each burst now
// generates a fresh random static BLE address (host-stack "static random"
// address, distinct from the chip's real public address) before
// advertising, and restores the public address type afterward so the
// device's real identity is unaffected everywhere else (WiFi MAC, BLE
// scanning, etc. are untouched by this).
//
// Enable via: -DBLE_SELF_TEST=1  (see platformio.ini's *-selftest envs)
// NEVER define this in a production build -- the device would self-report
// fake "detections" it generated itself, defeating the point of a detector.

#pragma once

#if defined(BLE_SELF_TEST) && BLE_SELF_TEST

// Same conditional include path NimBLE-Arduino's own headers use (see
// NimBLEScan.h) — the plain "host/ble_hs_id.h" form only resolves when
// building against ESP-IDF's native NimBLE component (CONFIG_NIMBLE_CPP_IDF),
// which arduino-esp32 framework builds don't define.
#if defined(CONFIG_NIMBLE_CPP_IDF)
#include "host/ble_hs_id.h"
#else
#include "nimble/nimble/host/include/host/ble_hs_id.h"
#endif


#define BLE_SELFTEST_INTERVAL_MS 15000UL   // gap between scenarios
#define BLE_SELFTEST_ADV_MS       1500UL   // how long each burst advertises
#define BLE_SELFTEST_FIRST_MS      5000UL  // delay after boot before scenario 0

static uint8_t       bleSelfTestScenario = 0;
static unsigned long bleSelfTestNextAt   = 0;

// Generates and applies a fresh random static BLE address so each self-test
// burst doesn't look like the exact same device every time. Best-effort:
// if address generation ever fails, silently keeps whatever address is
// already set (advertising with the real public address still exercises
// the same match/alert code path).
//
// DIAGNOSTIC LOGGING added while root-causing "advertising never received
// by either the device's own scan or a second board" -- ble_gap_adv_start()
// requires a valid static-random address to ALREADY be registered via
// ble_hs_id_set_rnd() before advertising with BLE_OWN_ADDR_RANDOM; if
// ble_hs_id_gen_rnd() ever fails, this logs it instead of failing silently.
static void bleSelfTestRandomizeAddr() {
  ble_addr_t addr;
  int rc = ble_hs_id_gen_rnd(0 /* static random, not NRPA */, &addr);
  if (rc == 0) {
    int rc2 = ble_hs_id_set_rnd(addr.val);
    if (rc2 == 0) {
      NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    } else {
      Serial.printf("[flockyou] BLE self-test: ble_hs_id_set_rnd FAILED rc=%d -- "
                    "keeping public address\n", rc2);
    }
  } else {
    Serial.printf("[flockyou] BLE self-test: ble_hs_id_gen_rnd FAILED rc=%d -- "
                  "keeping public address\n", rc);
  }
}

static void bleSelfTestRestoreAddr() {
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
}

// Broadcasts scenario N (0=mfr-ID, 1=Raven UUID, 2=name) for
// BLE_SELFTEST_ADV_MS, then stops advertising.
//
// SCAN PAUSE/RESUME (root-caused via the ble_hs_id_set_rnd() diagnostic
// logging above): the BLE controller rejects HCI_LE_Set_Random_Address
// with "Command Disallowed" (HCI 0x0C, surfaces here as NimBLE rc=524)
// while a scan is active. Since BLE_COEX_MODE keeps a scan running
// continuously (see bleCoexStart() in main.cpp), address randomization
// silently failed on every burst and the self-test always fell back to
// the fixed public address. Briefly stopping the scan just for the
// randomize+advertise window (then restarting it with the same
// overload-safe async call bleCoexStart() uses) fixes this without
// giving up continuous scanning for more than this single burst's
// duration -- self-test builds are never used as real detectors anyway
// (see file-level warning above), so this brief pause is harmless here.
static void bleSelfTestFire(NimBLEAdvertising* adv, uint8_t scenario) {
  if (!adv) return;
  NimBLEAdvertisementData data;
  const char* label = "?";

  bool wasScanning = g_pBLEScan && g_pBLEScan->isScanning();
  if (wasScanning) g_pBLEScan->stop();


  switch (scenario) {
    case 0: {
      uint16_t id = BLE_FLOCK_MFR_ID;
      uint8_t mfr[3] = { (uint8_t)(id & 0xFF), (uint8_t)(id >> 8), 0x00 };
      data.setManufacturerData(std::string((char*)mfr, sizeof(mfr)));
      label = "mfr-ID";
      break;
    }
    case 1:
      data.setCompleteServices(NimBLEUUID(fy_raven_uuids[0]));
      label = "Raven-UUID";
      break;
    default:
      data.setName("Flock-SelfTest");
      label = "name";
      break;
  }

  bleSelfTestRandomizeAddr();

  Serial.printf("[flockyou] BLE self-test: advertising %s scenario (addr=%s)...\n",
                label, NimBLEDevice::getAddress().toString().c_str());

  adv->setAdvertisementData(data);
  bool started = adv->start();
  if (!started) {
    Serial.println("[flockyou] BLE self-test: adv->start() FAILED -- "
                    "nothing was actually transmitted this burst!");
  }
  delay(BLE_SELFTEST_ADV_MS);
  adv->stop();

  bleSelfTestRestoreAddr();

  if (wasScanning) {
    // Same overload-resolution fix as bleCoexStart()/bleScanTick() in
    // main.cpp: an explicit typed null callback forces NimBLEScan::start()
    // to resolve to the async (non-blocking) overload instead of the
    // blocking one.
    g_pBLEScan->clearResults();
    g_pBLEScan->start(0, (void (*)(NimBLEScanResults))nullptr);
  }
}


// Call once from setup(), after initBLE().
static void bleSelfTestInit() {
  Serial.println("[flockyou] *** BLE SELF-TEST BUILD *** -- this device will "
                  "periodically advertise fake Flock BLE signals to itself. "
                  "DO NOT use this build as a real detector.");
  bleSelfTestNextAt = millis() + BLE_SELFTEST_FIRST_MS;
}

// Call every loop() iteration.
static void bleSelfTestTick(NimBLEAdvertising* adv) {
  if (millis() < bleSelfTestNextAt) return;
  bleSelfTestFire(adv, bleSelfTestScenario);
  bleSelfTestScenario = (bleSelfTestScenario + 1) % 3;
  bleSelfTestNextAt = millis() + BLE_SELFTEST_INTERVAL_MS;
}

#endif // BLE_SELF_TEST

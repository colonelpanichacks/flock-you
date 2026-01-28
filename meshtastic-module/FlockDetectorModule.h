// ============================================================================
// FLOCK DETECTOR MODULE FOR MESHTASTIC
// ============================================================================
// Surveillance camera detection module that scans WiFi/BLE for:
// - Flock Safety ALPR cameras
// - Raven acoustic gunshot detectors (SoundThinking/ShotSpotter)
// - Penguin surveillance devices
// - Pigvision systems
//
// Detections are broadcast as text messages on the mesh.
// ============================================================================

#pragma once

#include "configuration.h"

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_FLOCKDETECTOR

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"
#include <NimBLEDevice.h>

// Detection types
enum FlockDetectionType {
    DETECT_FLOCK_WIFI = 1,
    DETECT_FLOCK_BLE = 2,
    DETECT_RAVEN = 3,
    DETECT_PENGUIN = 4,
    DETECT_PIGVISION = 5
};

// Detection record
struct FlockDetection {
    FlockDetectionType type;
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
    uint32_t timestamp;
    bool reported;
};

class FlockDetectorModule : public SinglePortModule, public concurrency::OSThread
{
  public:
    FlockDetectorModule();

    // Send detection alert to mesh
    void sendDetectionAlert(FlockDetection *detection);

  protected:
    // OSThread interface
    virtual int32_t runOnce() override;

    // Module interface
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override { return false; }
    virtual meshtastic_MeshPacket *allocReply() override { return nullptr; }

  private:
    // WiFi scanning
    void initWifiScanner();
    void hopChannel();
    static void wifiPacketHandler(void *buff, wifi_promiscuous_pkt_type_t type);

    // BLE scanning
    void initBleScanner();
    void runBleScan();

    // Pattern matching
    static bool checkMacPrefix(const uint8_t *mac);
    static bool checkSsidPattern(const char *ssid);
    static bool checkDeviceNamePattern(const char *name);
    static bool checkRavenServiceUuid(NimBLEAdvertisedDevice *device);

    // Detection queue
    void queueDetection(FlockDetectionType type, const uint8_t *mac, int8_t rssi, const char *name);
    void processDetectionQueue();

    // State
    uint8_t currentChannel = 1;
    uint32_t lastChannelHop = 0;
    uint32_t lastBleScan = 0;
    uint32_t lastReport = 0;
    bool wifiInitialized = false;
    bool bleInitialized = false;

    // Detection queue (circular buffer)
    static const size_t QUEUE_SIZE = 16;
    FlockDetection detectionQueue[QUEUE_SIZE];
    size_t queueHead = 0;
    size_t queueTail = 0;

    // Deduplication - don't spam same device
    static const size_t SEEN_SIZE = 32;
    uint8_t seenMacs[SEEN_SIZE][6];
    uint32_t seenTimestamps[SEEN_SIZE];
    size_t seenIndex = 0;
    bool wasRecentlySeen(const uint8_t *mac);
    void markAsSeen(const uint8_t *mac);
};

extern FlockDetectorModule *flockDetectorModule;

#endif // ARCH_ESP32 && !MESHTASTIC_EXCLUDE_FLOCKDETECTOR

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// beacon_frames.h — minimal raw 802.11 management-frame builder.
//
// Used by beacon_test.cpp (the "beacon tester" / detection-test firmware)
// to construct Beacon / Probe Request / Probe Response frames byte-for-byte
// and transmit them with esp_wifi_80211_tx(). This is intentionally a
// separate, tiny, dependency-free header (no fy_detect.h / Arduino types)
// so it is easy to reuse or unit-test independently of the OUI/BLE pattern
// data that lives in fy_detect.h.
//
// NOTES:
//   - No FCS is written to the buffer; esp_wifi_80211_tx() has the radio
//     hardware append the FCS automatically (per ESP-IDF docs).
//   - Sequence-control is auto-incremented per frame but receivers don't
//     validate it, so it's only cosmetic/traceable, never a correctness
//     requirement.
//   - Only Beacon / Probe Request / Probe Response subtypes are supported —
//     these are the well-established, universally-supported subtypes for
//     esp_wifi_80211_tx() raw injection. Data/Null frames are intentionally
//     NOT supported here (unclear/inconsistent support when STA isn't
//     associated on stock ESP-IDF).

#pragma once

#include <stdint.h>
#include <string.h>

#define BF_MAX_FRAME 160

// Frame Control field values (little-endian uint16_t on this platform —
// ESP32/Xtensa is little-endian, matching main.cpp's own bit-shift parsing
// of the same field: fc0=frame_ctrl&0xFF; ftype=(fc0>>2)&3; subtype=(fc0>>4)&0xF).
#define BF_FC_BEACON     0x0080   // type=0 (mgmt), subtype=8  (Beacon)
#define BF_FC_PROBE_REQ  0x0040   // type=0 (mgmt), subtype=4  (Probe Request)
#define BF_FC_PROBE_RESP 0x0050   // type=0 (mgmt), subtype=5  (Probe Response)

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];   // receiver / destination
  uint8_t  addr2[6];   // transmitter / source
  uint8_t  addr3[6];   // BSSID
  uint16_t seq_ctrl;
} bf_80211_hdr_t;

static uint16_t bf_seq_counter = 0;

// Writes the 24-byte MAC header. Returns bytes written (sizeof header).
static size_t bfWriteHeader(uint8_t* buf, uint16_t frameCtrl,
                             const uint8_t* addr1, const uint8_t* addr2,
                             const uint8_t* addr3) {
  bf_80211_hdr_t* hdr = (bf_80211_hdr_t*)buf;
  hdr->frame_ctrl = frameCtrl;
  hdr->duration   = 0;
  memcpy(hdr->addr1, addr1, 6);
  memcpy(hdr->addr2, addr2, 6);
  memcpy(hdr->addr3, addr3, 6);
  hdr->seq_ctrl = (uint16_t)(bf_seq_counter++ << 4);
  return sizeof(bf_80211_hdr_t);
}

// Appends a generic Information Element {id, len, data...}. Returns new offset.
static size_t bfAppendIE(uint8_t* buf, size_t off, uint8_t id,
                          const uint8_t* data, uint8_t len) {
  buf[off++] = id;
  buf[off++] = len;
  if (len && data) { memcpy(buf + off, data, len); off += len; }
  return off;
}

// SSID IE (id=0). ssid==nullptr or "" -> zero-length ("wildcard") SSID IE,
// exactly what main.cpp's isWildcardProbeIE() looks for in probe requests.
static size_t bfAppendSSID(uint8_t* buf, size_t off, const char* ssid) {
  uint8_t len = ssid ? (uint8_t)strnlen(ssid, 32) : 0;
  return bfAppendIE(buf, off, 0x00, (const uint8_t*)ssid, len);
}

// Supported Rates IE (id=1) — minimal 802.11b basic-rate set.
static size_t bfAppendRates(uint8_t* buf, size_t off) {
  static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96 };
  return bfAppendIE(buf, off, 0x01, rates, sizeof(rates));
}

// DS Parameter Set IE (id=3) — current channel number.
static size_t bfAppendDSParam(uint8_t* buf, size_t off, uint8_t channel) {
  return bfAppendIE(buf, off, 0x03, &channel, 1);
}

// Builds a Beacon or Probe-Response frame:
//   24-byte hdr + 12-byte fixed params (timestamp/interval/capability) +
//   SSID IE + Rates IE + DS-Param IE.
// This exactly matches the offset main.cpp's wifiSniffer() expects for
// subtype 8 (Beacon) / 5 (Probe Response): SSID IE starts at hdr+12.
static size_t bfBuildBeaconLike(uint8_t* buf, uint16_t frameCtrl,
                                 const uint8_t* addr1, const uint8_t* addr2,
                                 const uint8_t* addr3, const char* ssid,
                                 uint8_t channel) {
  size_t off = bfWriteHeader(buf, frameCtrl, addr1, addr2, addr3);
  memset(buf + off, 0, 8); off += 8;      // timestamp (8 bytes, value irrelevant)
  buf[off++] = 0x64; buf[off++] = 0x00;   // beacon interval = 100 TU
  buf[off++] = 0x01; buf[off++] = 0x00;   // capability info (ESS bit set)
  off = bfAppendSSID(buf, off, ssid);
  off = bfAppendRates(buf, off);
  off = bfAppendDSParam(buf, off, channel);
  return off;
}

// Builds a Probe Request frame: 24-byte hdr + SSID IE + Rates IE directly
// (no fixed params) — matches main.cpp's subtype==4 parsing (IEs start
// immediately at hdr+0... i.e. right after the 24-byte header).
static size_t bfBuildProbeRequest(uint8_t* buf, const uint8_t* addr1,
                                   const uint8_t* addr2, const uint8_t* addr3,
                                   const char* ssid) {
  size_t off = bfWriteHeader(buf, BF_FC_PROBE_REQ, addr1, addr2, addr3);
  off = bfAppendSSID(buf, off, ssid);
  off = bfAppendRates(buf, off);
  return off;
}

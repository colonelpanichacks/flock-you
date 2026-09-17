# Firmware-Derived Flock Safety Signatures

**Provenance:** Extracted from Flock Safety ALPR camera firmware dump (Qualcomm MSM8953 + QCA9377, Android 8.1, codename `hpnotiq`), analyzed 2026-09-16.

Every signature below comes directly out of that firmware image — default MACs baked into modem/NVRAM binaries, strings and constants found in decompiled services and config files, GATT service definitions, and Bluetooth stack configuration. This is the **only** detection set used by the `dev-firmware-detections` version; the community OUI list in [`NitekryDPaul_wifi_ouis.md`](NitekryDPaul_wifi_ouis.md) is **not** active on this branch.

---

## WiFi

### OUIs

| OUI | Vendor | Internal source |
|---|---|---|
| `b4:1e:52` | Flock Safety | Flock Safety's own IEEE-registered OUI (MA-L assignment, Atlanta HQ) — the camera's real corporate prefix |
| `00:03:7f` | Qualcomm Atheros | Default MACs hard-coded in the firmware's radio blobs: `00:03:7f:50:00:01` in `bdwlan30.bin` / `fakeboar.bin`, `00:03:7f:4f:00:16` in `otp30.bin` |

The on-board radio is a **Qualcomm QCA9377**. The cameras emit **broadcast probe requests at roughly 125 ms intervals, channel-hopping**, originating from Qualcomm's LOWI geolocation-scanning stack — so probe-request traffic from these OUIs at that cadence is itself a strong indicator even without an SSID.

### SSID patterns

| Pattern | Meaning | Internal source |
|---|---|---|
| `Flock-XXXXXX` | SoftAP broadcast by the camera | Built in `WifiApService.java` as the literal string `"Flock-"` + last 6 characters of the WiFi MAC; WPA2 password is the literal `"security"` |
| `Flock` | Bare SSID | Seen on provisioned units |

---

## BLE / Bluetooth Low Energy

### Penguin battery-pack advertisements

| Signature | Internal source |
|---|---|
| Complete local name `Penguin-NNNNNNNNNN` (exactly 10 digits) | Penguin external battery pack BLE advertising data |
| Complete local name that is a bare 10-digit number | Same pack, alternate firmware naming |
| Complete local name `FS Ext Battery` | Same pack, "Flock Safety External Battery" label |
| Manufacturer-specific data, company ID `0x09C8` (XUNTONG), payload embedding serials like `TN72023022000771` | Battery-pack BLE advertisement manufacturer data |

### Flock accessory GATT service

| Item | UUID | Internal source |
|---|---|---|
| Service | `e8ccbb38-9532-46a8-9fe5-1814df172e6f` | Flock accessory GATT definition in firmware |
| Key characteristic | `628913a6-8701-40ff-a3ce-8f453ff0818d` | Same GATT definition |
| Control characteristic | `bb18d1d2-fe71-439f-9529-d4b472d139b5` | Same GATT definition |

### Raven camera GATT services

16-bit service UUIDs **`0x3100`–`0x3500`**, exposed **unauthenticated**. Notably `0x3101` / `0x3102` leak GPS latitude/longitude directly.

---

## Bluetooth Classic

| Signature | Internal source |
|---|---|
| Device name `msm8953_32` | Fallback classic BT name from the Qualcomm MSM8953 platform base |
| Device name `Android` | Persist property `net.bt.name=Android` with no vendor override in the image |
| SDP Device-ID record: vendor ID `0x001D` (Qualcomm), product ID `0x1200` | `bt_did.conf` in the Bluetooth stack configuration |

---

## Notes

- These signatures target the specific hardware/firmware generation in the dump (MSM8953 + QCA9377, Android 8.1). Other Flock hardware generations (e.g. Espressif-based Falcon units covered by the community OUI list) may not match anything here.
- The classic-Bluetooth names are generic Qualcomm/Android defaults, so they are low-specificity on their own — treat them as corroborating signals alongside the SDP Device-ID record, not standalone detections.

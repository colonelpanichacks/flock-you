# Flock Safety WiFi OUIs — Research by @NitekryDPaul

**Researcher:** OrdoOuroborous (**@NitekryDPaul**, GitHub [@nitekry](https://github.com/nitekry))

This dataset documents Flock Safety and related surveillance-infrastructure WiFi MAC-address OUIs (first three octets) discovered through 2.4 GHz promiscuous-mode analysis. The 40 active prefixes below come from @NitekryDPaul's promiscuous-mode research on Flock camera air traffic (39) and DeFlockJoplin's wildcard-probe drive-testing (1). Upstream OUI source: [nitekry/nite-oui-collection](https://github.com/nitekry/nite-oui-collection). Three prefixes from earlier revisions (`f8:a2:d6`, `94:2a:6f`, `f4:e2:c6`) have been demoted — see the [Demoted](#demoted--low-confidence) section.

## Why promiscuous mode

Flock stations spend most of their duty cycle asleep, waking briefly to upload and then sleeping again. During the sleep windows a passive sniffer will never see them transmit — but it can still see them named as the **destination** (addr1) of probe responses and data frames from nearby APs. Matching on addr1 in addition to addr2 therefore reveals devices that a transmitter-only sniff would miss entirely.

This addr1 technique is @NitekryDPaul's discovery and is the basis of the `promiscuis-flock-you` firmware.

## OUI list (40 prefixes, lowercase, colon-separated)

@NitekryDPaul contributed 39 active prefixes — 29 from his original
promiscuous-mode set plus 10 from his April 2026 additions in
[nite-oui-collection](https://github.com/nitekry/nite-oui-collection)
(two of the original 12 April adds — `94:2a:6f` and `f4:e2:c6` — were
subsequently marked as Ubiquiti false positives in his June 2026 update
and have been demoted). The 40th (`82:6b:f2`) was contributed by
**DeFlockJoplin** during follow-up drive-testing in Joplin —
it's the OUI of the 12th camera in his field test, which the original
list didn't catch. See
[DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you).

```
70:c9:4e
3c:91:80
d8:f3:bc
80:30:49
b8:35:32
14:5a:fc
74:4c:a1
08:3a:88
9c:2f:9d
c0:35:32
94:08:53
e4:aa:ea
f4:6a:dd
24:b2:b9
00:f4:8d
d0:39:57
e8:d0:fc
e0:4f:43
b8:1e:a4
70:08:94
58:8e:81
ec:1b:bd
3c:71:bf
58:00:e3
90:35:ea
5c:93:a2
64:6e:69
48:27:ea
a4:cf:12
04:0d:84
f0:82:c0
1c:34:f1
38:5b:44
94:34:69
b4:e3:f9
b4:1e:52
14:b5:cd
d4:11:d6
e0:0a:f6
82:6b:f2
```

## CSV form

| oui | vendor_context | detection_protocol | source |
|---|---|---|---|
| 70:c9:4e | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 3c:91:80 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| d8:f3:bc | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 80:30:49 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| b8:35:32 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 14:5a:fc | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 74:4c:a1 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 08:3a:88 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 9c:2f:9d | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| c0:35:32 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 94:08:53 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| e4:aa:ea | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| f4:6a:dd | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 24:b2:b9 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 00:f4:8d | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| d0:39:57 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| e8:d0:fc | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| e0:4f:43 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| b8:1e:a4 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 70:08:94 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 58:8e:81 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| ec:1b:bd | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 3c:71:bf | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 58:00:e3 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 90:35:ea | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 5c:93:a2 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 64:6e:69 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul |
| 48:27:ea | Flock Safety infrastructure (low confidence — Espressif plausible Flock variant, WiGLE crowdsource) | WiFi 2.4 GHz | @NitekryDPaul |
| a4:cf:12 | Flock Safety infrastructure (low confidence — Espressif plausible Flock variant, WiGLE crowdsource) | WiFi 2.4 GHz | @NitekryDPaul |
| 04:0d:84 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| f0:82:c0 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| 1c:34:f1 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| 38:5b:44 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| 94:34:69 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| b4:e3:f9 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| b4:1e:52 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| 14:b5:cd | Flock Safety infrastructure (new finding, testing) | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| d4:11:d6 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| e0:0a:f6 | Flock Safety infrastructure | WiFi 2.4 GHz | @NitekryDPaul (Apr 2026) |
| 82:6b:f2 | Flock Safety infrastructure | WiFi 2.4 GHz (wildcard probe) | DeFlockJoplin |

## Flagged but still tracked

These prefixes remain in the active firmware OUI array but have a known
caveat documented by @NitekryDPaul.

| Prefix | Caveat | Source |
|---|---|---|
| `08:3a:88` | BLE Ring conflict — expect occasional Ring-doorbell false positives when this OUI is matched against BLE advertisements. WiFi-side detection is unaffected. Flagged per @NitekryDPaul's [my_tested_flock.md](https://github.com/nitekry/nite-oui-collection/blob/main/groups/flockers/my_tested_flock.md). | @NitekryDPaul |
| `48:27:ea` | Low confidence — Espressif plausible Flock variant. Sourced from WiGLE crowdsource per @NitekryDPaul's Crowdsource section in [my_tested_flock.md](https://github.com/nitekry/nite-oui-collection/blob/main/groups/flockers/my_tested_flock.md). Kept in the active OUI array per upstream `main.cpp`. | @NitekryDPaul / Crowdsource |
| `a4:cf:12` | Low confidence — Espressif plausible Flock variant. Same source and status as `48:27:ea`. | @NitekryDPaul / Crowdsource |

## Demoted / low confidence

These prefixes were in earlier revisions of the list but have been removed
from the active firmware OUI array.

| Prefix | Reason | Source |
|---|---|---|
| `f8:a2:d6` | Low confidence; observed hitting a Sony Media Player rather than a Flock device. Demoted per @NitekryDPaul's [my_tested_flock.md](https://github.com/nitekry/nite-oui-collection/blob/main/groups/flockers/my_tested_flock.md) field notes. | @NitekryDPaul |
| `94:2a:6f` | Marked "Nope - Ubiquiti" in @NitekryDPaul's June 2026 [my_tested_flock.md](https://github.com/nitekry/nite-oui-collection/blob/main/groups/flockers/my_tested_flock.md) update. Was previously carried as a high-confidence April 2026 addition; hits on Ubiquiti radios rather than Flock. | @NitekryDPaul |
| `f4:e2:c6` | Marked "Nope - Ubiquiti" in @NitekryDPaul's June 2026 [my_tested_flock.md](https://github.com/nitekry/nite-oui-collection/blob/main/groups/flockers/my_tested_flock.md) update. Was previously carried as a high-confidence April 2026 addition; hits on Ubiquiti radios rather than Flock. | @NitekryDPaul |
| `cc:cc:cc` | No observed hits across @NitekryDPaul's testing. Originally a placeholder/test prefix from the early GainSec list; never produced a real detection. | @NitekryDPaul |
| `00:0c:e7` | MediaTek prefix that appeared in early field tests as a possible Flock-adjacent observation; later determined to be a likely false positive. | @NitekryDPaul / Crowdsource |
| `6c:cd:d6` | Marked "Nope - Netgear" in @NitekryDPaul's April 2026 additions section — never entered our active list; recorded here for completeness. | @NitekryDPaul |

## Detection strategy

For each observed 802.11 management or data frame:

1. Skip if multicast (addr1 byte 0 bit 0 set) — filters broadcasts, 33:33:* groups, etc.
2. Skip if locally-administered (byte 0 bit 1 set) — filters randomised MACs
3. Match `addr2` (transmitter) against the OUI list
4. Match `addr1` (receiver) against the OUI list — **the addr1 insight**
5. Optional: match `addr3` (BSSID) on mgmt frames when addr2 is randomised

### Wildcard-probe tightening (DeFlockJoplin)

DeFlockJoplin observed that Flock cameras channel-hop and spam
wildcard 802.11 Probe Requests on every channel. Combining that with the
OUI match yields a very tight signature:

1. Frame is Management, type=0 subtype=4 (Probe Request)
2. SSID Information Element (tag 0) is present with length 0
3. `addr2` (transmitter) matches the OUI list

Field-tested in Joplin: **11 of 12 cameras caught with only 2 false
positives**. The 12th camera used OUI `82:6b:f2`, which is now in the
list above. Source: [DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you).

## Firmware

The `promiscuis-flock-you` firmware implementing this research is a mod of @NitekryDPaul's promiscuous-mode firmware. It emits Flask-compatible JSON over USB for ingestion by the `flock-you` dashboard and persists detections to on-device SPIFFS.

## Credit

All OUI research and the promiscuous-mode detection strategy are the work of **@NitekryDPaul**. Please cite him when using or redistributing this list.

---

Last synced with [nitekry/nite-oui-collection](https://github.com/nitekry/nite-oui-collection): 2026-07-04.

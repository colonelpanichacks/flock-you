# File Size & Decomposition Rules — flock-you-esp32

`main.cpp` grew large (~2000 lines) before a deliberate decomposition
effort split shared logic into single-purpose headers
(`fy_detect.h`, `fy_confidence.h`, `led_gpio.h`, `led_neopixel.h`,
`ui_task.h`, board-specific `*_display.h` files, `ble_selftest.h`). Keep
that discipline going forward.

## When adding new functionality

- **New, logically-separable functionality gets its own header**, not
  another few hundred lines bolted onto `main.cpp`. Ask: "if I deleted
  this feature entirely, could I remove exactly one `#include` line and
  the files it names, with no scattered remnants?" If not, it isn't
  decomposed enough yet.
- **One responsibility per file.** `beacon_frames.h` builds raw 802.11
  frames and nothing else. `fy_confidence.h` computes confidence scores
  and nothing else. `ble_selftest.h` only self-advertises test BLE
  signatures. Don't let a "detection" header grow WiFi-sniffing logic, or
  a "display" header grow BLE code, etc.
- **Prefer a new file over a new `#if defined(...)` block that spans
  hundreds of lines inside an existing file.** A handful of `#if`/`#endif`
  lines wrapping a short block is fine (that's how board variants are
  selected throughout this codebase); a multi-hundred-line conditional
  region is a sign the feature should be its own header, included
  conditionally instead (see how `ble_selftest.h` and the board
  `*_display.h` files are wired into `main.cpp`).
- **Keep standalone test/tooling firmware genuinely standalone.**
  `beacon_test.cpp` intentionally does NOT share `main.cpp` — it's a
  separate `.cpp` entry point selected via `build_src_filter` in
  `platformio.ini`. Don't merge test-only broadcast code into the
  production detector's call graph; gate it behind a build flag in its
  own header instead (as `ble_selftest.h` does via `BLE_SELF_TEST`).

## When editing existing large files

- If a change to `main.cpp` doesn't have an obvious existing home in one
  of the included headers, that's a signal — either it belongs in a new
  header, or an existing header's scope should expand to explicitly cover
  it (update that header's own top-of-file comment describing its scope
  when you do).
- Don't grow `platformio.ini` environments by copy-pasting a whole
  environment block for a minor variant. Check whether the difference can
  be expressed as one additional `build_flags` entry on a shared pattern
  instead (see how `-DBLE_SELF_TEST=1` layers on top of the same
  BLE_COEX_MODE config as the production `-ble` environments).

## Web flasher (`docs/index.html`) firmware-variant convention

Every PlatformIO environment that represents a genuinely distinct,
user-flashable firmware is built in CI (`.github/workflows/build-firmware.yml`)
and exposed in the web flasher, not just the recommended default. Concretely:

- **All 18 non-experimental environments** (every `-ble` and non-`-ble` pair
  for each board, plus `m5atom-lite-ble-selftest` and `m5atom-lite-beacon`)
  are built and required in CI's "Verify build artifacts" step; the two
  experimental `lilygo-t-dongle-c5[-ble]` environments are built in a
  separate `continue-on-error: true` step and are advisory-only.
- In `docs/index.html`'s `BOARDS` object, each board's `normal` entry is
  always its `-ble` build (BLE-vs-no-BLE is a real, meaningful compile-time
  difference in this project — see `fy_detect.h`/`main.cpp`'s
  `ENABLE_BLE_SCAN`/`BLE_COEX_MODE` gating). Every board also gets a
  `noBleVariant()` entry in its `extra[]` array so the plain WiFi-only build
  remains selectable for anyone who wants the smaller/simpler firmware.
  `m5atom-lite` additionally exposes the `m5atom-lite-ble-selftest` and
  `m5atom-lite-beacon` test-tool firmwares in the same `extra[]` array.
  `renderFirmwareVariants()` in `docs/index.html`'s JS already generically
  iterates `extra[]`, so adding a new variant to a board only requires a
  data change, not new JS logic.
- **This is NOT mirrored 1:1 in the sibling `eye-spy` project.** That
  repo's `-ble`-suffixed environments are confirmed vestigial (its
  `ENABLE_BLE_SCAN`/`BLE_COEX_MODE` flags don't gate any code — BLE
  scanning always runs unconditionally there), so exposing both as if they
  were a meaningful choice would be misleading. eye-spy's web flasher
  instead just points each of the 3 boards that have a `-ble` env
  (`m5stack-basic`, `m5stack-core2-aws`, `m5stickc-plus-se`) at that env's
  manifest as its *only* option, satisfying "default to BLE" without a
  fake duplicate choice. If eye-spy ever wires up a real BLE-coexistence
  mode that its build flags actually gate, this discrepancy should be
  revisited.


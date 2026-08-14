# Test-Before-Commit Rules — flock-you-esp32

This project is embedded firmware for a passive Flock Safety ALPR/camera
detector. A change that "compiles" is not a change that "works" — this
codebase has repeatedly shipped bugs (blocking calls masquerading as
async, radio coexistence hangs, silently-ignored return values) that only
a real build + real hardware test would have caught. Follow this workflow
before considering any firmware change complete.

## Minimum bar before calling a fix "done"

1. **Build-verify, not just edit-and-hope.** Use the platformio-mcp
   `build_project` tool (or `pio run -e <env>`) for every environment your
   change plausibly affects. If you touch shared code (`main.cpp`,
   `fy_detect.h`, `fy_confidence.h`, `led_*.h`, `ui_task.h`), that means
   ALL `-ble` (and non-`-ble`) environments in `platformio.ini`, not just
   the one you're actively testing on. A change that only compiles for one
   board/config can silently break another (different NimBLE version,
   different display header, etc.).
2. **Never trust a build-cache hit after editing source.** If a build
   reports `cacheHit: true` immediately after you edited a file it should
   depend on, treat that as suspicious — force a clean rebuild
   (`clean_project` then `build_project`) before trusting the result.
3. **Hardware-verify claims about runtime behavior.** "This should fix the
   freeze" or "this should randomize the MAC" are hypotheses, not facts,
   until confirmed via an actual serial capture from a real board. Don't
   mark a fix complete based on code review alone if the bug was
   originally reported from live hardware behavior.
4. **Prefer two-board (cross-device) tests over same-device loopback for
   anything involving a radio.** Self-transmit/self-receive on a single
   BLE/WiFi radio has real hardware limitations (e.g. a device cannot
   always receive its own transmission) that can produce false negatives
   unrelated to the actual bug. If two boards are available, use one as
   transmitter (`ble_selftest.h` build or `beacon_test.cpp`) and the other
   as the real detector (a `-ble` production environment) and confirm the
   detection pipeline end-to-end: raw radio event → match → `enqueueAlert`
   → `drainAlertQueue` → LED/chirp/JSON/SPIFFS session save.
5. **Capture serial output reliably.** `pio device monitor` cannot run
   non-interactively in this environment. Use the PlatformIO virtualenv's
   `pyserial` directly with DTR/RTS both explicitly forced `false` before
   AND after `.open()` (auto-reset boards can otherwise reset unexpectedly
   mid-capture). Read captured logs with `strings`/`cat -v`, not plain
   `grep`, since the ROM bootloader's 74880-baud banner mixed into a
   115200-baud capture can make tools misdetect the file as binary.
6. **Look for silence, not just errors.** A hang/freeze often produces NO
   error message at all — the last clean log line simply never gets a
   successor. When diagnosing a suspected hang, always capture the FULL
   boot sequence (from the ROM bootloader banner onward) and check where
   printed output actually stops, not just whether a build reported errors.
7. **Don't declare victory on a single successful run.** Radio-based tests
   are inherently a little noisy (timing, RF conditions). Repeat the test
   at least once, or capture a long-enough window, to make sure the fix is
   robust and not a lucky first pass.

## Before committing

- Re-run `git status`/`git diff --stat` and confirm every changed file is
  intentional — no stray debug prints, no leftover experiment files
  committed by accident.
- Write a commit message that states the *root cause*, not just the
  symptom, especially for hard-to-find bugs — see `git log` for examples
  of this project's existing style (e.g. the `NimBLEScan::start()`
  overload-resolution fix). Future debugging sessions should be able to
  `git blame`/`git log -p` their way to the "why" without re-deriving it.

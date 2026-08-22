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
   - **On ESP32-S3 native-USB-Serial/JTAG boards, `pyserial`'s own
     `Serial()` construction/`.open()` reliably triggers a chip reset
     (`rst:0x15 (USB_UART_CHIP_RESET)`) even when `dtr=False; rts=False`
     are set immediately after construction** — the OS-level control-line
     assertion happens transiently during the open() syscall itself, before
     Python code can react. Confirmed via a controlled A/B test: a plain
     `stty -f <port> 115200 && cat <port>` session produced zero reset
     banners over a 12s window, while repeated `pyserial` reopens on the
     same port produced an `rst:0x15` boot banner on every single reopen.
     A naive "reopen on error and keep reading" loop with `pyserial` will
     therefore look exactly like a firmware crash-loop even when the
     firmware is completely healthy. Always check the ROM banner's
     reset-reason code before concluding a hang/crash-loop is real:
     `rst:0x15 (USB_UART_CHIP_RESET)`/other host-triggered codes mean a
     *tool* reset the board; panic/watchdog/brownout codes
     (`RTC_SW_CPU_RST`, `TG0WDT_SYS_RST`, `RTC_BROWN_OUT_RESET`, etc.) mean
     the firmware actually reset itself. Prefer the PlatformIO MCP's
     `start_monitor`/`query_logs` tools (which shell out to `pio device
     monitor` and do not exhibit this reset-on-attach behavior) for
     passive "is it currently healthy" checks, and reserve manual
     `pyserial` reopen-loops for when you specifically want to force a
     fresh boot capture.
   - **A single ESP32-S3 native-USB board can also fail to re-enumerate
     cleanly after an `esptool.py` flash's own reset sequence races with an
     already-attached monitor/reader.** If a monitor attached right after a
     flash returns empty output for 20+ seconds despite the board being
     confirmed alive by another means, don't assume the board is hung —
     detach and reattach the monitor, or briefly power-cycle, before
     concluding there's a real problem.
6. **Look for silence, not just errors.** A hang/freeze often produces NO
   error message at all — the last clean log line simply never gets a
   successor. When diagnosing a suspected hang, always capture the FULL
   boot sequence (from the ROM bootloader banner onward) and check where
   printed output actually stops, not just whether a build reported errors.
7. **Don't declare victory on a single successful run.** Radio-based tests
   are inherently a little noisy (timing, RF conditions). Repeat the test
   at least once, or capture a long-enough window, to make sure the fix is
   robust and not a lucky first pass.
8. **ESP32-S3 native-USB-Serial/JTAG boards can fail PlatformIO's normal
   upload path with `"A fatal error occurred: No serial data received."`**
   right after esptool logs "Changing baud rate to 460800" — setting
   `PLATFORMIO_UPLOAD_SPEED` has no effect since esptool still requests
   460800 regardless. Workaround: invoke `esptool.py` directly with
   `--baud 115200 --before default_reset --after hard_reset --no-stub
   write_flash -z --flash_mode dio --flash_freq 80m --flash_size 8MB`
   plus the four `bootloader.bin@0x0`, `partitions.bin@0x8000`,
   `boot_app0.bin@0xe000`, `firmware.bin@0x10000` image/offset pairs
   pulled from the PlatformIO build's `.pio/build/<env>/` output.
   Separately, the platformio-mcp server's `upload_firmware` tool can be
   gated behind an internal approval policy with no in-session
   approval-granting tool available — if `get_policy_status` confirms
   `upload_firmware` requires approval you can't grant, fall back to a raw
   `execute_command` invocation of `esptool.py` instead (subject to the
   normal human-approval flow for potentially-dangerous commands).

## Before committing

- Re-run `git status`/`git diff --stat` and confirm every changed file is
  intentional — no stray debug prints, no leftover experiment files
  committed by accident.
- Write a commit message that states the *root cause*, not just the
  symptom, especially for hard-to-find bugs — see `git log` for examples
  of this project's existing style (e.g. the `NimBLEScan::start()`
  overload-resolution fix). Future debugging sessions should be able to
  `git blame`/`git log -p` their way to the "why" without re-deriving it.

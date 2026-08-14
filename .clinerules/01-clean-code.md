# Clean Code Rules — flock-you-esp32

These rules apply to all C/C++ firmware code (`*.cpp`, `*.h`) and Python
tooling (`api/*.py`) in this repository.

## General

- **Comment the "why", not just the "what".** This codebase has repeatedly
  been bitten by subtle bugs (wrong overload resolution, blocking calls
  disguised as async ones, HCI restrictions, radio coexistence quirks).
  When you fix a non-obvious bug, leave a comment at the fix site
  explaining the root cause and symptom, not just what changed. Future
  agents (and humans) must be able to understand *why* code looks the way
  it does without re-deriving the investigation from scratch.
- **Never silently swallow an error return value.** If a function returns
  `bool`/an error code and that return is ignored, and the failure mode is
  not obviously harmless, log it. This project has been bitten by exactly
  this (`adv->start()`'s return value being ignored masked a completely
  silent BLE transmission failure).
- **Prefer explicit over implicit in ambiguous API calls.** When a C++ API
  has multiple overloads that could plausibly be selected by argument
  types (e.g. `NimBLEScan::start(uint32_t, bool)` vs.
  `start(uint32_t, callback, bool)`), make the call unambiguous — pass an
  explicit typed value/cast rather than relying on default-argument or
  implicit-conversion resolution. This bit the entire BLE_COEX_MODE code
  path project-wide; see `main.cpp`'s `bleCoexStart()` fix.
- **No magic numbers without a named `#define`/`constexpr`.** Timing
  constants, thresholds, RSSI cutoffs, etc. must be named constants at the
  top of the relevant section, not inline literals.
- **Match existing naming conventions** in the file/module you're editing
  (`fy*` prefix for detection-pipeline functions, `ble*` prefix for BLE
  helpers, `bf*` prefix for `beacon_frames.h`, etc.) rather than
  introducing a new style.
- **Keep ISR/callback-context code minimal.** Code that runs in
  `IRAM_ATTR` functions, WiFi promiscuous callbacks, or BLE host-task
  callbacks must avoid `Serial.print`, dynamic allocation, and anything
  that isn't safe from an interrupt/foreign-task context. Push data through
  the existing lock-free alert queue (`enqueueAlert()`/`drainAlertQueue()`)
  instead.

## Python (`api/`)

- Follow PEP 8. Use type hints on new functions.
- No bare `except:` — catch specific exceptions.

## Reviewing your own changes

Before considering a change complete, re-read the diff and ask:
1. Does every changed line have an obvious reason to exist?
2. Would a future engineer understand *why*, not just *what*, from reading
   the surrounding comments?
3. Did I leave any debug-only logging in that should be removed or gated?

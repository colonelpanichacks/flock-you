# Keep These Rules Current — flock-you-esp32

This `.clinerules/` folder is meant to be a living record of hard-won
project knowledge, not a static document written once and forgotten. It
exists specifically because this project has repeatedly re-discovered the
same categories of bugs (blocking calls disguised as async APIs, radio
coexistence quirks, silently-swallowed return values) across multiple
sessions — the goal is for each new agent session to start from where the
last one left off, not re-derive the same investigations from scratch.

## When to update `.clinerules/`

Update the relevant file in this folder (or add a new one) whenever you:

1. **Fix a bug whose root cause wasn't obvious from the code alone** —
   especially anything involving C++ overload resolution, blocking vs.
   async APIs, ISR/callback-context restrictions, or hardware-specific
   quirks (HCI command restrictions, radio coexistence, GPIO
   strapping/flash pins, etc.). Add a short note to `01-clean-code.md` or
   create a new topic file if the lesson is broadly applicable.
2. **Add, remove, or change the scoring of a detection path** — update
   `04-detection-methods.md`'s tables so it stays a single accurate
   source of truth. Don't let it drift out of sync with `fy_detect.h`/
   `fy_confidence.h`/`main.cpp`.
3. **Add a new test/validation method or tool** (e.g. a new self-test
   build, a new capture script, a new board pairing workflow) — document
   it in `02-test-before-commit.md` so future sessions reuse it instead of
   re-inventing it.
4. **Discover a new architectural convention worth enforcing** (a new
   naming prefix, a new file-decomposition boundary, a new board-variant
   pattern in `platformio.ini`) — add it to `03-file-size-and-decomposition.md`.
5. **Find that an existing rule in this folder is stale, wrong, or no
   longer applies** (e.g. a workaround that's no longer needed because the
   underlying bug was fixed) — correct or remove it rather than leaving
   contradictory guidance for the next session.

## What NOT to do

- Don't let this folder become a changelog. Rules should describe durable
  facts and conventions ("BLE_COEX_MODE keeps a continuous scan running"),
  not one-off narration of a specific session's blow-by-blow work (that
  belongs in commit messages / `git log`, not here).
- Don't duplicate information that's better expressed as a code comment at
  the exact call site it concerns. Use `.clinerules/` for cross-cutting
  knowledge a reader wouldn't find by reading one file in isolation.
- Don't add speculative/unverified guidance. Everything here should
  reflect something actually confirmed true about this codebase (via
  hardware testing, careful code reading, or direct experience fixing a
  real bug) — see `02-test-before-commit.md`'s standard for what counts as
  "confirmed."

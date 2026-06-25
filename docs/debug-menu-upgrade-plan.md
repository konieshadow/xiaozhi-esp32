# Debug Menu Upgrade Plan

## Goals

- Use a larger, round-screen-safe debug panel so labels do not truncate.
- Keep debug interactions inside the panel instead of writing information to the chat area.
- Add simulated recording turns for common spoken phrases through the real Kids English audio-upload flow.
- Restructure the debug UI into reusable view/action helpers for later expansion.

## Progress

- [x] Expand and refactor LCD debug menu UI.
- [x] Move device info into an in-panel list.
- [x] Add simulated recording actions and application/protocol hooks.
- [x] Build or compile-check the ESP32-S3 target.

## Verification

- `idf.py build` succeeded for the existing ESP32-S3 build configuration.

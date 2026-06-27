# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF C++ firmware project. Core application code lives in `main/`, with entry points such as `main/main.cc`, `application.cc`, `settings.cc`, and `ota.cc`. Feature modules are grouped under `main/audio/`, `main/display/`, `main/led/`, and `main/protocols/`. Board support is under `main/boards/`; shared board utilities are in `main/boards/common/`, while each hardware target has its own directory with `config.h`, board implementation, and optional `README.md`. Build metadata is in root `CMakeLists.txt`, `main/CMakeLists.txt`, `main/idf_component.yml`, and `sdkconfig.defaults*`. Documentation and assets are in `docs/`, `partitions/`, and `scripts/`.

## Build, Test, and Development Commands

Dependencies are managed through EMI. Prepare the recommended upstream-compatible ESP-IDF 5.5.2 environment first:

```bash
source $HOME/.espressif/tools/activate_idf_v5.5.2.sh
```

Default local development targets the Waveshare `ESP32-S3-Touch-LCD-1.85` board at `main/boards/waveshare/esp32-s3-touch-lcd-1.85/`. Common build commands:

```bash
idf.py set-target esp32s3
idf.py build
idf.py menuconfig
idf.py flash monitor
idf.py fullclean
```

`set-target` selects the chip family, `build` compiles firmware, `menuconfig` selects board type and features, and `flash monitor` deploys to hardware and opens logs. For board-specific settings, read the matching `main/boards/<board>/README.md`.

When running ESP-IDF from a noninteractive agent shell, the activation script may not leave `idf.py` and tools on `PATH`. Use an explicit environment if needed:

```bash
rtk proxy sh -lc 'IDF_PATH=$HOME/.espressif/v5.5.2/esp-idf IDF_PYTHON_ENV_PATH=$HOME/.espressif/tools/python/v5.5.2/venv IDF_TOOLS_PATH=$HOME/.espressif/tools ESP_IDF_VERSION=5.5.2 ESP_ROM_ELF_DIR=$HOME/.espressif/tools/esp-rom-elfs/20241011 PATH="$HOME/.espressif/tools/ninja/1.12.1:$HOME/.espressif/tools/cmake/3.30.2/CMake.app/Contents/bin:$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin:$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin:$HOME/.espressif/tools/python/v5.5.2/venv/bin:$HOME/.espressif/v5.5.2/esp-idf/tools:$PATH" $HOME/.espressif/tools/python/v5.5.2/venv/bin/python $HOME/.espressif/v5.5.2/esp-idf/tools/idf.py build'
```

For connected hardware, first verify the serial device, usually with `rtk ls /dev/cu.usbmodem*`. The locally used Waveshare ESP32-S3-Touch-LCD-1.85 has appeared as `/dev/cu.usbmodem1201`; use `idf.py -p /dev/cu.usbmodem1201 flash` only after confirming it still matches.

Keep `KIDS_ENGLISH_AUTO_SELF_TEST` disabled for normal development, formal firmware builds, and formal device flashes so ordinary boots do not automatically run the Kids English self-test. Run the self-test manually from the Kids English debug menu when possible; enable `KIDS_ENGLISH_AUTO_SELF_TEST` only for explicit temporary integration-test firmware, and disable it again after testing.

When `CONFIG_USE_KIDS_ENGLISH_SERVER` is enabled, the default device server URL is the production service `https://xiaozhi.comellia.com`. The previous LAN address `http://192.168.2.152:3000` is still the local debug address; use it only as a menuconfig/local `sdkconfig` override when testing a service running on that host, never as the committed default.

## Coding Style & Naming Conventions

C++ uses the repository `.clang-format`, based on Google style: 4-space indentation, 100-column limit, attached braces, sorted includes, and left-aligned pointers. Run formatting before submitting:

```bash
clang-format -i main/path/to/file.cc main/path/to/file.h
```

Prefer existing naming patterns: board directories use lowercase kebab-case, C++ classes use PascalCase, functions and variables use lower_snake_case where local code does so, and compile-time config symbols use `CONFIG_*`.

## Testing Guidelines

There is no central host-side test suite in this repository. Validate changes with at least `idf.py set-target esp32s3` and `idf.py build` for the ESP32-S3-Touch-LCD-1.85 target unless another board is affected. For hardware, run `idf.py flash monitor` and verify boot, network provisioning, audio path, display behavior, and protocol-specific logs. For scripts, run the relevant Python tool directly from `scripts/` with representative inputs.

For quick post-flash validation, a short serial read is often enough to confirm reboot without occupying the port for a long monitor session. If a flash fails because the serial port is busy, stop any existing monitor before retrying.

## Board Notes: Waveshare ESP32-S3-Touch-LCD-1.85

This board has a round 360x360 ST77916 display. Status-bar UI placed near the top edge is clipped by the circular active area; keep top status content centered, narrow, and lower than on rectangular screens. Existing board-specific display adjustments should be guarded with compile definitions such as `DISPLAY_ROUND_SAFE_STATUS_BAR` rather than applied globally.

Battery voltage is readable on this board via `BAT_ADC` on `GPIO8`, ESP-IDF `ADC_CHANNEL_7` on ADC1. The Waveshare demo/schematic-derived conversion used in this project is:

```cpp
voltage = (adc_mv * 3.0f / 1000.0f) / 0.9945f;
```

Prefer adding optional battery capability through shared board interfaces, so boards without battery ADC can return `false` instead of inheriting Waveshare-specific behavior.

When `CONFIG_USE_KIDS_ENGLISH_SERVER` is enabled, the main LCD UI shows a small gear-shaped debug entry near the right side of the screen. Tap it to open the debug menu. `开始/提交练习` toggles the Kids English practice flow, `设备信息` prints board/state/heap/UUID/battery details on screen, and `清空消息` clears the visible debug/chat messages. On the Waveshare ESP32-S3-Touch-LCD-1.85 this relies on the CST816 touch controller being registered with LVGL.

## Commit & Pull Request Guidelines

Recent history uses concise subjects, often conventional prefixes such as `feat(scope): ...`, `fix(scope): ...`, or short imperative summaries like `add <board>`. Keep commits focused and mention the affected board or module when useful. Pull requests should include a clear description, target hardware, build command used, runtime test results, linked issues, and screenshots or serial logs for UI/display or boot behavior changes.

## Agent-Specific Instructions

Follow `/Users/konie/.codex/RTK.md`: prefix shell commands with `rtk` when working in this repository.

Use `rtk proxy` for commands that `rtk` cannot parse directly, such as complex `find` predicates or shell snippets. Keep firmware edits narrowly scoped, especially in shared display and board abstractions. Avoid broad `clang-format` runs over large existing files; format only the lines or files you intentionally changed.

## Related Service Project

The children's English speaking practice service is maintained outside this firmware repository at `/Users/konie/Codes/github/xiaozhi-server` (`../xiaozhi-server` from this repository root). Do not add service-side source code to this ESP32 firmware repo unless explicitly requested.

When implementing firmware-to-service integration, read the service documentation first:

- Integration guide: `/Users/konie/Codes/github/xiaozhi-server/docs/integration.md`
- Protocol reference: `/Users/konie/Codes/github/xiaozhi-server/docs/protocol.md`

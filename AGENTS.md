# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF C++ firmware project. Core application code lives in `main/`, with entry points such as `main/main.cc`, `application.cc`, `settings.cc`, and `ota.cc`. Feature modules are grouped under `main/audio/`, `main/display/`, `main/led/`, and `main/protocols/`. Board support is under `main/boards/`; shared board utilities are in `main/boards/common/`, while each hardware target has its own directory with `config.h`, board implementation, and optional `README.md`. Build metadata is in root `CMakeLists.txt`, `main/CMakeLists.txt`, `main/idf_component.yml`, and `sdkconfig.defaults*`. Documentation and assets are in `docs/`, `partitions/`, and `scripts/`.

## Build, Test, and Development Commands

Dependencies are managed through EMI. Prepare the recommended upstream-compatible ESP-IDF 5.5.2 environment first:

```bash
$HOME/.espressif/tools/activate_idf_v5.5.2.sh
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

## Coding Style & Naming Conventions

C++ uses the repository `.clang-format`, based on Google style: 4-space indentation, 100-column limit, attached braces, sorted includes, and left-aligned pointers. Run formatting before submitting:

```bash
clang-format -i main/path/to/file.cc main/path/to/file.h
```

Prefer existing naming patterns: board directories use lowercase kebab-case, C++ classes use PascalCase, functions and variables use lower_snake_case where local code does so, and compile-time config symbols use `CONFIG_*`.

## Testing Guidelines

There is no central host-side test suite in this repository. Validate changes with at least `idf.py set-target esp32s3` and `idf.py build` for the ESP32-S3-Touch-LCD-1.85 target unless another board is affected. For hardware, run `idf.py flash monitor` and verify boot, network provisioning, audio path, display behavior, and protocol-specific logs. For scripts, run the relevant Python tool directly from `scripts/` with representative inputs.

## Commit & Pull Request Guidelines

Recent history uses concise subjects, often conventional prefixes such as `feat(scope): ...`, `fix(scope): ...`, or short imperative summaries like `add <board>`. Keep commits focused and mention the affected board or module when useful. Pull requests should include a clear description, target hardware, build command used, runtime test results, linked issues, and screenshots or serial logs for UI/display or boot behavior changes.

## Agent-Specific Instructions

Follow `/Users/konie/.codex/RTK.md`: prefix shell commands with `rtk` when working in this repository.

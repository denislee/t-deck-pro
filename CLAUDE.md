# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Firmware for the **LilyGo T-Deck-Pro** (ESP32-S3 + 320×240 e-paper, LoRa SX1262, GPS, QWERTY keypad, optional 4G/A7682E or PCM5102A audio). Each subfolder under `examples/` is a standalone Arduino-style sketch; `platformio.ini` selects which sketch to build via `src_dir =`. There is no monolithic application — `examples/factory/` is the largest one and serves as both the shipped firmware and the de facto reference for peripheral usage.

`GEMINI.md` at the repo root is a parallel context file (kept in sync with this one). It has additional detail on dev conventions, shared bus management, and pin tables. Read it when you need the longer reference.

## Build / flash / monitor

The `Makefile` is the canonical build interface — wrap PlatformIO and esptool with sensible defaults (auto-detects `/dev/ttyACM0` then `/dev/ttyUSB0`, `ENV=T-Deck-Pro`).

```
make build                 # compile current src_dir
make upload                # build + flash
make monitor               # serial monitor at 115200
make flash-factory         # flash newest firmware/H693_factory_v*.bin
make flash-meshtastic      # flash newest firmware/meshtastic/firmware-t-deck-pro-2.*.bin
make erase
make chip-info

# Override device or example:
make upload PORT=/dev/ttyUSB0
make upload ENV=T-Deck-Pro
```

To switch which example builds, pass the sketch directory to the Makefile:
`make build SRC_DIR=examples/<name>` (or `make upload SRC_DIR=examples/<name>`) —
no `platformio.ini` edit needed. Default is `examples/factory`; alternatively
edit `src_dir` in `platformio.ini` (only one active at a time). There is **no
test suite** — verification is via flashing to hardware and watching serial.

DFU mode (when upload won't auto-reset): hold BOOT (top of right rocker) → tap RST (back) → release BOOT.

## Hardware variant notes

This codebase targets the **4G variant** by default: the modem is a SIM7672
(`TINY_GSM_MODEM_SIM7672` is defaulted in `examples/factory/factory.h` — it is
no longer a `platformio.ini` build flag), and `examples/A7682E/*` examples
assume the modem is present and the battery is plugged in. The audio variant
uses PCM5102A on the same pins as the modem UART — they're mutually exclusive in
hardware.

Hardware revision detection happens at runtime by I²C-probing `0x5A` (DRV2605, present on V1.1 only). See `isT_Deck_Pro_v1_0` in `examples/factory/factory.ino`.

## Architecture of `examples/factory/`

The factory sketch is the only place where most peripherals are exercised together, and it has cross-cutting concerns that are easy to break:

- **`factory.ino`** — `setup()`/`loop()`, owns global driver instances (`display`, `modem`, `PPM`, `bq27220`, `audio`), implements the LVGL→GxEPD2 flush callback, and exposes the **shared SPI mutex** (`shared_spi_lock/unlock`, or the RAII `SpiGuard` in `factory.h`). LoRa, SD, and the e-paper share one SPI bus — every transaction must take this mutex; the outermost unlock deasserts every CS.
- **`peripheral.h` + `peri_*.cpp`** — one driver wrapper per peripheral (LoRa, GPS, gyro, LTR553, keypad). Also declares the **I²C-0 mutex** (`i2c0_lock/unlock`); the keypad runs as a high-priority FreeRTOS task and will preempt any unguarded `Wire` call, corrupting the TwoWire txBuffer. Take `i2c0_lock` (or the RAII `I2C0Guard`) around every multi-step I²C transaction.
- **`ui_deckpro.cpp` / `ui_deckpro_port.cpp`** — LVGL screens (numbered `SCREEN*_ID` in `ui_deckpro.h`) and the input device port. Feature screens are split into `ui_lora.cpp`, `ui_notes.cpp`, `ui_reader.cpp`, `ui_lockscreen.cpp`; navigate by screen ID.
- **Touch** — not used by the factory firmware: the Hynitron touch stack (`hyn_*.c/cpp`) was removed from `examples/factory/`. `examples/touch_hyn_core/` is the standalone CST3xx/CST66xx driver (see its `arduino/` sketch) if touch is needed again.
- **`src/`** — bundled image and font assets compiled into the binary (LVGL `lv_img_dsc_t` and font `lv_font_t` blobs).

### Power rails (don't get this wrong)

Several peripherals are gated by EN GPIOs that must be driven HIGH **before** their driver init:

| Pin | Rail | Powers |
| --- | --- | --- |
| `BOARD_GPS_EN` (39) | GPS | MIA-M10Q |
| `BOARD_1V8_EN` (38) | VDD1V8 | **BHI260AP gyro AND CST328 touch IC** — keep HIGH at boot, never gate on a "gyro toggle" alone |
| `BOARD_6609_EN` (41) | — | A7682E modem / PCM5102A |
| `BOARD_LORA_EN` (46) | — | SX1262 |

### LVGL + e-paper

Custom LVGL flush callback drives `GxEPD2`. Two refresh modes (`DISP_REFR_MODE_FULL` / `DISP_REFR_MODE_PART`) — partial is the default for UI work. **LVGL software rotation (8.3) is bypassed** — use `factory_set_landscape(bool)` (declared in `factory.h`) which calls `display.setRotation()`. Rotating in LVGL causes refresh artifacts on this panel.

## Other examples worth knowing

- `examples/A7682E/test_AT`, `.../deep_sleep` — modem-only tests. Battery **must** be connected; modem is supplied from the battery rail.
- `examples/test_*` — single-peripheral smoke tests. Use these to bisect a peripheral problem before debugging in `factory`.
- `examples/WireScan` — I²C scanner; good first step when a sensor isn't responding.

## Libraries

`lib/` contains vendored copies of all third-party libs (GxEPD2, RadioLib, lvgl 8.3.x, SensorLib, TinyGPSPlus, TinyGSM, XPowersLib, ESP32-audioI2S, etc.). PlatformIO uses these directly — `lib_deps` in `platformio.ini` lists only the core ESP32 libraries (SPI, Wire, FS, SPIFFS, EEPROM), and third-party libs are vendored under `lib/`. **Edit cautiously**: changes to `lib/` are local forks, not upstream.

## Conventions

- Pin definitions live in `examples/factory/utilities.h` (and are duplicated near the top of standalone examples). Treat `utilities.h` as the source of truth for the V1.0/V1.1 board.
- Shared-bus invariants are non-obvious and unenforced by the compiler. When adding code that touches SPI or I²C-0 from outside the existing driver wrappers, take the appropriate mutex.
- Don't silently change `platformio.ini`'s active `src_dir` — that decides what `make build` produces. Confirm with the user before switching.

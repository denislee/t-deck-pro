# LilyGo T-Deck-Pro Project Context

This project is for the **LilyGo T-Deck-Pro**, a handheld development platform based on the ESP32-S3. It features an e-paper display, LoRa, GPS, a full QWERTY keyboard, and various environmental sensors.

## Project Overview

- **MCU**: ESP32-S3 (16MB Flash, 8MB PSRAM)
- **Display**: GDEQ031T10 (3.1 inch E-paper, 320x240)
- **Connectivity**: LoRa (SX1262), GPS (MIA-M10Q), WiFi, Bluetooth, optional 4G (A7682E with a SIM7672 modem — `TINY_GSM_MODEM_SIM7672` is defaulted in `examples/factory/factory.h`, not a `platformio.ini` flag)
- **Input**: QWERTY Keyboard (TCA8418), Touchpad (CST328 — not driven by the factory firmware; the standalone Hynitron driver lives in `examples/touch_hyn_core/`), Gyroscope (BHI260AP)
- **Power**: BQ25896 (Charger), BQ27220 (Fuel Gauge), 1400mAh Li-Po battery support.

## Building and Running

### PlatformIO (Recommended)
1.  Open the root directory in VS Code with the PlatformIO extension.
2.  Pick the sketch with `make build SRC_DIR=examples/<name>` (or `make upload SRC_DIR=examples/<name>`) — no `platformio.ini` edit needed. To make it the default, set `src_dir` under the `[platformio]` section instead.
3.  Use the PlatformIO **Build** and **Upload** buttons.
4.  The default environment is `T-Deck-Pro`.

### Arduino IDE
1.  Copy the contents of the `lib/` directory to your Arduino `libraries` folder.
2.  Open an example `.ino` file from the `examples/` directory.
3.  **Board Settings**:
    - Board: `ESP32S3 Dev Module`
    - USB CDC On Boot: `Enabled`
    - Flash Size: `16MB`
    - Partition Scheme: `16M Flash (3M APP/9.9MB FATFS)`
    - PSRAM: `OPI PSRAM`

## Development Conventions

### 1. Shared Bus Management
The project uses shared buses for multiple peripherals, requiring strict synchronization:
- **SPI Bus**: Shared by LoRa, SD Card, and EPD. Use `shared_spi_lock()` and `shared_spi_unlock()` (defined in `factory.ino`) to wrap SPI transactions. All CS pins must be pulled HIGH before initialization.
- **I2C Bus 0**: Shared by most sensors and the keyboard. Use `i2c0_lock()` and `i2c0_unlock()` (defined in `peripheral.h`) to prevent collisions, especially with the background keypad task.

### 2. Initialization Sequence
Peripherals often depend on power enable pins (`EN`). Before initializing a module, ensure its corresponding `EN` pin is set to `OUTPUT` and `HIGH`:
- `BOARD_GPS_EN` (GPIO 39)
- `BOARD_1V8_EN` (GPIO 38 - Powers Gyro AND Touch IC — keep HIGH at boot)
- `BOARD_6609_EN` (GPIO 41 - Powers 4G/Audio)
- `BOARD_LORA_EN` (GPIO 46)

### 3. Hardware Version Detection
Distinguish between V1.0 and V1.1 by checking for the DRV2605 haptic driver at I2C address `0x5A`:
- **V1.1**: Responds at `0x5A`.
- **V1.0**: Does **not** respond at `0x5A`.

### 4. LVGL & E-Paper Refresh
The e-paper refresh is handled through a custom LVGL flush callback.
- **Partial Refresh**: Default for UI updates to maintain speed.
- **Full Refresh**: Used for complete screen clears.
- **Note**: LVGL 8.3 software rotation is bypassed in favor of `GxEPD2` rotation to avoid refresh bugs. Use `factory_set_landscape(bool)` to swap orientations.

## Key Files & Directories

- `platformio.ini`: Build configuration and example selection.
- `boards/T-Deck-Pro.json`: Custom board definition.
- `examples/factory/`: The most comprehensive example, including UI and peripheral drivers.
- `examples/touch_hyn_core/`: Standalone Hynitron CST3xx/CST66xx touch driver (the factory firmware does not include the `hyn_*` stack).
- `examples/factory/utilities.h`: Centralized pin definitions and I2C addresses.
- `lib/`: Local copies of required libraries.
- `hardware/`: Schematics and datasheets (PDF).
- `firmware/`: Pre-compiled binaries, including Meshtastic support.

## Common Pins & Addresses

| Peripheral | Pins / Address |
| :--- | :--- |
| **I2C SDA/SCL** | GPIO 13 / 14 |
| **SPI SCK/MOSI/MISO** | GPIO 36 / 33 / 47 |
| **Keyboard (I2C)** | 0x34 (INT: 15, LED: 42) |
| **Touch (I2C)** | 0x1A (INT: 12, RST: 45) |
| **LoRa CS/RST/INT** | GPIO 3 / 4 / 5 |
| **EPD CS/DC/BUSY** | GPIO 34 / 35 / 37 |
| **GPS TX/RX** | GPIO 43 / 44 |
| **Battery Fuel Gauge** | 0x55 (BQ27220) |
| **Battery Charger** | 0x6B (BQ25896) |

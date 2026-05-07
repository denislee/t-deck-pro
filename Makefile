# T-Deck-Pro firmware build & flashing
#
# Requires platformio (pip install platformio) for build/upload.
# Requires esptool.py (pip install esptool) for the prebuilt-bin flash targets.
#
# To enter DFU mode before flashing:
#   1. Hold BOOT (top of right-side rocker)
#   2. Tap RST (back) and release
#   3. Release BOOT
#
# Usage:
#   make build                  # compile current src_dir from platformio.ini
#   make upload                 # compile and flash to device
#   make flash-factory          # flash a prebuilt factory firmware bin
#   make flash-spiffs           # flash spiffs.bin at SPIFFS partition offset
#   make flash-meshtastic       # flash a prebuilt meshtastic build
#   make erase                  # erase entire flash
#   make monitor                # serial monitor
#   make chip-info              # detect chip / flash
#
# Overrides:
#   make upload PORT=/dev/ttyUSB0
#   make upload ENV=T-Deck-Pro
#   make flash-factory FACTORY=firmware/H693_factory_v1.5_20251230.bin
#   make flash-meshtastic MESHTASTIC=firmware/meshtastic/firmware-t-deck-pro-2.7.13.597fa0b.bin

PIO           ?= pio
ESPTOOL       ?= esptool.py
ENV           ?= T-Deck-Pro
CHIP          ?= esp32s3
BAUD          ?= 921600
MONITOR_BAUD  ?= 115200

# Auto-detect port: prefer /dev/ttyACM0 then /dev/ttyUSB0
PORT ?= $(firstword $(wildcard /dev/ttyACM0 /dev/ttyUSB0) /dev/ttyACM0)

FIRMWARE_DIR := firmware

# Prebuilt bin defaults (override on command line)
FACTORY    ?= $(lastword $(sort $(wildcard $(FIRMWARE_DIR)/H693_factory_v*.bin)))
SPIFFS     ?= $(FIRMWARE_DIR)/spiffs.bin
# Default to base T-Deck-Pro builds; pass MESHTASTIC=...v1.1-*.bin for v1.1 hw.
MESHTASTIC ?= $(lastword $(sort $(wildcard $(FIRMWARE_DIR)/meshtastic/firmware-t-deck-pro-2.*.bin)))

# Flash offsets (default_16MB.csv partition table)
FACTORY_OFFSET    := 0x0
SPIFFS_OFFSET     := 0xc90000
MESHTASTIC_OFFSET := 0x0

ESPTOOL_FLAGS := --chip $(CHIP) --port $(PORT) --baud $(BAUD)
WRITE_FLAGS   := --flash_mode qio --flash_freq 80m --flash_size 16MB

.PHONY: help build upload clean flash-factory flash-spiffs flash-meshtastic erase monitor chip-info

help:
	@awk 'BEGIN{FS=":.*##"} /^[a-zA-Z_-]+:.*##/ {printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@echo
	@echo "PORT=$(PORT)"
	@echo "ENV=$(ENV)"
	@echo "FACTORY=$(FACTORY)"
	@echo "SPIFFS=$(SPIFFS)"
	@echo "MESHTASTIC=$(MESHTASTIC)"

build: ## Compile current src_dir for $(ENV)
	$(PIO) run -e $(ENV)

upload: build ## Compile and flash to device
	$(PIO) run -e $(ENV) -t upload --upload-port $(PORT)

clean: ## Clean build artifacts
	$(PIO) run -e $(ENV) -t clean

flash-factory: ## Flash a prebuilt factory firmware bin at 0x0
	@test -f "$(FACTORY)" || { echo "Factory firmware not found: $(FACTORY)"; exit 1; }
	@echo ">>> Flashing $(FACTORY) -> $(FACTORY_OFFSET)"
	$(ESPTOOL) $(ESPTOOL_FLAGS) write_flash $(WRITE_FLAGS) $(FACTORY_OFFSET) "$(FACTORY)"

flash-spiffs: ## Flash spiffs.bin at SPIFFS partition offset
	@test -f "$(SPIFFS)" || { echo "SPIFFS image not found: $(SPIFFS)"; exit 1; }
	@echo ">>> Flashing $(SPIFFS) -> $(SPIFFS_OFFSET)"
	$(ESPTOOL) $(ESPTOOL_FLAGS) write_flash $(SPIFFS_OFFSET) "$(SPIFFS)"

flash-meshtastic: ## Flash a prebuilt meshtastic build at 0x0
	@test -f "$(MESHTASTIC)" || { echo "Meshtastic firmware not found: $(MESHTASTIC)"; exit 1; }
	@echo ">>> Flashing $(MESHTASTIC) -> $(MESHTASTIC_OFFSET)"
	$(ESPTOOL) $(ESPTOOL_FLAGS) write_flash $(WRITE_FLAGS) $(MESHTASTIC_OFFSET) "$(MESHTASTIC)"

erase: ## Erase entire flash
	$(ESPTOOL) $(ESPTOOL_FLAGS) erase_flash

monitor: ## Open serial monitor at $(MONITOR_BAUD)
	@command -v $(PIO) >/dev/null 2>&1 && exec $(PIO) device monitor --port $(PORT) --baud $(MONITOR_BAUD) || \
	 command -v miniterm >/dev/null 2>&1 && exec miniterm $(PORT) $(MONITOR_BAUD) || \
	 { echo "Install platformio (pio) or pyserial (miniterm) for monitor"; exit 1; }

chip-info: ## Print chip ID and flash info
	$(ESPTOOL) --chip $(CHIP) --port $(PORT) chip_id
	$(ESPTOOL) --chip $(CHIP) --port $(PORT) flash_id

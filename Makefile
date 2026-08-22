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

# Auto-detect port by:
#   1) preferring /dev/serial/by-id/* (udev-stable symlinks keyed by USB
#      serial number, so the path resolves to whichever ttyACMx the device
#      currently holds and survives disconnect/reconnect cycles), and
#   2) falling back to walking /sys/class/tty/<name>/device for any
#      /dev/ttyACM*/ttyUSB* whose USB vendor matches an ESP32-class chip.
#
# Matched vendors:
#   303a -- Espressif (ESP32-S3 native USB-CDC -- the T-Deck-Pro's default)
#   10c4 -- Silicon Labs (CP210x UART bridges)
#   1a86 -- WCH (CH340/CH341 UART bridges)
#   0403 -- FTDI (FT232 UART bridges)
#
# Resolution order:
#   1. explicit PORT= override (always wins)
#   2. /dev/serial/by-id/* matching an ESP32-class vendor (stable path)
#   3. first ttyACM*/ttyUSB* whose USB vendor matches an ESP32-class chip
#   4. first ttyACM*/ttyUSB* that exists (legacy fallback)
#   5. empty (the upload recipe waits + re-detects in this case)
#
# Snippet is defined once and used both at make-parse time (for `make help`
# display) and re-evaluated at recipe time in `upload` (so a device that
# moves to a different node mid-upload is picked up).
#
# NOTE: avoid unbalanced parens inside $(shell ...) -- make's parser pairs
# them naively, so a shell `case .. in foo) ..` would close the function
# prematurely. We use grep -E with anchored alternations (no parens) instead.
define DETECT_PORT_SH
match=""; \
for s in /dev/serial/by-id/*; do \
    [ -L "$$s" ] || continue; \
    target=$$(readlink -f "$$s"); \
    [ -e "$$target" ] || continue; \
    name=$$(basename "$$target"); \
    cur=$$(readlink -f "/sys/class/tty/$$name/device" 2>/dev/null); \
    [ -n "$$cur" ] || continue; \
    vid=""; \
    for _ in 1 2 3 4 5 6; do \
        [ -f "$$cur/idVendor" ] && { vid=$$(cat "$$cur/idVendor"); break; }; \
        p=$$(dirname "$$cur"); [ "$$p" = "$$cur" ] && break; cur="$$p"; \
    done; \
    if echo "$$vid" | grep -qE '^303a$$|^10c4$$|^1a86$$|^0403$$'; then match="$$s"; break; fi; \
done; \
if [ -z "$$match" ]; then \
    for d in /dev/ttyACM* /dev/ttyUSB*; do \
        [ -e "$$d" ] || continue; \
        name=$$(basename "$$d"); \
        cur=$$(readlink -f "/sys/class/tty/$$name/device" 2>/dev/null); \
        [ -n "$$cur" ] || continue; \
        vid=""; \
        for _ in 1 2 3 4 5 6; do \
            [ -f "$$cur/idVendor" ] && { vid=$$(cat "$$cur/idVendor"); break; }; \
            p=$$(dirname "$$cur"); [ "$$p" = "$$cur" ] && break; cur="$$p"; \
        done; \
        if echo "$$vid" | grep -qE '^303a$$|^10c4$$|^1a86$$|^0403$$'; then match="$$d"; break; fi; \
    done; \
fi; \
if [ -z "$$match" ]; then \
    for d in /dev/ttyACM* /dev/ttyUSB*; do [ -e "$$d" ] && { match="$$d"; break; }; done; \
fi; \
echo "$$match"
endef

ifndef PORT
PORT := $(shell $(DETECT_PORT_SH))
ifeq ($(PORT),)
PORT := /dev/ttyACM0
endif
endif

FIRMWARE_DIR := firmware

# Prebuilt bin defaults (override on command line)
FACTORY    ?= $(lastword $(sort $(wildcard $(FIRMWARE_DIR)/H693_factory_v*.bin)))
SPIFFS     ?= $(FIRMWARE_DIR)/spiffs.bin
# Default to base T-Deck-Pro builds; pass MESHTASTIC=...v1.1-*.bin for v1.1 hw.
MESHTASTIC ?= $(lastword $(sort $(wildcard $(FIRMWARE_DIR)/meshtastic/firmware-t-deck-pro-2.*.bin)))

# Flash offsets. SPIFFS_OFFSET is derived from the partition CSV so a
# repartition can never leave this hard-coded at a stale address; it falls back
# to the stock default_16MB.csv offset if the CSV is missing.
PARTITIONS        ?= partitions_tdeckpro.csv
FACTORY_OFFSET    := 0x0
SPIFFS_OFFSET     := $(or $(shell awk -F, '/^[^#]/ && $$1 ~ /^[[:space:]]*spiffs/ {gsub(/ /,"",$$4); print $$4}' $(PARTITIONS) 2>/dev/null),0xc90000)
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
	@port="$(PORT)"; \
	if [ ! -e "$$port" ]; then \
	    port=$$($(DETECT_PORT_SH)); \
	fi; \
	if [ -z "$$port" ] || [ ! -e "$$port" ]; then \
	    echo ">>> Waiting up to 10s for T-Deck-Pro to appear..."; \
	    for i in $$(seq 1 20); do \
	        sleep 0.5; \
	        np=$$($(DETECT_PORT_SH)); \
	        if [ -n "$$np" ] && [ -e "$$np" ]; then port="$$np"; break; fi; \
	    done; \
	fi; \
	if [ -z "$$port" ] || [ ! -e "$$port" ]; then \
	    echo "ERROR: no T-Deck-Pro detected on any /dev/ttyACM* or /dev/ttyUSB*"; \
	    echo "       (check USB cable / try replugging; override with PORT=/dev/...)"; \
	    exit 1; \
	fi; \
	echo ">>> Uploading to $$port"; \
	$(PIO) run -e $(ENV) -t upload --upload-port "$$port"

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

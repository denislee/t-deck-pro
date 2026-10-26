# T-Deck-Pro firmware — optimization findings

Audit of `examples/factory` (the default `src_dir`) plus build configuration,
carried out against the **existing build artefacts** in `.pio/build/T-Deck-Pro/`
(`firmware.elf`, `firmware.map`, per-object `.o` files) so every size number
below is a measured link-time value, not an estimate.

- Build audited: `.pio/build/T-Deck-Pro/firmware.bin`, **1,795,824 bytes**, built 2026-05-24.
- Toolchain: `xtensa-esp32s3-elf` 8.4.0, `espressif32@6.5.0`, Arduino core 2.0.14.
- Analysis method: `nm --size-sort -S`, `size -A`, and a map-file parser summing
  `.text/.rodata/.data/.bss` per input object (debug sections excluded).

> **2026-10-25 (night) — §9 records the implementation of every §8 item**
> (build-verified only; the hardware checks still to do are listed at its end).
>
> **2026-10-25 (evening) — read §8 first.** It re-audits the current working tree:
> nearly all of §1–§7 is now fixed (status scoreboard in §8.1), the image is
> 1,226,608 B, and it adds 10 new bugs (§8.2) plus power, e-paper, storage and memory
> findings (§8.3–§8.6) with a fresh order of work (§8.8). §0–§7 are kept as history;
> their line numbers are stale.
>
> **2026-10-25:** §7 adds a dead-code and simplification audit of HEAD `799e7e0`,
> backed by a fresh instrumented build. It also lists 10 real bugs found along the
> way (§7.1), including a taskbar use-after-free and a screen-manager leak.

Findings are ordered by measured impact within each section. Each carries a
**Confidence** tag: *Measured* (taken from the linked image), *Verified in code*
(read directly in the source), or *Inferred* (reasoned, not instrumented).

---

## 0. Executive summary

| Category | Current | Recoverable | Notes |
| --- | --- | --- | --- |
| Flash (app image) | 1,795,824 B | **~553 KB (~31%)** | 5 changes, all mechanical |
| Static internal DRAM | 147,344 B (`.data`+`.bss`) | **~33 KB, up to ~72 KB** | 33 KB is uncontroversial; the rest depends on how `LV_MEM_SIZE` is handled (§2.1) |
| Usable flash for content | 3.4 MB SPIFFS | **+10 MB** | partition table change |
| Runtime | — | see §3 | I²C polling, SD byte-reads, resume seek |

The five highest-value flash items:

| # | Item | Bytes | Kind |
| --- | --- | --- | --- |
| 1 | `libESP32-audioI2S` + the TLS stack it drags in | ~240,000 | dead on the 4G variant |
| 2 | `img_start` splash stored at 2 bytes/px on a 1-bit panel | 139,992 | wrong pixel format |
| 3 | `bhy2_firmware_image` (BHI260AP blob) | 103,164 | driver disabled at runtime |
| 4 | 10 menu icons stored at 2 bytes/px | 50,120 | wrong pixel format |
| 5 | `lv_font_montserrat_26` | 32,897 | reachable only via a `.fallback` pointer |

---

## 1. Flash / image size

### 1.1 The audio stack is linked but unreachable on the 4G board — ~240 KB
**Confidence: Measured + Verified in code.** **Impact: ~240 KB flash, 10,096 B DRAM.**

Map-file totals:

```
140,813  libESP32-audioI2S.a   (text 92,130 / rodata 48,050)
 55,474  libmbedx509.a
 41,284  libmbedtls_2.a
      +  libWiFiClientSecure.a, plus part of libmbedcrypto.a
 10,096  .bss.audio            (the global Audio object, factory.ino.cpp.o)
```

The TLS chain is pulled in *solely* by the audio library. From the map's
`Archive member included because of file (symbol)` table:

```
libWiFiClientSecure.a(WiFiClientSecure.cpp.o)
        <- libESP32-audioI2S.a(Audio.cpp.o)  (WiFiClientSecure::stop())
libmbedtls_2.a(net_sockets.c.obj)
        <- libWiFiClientSecure.a(ssl_client.cpp.o)  (mbedtls_net_recv)
libmbedx509.a(x509_crt.c.obj)
        <- libmbedtls_2.a(ssl_tls.c.obj)
```

`Audio::connecttohost()` (the internet-radio path) is what references
`WiFiClientSecure`. This firmware never calls it — the only uses are
`connecttoFS()` from SPIFFS (`ui_deckpro_port.cpp:1316`) and `stopSong()`
(`:1322`). WiFi itself is *not* affected; `libWiFi.a` is pulled independently by
`ui_deckpro_port.cpp`.

Worse, the audio hardware is mutually exclusive with the modem, and
`peripheral_init_task()` only reaches it on modem failure:

```c
// factory.ino:658-663
peri_init_st[E_PERI_A7682E] = A7682E_init();
if (peri_init_st[E_PERI_A7682E] == false) {
    peri_init_st[E_PERI_PCM5102A] = pcm5102a_init();
}
```

`platformio.ini` sets `-DTINY_GSM_MODEM_SIM7672`, and `CLAUDE.md` states this
codebase targets the 4G variant by default. On that board the whole audio +
TLS stack is dead weight.

**Fix.** Put audio behind a build flag, e.g. `-DBOARD_HAS_PCM5102A`, guarding the
`Audio audio;` definition, `pcm5102a_init()`, `audio.loop()`, and the two call
sites in `ui_deckpro_port.cpp`. Ship the 4G build without it.
Secondary option if audio must stay: patch the vendored
`lib/ESP32-audioI2S` to compile out `connecttohost()` — that alone removes
~97 KB of mbedTLS.

### 1.2 `img_start` is stored at 2 bytes/pixel on a 1-bit panel — 131 KB
**Confidence: Measured + Verified in code.** **Impact: ~131 KB flash.**

```
3c128f94 000222d8 D img_start_map        # 139,992 bytes
```

From `examples/factory/src/img_start.c:1267-1272`:

```c
.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
.header.w  = 228,
.header.h  = 307,
.data_size = 69996 * LV_IMG_PX_SIZE_ALPHA_BYTE,
```

`lv_conf.h:27` sets `LV_COLOR_DEPTH 1`, so `LV_IMG_PX_SIZE_ALPHA_BYTE` is
`(8/8)+1 = 2`. 69,996 px × 2 B = 139,992 B — exactly the map figure. The panel
is 1-bit black/white, so 15 of every 16 bits stored are redundant.

Re-encoding as `LV_IMG_CF_INDEXED_1BIT` (or `ALPHA_1BIT`) gives
`ceil(228/8) × 307 = 8,903` bytes plus an 8-byte palette.

The image is used exactly once — the shutdown screen when running on battery
(`ui_deckpro.cpp:4024`, inside `create9`). 140 KB of a 1.79 MB image for one
farewell screen.

**Fix.** Re-run LVGL's image converter with output format
`Indexed 1 bit` (or `CF_ALPHA_1_BIT`). Saving ≈ **131,089 B**.

### 1.3 The BHI260AP firmware blob ships in a build where the gyro is disabled — 103 KB
**Confidence: Measured + Verified in code.** **Impact: 103,164 B flash.**

```
$ xtensa-esp32s3-elf-nm -S .pio/build/T-Deck-Pro/src/peri_gyroscope.cpp.o
00000000 000194fc r bhy2_firmware_image      # 103,164 bytes
```

`peri_gyroscope.cpp.o` contributes **105,893 B** to the image, 97% of it this
blob. But the driver is unconditionally disabled:

```c
// factory.ino:653-656
// Gyro disabled — BHI260AP responds at the I2C scan but its chip-ID
// readback returns 0xFFFF on this V1.1 board [...]
peri_init_st[E_PERI_BHI260AP] = false;
```

`BHI260AP_init()` is never called, yet the blob survives `--gc-sections` because
`SensorBHI260AP::initImpl()` references it and the vtable/`bhy` global keeps that
alive.

**Fix.** Wrap `peri_gyroscope.cpp` in `#ifdef BOARD_HAS_BHI260AP` (off by
default) and stub `BHI260AP_init()`/`BHI260AP_get_val()` to no-ops in
`peripheral.h`. Re-enable via a build flag when the sensor is fixed.

### 1.4 Menu icons have the same wrong pixel format — 46.5 KB
**Confidence: Measured.** **Impact: ~46,500 B flash.**

Ten 50×50 `LV_IMG_CF_TRUE_COLOR_ALPHA` icons at 5,012 B each = **50,120 B**:

```
img_A7682E  img_GPS  img_PCM5102  img_batt  img_lora
img_test    img_touch  img_wifi   img_SD    img_setting
```

At 1 bpp each becomes `ceil(50/8) × 50 = 350` B plus descriptor ≈ 362 B.
Ten icons: 50,120 → ~3,620 B.

Combined with §1.2, total image data drops **190,124 → ~12,500 B (a 177.6 KB
saving)**.

### 1.5 `lv_font_montserrat_26` is linked only as a fallback pointer — 32.9 KB
**Confidence: Measured + Verified in code.** **Impact: 32,897 B flash.**

No source file references `lv_font_montserrat_26` in a style, label, or the
font-picker tables. It enters the link through two generated bitmap fonts:

```
examples/factory/src/fonts_bitmap/lv_font_spleen_16x32.c:251:  .fallback = &lv_font_montserrat_26,
examples/factory/src/fonts_bitmap/lv_font_spleen_32x64.c:251:  .fallback = &lv_font_montserrat_26,
```

Both are fixed-pitch ASCII bitmap faces used for the lock-screen clock. A
proportional 26 px Montserrat is never a sensible fallback for them, and the
reader already folds non-ASCII to ASCII before display
(`reader_normalize_for_display`, `ui_deckpro.cpp:5620`).

Linked font cost for reference:

| Font | Bytes | Used directly? |
| --- | --- | --- |
| `lv_font_montserrat_26` | 32,897 | **no** — fallback only |
| `lv_font_spleen_32x64` | 25,168 | yes |
| `lv_font_montserrat_20` | 21,795 | yes (`ui_deckpro.cpp:5416,5421`) |
| `lv_font_montserrat_18` | 19,022 | yes |
| `lv_font_montserrat_16` | 15,899 | yes |
| `lv_font_montserrat_14` | 13,641 | yes |
| `lv_font_montserrat_12` | 11,480 | yes |
| all bitmap fonts | ~54,900 | yes (user-selectable faces) |

**Fix.** Set `.fallback = NULL` in those two files and
`#define LV_FONT_MONTSERRAT_26 0` in `config/lv_conf.h:365`. Every other
`.fallback` target (12/14/16/18/20) is genuinely used elsewhere, so leave them.

### 1.6 Precompiled ESP-IDF blobs that are linked but unused
**Confidence: Measured. Low actionability with the Arduino core.**

| Archive | Bytes | Pulled in by |
| --- | --- | --- |
| `libmesh.a` | 176,751 | `libnet80211.a(ieee80211_ioctl.o)` → `mesh_set_ie_crypto_config` |
| `libbtdm_app.a` + `libbt.a` | ~67,000 | `esp32-hal-misc.c` → `esp_bt_controller_mem_release` |
| `libsmartconfig.a` | 32,913 | `libWiFi.a(WiFiGeneric.cpp.o)` |
| `libwifi_provisioning.a` + `libprotocomm.a` | ~35,000 | `libWiFi.a(WiFiGeneric.cpp.o)` → `WIFI_PROV_EVENT` |

None of these features are used by the firmware. They are hard dependencies of
the *precompiled* Arduino-ESP32 WiFi libraries, so they cannot be removed by
changing this repo's code — it needs a custom ESP-IDF sdkconfig build
(`CONFIG_BT_ENABLED=n`, `CONFIG_ESP_WIFI_MESH_*=n`) or the
`framework-arduinoespressif32` "no-BT" variant. **Recorded for completeness;
not recommended as a first move** — ~310 KB, but it means owning a custom core.

### 1.7 Full float `printf`/`scanf` from newlib — ~47 KB
**Confidence: Measured. Medium actionability.**

```
_vfprintf_r      12,250
_svfprintf_r     11,979
_vfiprintf_r      8,354
_svfiprintf_r     8,130
__ssvfiscanf_r    8,077
```

`lv_conf.h:290` already sets `LV_SPRINTF_CUSTOM 1` pointing at `<stdio.h>`, so
LVGL routes into the full newlib formatter. `%f` is used in a handful of
diagnostics (`peri_gyroscope.cpp` commented-out logs, LoRa RSSI). Linking
`-Wl,-u,_printf_float` policy / nano-formatting on the ESP-IDF side is fiddly
under Arduino; the cheaper half is to drop `LV_SPRINTF_USE_FLOAT` (already 0)
and audit remaining `%f` uses. Flagged, not urgent.

### 1.8 Minor build-config cleanups
**Confidence: Verified in code. Impact: build time, clarity.**

- `platformio.ini` `lib_deps` lists **`EEPROM`**, which no file under
  `examples/factory/` includes. `libEEPROM.a` is compiled every build for
  nothing.
- `SD_MMC` and `FFat` are compiled (`libd69`, `libace`) via LDF chain
  resolution but never referenced. `--gc-sections` drops them from the image,
  so this is pure build-time cost. Setting `lib_ldf_mode = chain` and listing
  deps explicitly avoids both.
- `config/lv_conf.h:256` `LV_BUILD_EXAMPLES 1` and `:272`
  `LV_USE_DEMO_BENCHMARK 1`. Harmless *today* — PlatformIO only compiles
  `lib/lvgl/src`, so `lib/lvgl/demos` and `lib/lvgl/examples` never build — but
  both should be 0 so a future LDF/`library.json` change cannot silently pull
  in the benchmark demo.
- `factory.h` includes `"FS.h"` twice (lines 15 and 17).
- `boards/T-Deck-Pro.json` declares `-DARDUINO_TWATCH_S3_M10Q` and is named
  "LilyGo T-Watch S3 M10Q". Copy-paste from another board; harmless but
  misleading when grepping for board conditionals.
- `CLAUDE.md` states `-DTINY_GSM_MODEM_SIM7682` is set; the actual flag in
  `platformio.ini` and `factory.h:28` is `TINY_GSM_MODEM_SIM7672`. Doc drift.

---

## 2. RAM

Static internal DRAM: **`.dram0.data` 25,392 + `.dram0.bss` 121,952 = 147,344 B**
(plus `.iram0.text` 78,315). Against the 327,680 B the board profile advertises,
~45% is consumed before the heap opens.

Every block ≥ 1 KB in `.bss`:

| Bytes | Symbol | Object | Verdict |
| --- | --- | --- | --- |
| 49,152 | `work_mem_int$3696` | `liblvgl.a(lv_mem.c.o)` | LVGL's `LV_MEM_SIZE` heap — see §2.1 |
| 10,096 | `audio` | `factory.ino.cpp.o` | removable with §1.1 |
| 9,724 | `display` | `factory.ino.cpp.o` | GxEPD2 page buffer — see §2.2 |
| 8,192 | `reader_page_offsets` | `ui_deckpro.cpp.o` | move to PSRAM — see §2.3 |
| 4,116 | `_dfu_ctx` | `libarduino_tinyusb.a` | TinyUSB DFU runtime, unused |
| 4,096 | `_mscd_buf` | `libarduino_tinyusb.a` | **justified** — the USB-MSC screen uses it |
| 3,800 | `g_cnxMgr` | `libnet80211.a` | WiFi, justified |
| 2,776 | `ftm_initiator` | `libnet80211.a` | 802.11 fine-timing, unused (precompiled) |
| 1,025 | `reader_page_buf` | `ui_deckpro.cpp.o` | see §2.3 |
| 1,025 | `reader_disp` (fn-static) | `ui_deckpro.cpp.o` | see §2.3 |

### 2.1 LVGL's 48 KB static heap is one third of all static DRAM
**Confidence: Measured.** **Impact: up to 49,152 B.**

`config/lv_conf.h:49,52` — `LV_MEM_CUSTOM 0`, `LV_MEM_SIZE (48U * 1024U)`. LVGL
carves a fixed 48 KB array out of internal `.bss` at link time whether or not
the UI needs it.

Two options, with a real trade-off:

- **Keep it internal but right-size it.** Turn on `LV_USE_MEM_MONITOR 1`
  (`lv_conf.h:281`) for one session, read the high-water mark, and set
  `LV_MEM_SIZE` to that + ~25% headroom. On a 240×320 mono UI with no image
  cache (`LV_IMG_CACHE_DEF_SIZE 0`), 24–32 KB is a plausible landing spot.
- **Move it to PSRAM**: `LV_MEM_CUSTOM 1` with `ps_malloc`/`free`. Frees all
  48 KB of internal DRAM but pushes every widget allocation into QSPI PSRAM.
  Given the panel already costs ~700 ms per refresh (the repo's own figure,
  `factory.ino:54`), the allocation slowdown would be invisible — but it also
  makes LVGL contend with the draw buffer for PSRAM bandwidth.

Recommendation: measure first, shrink second. Only move to PSRAM if internal
DRAM actually becomes the binding constraint.

### 2.2 The GxEPD2 page buffer is 9.7 KB for a path that is almost never used
**Confidence: Measured + Verified in code.** **Impact: up to ~7 KB.**

```c
// factory.ino:46
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(...);
```

The second template parameter is the page height. `HEIGHT` (320) means one
full-screen page buffer: 240 × 320 / 8 = 9,600 B, matching `.bss.display`
= 9,724.

But the normal render path **bypasses it entirely** — `flush_epd_bitmap()`
writes `decodebuffer` straight to controller memory via
`display.epd2.writeImage()` (`factory.ino:232`), never touching
`firstPage()`/`nextPage()`. The paged buffer is used only by
`disp_hard_refresh()` and `disp_white_clear()`, both of which do nothing but
`fillScreen(GxEPD_BLACK/WHITE)`.

**Fix.** Change the page parameter to `GxEPD2_310_GDEQ031T10::HEIGHT / 8`
(40 rows, 1,200 B). The two `fillScreen` loops just iterate 8 pages instead of
1 — no visual change, and they are the rarest operations in the firmware.
Saves ~8.4 KB. (Or bypass GxEPD2's rasteriser there too and push a constant
`decodebuffer` — then the page size can go to `/16`.)

### 2.3 Reader statics sit in internal DRAM — 10.2 KB
**Confidence: Measured + Verified in code.** **Impact: ~10,240 B.**

```c
// ui_deckpro.cpp:5380-5385
#define READER_PAGE_BYTES   1024
#define READER_MAX_PAGES    2048
static char   reader_page_buf[READER_PAGE_BYTES + 1];      // 1,025 B
static size_t reader_page_offsets[READER_MAX_PAGES];       // 8,192 B
// ui_deckpro.cpp:5699
static char reader_display_buf[READER_PAGE_BYTES + 1];     // 1,025 B
```

All three are resident for the lifetime of the firmware even though the reader
is one screen among ~25. The offsets array is also the reason a book is capped
at 2,048 pages.

**Fix.** Allocate all three from PSRAM in `entry13_1` and free them in
`exit13_1`. That frees 10.2 KB of internal DRAM permanently *and* lets
`READER_MAX_PAGES` grow well past 2,048 for free.

### 2.4 TinyUSB DFU context — 4,116 B
**Confidence: Measured. Low certainty on removability.**

`_dfu_ctx` in `libarduino_tinyusb.a(dfu_device.c.obj)`. USB MSC is genuinely
used (`SCREEN_USB_MSC_ID`, `ui_usb_msc_begin()`), so `_mscd_buf` stays — but the
DFU *runtime* class is separate and this firmware never advertises it. Whether
it can be disabled without patching the core needs a check of
`CONFIG_TINYUSB_DFU_ENABLED`; flagged as a lead, not a confirmed win.

---

## 3. Runtime behaviour

### 3.1 The keypad polls I²C 200×/second, forever
**Confidence: Verified in code.** **Impact: power, I²C contention, CPU.**

```c
// peri_keypad.cpp
static void keypad_task(void *param) {
    const TickType_t period = pdMS_TO_TICKS(5);
    for (;;) { keypad_loop(); vTaskDelayUntil(&last, period); }
}
```

`keypad_loop()` takes `i2c0_lock()` and issues `keypad.getEvent()` — an
`Adafruit_TCA8418` register read — every 5 ms, whether or not a key was
pressed. That is 200 I²C transactions per second on a battery device that
spends most of its life showing a static e-paper page.

The board wires the TCA8418 interrupt: `utilities.h:36` defines
`BOARD_KEYBOARD_INT 15`, and it is never used anywhere in the sketch.

Second-order cost: because the task holds `i2c0_lock` 200×/s, every other I²C
user (fuel gauge, PMU, ALS) contends with it.

**Fix.** Attach a falling-edge ISR on `BOARD_KEYBOARD_INT` that gives a binary
semaphore; have `keypad_task` block on that semaphore with a long (250 ms–1 s)
timeout as a safety net, then drain the FIFO. Idle I²C traffic drops from 200/s
to ~1–4/s. The 5 ms cadence's stated purpose (surviving a >300 ms blocking EPD
refresh, per the comment at `peri_keypad.cpp`) is *better* served by the
interrupt — the FIFO fills and one wake drains it all.

Cheaper interim step: read `REG_KEY_LCK_EC` (the event-count register) first
and skip `getEvent()` when it reads 0. One register read instead of the full
event read; still 200 transactions/s, but shorter ones.

### 3.2 The GPS task runs at the highest priority in the system and busy-spins
**Confidence: Verified in code.** **Impact: scheduling, power.**

```c
// peripheral.h:4
#define GPS_PRIORITY (configMAX_PRIORITIES - 1)   // == 24
```

```c
// peri_gps.cpp:116-138
void gps_task(void *param) {
    vTaskSuspend(gps_handle);
    while (1) {
        while (Serial.available())    SerialGPS.write(Serial.read());
        while (SerialGPS.available()) { int c = SerialGPS.read(); if (gps.encode(c)) {...} }
        ...
        vTaskDelay(1);   // 1 tick == 1 ms
    }
}
```

Three problems:

1. **Priority 24** puts NMEA parsing above the keypad (19), the LoRa task (23),
   and everything else. A u-blox M10Q at 1 Hz produces a few hundred bytes per
   second; nothing about that is latency-critical. It should sit *below* the
   keypad, around `configMAX_PRIORITIES - 8`.
2. **1 ms poll loop** — 1,000 wakeups/second to drain a UART that has data ~1×
   per second. `SerialGPS.setRxTimeout()` / an event-queue-driven read, or
   simply `vTaskDelay(pdMS_TO_TICKS(20))`, cuts wakeups 20-fold with no
   functional change.
3. **It steals bytes from the USB console.** `while (Serial.available())
   SerialGPS.write(Serial.read())` drains `Serial` — and `a7682_task`
   (`factory.ino:519-534`) drains the *same* `SerialMon` in its own loop. When
   both are resumed, console input is split non-deterministically between the
   GPS and the modem. This is a correctness bug, not just an optimization: the
   GPS passthrough should be behind a debug flag or removed.

### 3.3 `vTaskSuspend(handle)` before `xTaskCreate` has written the handle
**Confidence: Verified in code.** **Impact: correctness (currently benign).**

Both `gps_task` (`peri_gps.cpp:118`) and `a7682_task` (`factory.ino:521`) begin
with `vTaskSuspend(<their own global handle>)`. Both are created at a priority
far above their creator (24 and 20 vs. the `peri_init` task's 1), so FreeRTOS
switches to them **immediately**, before `xTaskCreate()` returns and assigns the
global. The handle read is therefore uninitialised (`NULL` at boot).

It works only because `vTaskSuspend(NULL)` means "suspend the calling task",
which happens to be the intent. Any future change that gives those globals a
non-NULL value first, or lowers the new task's priority, turns this into
suspending the wrong task.

**Fix.** `vTaskSuspend(NULL)` explicitly, with a comment. One-line change,
removes a landmine.

### 3.4 Dictionary lookup reads the SD card one byte at a time
**Confidence: Verified in code.** **Impact: likely the slowest interactive path.**

`ui_dict_lookup()` (`ui_deckpro_port.cpp`) does a well-built binary search over
a sorted TSV — good design. But every byte goes through:

```c
static int dict_line_read(File &f, char *buf, size_t buf_size) {
    while (true) {
        int c = f.read();          // one-byte VFS -> FATFS -> SPI call
        ...
    }
}
```

and the "discard the partial line at the seek point" loops do the same:

```c
while (true) { int c = f.read(); if (c < 0 || c == '\n') break; }
```

Each `File::read()` is a full `read(fd, &c, 1)` through the VFS and FATFS
layers. A binary search over a 106 MB `eng-pob.tsv` is ~18 iterations, each
discarding a partial line (up to ~512 B) and reading a full line (up to 512 B),
then a linear scan of the final 512-byte window. That is on the order of
**20,000 single-byte VFS calls per word lookup**.

**Fix.** Read in blocks: `f.read((uint8_t*)chunk, 512)` into a small buffer and
scan for `\n` in memory. Same algorithm, ~40 SD transactions instead of ~20,000.
This is the single largest interactive-latency win in the firmware.

### 3.5 Reader resume is O(pages) with a full text layout per page
**Confidence: Verified in code.** **Impact: seconds of stall when resuming a book.**

```c
// ui_deckpro.cpp — reader_seek_to_offset()
while (idx + 1 < READER_MAX_PAGES) {
    size_t got = reader_read_one_page(off);   // SD read + wrap + layout
    ...
}
```

`reader_read_one_page()` → `reader_clip_to_visible()` → for **every** page it
walks:

```c
lv_obj_update_layout(reader_view_label);        // full layout recalc
while (pos < got) { _lv_txt_get_next_line(...); }  // full text wrap
```

Resuming at 60% of a 500 KB book means ~400 SD reads *and* ~400 full LVGL
layout passes before the first pixel appears.

Two independent fixes:

1. **Hoist the layout out of the loop.** `max_w`, `avail_h`, `line_h`,
   `letter_space`, `line_space` only change when the font, rotation, or bar
   visibility changes. Compute them once into a struct on those events and pass
   them into `reader_clip_to_visible()`. Removes ~400 `lv_obj_update_layout()`
   calls from a resume.
2. **Persist the page index with the bookmark.** `ui_reader_resume_set()`
   already writes `rd_off` and a per-book `b_%08x` key; add the page number and
   seed `reader_page_offsets[0] = saved_off` directly, walking backwards only
   if the user actually pages back past the start. Turns an O(n) resume into
   O(1). Invalidate on a font/geometry change by folding those settings into
   the bookmark key hash.

### 3.6 The e-paper is powered down after every single flush
**Confidence: Verified in code. Inferred impact.**

```c
// factory.ino:240
display.epd2.powerOff();
```

`flush_epd_bitmap()` calls `powerOff()` at the end of *every* flush, including
the small partial updates produced by typing a character or ticking the clock.
The next flush must run GxEPD2's `_PowerOn()` sequence again, which waits on the
BUSY line for the panel's DC-DC to come up.

For batch UI changes (typing in the notes editor, stepping through settings)
this pays the power-on cost on every keystroke.

**Fix.** Replace the unconditional `powerOff()` with a deferred one: an LVGL
one-shot timer, re-armed on each flush, that powers the panel down after
~2 s of display idleness. `ink_screen_prepare_shutdown()` already exists as the
explicit path for sleep/shutdown, so correctness is preserved.

I have not measured the per-flush power-on cost on this panel, so treat the
magnitude as unquantified — but the change is cheap and strictly reduces work.

### 3.7 `rounder_cb` inflates the dirty rectangle on both axes
**Confidence: Verified in code.** **Impact: minor; larger partial refreshes.**

```c
// factory.ino:249-255
static void display_driver_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area) {
    area->x1 &= ~0x7;  area->x2 |= 0x7;
    area->y1 &= ~0x7;  area->y2 |= 0x7;
}
```

`writeImage` needs 8-pixel alignment on the *panel's* X axis only. In portrait
that is logical X; in landscape (where the pack rotates 90°) it is logical Y.
Rounding both axes unconditionally grows every dirty rectangle by up to 7 px on
each of two extra edges — for a one-line label update, that can double the
refreshed area.

**Fix.** Branch on `s_landscape` and round only the axis that maps to panel X.
The `s_landscape` flag is already a file-scope static in the same translation
unit.

### 3.8 The gesture timer polls a disabled touch controller
**Confidence: Verified in code.** **Impact: small, but free to fix.**

```c
// ui_deckpro.cpp:4753
touch_chk_timer = lv_timer_create(indev_get_gesture_dir, LV_INDEV_DEF_READ_PERIOD, NULL);
```

`LV_INDEV_DEF_READ_PERIOD` is 10 ms (`lv_conf.h`), and LVGL independently calls
`touchpad_read` on the same period. Touch is hard-disabled:

```c
// factory.ino:774-776
// Touch driver disabled — the CST328 isn't responding on this V1.1 board
peri_init_st[E_PERI_TOUCH] = false;
```

`touchpad_read()` does early-out on `ui_setting_get_touch_status()`, but
`indev_get_gesture_dir` runs unconditionally 100×/s to read a gesture direction
that can never be set.

**Fix.** Pause `touch_chk_timer` (and skip registering the LVGL indev entirely)
when `ui_setting_get_touch_status()` is false; resume on toggle.

### 3.9 `ui_settings_save()` writes 26 NVS keys and commits after each one
**Confidence: Verified in code.** **Impact: latency on every settings toggle.**

`ui_settings_save()` (`ui_deckpro_port.cpp:160-193`) writes every setting —
26 keys including three `putString` calls — and is invoked from **24 call
sites**, including one-liners like:

```c
void ui_reader_bars_hidden_set(bool hidden) { default_reader_bars_hidden = hidden; ui_settings_save(); }
```

Arduino's `Preferences::putX()` calls `nvs_commit()` after *every* set. So one
checkbox toggle triggers 26 NVS writes and 26 commits.

NVS itself skips the flash write when the stored value is unchanged, which
bounds the flash-wear damage — but the 26 `nvs_set` + `nvs_commit` round-trips
still run, in the LVGL callback, on the UI thread.

**Fix.** Either (a) give each setter its own single-key save, or (b) set a
dirty flag and flush from a debounced one-shot `lv_timer` a second later. (b) is
~15 lines and fixes all 24 call sites at once.

### 3.10 `delay(500)` inside LVGL timer callbacks
**Confidence: Verified in code.** **Impact: UI freeze on two user actions.**

`ui_do_ping_1111()` (`ui_deckpro.cpp:4564`) and `ui_do_ntp_sync()` (`:4599`)
both call `delay(500)` — and ping additionally blocks for up to 2,000 ms inside
`ui_ping()`. These run from `menu_keypay_get_event`, i.e. inside
`lv_task_handler()` on the Arduino loop task, so the whole UI, all timers, and
`bq25896_runtime_maintain()` stall for ~2.5 s.

Both already show a "…" toast first, so the user sees *something* — but keypresses
during the stall queue up in the 32-entry ring and then all replay at once.

**Fix.** Replace the `delay(500)` with a one-shot `lv_timer` that re-checks
`ui_time_is_synced()` and updates the toast. For the ping's 2 s block, run it on
a short-lived worker task and post the result back through an LVGL message.

### 3.11 `shared_spi_unlock()` deasserts all CS lines even on a nested unlock
**Confidence: Verified in code. No current trigger found.**

```c
// factory.ino:93-99
void shared_spi_unlock(void) {
    if (shared_spi_mutex != nullptr) {
        shared_spi_release_all_cs();          // <- unconditional
        xSemaphoreGiveRecursive(shared_spi_mutex);
    }
}
```

The mutex is **recursive** (`xSemaphoreCreateRecursiveMutex`), so an inner
lock/unlock pair inside an outer critical section will drive every CS high
while the outer holder is still mid-transaction. I traced all 32
`shared_spi_lock()` call sites and found no nesting today, so this is latent,
not live — but it silently breaks the moment someone wraps an existing locked
helper.

**Fix.** Track depth and only release CS when the outermost unlock runs:

```c
static uint32_t s_spi_depth = 0;
// lock:   s_spi_depth++;
// unlock: if (--s_spi_depth == 0) shared_spi_release_all_cs();
```

Related: `shared_spi_prepare_device(int cs_pin)` (`factory.ino:101-107`) calls
`shared_spi_release_all_cs()` — which already drives `cs_pin` HIGH — and then
writes `cs_pin` HIGH again. The second write is a no-op. The function name
promises it selects a device; it does not (drivers assert their own CS). Rename
or drop the redundant write.

---

## 4. Flash layout / storage

### 4.1 ~10 MB of the 16 MB flash is unreachable
**Confidence: Measured + Verified in config.** **Impact: +10 MB usable storage.**

`boards/T-Deck-Pro.json` selects `default_16MB.csv`:

```
nvs,      data, nvs,      0x9000,   0x5000
otadata,  data, ota,      0xe000,   0x2000
app0,     app,  ota_0,    0x10000,  0x640000     # 6.5 MB
app1,     app,  ota_1,    0x650000, 0x640000     # 6.5 MB  <- never written
spiffs,   data, spiffs,   0xc90000, 0x360000     # 3.4 MB
coredump, data, coredump, 0xFF0000, 0x10000
```

The firmware is 1.79 MB. There is no OTA path in this codebase — no
`Update.h`, no `esp_ota_*`, and the Makefile flashes over USB/UART at `0x0`.
So:

- `app1` — **6.5 MB, entirely dead.**
- `app0` — 4.7 MB unused tail.
- Content (books, `eng-pob.tsv`, MP3s) is squeezed into 3.4 MB of SPIFFS.

**Fix.** Add a `partitions_tdeckpro.csv` with a single 2.5 MB app partition and
~13 MB of SPIFFS, and point `board_build.partitions` at it in
`platformio.ini`. That is ~10 MB more content storage on hardware you already
own, for one file and one line. Note it makes future OTA impossible without
another repartition — a fair trade given none exists.

Also worth fixing while there: `Makefile:SPIFFS_OFFSET := 0xc90000` is
hard-coded and would silently write to the wrong offset after a repartition.
Derive it from the CSV.

### 4.2 Multi-gigabyte data files sit untracked in the repo root
**Confidence: Measured.** **Impact: risk, not size.**

```
2,929,563 KB  kaikki.org-dictionary-English.jsonl
  106,146 KB  eng-pob.tsv
```

Neither is in `.gitignore`. They show as untracked in `git status`, so any
`git add -A` commits ~3 GB. (Your global rules already forbid `git add -A`,
which is what has saved this so far.) `eng-pob.tsv` is the runtime dictionary
the firmware reads from `/dict/` on SD; the `.jsonl` is presumably the source it
was generated from.

**Fix.** Add both to `.gitignore`, and note in `CLAUDE.md` how `eng-pob.tsv` is
derived from the kaikki dump so the generation step is reproducible.

---

## 5. Code structure

These do not change the binary much, but they are what makes the above hard to
fix.

### 5.1 Twenty near-identical `scrX_kb_timer_cb` functions
**Confidence: Verified in code.**

`ui_deckpro.cpp` creates a per-screen 20 ms keypad-poll timer for ~20 screens:
`scr1_kb_timer_cb`, `scr1_1_kb_timer_cb`, `scr1_2_kb_timer_cb`,
`scr2_1_kb_timer_cb`, `settings_kb_timer_cb`, `scr3_kb_timer_cb`,
`scr4_kb_timer_cb`, `scr4_1_kb_timer_cb`, `test_kb_timer_cb`,
`scr6_kb_timer_cb`, `scr6_1_kb_timer_cb`, `scr6_2_kb_timer_cb`,
`a7682_kb_timer_cb`, `pcm5102_kb_timer_cb`, `notes_editor_kb_timer_cb`,
`dict_kb_timer_cb`, `reader_kb_timer_cb`, `reader_view_kb_timer_cb`,
`system_font_kb_timer_cb`, …

To their credit these *are* correctly torn down — I checked every `entry`/`exit`
pair and each one deletes its timer and nulls the pointer, so there is no
accumulation. But the boilerplate is ~20 copies of the same
`keypad_get_val()` / `switch (key)` / `keypad_set_flag()` shape.

**Fix.** One global 20 ms keypad timer that dispatches to a
`scr_lifecycle_t::on_key(char)` hook, added to the struct in `ui_scr_mrg.h`.
The screen manager already tracks the current screen, so dispatch is one
indirect call. Removes ~20 timer create/delete pairs and ~600 lines.

### 5.2 `ui_deckpro.cpp` is 6,308 lines
**Confidence: Measured.**

It holds all 25 screens, the reader, the notes editor, the dictionary UI, the
lock screen, the taskbar, and the toast system. `peripheral_init_task()` in
`factory.ino` compiles to a single 6,722-byte function
(`00001a42 t peripheral_init_task(void*)`) because everything inlines into it.

Splitting per feature area (`ui_reader.cpp`, `ui_notes.cpp`, `ui_settings.cpp`,
`ui_lora.cpp`, …) would cut incremental build time substantially — this file
alone produces a 1.3 MB object.

### 5.3 The Hynitron touch stack compiles into a build with touch disabled
**Confidence: Measured.**

`hyn_touch.cpp`, `hyn_i2c.cpp`, `hyn_cst3xx.c`, `hyn_cst226se.c`,
`hyn_cst66xx.c`, `hyn_ts_ext.c` contribute **5,878 B** to the linked image while
`peri_init_st[E_PERI_TOUCH]` is hard-coded `false`. Small in flash terms, but
it is ~1,000 lines of driver compiled on every build for a disabled peripheral,
and `examples/touch_hyn_core/` already exists as the standalone version.

Guard behind the same `BOARD_HAS_TOUCH` flag suggested for the gyro.

---

## 6. Suggested order of work

Grouped by effort-to-payoff. Each is independent.

**Tier 1 — mechanical, high payoff (~554 KB flash, ~10 KB RAM, +10 MB storage):**

1. Re-encode `img_start` + the 10 menu icons as 1-bit — **~178 KB**, no code change.
2. `#ifdef` out `peri_gyroscope.cpp` — **103 KB**, ~5 lines.
3. `.fallback = NULL` on the two Spleen fonts + `LV_FONT_MONTSERRAT_26 0` — **33 KB**, 3 lines.
4. Guard the audio stack behind `BOARD_HAS_PCM5102A` — **~240 KB flash + 10 KB RAM**, ~20 lines.
5. New partition CSV — **+10 MB content storage**, one file.

**Tier 2 — small code changes, real runtime effect:**

6. Buffered reads in `dict_line_read()` — largest interactive-latency win (§3.4).
7. Interrupt-driven keypad on `BOARD_KEYBOARD_INT` — 200 I²C txn/s → ~2 (§3.1).
8. Drop GPS task priority and its 1 ms spin; remove the `Serial` passthrough race (§3.2).
9. Hoist `lv_obj_update_layout()` out of the reader page loop (§3.5).
10. Debounce `ui_settings_save()` (§3.9).
11. Deferred `powerOff()` after EPD flush (§3.6).

**Tier 3 — correctness hardening (do alongside Tier 2):**

12. `vTaskSuspend(NULL)` in both tasks (§3.3).
13. Depth-counted `shared_spi_unlock()` (§3.11).
14. Axis-aware `rounder_cb` (§3.7).
15. Pause the gesture timer when touch is off (§3.8).
16. Replace `delay(500)` in the ping/NTP callbacks with one-shot timers (§3.10).

**Tier 4 — structural:**

17. Single keypad timer + `on_key` lifecycle hook (§5.1).
18. Split `ui_deckpro.cpp` by feature (§5.2).
19. Move reader buffers to PSRAM (§2.3); right-size `LV_MEM_SIZE` (§2.1);
    shrink the GxEPD2 page buffer (§2.2).
20. `.gitignore` the dictionary dumps (§4.2); fix the doc drift in §1.8.

---

## 7. Dead code & simplification audit (2026-10-25)

Second pass, against **HEAD `799e7e0` plus the uncommitted working tree** (tethering
env/Makefile changes). Scope: `examples/factory/` (all files), `examples/tethering/`,
`platformio.ini`, `Makefile`, `config/lv_conf.h`, `.gitignore`, `tools/`. Vendored
`lib/` is out of scope.

**Method**

1. A fresh instrumented build (separate dir, `platformio.ini` untouched):
   ```bash
   PLATFORMIO_BUILD_DIR=/tmp/tdeck-audit-build \
   PLATFORMIO_BUILD_FLAGS="-Wunused-function -Wunused-variable -Wunused-but-set-variable \
     -Wunused-parameter -Wunused-label -Wunused-value -Wl,--print-gc-sections" \
   pio run -e T-Deck-Pro          # SUCCESS, firmware.bin 1,305,120 B
   ```
   - The compiler output is the evidence for unused statics and locals.
   - `--print-gc-sections` shows each non-static function or global the linker
     discarded because nothing reachable references it.
2. A per-symbol reference sweep of every file in scope, using `graft grep`/`callers`
   and `grep -w` over `examples/factory/*.{c,cpp,h,ino}`.
   - **Caveat:** graft does not index `factory.ino`. Any "no callers" result from
     graft was cross-checked against `factory.ino` with grep.
   - Functions called only from `factory.ino` look dead in graft but are not:
     `lora_init`, `LTR553_init`, `gps_init`, `keypad_task_create`,
     `ink_screen_prepare_shutdown`.
3. Line-by-line reads of every call site that decides liveness, such as screen
   registration vs. push, and `#ifdef` guards vs. the default build flags.

**Default build flags matter.** `[env:T-Deck-Pro]` defines none of `BOARD_HAS_TOUCH`,
`BOARD_HAS_BHI260AP` or `BOARD_HAS_PCM5102A`. The modem flag is
`TINY_GSM_MODEM_SIM7672`; `CLAUDE.md`/`GEMINI.md` say `SIM7682`, which is doc drift.
Everything behind those three flags is a stub in the shipped image.

**Confidence tags** follow §0:
- *Compiler*: a `-Wunused-*` warning.
- *Linker*: discarded by `--gc-sections`.
- *Verified*: the full reference set was read.
- *Likely*: reasoned, not instrumented or reproduced on hardware.

**Status of earlier sections.** The fresh build is 1,305,120 B, against 1,795,824 B
when §0 was written. Commits `c6ee822`, `4d0f1f3`, `71ce28e` and `799e7e0` already
landed most of Tier 1 and §5.1–§5.3. For example, the `hyn_*` objects now link
0 bytes, there is a global `global_kb_timer_cb` dispatch, and `ui_deckpro.cpp` is
split. This section only lists what is **still** present.

### 7.0 Summary

| Bucket | Items | Rough size |
| --- | --- | --- |
| Real bugs found while auditing (§7.1) | 10 | fix first, independent of the cleanup |
| Dead code: compiler/linker-proven (§7.2) | 37 warnings + 24 discarded functions | ~600 lines |
| Dead code: unreachable screens / features (§7.3) | 6 screens + gesture stack + touch stack | ~1,000–1,400 lines |
| Dead code: write-only state, stale decls, no-op code (§7.4) | ~60 | ~300 lines |
| Simplification (§7.5) | 25 | ~1,000–1,200 lines |
| Build / config / tooling (§7.6) | 15 | **~43–53 KB flash** (LVGL), faster builds |

The firmware source in scope is ~14k lines. A realistic target is **~2,500 fewer
lines** with no behaviour change, apart from the bug fixes.

---

### 7.1 Real bugs found along the way — fix these first

These are not cleanup. Each one is user-visible or a memory-safety issue.

**B1. Taskbar use-after-free after leaving any LoRa screen.** *Verified (code path); not reproduced on hardware.*
- **Where:** `destroy1`, `destroy1_1` and `destroy1_2` are empty (`ui_lora.cpp:115`,
  `:286`, `:549`). All three screens call `ui_taskbar_create()` (`ui_lora.cpp:66`,
  `:209`, `:487`), which repoints the global `menu_taskbar` and its labels.
- **How it happens:**
  - Every other screen does `lv_obj_del(menu_taskbar); menu_taskbar = NULL;` in
    `destroy` (e.g. `ui_reader.cpp:273-277`).
  - `scr_mgr_pop()` (`ui_scr_mrg.c`) calls `destroy`, then the parent's `entry`,
    then `lv_obj_del(cur_obj)`.
  - After Settings → LoRa → Esc, `menu_taskbar` still points into the deleted
    screen.
  - The parent's `entry` writes the battery labels under `if (menu_taskbar)`, and so
    does `menu_taskbar_update_timer_cb` (`ui_deckpro.cpp:3303`). Both write freed
    LVGL memory.
- **Related:** on every *other* pop, `menu_taskbar` becomes NULL, so the parent's
  taskbar silently stops updating. Only `entry0` re-points the globals
  (`ui_deckpro.cpp:602`).
- **Fix:** each screen keeps its own taskbar handle, and `entry` re-points the
  globals. Put this in a `ui_taskbar_enter()`/`ui_taskbar_leave()` helper pair
  (S3, §7.5), which also deletes ~21 copies of boilerplate.

**B2. `scr_mgr_switch()` leaks every screen above the stack root.** *Verified (code path).*
- `ui_scr_mrg.c:139-171`: `curr_obj` is overwritten on each loop iteration, so only
  the **root** card's `obj` is `lv_obj_del`'d.
- `lv_scr_load()` uses `auto_del=false`, and the `destroy` callbacks don't delete the
  screen object. Each switch issued from a stack ≥2 deep therefore leaks the top
  screen and every intermediate one from the LVGL heap (48 KB, §2.1).
- Lines 139-145 also duplicate one iteration of the loop below them.
- **Fix:** `lv_obj_del` each non-top card's `obj` inside the loop, delete the old
  top's `obj` after `lv_scr_load`, and drop the duplicated prologue.

**B3. Capital `E` cannot be typed.** *Verified.*
- Enter emits `'E'` (`peri_keypad.cpp:31`, also `:48`, `:58`), and Shift+e also
  emits `'E'` (`:56`).
- Every consumer treats `'E'` as Enter: `ui_notes.cpp:201`, `ui_reader.cpp:53/221/1263`,
  `ui_lora.cpp:382`, and `ui_deckpro.cpp:1024/1395/1599/1972/2708/3106`.
- So Shift+E in the notes editor or the WiFi password field submits instead of
  typing.
- **Fix:** map Enter to `'\r'` (or a `KEY_ENTER` sentinel ≥0x80) and update the ~12
  comparisons.

**B4. No keypad exit from screens 8.1 (Call test), 8.2 (AT test) and 9 (Shutdown while on USB).** *Verified.*
- Their `scr_lifecycle_t` has no `.on_key` (`ui_deckpro.cpp:2877`, `:2915`, `:2972`).
  `global_kb_timer_cb` returns when `on_key` is NULL (`ui_deckpro.cpp:3683`).
- Their only exit is the touch-only `scr_back_btn_create` button, and touch is
  disabled, so the user is stuck until reset.
- **Fix:** give them the shared ESC-pops handler (S2).

**B5. Turning GPS on in Settings does nothing if it was off at boot.** *Verified.*
- `gps_init()` only runs at boot when the setting is on (`factory.ino:720`).
- `ui_setting_set_gps_status(true)` (`ui_deckpro_port.cpp:459-463`) only calls
  `gps_task_resume()` on a NULL `gps_handle`.
- **Fix:** lazily `gps_init()` from the setter.

**B6. A failed LoRa config step freezes the whole UI.** *Verified.*
- The `while (true);` in each of the 12 config steps of `lora_init` /
  `lora_param_set` (`peri_lora.cpp:47-119`, `:234-258`) spins **while holding the
  shared SPI mutex**.
- The EPD flush needs that mutex, so the device hangs with a stale screen.
- **Fix:** see S12.

**B7. Two sensor inits skip `i2c0_lock`.** *Verified.*
- `LTR553_init()` (`peri_ltr553.cpp:10`) and `BHI260AP_init()` (gated) run from
  `peripheral_init_task` while the keypad task is already live.
- That is exactly the TwoWire corruption race `peripheral.h:13-17` warns about.
- The `ui_batt_25896_*` getters (`ui_deckpro_port.cpp:1129-1187`) are also unguarded,
  although the comment at `:1189-1197` claims all battery helpers are.

**B8. Modem init miscounts and runs when its rail is off.** *Verified.*
- `A7682E_init` returns `retry < 5` but loops up to 7 times, so a modem that answers
  on try 6 or 7 is reported as failed.
- It also runs ~7 s of `testAT` and PWRKEY pulses even when `BOARD_6609_EN` is off
  by setting.
- Likewise, LoRa init runs and transmits "Hello World!" with its rail disabled.

**B9. LoRa settings are marked dirty but never persisted.** *Verified.*
- `ui_lora_set_freq/bandwidth/power` (`ui_deckpro_port.cpp:393-397`) call
  `settings_mark_dirty()`, but `ui_settings_save/load` (`:256-361`) have no LoRa keys.
- Each change rewrites all 26 NVS keys and saves nothing.
- `set_bandwidth(float)` and `set_power(float)` also store into `int`.
- **Fix:** add `lr_freq/lr_bw/lr_pwr` keys (or drop the dirty calls), and make the
  parameters `int`.

**B10. Smaller correctness slips.** *Verified unless noted.*
- `ui_other_get_gyro` (`ui_deckpro_port.cpp:1356`) NULL-checks `gyro_x` three times
  instead of x/y/z.
- `scr_mgr_pop` always returns `false`, even on success (`ui_scr_mrg.c:244`).
- `show_wifi_scan` passes `BUFF_LEN`, not `BUFF_LEN - offs`, to each appending
  `lv_snprintf`. The function is dead anyway (D-S5).
- `ui_setting_get_sd_capacity` (`ui_deckpro_port.cpp:561-585`):
  - its `printf` dereferences `*total`/`*used` even when the caller passed NULL;
  - it calls the slow `SD.usedBytes()` (a FAT walk) twice.
- `ntp_synced` is effectively always true: it is set at every boot by
  `ui_time_persist_restore` (`:924`) and again by `ui_time_get_local` when year
  ≥ 2024 (`:841-843`). The resulting "Time Synced!" toast before SNTP answers is
  *Likely*. **Fix:** use `sntp_get_sync_status()`.
- `ui_wifi_set_enabled` writes NVS directly (`:1094-1100`). The lock screen toggles
  it on every lock/unlock (`ui_lockscreen.cpp:393-410`), which costs 2 NVS writes
  per cycle and leaves WiFi off after a power loss while locked.
  **Fix:** split it into a non-persisting `ui_wifi_radio_set()`.
- A SPIFFS mount failure `return`s from `setup()` (`factory.ino:854-857`) before the
  display, keypad and LVGL init, so the user sees a blank device.
- `T_DECK_PRO_V1_0` is hard-defined (`utilities.h:10`), so a V1.1 board prints
  "Firmware mismatch" on every boot (`factory.ino:844-851`). *Likely.*

---

### 7.2 Dead code proven by the compiler and linker

These are delete-only changes. The toolchain guarantees nothing references them.

**Compiler `-Wunused-*` (37 warnings, all in `examples/factory/`).**

| File | Unused functions | Unused variables |
| --- | --- | --- |
| `ui_deckpro.cpp` | `menu_btn_create` :326, `scr2_btn_event_cb` :829, `setting_page_switch_cb` :901, `scr4_btn_event_cb` :1276, `a7682_page_switch_cb` :2637, `scr9_btn_event_cb` :2926, `pcm5102_page_switch_cb` :3035, `scr11_btn_event_cb` :3195 | `scr2_1_cont` :656, `tgt` :838/:2598/:2996, `alt`/`accuracy`/`usat` :1108-1111, `back4_label` :1796, `scr6_lab_buf` :2032, `code` :2036, `len` :2811, `back8_1_label` :2864, `back8_2_label` :2902, `shutdown_timer` :2924, `menu_keypad` :3283, `wifi` :3320 |
| `ui_notes.cpp` | `scr12_back_btn_event_cb` :60, `notes_save_btn_event_cb` :219, `notes_editor_back_btn_event_cb` :226 | — |
| `ui_reader.cpp` | `scr13_back_btn_event_cb` :202, `scr13_1_back_btn_event_cb` :463 | — |
| `ui_lora.cpp` | — | `scr1_lab_buf` :13, `code` :17/:406 |
| `ui_deckpro_port.cpp` | — | `oper` :1283 |
| `ui_scr_mrg.c` | `scr_mgr_get_top` :47 | — |
| `peri_gps.cpp` | — | `gps_altitude` :20, `ubxFrame` :409 |
| `factory.ino` | `pcm5102a_init` :104, declared `static` but never defined | — |

**Linker `--gc-sections`.** The functions below have external linkage, so the
compiler cannot warn about them, but nothing reachable calls them.

| File | Discarded symbols |
| --- | --- |
| `ui_deckpro_port.cpp` | `ui_setting_get_language`, `ui_setting_set_language`, `ui_gps_task_suspend`, `ui_wifi_get_ssid`, `ui_wifi_get_password`, `ui_wifi_get_tz`, `ui_test_a7682e`, `ui_test_pcm5102a`, `ui_battery_27220_get_design_capacity`, `ui_usb_msc_is_active`, `ui_notes_read` (28 lines), `ui_notes_delete` (16), `ui_reader_read` (41), `ui_reader_size` (27), `ui_other_get_LTR` (18, not even declared) |
| `ui_deckpro.cpp` | `ui_auto_timer_cb` + `menu_buf` + `cursor` (auto-test; its only `lv_timer_create` is commented out at :3763), `ui_wifi_get_selected_ssid` |
| `peri_gps.cpp` | `setupGPS()` (`:353-403`). This is an L76K init sequence; the board has a u-blox MIA-M10Q, and the only call is commented out at `:96`. |
| `peri_keypad.cpp` | `keypad_regetser_cb` + `keypad_listener` (written, never invoked) |
| `peri_ltr553.cpp` | `LTR_553ALS_get_ps`, `LTR_553ALS_get_channel` |
| `hyn_touch.cpp` | `hyn_touch_init`, which has **zero callers in any configuration** (see D-T1) |
| `ui_scr_mrg.c` | `scr_mgr_set_bg_color` |
| `src/` assets | `img_A7682E`, `img_batt`, `img_GPS`, `img_lora`, `img_PCM5102`, `img_test`, `img_touch`, `img_wifi`: 8 files declared only in `assets.h`, never used |

Two discarded entries are **not** dead: `ui_test_sd_card` and `is_chinese_utf8`. They
were inlined into their callers, so only the out-of-line copy was dropped. Make
them `static`.

**Header declarations with no definition.** *Verified.*
- `ui_test_pcm5102` (`ui_deckpro_port.h:146`). The definition is spelled `…5102a`.
- `ui_battery_27220_get_remaining_capacity` (`:172`). The definition is
  `…_remain_capacity`.
- `hyn_core.h:233-239` declares `extern` drivers that are never defined: `cst1xx`,
  `cst7xx`, `cst8xxT`, `cst92xx`, `cst3240`.

**Fix:** delete everything in this subsection. ~600 lines, 0 bytes of flash (the linker
already drops them), but far less noise in search and review.

---

### 7.3 Unreachable screens and features

These compile and link, but no runtime path reaches them in the default build.

**D-S1. Screen 2.2 "Hidden Apps".** *Verified.*
- **Where:** `ui_deckpro.cpp:736-794`, registered at :3773. `SCREEN2_2_ID` has 2
  references: the enum and the register call. Nothing pushes it.
- **Also removable:** `menu_btn_list`, `MENU_BTN_NUM`, `app_hidden_status` and the
  `APP_HIDDEN_STATUS_DEFINED` guard (:235, :271-283), `struct menu_btn`
  (`ui_deckpro.h:91-95`), and the `img_setting`/`img_SD` assets (only referenced from
  `menu_btn_list`; 362 B each).
- ~85 lines.

**D-S2. Gesture/swipe stack.** It does nothing in any build. *Verified.*
- `menu_get_gesture_dir` (:291-324) starts with `if (MENU_BTN_NUM <= 9) return;`, and
  `MENU_BTN_NUM` is 5.
- Never assigned: `menu_screen1`, `menu_screen2`, `ui_Panel4`, `page_num`,
  `page_curr`.
- Also dead: `ui_get_gesture_dir`, `indev_get_gesture_dir` (:3285-3297),
  `touch_chk_timer` (:27, :613-615, :628, :3753), and the `ui_indev_read_cb` typedef.
- If the user enables "Touchscreen", the timer starts firing a function that returns
  at once.
- ~110 lines.

**D-S3. Auto-test sequencer** (`ui_auto_timer_cb`, `menu_buf`, `cursor`, `SCREEN_POP`,
:3399-3433). Linker-proven, ~35 lines.

**D-S4. Screen 12, the notes list** (`ui_notes.cpp:15-124`). *Verified.*
- `SCREEN12_ID` is only in the enum and the register call. The menu and the `n`
  shortcut push `SCREEN12_1_ID` directly.
- Knock-on effects:
  - `notes_use_sd` is always `true`.
  - `notes_selected_file` is write-only, so the editor never loads an existing note.
  - `ui_notes_read`/`ui_notes_delete` are dead (§7.2).
- **Product decision:** either wire the list into the menu and load the selected
  file in `entry12_1`, or delete the screen and its externs (~110 lines + ~45 in the
  port file).

**D-S5. Screen 4.2, the synchronous WiFi scan** (`ui_deckpro.cpp:1725-1820`). *Verified.*
- It is only pushed from the dead `menu_buf`.
- Its sole callee, `ui_wifi_get_scan_info` (`ui_deckpro_port.cpp:624-693`, 70 lines),
  is a copy of the live async `wifi_scan_collect`. The comment at `:695-697` wrongly
  says the two share code.
- ~165 lines.

**D-S6. Paging code that cannot run.** *Verified by constants.*
- The Test (12 items), A7682 (3) and PCM5102 (1) lists are all ≤
  `SETTING_PAGE_MAX_ITEM` (15).
- So `test_page_switch_internal` (:1847-1872), `a7682_page_switch_internal`
  (:2609-2635) and `pcm5102_page_switch_internal` (:3007-3033) always return on their
  first line.
- Their `*_page_num` variables stay 0 and their shortcut `while` loops never run.
- The `a7682_page`/`pcm5102_page`/`setting_page` pointers are never assigned. Only
  Settings (21 items) really pages.
- ~110 lines; better still, fold into S1.

**D-T1. Hynitron touch stack: dead in every configuration, not just the default one.** *Verified.*
- `hyn_touch_init()` has no caller, and `setup()` hard-sets
  `peri_init_st[E_PERI_TOUCH] = false` (`factory.ino:868`).
- So even with `-DBOARD_HAS_TOUCH`, `hyn_data` stays NULL and
  `hyn_touch_get_point()` returns 0.
- Inside the stack:
  - `hyn_cst66xx.c` (279 lines) and `hyn_cst226se.c` (253) drive chips this board
    doesn't have. The CST328 uses `cst3xx`. Worse, they are probed *first* and send
    foreign protocols to the CST328.
  - Six `hyn_ts_ext.c` helpers have no callers: `hyn_sum32`, `copy_for_updata`,
    `hyn_wait_irq_timeout`, `hyn_get_threshold`, `factory_multitest`,
    `fac_test_log_save`.
  - `hyn_write_data` and `gpio_get_value` are uncalled.
  - The non-Arduino `#else` branches in `hyn_i2c.cpp` can never compile here.
  - `gpio_evt_queue` is filled by the ISR, and its consumer task is commented out.
  - `hyn_touch.cpp:8-9` includes GxEPD2 and a font for nothing.
- **Option A, drop touch:** delete `hyn_*`, `touchpad_read`, the pointer-indev
  registration (`factory.ino:344-369`, `:397-405`) and the Touchscreen setting.
  ~1,000 lines.
- **Option B, keep it revivable:** list only `{&cst3xx_fuc}`, delete
  cst66xx/cst226se, the dead ext helpers and the IDF branches (~620 lines), and add
  `#ifdef BOARD_HAS_TOUCH peri_init_st[E_PERI_TOUCH] = hyn_touch_init(); #endif` so
  the flag actually works.
- **Never gate `BOARD_1V8_EN` either way.** It powers the touch IC *and* the gyro.

**D-T2. `TouchDrvCSTXXX touch`** (`factory.ino:47`). *Verified.*
- It is constructed but `begin()` is never called. Its single use, `touch.sleep()`
  (`ui_deckpro.cpp:3211`), returns at once on a NULL `drv`.
- **Fix:** remove the global, the includes (`factory.ino:14`,
  `ui_deckpro_port.cpp:18`, `ui_deckpro.cpp:3194`), the externs and the call.

**D-F1. Features that are stubs in the 4G build but still appear in the UI.** *Verified.*
- **Screen 10, PCM5102** (`ui_deckpro.cpp:2979-3191`, ~210 lines) and its Settings
  row (:820). The only action calls a stub that returns `false`.
- **"Power Gyro" (:811) and "Touchscreen" (:813) settings.**
  `ui_setting_set_gyro_status` only persists a bool that no driver reads.
- **Test rows** Touch, PCM5102A and BHI260AP (:1830, :1835, :1838) always show
  "----".
- **Screen 7:** the gyro block redraws "0.000" every 3 s. The comment at :2461 says
  2 s, but 60 × 50 ms is 3 s.
- **Fix:** `#ifdef BOARD_HAS_PCM5102A` / `BOARD_HAS_BHI260AP` / `BOARD_HAS_TOUCH`
  around the rows, the screens and their `scr_mgr_register`. Keep the `E_PERI_*`
  enum values, because the test table indexes them.

---

### 7.4 Write-only state, stale declarations, no-op code

**`factory.ino` / `factory.h` / `peripheral.h` / `utilities.h`.** *Verified.*
- **Unused globals:**
  - `Preferences preferences` (:26); the port uses local `prefs`.
  - `Version_str1`/`Version_str2` (:65-66).
  - `listDir()` (:675-704, 30 lines).
  - `tick` plus the `loop()` block containing only commented-out printfs
    (:890, :908-916).
- **Unneeded includes:** `esp_pm.h` (:24), `TinyGPS++.h` (:15), and
  `FreeMonoBold9pt7b` (:18).
  - The `setFont`/`setTextColor` calls at :149-151 are dead because nothing calls
    `display.print`.
  - The `WIDTH < 104` branch is constant-false.
  - Removing them frees ~1.8 KB of flash.
- **Unused definitions:** `union flush_buf_pixel` and `MODEM_GPS_ENABLE_GPIO`
  (`factory.h:52-65`, `:33`); `LORA_PRIORITY`, `WS2812_PRIORITY`,
  `BATTERY_PRIORITY`, the commented `LORA_FREQ` and `E_PERI_MIC` (`peripheral.h`);
  and the `keypad_cb` typedef.
- **Should be `static`:** `keypad_loop`, `gps_task_create` and `i2c0_lock_init` are
  only called inside their own file.
- **Redundant code:**
  - The `#ifdef BOARD_HAS_BHI260AP` at `factory.ino:739-743`; `peripheral.h:73`
    already stubs the function.
  - `peri_init_st[...] = false` at :732, :754 and :868; the array is
    zero-initialised.
  - The `!i2cIsInit(0)` branch in `keypad_init` (`peri_keypad.cpp:142-146`);
    `setup()` already began `Wire`.
- **Pin macros never used** (`utilities.h`):
  - `BOARD_ALS_SCL/SDA/INT`, `BOARD_GYROSCOPDE_SCL/SDA`, `BOARD_EPD_SCK/MOSI`,
    `BOARD_SD_SCK/MOSI/MISO`, `BOARD_LORA_SCK/MOSI/MISO`, `BOARD_A7682E_RI/ITR/RST`,
    `BOARD_MIC_DATA/CLOCK`.
  - These document the board, so fold them into a comment block rather than delete
    them.
- **`peri_ltr553.cpp:21-46`:** programs ALS threshold/IRQ/persist for an INT pin
  nothing reads (~25 lines).
- **Write-only:** `keypad_state` (`peri_keypad.cpp:130`).
- **Unused buffer:** `gps_altitude`. `buffer[256]` in `peri_gps.cpp:27` is a
  non-static global with a generic name; make it a local in `GPS_Recovery`.
- **`peri_gyroscope.cpp` (gated):** `magn_process_callback` is registered but
  `SENSOR_ID_MAG_PASS` is never configured.

**`ui_deckpro_port.cpp` / `ui_scr_mrg.*`.** *Verified.*
- **The language feature is dead:** `default_language`, the `"lang"` NVS key and
  `DEFAULT_LANGUAGE_CN`.
- **Should be `static` (non-`volatile`):** the nine `volatile default_*` settings
  globals (:30-38), plus `scr_mgr_head`/`top`, `scr_stack_*`, `scr_anim_*` and
  `default_bg_color`.
- **`ui_battery_27220_get_input`** is an alias of
  `ui_battery_is_external_power_present`.
- **Screen-manager animation is never used.** Every push/pop/switch passes `false`.
  - Remove the `lv_scr_load_anim` branches, `scr_anim_*`, `SCR_MGR_ANIM_TIME`, the
    `*_ANIM` macros and `scr_mgr_set_anim` (+1 call).
  - ~30 lines.
- **Dead enum branch:** `SCR_MGR_STATE_DESTROYED`/`ACTIVE_BG` are never assigned,
  so the branch at `ui_scr_mrg.c:59-63` is dead.
- **Unused return values:** no caller reads the `bool` from
  register/switch/push/pop.

**`ui_deckpro.cpp`.** *Verified.*
- **Never assigned or never read:** `LOW_VOLTAGE_SOC_THRESHOLD`/`soc_low` (always
  false; only used by commented-out :117), `scr4_1_focus` (only ever 0, so the
  button-focus state and loop at :1523-1532 are constant), `prev_id` (:3688-3690),
  and the struct fields `ui_a7682_handle.st`, `ui_pcm5102_handle.st` and
  `ui_test_handle.obj`.
- **`on_key_test` `'E'`** (:1972-1976) sends `CLICKED` to rows that have no event
  handler.
- **Unreachable:** `scr_back_btn_create` in `create11` comes after
  `esp_deep_sleep_start()` (:3263).
- **Redundant forward declarations** at :11, :12, :363, and the in-function
  `extern ui_wifi_get_selected_ssid` (:1574, :1641).
- **Redundant check:** the lazy popup create in `low_voltage_timer_cb` (:136-138).
- **Unused macros:** `UI_SLIDING_DISTANCE`, and in `ui_deckpro_int.h`
  `FONT_BOLD_SIZE_16/18/19` and `FONT_BOLD_MONO_SIZE_16..19`.
  - Non-mono `ui_get_font(pt, false)` ignores `pt`, so the used SIZE_14/15/17 are
    identical. Replace with `UI_FONT` / `UI_FONT_MONO(pt)`.

**Feature files.** *Verified.*
- **`ui_lockscreen.cpp`:** `lock_hint_label` is only ever set to NULL (:23, :437). The
  redundant `lock_last_yday = -1` before `lock_update_clock(true)` is at :469 and
  :489. The comment at :288 names the wrong font.
- **`ui_lora.cpp`:** `buf` and `lv_dropdown_get_selected_str` are computed and
  discarded (:405, :411). There is also a debug `printf` at :33.
- **Constant flags:** `reader_use_sd` is always `true`, and so is `notes_use_sd` (see
  D-S4). Drop the `is_sd` parameter throughout or make them `static const`.
- **Should be `static`:** `scr1_list`, `scr6_list`, `taskbar_statue`,
  `ui_a7682_call_test` and `ui_a7682_at_test`.

**Comment and debug noise (all files).**
- 33 no-op `#if 1`/`#endif` pairs: 21 in `ui_deckpro.cpp`, 2 in the port file and 10
  in the feature files.
- ~60 commented-out code lines, including ten `// return 4.5;` leftovers in the port
  file and the i18n block at `ui_deckpro.h:23-36`.
- Debug `printf`s in page-switch handlers.
- `touchpad_read` prints on every touch (`factory.ino:361`).
- `displayInfo()` (`peri_gps.cpp:275-350`) prints a full line for every NMEA
  sentence, forever. It is also the only thing filling the getter cache: replace it
  with a quiet ~15-line cache update.
- `A7682E_init` prints a copy-pasted "Place your board outside to catch satelite
  signal".
- Stale comments: the "~1 kHz polling" comment in `peri_keypad.cpp:107-110` (it is
  now IRQ-driven) and the LoRa comments ("433.5 MHz", "10 dBm") next to different
  values.

---

### 7.5 Simplification opportunities

Ordered by lines saved.

| # | What | Where | Fix | Saves |
| --- | --- | --- | --- | --- |
| S1 | Four near-identical paged list screens (Settings, Test, A7682, PCM5102): same list setup, row style, page switch, group handling, ~60-line `on_key` each | `ui_deckpro.cpp` 945-1050, 1893-1999, 2656-2772, 3054-3161 | One `ui_paged_list_t` with `list_create` / `list_style_row` / `list_on_key`; merge the identical `ui_a7682_handle` / `ui_pcm5102_handle` typedefs | ~300 |
| S2 | 10 copies of the "pop on click" callback; 4 identical "ESC pops" `on_key`s | `scr2_`…`scr11_btn_event_cb`; `on_key_scr2_1` :701, `scr3` :1205, `scr6_1` :2234, `scr6_2` :2364 | One `scr_pop_event_cb` + one `on_key_esc_pop` (also fixes B4) | ~85 |
| S3 | Taskbar resume + battery-label refresh, ×21 (12 in main, 9 in the feature files); destroy block ×6+ | everywhere | `ui_taskbar_enter()` / `ui_taskbar_leave()` / `ui_taskbar_destroy()` in `ui_deckpro_int.h` (also fixes B1) | ~100 |
| S4 | Settings persistence: 26 hand-written save keys and 26 load keys, 9 one-line bool getters, matching setters | `ui_deckpro_port.cpp:256-361`, 432-549 | `kSettings[] = {key, type, var, default, gpio, apply_fn}` table; keep the font-slot fallback, the `wifi_tz` migration and the clamps as explicit post-steps | ~80 |
| S5 | `SdLock`/`SpiGuard` RAII: `if(is_sd){shared_spi_lock(); shared_spi_prepare_device(BOARD_SD_CS);}` ×28, with unlocks duplicated on every early return | port file, `factory.ino`, `peri_lora.cpp` | `struct SdLock{…}` + `struct SpiGuard{…}` in `factory.h` | ~50 |
| S6 | `shared_spi_prepare_device()` is redundant at all 32 call sites: the outermost unlock already released every CS, and when nested it can only toggle the outer holder's CS (the bug class §3.11 fixed). The lazy init in `shared_spi_lock` (:101-103) duplicates `shared_spi_bus_init()` | `factory.ino`, `peri_lora.cpp`, port file | Delete it (or fold one release into lock at depth 0→1); fold into S5 | ~35 |
| S7 | `I2C0Guard` RAII: multi-exit unlocks in `bq25896_init` (3), `bq25896_runtime_maintain` (4), `hyn_i2c` (6); 13× `i2c0_lock(); v=…; i2c0_unlock();` in the port; ten identical BQ27220 getters | `factory.ino:454-526`, port :1216-1275 | 6-line guard in `peripheral.h` + `template<class F> static auto bq_read(F, T dflt)`; also covers B7's unguarded BQ25896 getters | ~50 |
| S8 | Key/value info screens (GPS 3, BQ25896 6.1, BQ27220 6.2): `gps_set_line` == `battery_set_line`; three identical `*_create_label`; identical container setup; `#define line_max 23` ×3 | `ui_deckpro.cpp` 1085-1188, 2151-2357 | `kv_screen_create()` + `kv_set_line()` | ~60 |
| S9 | Two "edit-mode" focus UIs (LoRa params, System Font) with identical 3-state styling and Esc/E/j/k state machine | `ui_lora.cpp:332-401`, `ui_reader.cpp:1174-1284` | Shared `edit_focus_style()` + `edit_focus_on_key(step_cb)` | ~60 |
| S10 | LoRa submenu items byte-identical to the battery submenu; both dispatch by `strcmp` on label text | `ui_lora.cpp:15-59` vs `ui_deckpro.cpp:2034-2078` | `ui_menu_item_create(list, name, screen_id)`, with the id in `user_data` | ~60 |
| S11 | Directory listing: `ui_notes_get_list` == `ui_reader_get_list` apart from the dir; `reader_list_update` ≈ `notes_list_update`; `fs = is_sd ? SD : SPIFFS` + path snprintf ×8–9 | port :1528-1565, :1934-1971 | `ui_fs_list_dir(dir, list, max, &n)` + `pick_fs()` / path helper | ~50 |
| S12 | LoRa config: 12× `if (radio.setX()==ERR) {println; while(true);}`; `lora_param_set` re-copies 3 blocks | `peri_lora.cpp:47-119`, :234-258 | `LORA_TRY(expr,"name")` that unlocks and returns false + shared `lora_apply_user_params()` (fixes B6) | ~50 |
| S13 | Lock-screen layout: `create_lock` duplicates what `lock_apply_orientation(false)` sets anyway | `ui_lockscreen.cpp:216-278`, :333-370 | Create bare objects, call `apply_orientation`; per-orientation `{font,align,x,y}` table | ~40 |
| S14 | LoRa settings handler compares `list[select]` against itself; three selection loops; option strings duplicate the arrays | `ui_lora.cpp:299-301`, 412-441, 504-524 | Index directly, one helper, build the strings from the arrays | ~35 |
| S15 | `setup()` sequences: 5× `gpio_hold_dis` mirroring 5× `gpio_hold_en`; 8 `pinMode`+`digitalWrite` pairs; CS/RST init duplicating `shared_spi_release_all_cs()`; I²C-scan if/else chain | `factory.ino:763-838`, `ui_deckpro.cpp:3250-3254` | Shared `k_rail_pins[]`, `{pin,level}` table, `{addr,name}` table | ~30 |
| S16 | System Font screen builds 4 buttons with the same 11 lines each | `ui_reader.cpp:1327-1369` | `make_btn(cont, cb)` | ~30 |
| S17 | `line_full_format` + 30-byte `global_buf` with unbounded `strncpy` | `ui_deckpro.cpp:14-19`, 209-230 | `lv_snprintf("%-*s%s", …)` as `gps_set_line` already does | ~25 |
| S18 | Empty lifecycle stubs (15 `destroy*`/`entry*`/`exit*`) exist only because the screen manager calls them without NULL checks | `ui_scr_mrg.c:32`, 62-85 | NULL-check the 4 call sites, delete the stubs | ~25 |
| S19 | Hide/show idiom ×4; magic numbers: `status_bar_height = 25` ×16, ESC `0x1B` ×15+, TZ `"<-03>3"` ×3 | various | `ui_obj_set_hidden()`, `UI_TASKBAR_H`, `KEY_ESC`, `UI_DEFAULT_TZ` | ~25 |
| S20 | Display helpers: power-off-timer cancel ×2, `firstPage/fillScreen/nextPage` loop ×3, `LCD_HOR_SIZE*LCD_VER_SIZE` duplicating `DISP_BUF_SIZE`; file-local state not `static` | `factory.ino:376-421`, 938-970 | `epd_cancel_poweroff_timer()`, `epd_fill(color)` | ~15 |
| S21 | GPS recovery: 3 identical write+ack blocks; `needRead` uninitialised on some paths | `peri_gps.cpp:471-499` | Loop over a `const` table | ~10 |
| S22 | `shift_keymap` is `toupper(keymap)` cell by cell | `peri_keypad.cpp:55-60` | `toupper` at lookup (do it together with B3) | ~6 |
| S23 | Small helpers: FNV key hash ×2, NVS namespace literal ×10, `strncpy`+NUL ×9, ASCII sanitize loop ×2, 300 ms `'r'` debounce ×2, default-group get-or-create ×2, toast delete ×2 | port, reader, notes, lockscreen | `fnv_key`, `kNvsNs` + `Prefs` RAII, `copy_str`, `ui_debounce` | ~60 |
| S24 | `scr6_list_event` picks the next screen by `strcmp` on label text and duplicates keys `on_key_scr6` already handles | `ui_deckpro.cpp:2034-2055` | Screen id in `user_data` (same pattern as S10) | ~15 |
| S25 | Screen lifecycle externs live in `ui_deckpro.cpp:3736-3745` rather than in the header | — | Move them into `ui_deckpro_int.h` | 0 (clarity) |

**Deliberately not recommended.**
- The `ui_lora_*`/`ui_gps_*`/`ui_input_*` pass-through wrappers in the port file look
  redundant. They are the intended UI↔hardware seam, so keep them.
- The MSC `#else` stubs never compile today (`CONFIG_TINYUSB_MSC_ENABLED=1`). Keep
  them for builds that turn MSC off.
- The PCM5102A real implementation behind `#ifdef` serves the audio variant. Keep
  it.

---

### 7.6 Build, config and tooling

**C1. `config/lv_conf.h`: LVGL modules linked but unused — ~43 KB, ~53 KB with the default theme.** *Measured (fresh map) + Verified (usage).*
- These modules are linked only because the default theme references every widget
  class. Bytes linked in `/tmp/tdeck-audit-build`:
  - chart 8,108
  - arc 4,105 (plus spinner 236)
  - table 3,425
  - grid 3,057
  - roller 2,961
  - colorwheel 2,894
  - meter 2,468
  - slider 2,337
  - bar 2,327
  - keyboard 1,943
  - menu 1,538
  - calendar 1,410, plus its header_dropdown 1,082 and header_arrow 664 (the lock
    screen draws its own calendar)
  - spinbox 1,239
  - tabview 1,095
  - led 574
  - line 514
  - tileview 504
  - win 212
  - msgbox 168
  - msg 167
- **`LV_USE_THEME_DEFAULT`** (9,639 B) is replaced by `lv_theme_mono_init`
  (`ui_deckpro.cpp:3751`), so it can likely be set to 0 too.
- **Keep:** label, btn, list, textarea, dropdown, switch, checkbox, btnmatrix, img,
  flex, theme_mono.
  - The only other LVGL sketch, `examples/test_lvgl`, uses btn/label/obj/timer, so
    the shared config can be trimmed safely.
- **Compile-time-only modules** (linked 0 B, but they slow every clean build):
  `LV_FONT_SIMSUN_16_CJK` (a 1.08 MB `.c`), canvas, span, imgbtn, animimg,
  theme_basic. Set them to 0.
- All fonts **are** reachable through the font-slot picker. Don't remove any.

**C2. `platformio.ini`.**
- `-DTINY_GSM_MODEM_SIM7672` duplicates the `#ifndef` default in `factory.h:29-30`.
  *Likely.*
- `-DDISABLE_ALL_LIBRARY_WARNINGS` is read by nothing in `lib/` or the core.
  *Verified.*
- `-Wnarrowing` is already the C++11 default.
- `[env:T-Deck-Pro-modem]` has **no `board_build.partitions`**, so it silently uses
  `default_16MB.csv`. Hoist the common keys into `[env]` (or use `extends`).
  *Verified.*
- Stale comments:
  - the `extra_scripts = ./script/…` line (`script/` doesn't exist);
  - `src_dir = examples/${platformio.default_envs}`;
  - the commented registry `lib_deps` block (now vendored);
  - contradictory USB-CDC comments;
  - the tethering line (:17), which says `espressif32@6.9.0` although the env
    itself uses the pioarduino fork.
- Now that `make build SRC_DIR=examples/<x>` works, the 19 commented `src_dir`
  lines can collapse to one usage comment. `A7682E/audio_test` is missing from that
  list anyway.

**C3. `Makefile`.** Nothing in it is dead.
- `DETECT_PORT_SH` repeats the idVendor walk (`:70-83` vs `:85-96`).
- `flash-factory` and `flash-meshtastic` are the same recipe; use a
  `define FLASH_BIN`.
- `build-modem`/`upload-modem` repeat their args; use a `MODEM_ARGS` variable.
- `upload: build` is redundant, because `pio run -t upload` builds incrementally.

**C4. `examples/tethering/`.**
- `BOARD_A7682E_RI/DTR` are unused (`utilities.h:5-6`).
- The README documents `TETHER_APN_USER/PASS`, which don't exist
  (`tethering.ino:108-110` says PAP/CHAP is unsupported).
- The README, `tethering.ino:7-8` and `platformio.ini:17` all prescribe
  `espressif32@6.9.0`, which is superseded by `[env:T-Deck-Pro-modem]` plus
  `make upload-modem`.
- `utilities.h` is the 5th copy of the A7682E pin block, and factory calls pin 41
  `BOARD_6609_EN` while the rest say `BOARD_A7682E_POWER_EN`. Share one header.

**C5. `.gitignore` / `tools/`.**
- `.vscode/` (line 6) makes lines 2-5 redundant.
- Lines 7-14 name paths that no longer exist.
- `eng-pob.tsv` is unanchored and also matches the tracked `tools/eng-pob.tsv`; use
  `/eng-pob.tsv`.
- `tools/fetch_eng_pob_dict.py` and `tools/freedict_tei_to_tsv.py` are superseded
  FreeDict pipelines (`kaikki_to_tsv.py` explains why it replaced them).
- `tools/eng-pob.tsv` (740 KB, tracked) is their stale output. Delete it, or label
  it as a sample.

**C6. Doc drift.** `CLAUDE.md`/`GEMINI.md` say `-DTINY_GSM_MODEM_SIM7682`; the flag
is `SIM7672`.

---

### 7.7 False-positive guard: things that look dead but are not

Re-check this list before deleting anything near these symbols.

- **Only called from `factory.ino`** (graft shows "no callers"): `lora_init`,
  `LTR553_init`, `gps_init`, `keypad_task_create`, `ink_screen_prepare_shutdown`,
  `a7682_handle`.
- **Macros read inside vendored headers:** `TINY_GSM_RX_BUFFER`.
- **`E_PERI_TOUCH`/`E_PERI_BHI260AP`/`E_PERI_PCM5102A`:** the test table indexes
  them.
- **`BOARD_TOUCH_RST`, `BOARD_GPS_PPS`:** the deep-sleep path uses them.
- **`T_DECK_PRO_V1_0`, `UI_T_DECK_PRO_VERSION`, `BOARD_T_DECK_PRO_VERSION`:** all
  referenced.
- **`BHI260AP_get_val` stub:** used by the port.
- **Called internally only (make `static`, don't delete):** `ui_test_sd_card`,
  `ui_wifi_connect`, `is_chinese_utf8`.
- **Live port functions:** `wifi_scan_collect` (the live async scan),
  `ui_lora_get_*` (read by `peri_lora.cpp`), `ui_setting_apply_keypad_light`,
  `ui_battery_27220_get_input` (3 callers), `scr_mgr_get_curr_life`/`_scr_id`, and
  `ui_pcm5102_cb`/`_stop` (live through their stubs).
- **Live UI pieces:** `scr_back_btn_create` (4.1, 8.1, 8.2, 9), `scr8_btn_event_cb`
  (`create9`), `setting_page_switch_internal` (Settings really pages), `GET_BUFF_LEN`
  (lora), `img_start`, `screen_dict` (`t` shortcut), and the
  `reader_selected_file`/`reader_resume_*` externs (`c` shortcut).
- **`hyn_cst3xx.c`:** the right CST328 driver if touch is kept.
- **`BOARD_1V8_EN`:** must stay HIGH. It powers the gyro **and** the touch IC.

---

### 7.8 Suggested order of work

Each step builds clean on its own. Flash and serial-check after each one: there is no
test suite.

1. **Bugs (§7.1):**
   - B1 + S3 together (the taskbar helpers fix the use-after-free);
   - B2 (the `scr_mgr_switch` leak);
   - B3 + S22 (Enter key);
   - B4 + S2 (ESC handler);
   - B6 + S12 (LoRa);
   - then B5 and B7–B10.
2. **Delete-only, toolchain-proven (§7.2):** every compiler/linker item, the two
   phantom declarations and the 8 orphan images. Re-run the instrumented build from
   *Method* until it shows no `examples/factory` warnings.
3. **Unreachable screens and features (§7.3):** D-S1, D-S2, D-S3, D-S5, D-S6 and D-T2
   need no decision.
   - **Decide** D-S4 (notes list: wire it in or delete it).
   - **Decide** D-T1 (touch: drop it, or keep it revivable).
   - **Decide** whether to `#ifdef` D-F1 rows or delete them.
4. **`lv_conf.h` trim (C1):** ~43–53 KB flash, a one-file change. Check the image
   size before and after.
5. **Write-only state and noise (§7.4):** a mechanical sweep, one commit per file.
6. **Simplification (§7.5):** S1, S4, S5–S7 and S8 give the most lines back.
7. **Tooling and docs:** C2–C6.

---

## 8. Third pass: status re-audit and new findings (2026-10-25, evening)

Third pass, against **HEAD `799e7e0` plus the large uncommitted cleanup** now in the
working tree (`git diff --stat`: 34 files, +1,650 / −5,628). That cleanup implements
most of §1–§7. This section covers three things:

1. a status scoreboard for every earlier finding (§8.1);
2. new bugs found on the way (§8.2);
3. new optimization findings that the earlier passes did not cover: power, e-paper
   pipeline, storage I/O, memory/flash, and leftovers (§8.3–§8.7).

**Method**
- Fresh measured build, in a separate directory with `platformio.ini` untouched:
  ```bash
  PLATFORMIO_BUILD_DIR=/tmp/tdeck-audit3 \
  PLATFORMIO_BUILD_FLAGS="-Wunused-function -Wunused-variable -Wunused-but-set-variable -Wl,--print-gc-sections" \
  pio run -e T-Deck-Pro      # SUCCESS, 106 s
  ```
- Per-archive totals come from the map parser described in the Appendix.
- Four read-only code audits: two status passes, one runtime/power pass and one
  memory/I/O pass.
- Every bug in §8.2 and every headline claim in §8.3–§8.5 was spot-checked by reading
  the code.

> **Caveat: the tree was changing during the audit.** Several `examples/factory/*.cpp`
> files were rewritten around 18:54, after the 18:51 build, by another editing
> session. Line numbers below can drift, so every finding is anchored by **function
> name**; treat `:NNN` as a hint only. Re-run the build before trusting the byte
> figures in §8.0 to the last KB.

### 8.0 Measured baseline

| Metric | §0 (first pass) | §7 (second pass) | **Now** |
| --- | --- | --- | --- |
| `firmware.bin` | 1,795,824 B | 1,305,120 B | **1,226,608 B** (46.8% of the 2.5 MB slot) |
| `.dram0.data` + `.dram0.bss` | 147,344 B | — | **114,876 B** (25,036 + 89,840) |
| `.flash.text` / `.flash.rodata` | — | — | 792,359 / 329,896 B |
| `-Wunused-*` warnings in `examples/factory` | — | 37 | **0** |

Largest contributors now (map parser, code + data, debug sections excluded):

| Bytes | Object | Note |
| --- | --- | --- |
| 235,360 | `liblvgl.a` | already trimmed by C1 |
| 132,690 + 100,734 + 41,670 + 40,286 + 33,231 | net80211 + lwIP + wpa_supplicant + pp + phy | the WiFi stack; needed for NTP/ping |
| 85,110 | `libc.a` | includes ~35 KB of float printf/scanf (§1.7, still open) |
| 30,305 | `libmbedcrypto.a` | WPA only; the TLS chain is gone |
| 29,767 | `libarduino_tinyusb.a` | CDC + MSC + DFU + MIDI + vendor class objects |
| 23,111 | `libspiffs.a` | linked only for a mount nothing reads (IO3) |
| 25,168 | `lv_font_spleen_32x64.c.o` | lock-screen clock digits only (M3) |
| 180,887 | all fonts together | ~55% of `.flash.rodata` |

The largest DRAM symbols are `work_mem_int` (LVGL heap) at 49,152 B, `_dfu_ctx` at
4,116 B, `_mscd_buf` at 4,096 B and `g_cnxMgr` (WiFi) at 3,800 B.

The audio/TLS stack (§1.1) and the gyro blob (§1.3) are no longer linked. **Flash is
no longer the constraint.** Power, refresh latency and SD throughput now matter
most.

---

### 8.1 Status scoreboard of §1–§7

Legend: ✅ fixed · ◐ partial · ⬜ open · — no longer applicable.

**§1–§5 (first pass)**

| ID | Status | Remaining work |
| --- | --- | --- |
| 1.1 audio + TLS | ✅ | Behind `BOARD_HAS_PCM5102A`. The LDF still *compiles* `libESP32-audioI2S`, but it is not linked. |
| 1.2 / 1.4 image formats | ✅ | All `ALPHA_1BIT`; `img_start` is 8,915 B. |
| 1.3 gyro blob | ✅ | `#ifdef BOARD_HAS_BHI260AP` plus stubs in `peripheral.h`. |
| 1.5 montserrat_26 | ✅ | — |
| 1.6 precompiled IDF blobs | ⬜ won't fix | Needs a custom core build. |
| 1.7 float printf/scanf | ⬜ | `LV_SPRINTF_CUSTOM 1` and about 12 live `%.Nf` calls remain (GPS, battery, LoRa debug). See M2. |
| 1.8 build-config cleanups | ◐ | `EEPROM` is still in `lib_deps`; there is no `lib_ldf_mode`, so SD_MMC/FFat/audio sources still compile. |
| 2.1 `LV_MEM_SIZE` 48 KB | ⬜ | See M1. |
| 2.2 GxEPD2 page buffer | ✅ | `display` is now 1,324 B. |
| 2.3 reader statics | ✅ | `ps_malloc` in `entry13_1`; `READER_MAX_PAGES` is 8192. The failure path has a bug (N2). |
| 2.4 TinyUSB DFU context | ⬜ | Needs a custom core build. Low priority. |
| 3.1 keypad polling | ✅ | IRQ + semaphore with a 500 ms safety net. INT-clear ordering issue: N8. |
| 3.2 GPS task priority / spin | ✅ | — |
| 3.3 `vTaskSuspend(NULL)` | ✅ | — |
| 3.4 dictionary byte reads | ✅ | 512 B chunks. The lookup has an off-by-one (N1) and runs on a 1 MHz bus (IO1). |
| 3.5 reader resume | ◐ | The geometry cookie lives in RAM only. See D6. |
| 3.6 deferred EPD power-off | ◐ | Implemented, but the driver re-inits the panel after every update, so it saves nothing. See D2. |
| 3.7 `rounder_cb` | ✅ | — |
| 3.8 gesture timer | — | The touch stack is gone. |
| 3.9 settings NVS debounce | ✅ | 1 s debounce. |
| 3.10 `delay(500)` in timers | ✅ | Ping runs on a worker; NTP polls an `lv_timer`. |
| 3.11 SPI unlock depth | ✅ | Depth-counted; `shared_spi_prepare_device` removed. |
| 4.1 partition table | ✅ | However, nothing in the 4G build reads SPIFFS. See IO3. |
| 4.2 data files in the repo | ◐ | They are gitignored, but CLAUDE.md doesn't say how `eng-pob.tsv` is built (`tools/kaikki_to_tsv.py`). |
| 5.1 / 5.2 / 5.3 structure | ✅ | `global_kb_timer_cb` + `on_key`; `ui_deckpro.cpp` is now 2,892 lines; `hyn_*` deleted. |

**§7.1 bugs B1–B10:** all ✅ except the following.
- **B7 ◐:** `ui_batt_25896_is_chg()` and `PPM.shutdown()` still skip `i2c0_lock` and
  don't check that init succeeded.
- **B8 ◐:** when LoRa is enabled it still transmits "Hello World!" on every boot. See
  N3.
- **B10:** `T_DECK_PRO_V1_0` is now unused.

**§7.2–§7.6 (dead code, simplification, build):**
- ✅ Every compiler/linker-proven item; D-S1…D-S6; D-T1 (option A, touch dropped);
  D-T2; D-F1.
- ✅ S2–S8, S10–S18, S20–S22, S24, S25; C1, C2, C3, C5, C6.
- The parts still open or partial are in §8.7.

---

### 8.2 New bugs: fix these first

**N1. Dictionary lookup misses words it contains.** *Verified (code read).*
- **Where:** `ui_dict_lookup` (`ui_deckpro_port.cpp`). The same code is in HEAD.
- **What goes wrong:**
  - On an exact hit during bisection, `hi = line_start` of the matching line. The
    final scan runs `while (tell < hi)`, so it never reads that line.
  - After a `cmp < 0` step, `lo` always sits at the **start** of a line. The final scan
    still calls `dict_buf_skip_line()` when `lo != 0`, which discards the first
    candidate line. That line can be the target.
- **Effect:** any headword that bisection probes exactly, or that is the first line
  after `lo`, returns "not found".
- **Fix:**
  - Scan with `tell <= hi`, or keep `hi` one line past the match.
  - Skip a line only when `lo` is a mid-line offset (that is, never, given how `lo` is
    updated).
- **Test:** look up the first and last headwords of the TSV, plus a few hundred random
  ones. The TSV is sorted by lowercase UTF-8 bytes (`kaikki_to_tsv.py`), which matches
  the firmware's ASCII `tolower` compare.

**N2. Use-after-free when the reader can't get PSRAM.** *Verified.*
- **Where:** `entry13_1` (`ui_reader.cpp`) calls `scr_mgr_pop()` from inside `entry`.
  `scr_mgr_push()` calls `entry` through `scr_mgr_active(stack_scr)`.
- **What goes wrong:**
  - `pop` frees `stack_scr` and deletes its screen object.
  - `push` then runs `lv_scr_load(stack_scr->obj)` on freed memory.
- **Fix:** do the allocation **before** pushing (in the caller), or have `entry` post
  the pop with `lv_async_call`.
- **Related:** `scr_mgr_push` doesn't check the `lv_mem_alloc` result.

**N3. Unexpected LoRa transmission at every boot.** *Verified.*
- `lora_init` (`peri_lora.cpp`) calls `startTransmit("Hello World!")`. It uses the
  default 850 MHz at 22 dBm, SF10/BW125: about 370 ms of airtime at about 118 mA.
  850 MHz is outside most regional ISM bands.
- **Fix:** delete the boot TX. See also P5.

**N4. USB mass storage exposes the SD card while FatFs still has it mounted.** *Verified.*
- **Where:** `ui_usb_msc_begin` / `ui_usb_msc_end` (`ui_deckpro_port.cpp`). Neither
  calls `SD.end()` before `msc.begin()` or re-runs the SD init after `msc.end()`.
- **What goes wrong:**
  - The host writes raw sectors while FatFs keeps its cached FAT window and FSINFO.
  - After MSC ends, the firmware keeps using the stale mount. The next note save can
    corrupt the FAT.
- **Fix:** call `SD.end()` before `msc.begin()`, re-init the SD after `msc.end()`, and
  block the reader, notes and dictionary screens while MSC is active.

**N5. A long note hangs the device and the note is lost.** *Verified; the threshold is Inferred.*
- **What goes wrong:**
  - `ui_notes.cpp` never calls `lv_textarea_set_max_length`.
  - Every keystroke reallocates and re-wraps the whole text in the 48 KB LVGL heap.
  - When the realloc fails, `LV_ASSERT_MALLOC` fires `LV_ASSERT_HANDLER`, which is
    `while(1);` (`lv_conf.h`). Nothing resets the device.
- **Fix:**
  - Cap the length, e.g. `lv_textarea_set_max_length(ta, 4096)`.
  - Set `LV_ASSERT_HANDLER` to `abort();`. That gives a reset plus a coredump; the
    `coredump` partition exists.

**N6. Peripheral toggles in Settings misbehave.** *Verified.*
- **Modem:** turning it on after a rail-off boot never probes it, so
  `peri_init_st[E_PERI_A7682E]` stays false (`ui_setting_set_a7682_status`). This is
  the same pattern as B5.
- **LoRa:**
  - Turning it on runs `lora_init()` synchronously on the LVGL task, blocking the UI.
  - Turning it off calls `lora_sleep()` on a radio that may never have been
    initialised.
- **GPS:** `gps_lazy_init_task` doesn't re-check the setting after its baud scan. Turn
  GPS off during the scan and the task resumes anyway, with the rail off.

**N7. WiFi writes flash on every connect.** *Verified.*
- **Where:** `wifi_connect_task` calls `WiFi.disconnect(false, true)`, which erases the
  stored AP config, then `WiFi.begin(ssid, pass)`, which stores it again.
- That is one NVS write per connect, including every lock-screen unlock.
- **Fix:** call `WiFi.persistent(false)` once at init.

**N8. Keypad interrupt race.** *Verified.*
- **Where:** `keypad_loop` clears `INT_STAT` *after* draining the FIFO.
- A key that arrives between the drain and the clear loses its interrupt edge. It
  then waits for the 500 ms safety-net timeout.
- **Fix:** clear `INT_STAT` first, then drain.
- **Also:** the "below GPS" priority comment is stale. The keypad is priority 18 and
  GPS is 17.

**N9. Reader file-name and count limits.** *Verified.*
- `fs_list_dir` uses `char[32]`, and so does `reader_selected_file`. Book names longer
  than 31 characters are truncated and then fail to open.
- More than `UI_READER_MAX_COUNT` (20) books are silently dropped.
- **Fix:** use 64–96-byte names (SD FAT LFN) and a scrolling or paged list.

**N10. NVS bookmark keys grow without bound.** *Inferred.*
- Each book opened adds two FNV-hashed keys (`b_`/`p_`), and nothing ever prunes them.
- The 20 KB NVS partition holds roughly 500 entries. After a few hundred books,
  settings saves start failing silently.
- **Fix:** an LRU of N bookmarks in one blob, or prune keys whose file no longer
  exists.

---

### 8.3 Power: the biggest remaining win

The device is a battery e-reader whose screen holds an image with zero power. Today
nothing is ever allowed to idle.

**P1. No idle power management at all.** *Verified (absence); currents Inferred from datasheets.*
- **What happens now:**
  - The CPU runs at 240 MHz permanently (board JSON `f_cpu`). There is no
    `setCpuFrequencyMhz`, `esp_pm` or light sleep.
  - There is no inactivity timeout. LVGL can't provide one because no indev is
    registered.
  - The lock screen turns off only WiFi and the keypad backlight.
  - The stock Arduino 2.0.14 sdkconfig has no `CONFIG_PM_ENABLE` and no tickless idle,
    so `esp_pm_configure` won't work in this build.
- **Cost:** the S3 idles at about 30 mA at 240 MHz, about 22 mA at 80 MHz, and about
  0.25 mA in light sleep. A static page spends nearly all its time idle.
- **Fix, staged:**
  1. Timestamp the last key in `keypad_buf_push`. After N minutes with no key,
     auto-lock.
  2. Interim step: `setCpuFrequencyMhz(80)` while idle; back to 240 MHz on a key or on
     a pending LVGL invalidation.
  3. On the lock screen, with nothing pending (no flush, settings flushed, EPD powered
     off), call `esp_light_sleep_start()`. Wake sources:
     - `gpio_wakeup_enable(BOARD_KEYBOARD_INT, GPIO_INTR_LOW_LEVEL)`. This works
       because the TCA8418 holds INT low until it is cleared.
     - A timer wake at the next minute boundary for the clock.
  4. Long term: a custom sdkconfig (pioarduino `custom_sdkconfig`, or IDF + the
     Arduino component) for DFS plus automatic light sleep.
- **Depends on:** P8 (fewer wakeups). USB-CDC drops while asleep, which is acceptable
  on battery.

**P2. A focused textarea's blinking cursor keeps the panel refreshing.** *Verified (LVGL path traced).* This is also the largest latency bug.
- **Why:** the mono theme sets `ta_cursor` `anim_time` to 500 ms on
  `LV_PART_CURSOR | LV_STATE_FOCUSED` (`lv_theme_mono.c:158,413`). Focus starts an
  infinite blink animation.
- **Affected screens:** Notes, Dictionary, the WiFi password field, and the call
  keypad textarea all focus a textarea on create.
- **Effect:**
  - Every blink is a partial refresh of about 700 ms, so the panel refreshes back to
    back for as long as the screen is open.
  - The 2 s power-off timer never fires.
  - The loop task stays inside `_waitWhileBusy`, so typed characters echo 0.7–1.4 s
    late.
- **Fix:** add one line per textarea,
  `lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);`. That gives
  a solid cursor. Alternatively, override it once in a theme-apply callback.

**P3. GPS tracks continuously by default, only to set the clock.** *Verified; current Inferred.*
- **What happens now:**
  - `default_gps_status = true`, and `exit3` deliberately leaves GPS running.
  - The clock resyncs from GPS once an hour, yet the module tracks at 1 Hz all the
    time: about 8–12 mA.
  - `gps_init` power-cycles the module on every boot. If V_BCKP shares the enable
    line, every boot is a cold start.
- **Fix:**
  - Once `gps_time_ever_set` is true, drop `BOARD_GPS_EN` (or send UBX-RXM-PMREQ for
    backup mode).
  - Re-enable it for an hourly or daily hot-start resync, and while screen 3 is open.
  - Disable the unused NMEA sentences (GSV/VTG/GLL).

**P4. The 4G modem is on by default, never sleeps, and its pump task polls at 1 kHz.** *Verified; current Inferred.*
- **What happens now:**
  - `default_a7682_status = true`. No `AT+CSCLK`, `AT+CFUN=4` or `AT+CPOF` is ever
    sent.
  - `BOARD_6609_EN` stays HIGH even when the AT probe fails.
  - `a7682_task` (priority 20, above the keypad) polls every 1 ms. It is created even
    when the rail is off (3 KB stack plus TCB).
  - It also consumes `SerialAT` bytes, racing TinyGSM in the call and AT-test screens.
- **Cost:** a registered modem without sleep idles at about 10–20 mA.
- **Fix:**
  - Default the modem to off, or send `AT+CPOF` after the probe and power it only on
    screens 8.x.
  - Drop the rail when the probe fails.
  - Create the task lazily and resume it only on the AT-test screen
    (`ui_a7682_loop_resume/suspend` already exist).
  - Block on `SerialAT.onReceive()`, or delay 10–20 ms, instead of 1 ms.

**P5. The LoRa radio is never put to sleep.** *Verified.*
- After init the SX1262 sits in STDBY_RC (about 0.6 mA) forever.
- Leaving a LoRa screen in receive mode leaves RX on (about 5 mA).
- **Fix:** call `radio.sleep()` after init and in `exit1_1`/`exit1_2`, and wake the
  radio on screen entry. Fix this together with N3.

**P6. The light/proximity sensor runs with nothing reading it.** *Verified.*
- `LTR553_init` enables ALS plus proximity, pulsing the IR LED at 50 mA every 200 ms.
  Its last consumer (`ui_other_get_LTR`) was deleted.
- **Cost:** about 0.3 mA continuous.
- **Fix:** keep only the presence probe for the Test screen and leave the part in
  standby.

**P7. Charger ADC runs continuously, and battery polling is too frequent.** *Verified; impact Inferred.*
- **What happens now:**
  - `PPM.enableMeasure()` puts the BQ25896 ADC in continuous mode (1 s conversions).
  - `menu_taskbar_update_timer_cb` does about 3 I²C reads every 1 s.
  - `low_voltage_timer_cb` reads VBUS, voltage and gauging status every 2 s, with a
    1 ms busy-wait.
  - The keypad safety net does 2 transactions every 500 ms.
  - I²C runs at the default 100 kHz.
  - Total: about 8–10 transactions per second at idle.
- **Fix:**
  - Use one-shot ADC mode, only while screen 6.1 is open. `VBUS_GD` and `CHRG_STAT`
    are status bits; verify they still update with the ADC off.
  - Poll SOC every 30–60 s and low-voltage every 10 s (or use the gauge alarm).
  - Raise the keypad safety net to 5 s.
  - `Wire.setClock(400000)`.

**P8. The UI loop wakes about 1,075 times per second.** *Verified.*
- **What happens now:**
  - `loop()` runs `lv_task_handler(); … vTaskDelay(1);`, which is 1 kHz.
  - `global_kb_timer` (20 ms) and `menu_timer` (40 ms) poll on every screen.
- **Why it matters:** the CPU cost is small, but it makes P1 impossible.
- **Fix:**
  - `uint32_t ms = lv_timer_handler(); ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(min(ms, 500)));`
  - Have `keypad_loop` call `xTaskNotifyGive(loopTaskHandle)` after pushing keys, and
    dispatch keys right after the wake. That replaces both polling timers; the home
    handler becomes SCREEN0's `on_key`.
  - Move `bq25896_runtime_maintain` into a 5 s `lv_timer`.

**P9. WiFi stays associated after NTP sync.** *Verified; impact Inferred.*
- WiFi is used only for NTP and ping, but the association is kept indefinitely with
  the default `WIFI_PS_MIN_MODEM`.
- **Fix:** disconnect once `ui_time_is_synced()` is true and reconnect daily, or use
  `WIFI_PS_MAX_MODEM` with a long listen interval.

**P10. Smaller power items.**
- **Clock saved to flash every minute:** `ui_time_persist_save()` runs from the taskbar
  timer and writes NVS each minute. A sector erase stalls the flash cache for tens of
  ms. Save every 15 min, plus on lock/sleep/shutdown. *Verified.*
- **Deep sleep wakes only on BOOT** (`create11`, ext1). The keypad INT (GPIO15) could
  also wake the chip on any key, if the TCA8418 stays powered. *Inferred.*
- **`BOARD_1V8_EN`:** the default build uses neither consumer (gyro not compiled in,
  touch removed). Gating it at compile time when **both** are absent is a separate,
  measure-first decision. **Never** gate it on a gyro-only toggle (CLAUDE.md, power
  rails). *Inferred.*
- **EPD hibernate is unavailable:** `BOARD_EPD_RST = -1`, so `hibernate()` is a no-op.
  It would need the RST line wired.

---

### 8.4 E-paper pipeline and UI latency

**D1. Each LVGL flush area gets its own e-paper refresh.** *Verified.*
- **What happens now:**
  - With `full_refresh = 0`, LVGL calls `flush_cb` once per unjoined area.
  - `flush_epd_bitmap` runs `writeImage` + `refresh()` + `writeImageAgain` on every
    call.
  - A frame that dirties the taskbar plus a list row therefore costs **two** sequential
    refreshes of about 700 ms each.
- **Fix:**
  - `decodebuffer` is already a full-panel 9,600 B buffer. Pack each area into it at
    its panel position and grow a union rectangle.
  - Write and refresh once, when `lv_disp_flush_is_last(drv)` is true (available in
    the vendored LVGL).
  - Decide FULL vs PART once per frame, not per area.

**D2. The panel driver re-inits after every update, so deferred power-off (§3.6) saves nothing.** *Verified; saving Inferred.*
- **Why:** `GxEPD2_310_GDEQ031T10::_Update_Part` and `_Update_Full` end with
  `_init_display_done = false; // needed, reason unknown`.
- **Effect:**
  - The `writeImageAgain` right after `refresh()` soft-resets the controller, and
    `_power_is_on` goes false.
  - So every flush sends PON and waits on BUSY again. `epd_poweroff_timer_cb` then
    does nothing.
- **Fix:** on hardware, try removing that line from `_Update_Part` only. Keep it after
  a full refresh if ghosting needs it.
- `lib/` is a local fork; document the change.

**D3. The shared SPI mutex is held through every BUSY wait.** *Verified; impact Inferred.*
- **What happens now:**
  - `flush_epd_bitmap` keeps its `SpiGuard` across `refresh()`, which spins in
    `_waitWhileBusy`.
  - `disp_hard_refresh` keeps it across six full refreshes.
  - The bus is idle the whole time (EPD CS is high), yet LoRa, SD and USB-MSC wait:
    - about 0.7 s per partial refresh;
    - about 1.1 s per full refresh;
    - about 7 s per hard refresh.
  - At boot, `sd_care_init`/`lora_init` queue behind the first full frame.
- **Fix:**
  - `display.epd2.setBusyCallback(cb)`, where `cb` runs
    `shared_spi_unlock(); vTaskDelay(1); shared_spi_lock();` at depth 1.
  - Scope the guard per `epd_fill` in `disp_hard_refresh`.
  - Drop `SD.usedBytes()`, a FAT walk, from boot.

**D4. The panel driver writes one byte per SPI call.** *Verified; timing Inferred.*
- `_writeImage` calls `_transfer()` once per byte. That is about 20–30 ms per full
  panel, and every flush writes twice (`writeImageAgain`).
- **Fix:** add a fast path for the no-invert/no-mirror case that calls
  `_pSPIx->writeBytes(row, w/8)` per row. Minor next to the refresh itself.

**D5. Every screen change does a full, flashing refresh.** *Verified.*
- Nearly every `entryN`/`exitN` calls `ui_disp_full_refr()`; there are 46 calls in
  `ui_deckpro.cpp` alone.
- A full refresh takes about 1.1 s and flashes the screen; a partial one takes about
  0.7 s.
- **Fix:** use partial refresh for transitions, plus a ghosting counter, the same way
  the reader uses `FULL_REFRESH_EVERY = 8`. Tune on hardware.
- **Related:** 9 screens share an identical `entry` (`ui_taskbar_enter();
  ui_disp_full_refr();`) and 8 share an identical `exit`. Two shared functions would
  centralise the policy.

**D6. Book resume walks from page 0 after every reboot.** *Verified.*
- `s_last_saved_cookie` (`ui_reader.cpp`) is RAM-only, so the first book opened after
  a boot falls back to the O(pages) walk.
- **Fix:** persist it as `c_%08x` next to `p_%08x`.

---

### 8.5 Storage and I/O

**IO1. The SD card runs at 1 MHz for the whole session.** *Verified (core source); timings Inferred.*
- **Why:** `sd_care_init` tries `freqs[] = {1000000, 4000000, 4000000}`. The first
  attempt normally succeeds, because `SD.begin` does the 400 kHz card init itself.
  Core 2.0.14 then keeps `card->frequency = hz` for every transfer.
- **Cost:**
  - About 110 KB/s, against 20 MHz already used for the EPD on the same bus.
  - A dictionary lookup takes about 0.6–1.5 s instead of about 30–60 ms.
  - USB-MSC copies crawl: more than 15 minutes for the ~104 MB dictionary.
  - Every SD operation holds the shared bus about 20× longer than needed.
  - The comment's rationale ("LoRa mid-transmit") no longer applies, because the bus
    mutex serialises access.
- **Fix:** try `{20000000, 10000000, 4000000, 1000000}` in that order.

**IO2. Dictionary I/O is still wasteful even after IO1.** *Verified.*
- **What happens now:**
  - `File` carries a 4 KB stdio buffer (`vfs_api.cpp`). Each of the ~18 bisection
    seeks refills 4 KB to use about 512 B.
  - `CONFIG_FATFS_USE_FASTSEEK` is off, so backward seeks walk the cluster chain.
- **Fix:**
  - `f.setBufferSize(0)` (or 512), since `dict_buf_t` already does its own chunking.
  - Keep the file open while the dictionary screen is up.
  - Build a sparse index in PSRAM, e.g. every 256th headword, on first open or as a
    sidecar `.idx` written by `kaikki_to_tsv.py`. A lookup then costs 1–2 block reads.

**IO3. SPIFFS is mounted at every boot with format-on-fail, but nothing in the 4G build reads it.** *Verified; costs Inferred.*
- **What happens now:**
  - The only SPIFFS reader is `audio.connecttoFS(SPIFFS, …)` behind
    `BOARD_HAS_PCM5102A`. Books and the dictionary come from **SD**.
  - `SPIFFS.begin(true)` runs before the display init. On an unformatted or corrupt
    partition it erases 3,424 blocks, about 1.5–2.5 min of blank screen.
  - The first boot after the repartition hits this unless `make flash-spiffs` was run.
  - A normal mount scans every block: about 0.2–1.5 s, plus about 3–4 KB of heap held
    permanently.
  - `libspiffs.a` (23 KB) is linked only for this call.
- **Fix:**
  - Drop the mount in the 4G build. Mount lazily on screen 10 with
    `formatOnFail = false` in the audio build.
  - If the 13.4 MB partition should hold content (books fit; the ~104 MB dictionary
    does not), switch to **LittleFS** with `board_build.filesystem = littlefs`:
    - millisecond mounts;
    - power-loss safety;
    - real directories and 255-character names, against SPIFFS's 32-byte path limit;
    - O(log n) seeks.
  - Otherwise, give most of the partition back to the app or OTA.

**IO4. Serial writes can block for 100 ms.** *Verified (core source); impact Inferred.*
- **Why:** with `ARDUINO_USB_MODE=1` the console is HWCDC. After the first host
  connection, `tx_timeout_ms` is 100 ms. If the terminal is closed, or the cable later
  only charges, any write that overflows the 256 B ring stalls.
- Many `Serial.printf` calls sit on LVGL-task paths.
- **Fix:** `Serial.setTxTimeoutMs(0)` right after `Serial.begin`.

**IO5. SD file handles.** *Inferred.*
- `SD.begin` uses the default `max_files = 5`, at about 550 B internal each, but at
  most 3 files are ever open. Setting 3 saves about 1.1 KB.
- The reader's cached `File` keeps a 4 KB stdio buffer in **internal** DRAM for the
  whole reader session (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`).
  `setBufferSize(1024)` would be enough.

---

### 8.6 Memory and flash

**M1. `LV_MEM_SIZE` (48 KB) is still the largest DRAM consumer.** *Measured.*
- Enable `LV_USE_MEM_MONITOR` (or log `lv_mem_monitor()` on every screen push) for one
  session of normal use.
- Then either right-size it, or move the pool to PSRAM with `LV_MEM_ADR`/`LV_MEM_POOL_ALLOC`.
  PSRAM is slower, but on a 1-bit, 500 ms-period UI that is acceptable.
- Do this **before** N5's length cap, so the cap has numbers behind it.

**M2. Float printf/scanf costs about 35 KB.** *Measured.*
- `_vfprintf_r` (12,250 B), `_svfprintf_r` (11,979 B), `__ssvfiscanf_r` (8,077 B) and
  `_dtoa_r` (3,200 B) are still linked.
- About 12 `%.Nf` calls remain (GPS, battery, LoRa debug). Replace them with fixed
  point (`%d.%02d`), and drop the `sscanf` user, if it is ours.
- Low priority: flash is not tight.

**M3. Fonts: 180,887 B, about 55% of `.flash.rodata`.** *Measured.*
- **Unused sizes:** `Font_Mono_Bold_17` (5,708 B) and `_19` (6,713 B) are reachable
  only through `ui_get_font`'s `pt` switch. No caller passes 17 or 19: the UI uses
  14/15, the picker 14/16/18/20, and one call site 20. Map 17→16 and 19→18 to save
  **12.4 KB**.
- **Clock-only font:** `lv_font_spleen_32x64` (25,160 B) draws only the portrait
  lock-screen clock ("HH:MM" / "--:--"). Subsetting it to `0-9 : -` saves about
  23 KB. It needs a range argument in `fonts_bitmap/bdf2lvgl.py`, which hardcodes
  0x20–0x7E.

**M4. Local styles everywhere.** *Inferred.*
- There are about 240 `lv_obj_set_style_*` calls and no shared `lv_style_t`. Each list
  row carries about 2 local style arrays, roughly 60–90 B per row in the 48 KB heap.
- A handful of `static lv_style_t` for row, focus and taskbar, added with
  `lv_obj_add_style`, would cut both heap use and allocations.

**M5. TinyUSB class buffers.** *Measured.*
- `_dfu_ctx` 4,116 B and `_mscd_buf` 4,096 B are in DRAM, along with MIDI and vendor
  interface state.
- These are compiled into the prebuilt `libarduino_tinyusb.a`, so removing them needs
  a custom core build. Note only; no action.

---

### 8.7 Leftovers from the §7 cleanup (low priority, mechanical)

**Still open from §7:**
- **Orphan images:** all 10 `src/img_*.c` files except `img_start` are unreferenced:
  A7682E, batt, GPS, lora, PCM5102, test, touch, wifi, SD and setting.
  - `assets.h` also declares `img_about`/`img_other`, which have no definition in
    factory.
  - Delete the 10 files and 12 `LV_IMG_DECLARE` lines. The linker already drops them,
    so this is 0 B, but it speeds up the build.
- **S1 ◐:** the A7682 and PCM5102 screens are still line-for-line copies, about 70
  lines each (`*_scr_event`, `*_item_create`, `on_key_*`, create/destroy).
- **S9 ◐:** the Esc/Enter/j/k edit-mode state machine is duplicated in `on_key_scr1_2`
  (`ui_lora.cpp`) and the System Font handler (`ui_reader.cpp`).
- **S19 ◐:** raw add-hidden-then-clear-if blocks remain in `ui_deckpro.cpp` and
  `lock_apply_orientation`.
- **S23 ◐:** 9 copies of `Preferences prefs; prefs.begin(kNvsNs, …)` with no RAII
  wrapper.
- **S4 nit:** every setting default is stated twice, once in its static initialiser
  and once in the `k_settings[]` `dflt` column.
- **§7.4 leftovers:**
  - `scr_mgr_push/pop/switch` keep a dead `bool anim` parameter (about 43 callers pass
    `false`), and no caller reads their `bool` returns.
  - Redundant forward declarations directly above `test_item_create`,
    `a7682_item_create` and `pcm5102_item_create`.
  - `lock_last_minute/yday = -1` before `lock_update_clock(true)`.
  - The redundant `peri_init_st[E_PERI_PCM5102A] = false`.
  - About 5 commented-out `Serial` lines (`peri_gyroscope.cpp`, `peri_keypad.cpp`).
- **C4 ◐:** `examples/tethering/utilities.h` is still a separate copy of the pin
  block.

**New:**
- **Dead code:**
  - With no indev registered, `scr_back_btn_create` and its click handlers can never
    fire, and its `cb` argument is always `scr_pop_event_cb`.
  - `E_PERI_TOUCH`, `T_DECK_PRO_V1_0` and `TASKBAR_ID_BATTERY_PERCENT` are unused.
  - `isT_Deck_Pro_v1_0` is only printed.
  - `entry11`/`exit11` can never run, because `create11` ends in
    `esp_deep_sleep_start()`.
  - `ui_test_handle.cb` is always `ui_test_get`.
  - The PCM5102 `#else` stubs in the port are dead, since both callers are now
    `#ifdef`'d.
- **Duplicated logic:**
  - `ui_setting_set_a7682_status` repeats `ui_a7682_loop_resume/suspend`.
  - The CS-HIGH loop in `setup()` duplicates `shared_spi_release_all_cs()`.
  - Screens 1 and 6 have the same menu shape (list + 2 items + ESC/shortcut → push),
    and each shortcut letter is written in both the label and `on_key`.
- **Includes and naming:**
  - Unused includes: `<Preferences.h>` in `factory.ino` and `<TinyGPS++.h>` in the
    port.
  - `gps`, `als` and `bhy` are non-static file-scope globals with generic names; make
    them `static`.
  - `taskbar_statue` is a `uint16_t[]` used as bools.
- **Build hygiene:**
  - `examples/factory/factory.ino.cpp` is a PlatformIO preprocessing leftover. It is
    untracked, not ignored, and graft indexes it twice. Add `*.ino.cpp` to
    `.gitignore`.
  - `EEPROM` in `lib_deps` and the stale `; -DLOG_PORT` line in `platformio.ini`.
- **Logging:**
  - `gps_task` prints "No GPS detected" once a second forever after 30 s, with a
    `delay(1000)` inside the task.
  - Debug `printf`s at `ui_lora.cpp` (settings handler) use `%.1f`, which keeps float
    printf linked (M2).
- **Stale comments:**
  - The port says `reader_faces[]` is in `ui_deckpro.cpp`; it is in `ui_reader.cpp`.
  - `ui_reader.cpp` and `ui_lockscreen.cpp` still say "Pure move… no behaviour
    change".
  - The port's `[ screen 9 ] Input` banner is wrong: screen 9 is Shutdown.
- **Gated build (`BOARD_HAS_BHI260AP`):** `BHI260AP_init` holds `i2c0_lock` while it
  uploads the ~103 KB firmware blob, which stalls the keypad for seconds. Upload in
  chunks and release the lock between them.

---

### 8.8 Suggested order of work

Flash and serial-check after each step; there is no test suite. Each tier is
independent of the next.

**Tier A: correctness (small, do first)**
1. N1 (dictionary off-by-one), with a host-side test over the real TSV.
2. N2 (reader pop inside `entry`) and N4 (MSC unmount/remount).
3. N5 (notes length cap + `LV_ASSERT_HANDLER abort()`).
4. N3 + P5 (no boot TX; sleep the radio).
5. N7 (`WiFi.persistent(false)`) and N8 (clear INT before draining).
6. N6 (Settings toggles), then N9/N10.

**Tier B: one-liners with a large user-visible effect**
7. P2: cursor blink off, a 1-line fix per textarea. Notes and Dictionary become
   usable at once.
8. IO1 + IO2: SD at 20 MHz, plus `setBufferSize`. Dictionary, reader and MSC get
   about 10–20× faster.
9. IO3: drop the SPIFFS mount from the 4G build. Faster boot, no surprise format, and
   23 KB of flash back.
10. IO4: `Serial.setTxTimeoutMs(0)`.
11. P6: LTR553 to standby.

**Tier C: power (the big battery win; do in order)**
12. P3 (GPS duty-cycling), P4 (modem off by default, lazy task) and P7 (one-shot ADC,
    slower polling).
13. P10 (time persist every 15 min) and P9 (WiFi off after sync).
14. P8 (event-driven loop) **then** P1 (auto-lock, 80 MHz idle, light sleep on the
    lock screen). Measure battery current before and after with a USB power meter on
    battery-only.

**Tier D: e-paper pipeline (verify each on hardware; touches the vendored GxEPD2)**
15. D1 (one refresh per frame).
16. D3 (release the SPI mutex during BUSY).
17. D2 (keep the panel initialised between partial updates).
18. D4 (bulk SPI writes).
19. D5 (partial transitions + ghosting counter) and D6 (persist the resume cookie).

**Tier E: memory/flash and cleanup**
20. M1 (measure, then size or move the LVGL heap) and M4 (shared styles).
21. M3 (fonts 17/19, subset spleen_32x64) and M2 (fixed-point printf).
22. Everything in §8.7, one commit per file.

### 7.9 Status (2026-10-25, uncommitted working tree)

All of §7.1–§7.6 is applied. Decisions taken: D-S4 notes list **deleted**, D-T1 touch **dropped
entirely** (hyn_* removed), D-F1 variant UI **#ifdef'd** behind `BOARD_HAS_PCM5102A` / `BOARD_HAS_BHI260AP`.

- Default build: 0 `examples/factory` warnings with the §7 instrumented flags; firmware.bin
  1,305,120 → **1,226,976 B** (−78 KB, mostly C1).
- Variant build (`-DBOARD_HAS_PCM5102A -DBOARD_HAS_BHI260AP`) and `[env:T-Deck-Pro-modem]` both build.
- Source in `examples/factory`: 13,707 → **10,110 lines**.
- Not done: C4 "share one A7682E pin header" — the factory `utilities.h` folds the pins it doesn't use
  (e.g. `BOARD_A7682E_RST`) into a comment, so tethering keeps its own copy.
- A review pass found 4 follow-ups in the new lazy-init code, now fixed: the GPS init race, LoRa radio
  calls before init, LoRa optional-config steps made non-fatal, and ESC now cancels the shutdown countdown.
  It also found an older bug, now fixed: the reader popped the screen from inside `scr_mgr_push` when
  PSRAM allocation failed.
- Still needs on-device verification (there is no test suite).

---

## 9. Fourth pass: §8 implemented (2026-10-25, night)

Every §8 item is now handled, either in code or below with a reason it was not.
All of this is **verified by compiling only; no hardware test was possible**:
- `pio run -e T-Deck-Pro` builds with no `-Wunused-*` warnings in `examples/factory`.
- The gated variant (`-DBOARD_HAS_PCM5102A -DBOARD_HAS_BHI260AP`) builds.
- The tethering env (`T-Deck-Pro-modem`) builds.

Flash each tier and watch serial before trusting it on battery.

| Metric | §8.0 | **Now** |
| --- | --- | --- |
| `firmware.bin` | 1,226,608 B | **1,175,488 B** |
| `.dram0.data` + `.dram0.bss` | 114,876 B | **65,908 B** (25,004 + 40,904). The LVGL pool moved to PSRAM (M1) |
| `.flash.text` / `.flash.rodata` | 792,359 / 329,896 B | 775,739 / 294,980 B |

**Host-side test:** `python3 -I tools/test_dict_lookup.py eng-pob.tsv` checks lookups against the real 108 MB TSV.
- The fixed bisection and the new indexed lookup miss 0 of 400 sampled words.
- The old algorithm misses 135, which is the negative control.

### 9.1 Status per §8 item

**Bugs (§8.2)**
- **N1 ✅** Scan `[lo, hi]` inclusive, with no skip at `lo`, and never let `hi` grow.
- **N2 ✅** Was already fixed (deferred pop via `lv_async_call`).
- **N3/P5 ✅** No boot TX. The radio warm-sleeps after init and on leaving screens 1.1/1.2. Turning LoRa off now forces a re-init on the next power-on (`lora_power_off`).
- **N4 ✅** MSC closes the reader and dictionary handles. `sd_fs_ready()` fences every FatFs entry point while the card is exported. `ui_usb_msc_end` remounts the card fresh.
- **N5 ✅** Notes are capped at 4,096 characters. `LV_ASSERT_HANDLER` is now `abort()`.
- **N6 ✅** Turning the modem on runs a lazy probe task. Turning it off clears `peri_init_st`. GPS and LoRa were already done.
- **N7 ✅** `WiFi.persistent(false)` in `setup()`.
- **N8 ✅** `INT_STAT` is cleared before draining, and the drain re-runs while INT stays low. The priority comment is fixed (the keypad is 19).
- **N9 ✅** Names are 96 B (`UI_READER_NAME_MAX`). The list holds 64 entries, allocated in PSRAM.
- **N10 ✅** An LRU blob of 48 books (`bm_lru`). Evicting a book removes its `b_`/`p_`/`c_` keys.

**Power (§8.3)**
- **P1 ✅ (steps 1–3).**
  - The event-driven `loop()` drops to 80 MHz while waiting at least 100 ms.
  - Auto-lock after 10 min without a key. It skips the USB, AT, LoRa-test and reader screens: the reader's `entry()` would restart the book.
  - Light sleep on the lock screen, on battery only, until the next minute or a key.
  - `-DFACTORY_IDLE_POWER_SAVE=0` reverts all of this.
  - Step 4 (custom sdkconfig, DFS + auto light sleep) is not done; it needs a custom core.
- **P2 ✅** All five textareas have a solid cursor.
- **P3 ✅** The GPS rail drops once the clock is set. The module powers up hourly with a 5 min fix window, and runs continuously while screen 3 is open. GLL/GSA/GSV/VTG are disabled.
- **P4 ✅**
  - The modem defaults to off, which only affects fresh NVS.
  - The rail drops when the probe fails.
  - The pump task is created lazily, only on the AT-test screen, and polls every 10 ms. The call screen no longer runs the pump, which raced TinyGSM.
- **P6 ✅** LTR553 is probed only, then left in standby.
- **P7 ✅** One-shot ADC, switched to continuous only on screen 6.1. Gauge reads every 30 s and charger presence every 5 s. The low-voltage check runs every 10 s, and every 1 s while latched. The keypad safety net is 5 s. I²C runs at 400 kHz.
- **P8 ✅** The keypad task notifies the loop task. Both key-polling timers are gone; the home screen has an `on_key` like every other screen. `bq25896_runtime_maintain` runs on a 5 s `lv_timer`.
- **P9 ◐** `WIFI_PS_MAX_MODEM` after connect. Disconnect-after-NTP was **not** done: it would fight the WiFi on/off state shown in the taskbar and used by ping.
- **P10**
  - **✅** The clock is saved every 15 min, plus on lock, deep sleep and shutdown.
  - **✅** Deep sleep also wakes on a key, using ext1 on the keypad INT with an RTC pull-up.
  - **⬜ by design:** `BOARD_1V8_EN` gating is measure-first.
  - **⬜ hardware:** EPD hibernate needs the RST line wired.

**E-paper (§8.4)**
- **D1 ✅** `decodebuffer` is now a persistent full-panel mirror. The areas of a frame are unioned and refreshed once, on `lv_disp_flush_is_last`, and FULL vs PART is decided per frame.
- **D2 ✅ (needs hardware check)** `_Update_Part` no longer clears `_init_display_done`. `-DGDEQ031T10_REINIT_AFTER_PARTIAL=1` restores the upstream behaviour.
- **D3 ✅** The busy callback lends the SPI bus out during BUSY waits when the task holds it at depth 1. `disp_hard_refresh` takes the guard per fill, and the boot `SD.usedBytes()` is gone.
- **D4 ✅** Bulk `writeBytes` per row in `_writeImage` / `_writeImagePart`.
- **D5 ✅** `ui_disp_full_refr()` is now a transition refresh: partial, with every 6th transition full (`DISP_FULL_EVERY_N_TRANSITIONS`, tune on hardware). `ui_disp_force_full_refr()` covers the ghost clean-up points. The shared `ui_entry_taskbar` / `ui_exit_taskbar` hooks replace 9 identical entry/exit pairs.
- **D6 ✅** A `c_%08x` cookie per book.

**Storage (§8.5)**
- **IO1 ✅** Mount at 20 → 10 → 4 → 1 MHz.
- **IO2 ✅**
  - `setBufferSize(0)` on the dictionary file.
  - The file stays open while the dictionary screen is up.
  - A sparse sidecar index (`tools/build_dict_index.py`, one record per 64 lines, about 760 KB) is loaded into PSRAM. If the index is missing or stale, the lookup falls back to bisection.
- **IO3 ✅** The SPIFFS mount happens only in the audio build, without format-on-fail. The LittleFS switch is not done; nothing in the 4G build uses the partition.
- **IO4 ✅** `Serial.setTxTimeoutMs(0)`.
- **IO5 ✅** `max_files = 3`, and the reader file buffer is 1 KB.

**Memory and flash (§8.6)**
- **M1 ✅** The pool is `ps_malloc`'d, 128 KB. A `[lvgl-mem]` line is logged on every screen change; size the pool from a real session's `max_used`.
- **M2 ◐** Our code has no float formats left (`ui_fmt_fixed`), and `__ssvfiscanf_r` dropped out. `_vfprintf_r`, `_svfprintf_r` and `_dtoa_r` are still linked, because the core and IDF logging use full newlib printf. Removing them needs nano specs or a custom core.
- **M3 ✅** Sizes 17/19 now map to 16/18, and those font files are deleted. `spleen_32x64` is subset to U+002D–U+003A by the new `tools/subset_lvgl_font.py`.
- **M4 ✅ (helpers)** Shared `lv_style_t` for list rows, the inverted focus state and menu items. Fonts stay local because they change at runtime. Most of the remaining ~240 one-off calls are layout and were left alone.
- **M5 —** Note only (prebuilt TinyUSB).

**Leftovers (§8.7)** ✅
- S1 (shared action-list helpers), S9 (`ui_edit_nav_key`), S19, S23 (`NvsPrefs`) and S4.
- `scr_mgr_*` lost the `anim` parameter and now return `void`.
- The redundant forward declarations and resets are deleted, as are the PCM5102 `#else` branches.
- C4: the tethering sketch includes the factory pin map.
- The back button is replaced by `scr_title_create`. `E_PERI_TOUCH`, `T_DECK_PRO_V1_0`, `isT_Deck_Pro_v1_0`, `entry11`/`exit11` and `ui_test_handle.cb` are gone.
- Screens 1 and 6 use the `ui_shortcut_menu_*` tables.
- Includes are cleaned up, and `gps`, `als` and `bhy` are static. `taskbar_statue` is now `int taskbar_state[]`.
- `*.ino.cpp` is gitignored, and `EEPROM` and the `LOG_PORT` line are gone.
- The GPS log spam and the stale comments are fixed.
- BHI260AP: a weak SensorLib hook lets `BHI260AP_init` yield i2c0 between BHY2 transfers.
- `TASKBAR_ID_BATTERY_PERCENT` is used; §8.7 was wrong about it.
- §1.8 `lib_ldf_mode = chain+` was tried and reverted: it cannot evaluate ESP-IDF `SOC_*` macros and breaks the audio build.

**Still open: hardware verification.**
1. D1/D2/D5 refresh quality and ghosting.
2. P1 light sleep and wake: keypad INT re-arm, and the clock on a timer wake.
3. P3 GPS resync.
4. P10 key wake from deep sleep: does the keypad INT have a usable pull-up?
5. IO1 SD at 20 MHz on the shared bus.
6. Measure battery current before and after with `-DFACTORY_IDLE_POWER_SAVE=0/1`.

**Re-verified 2026-10-25 (late), from a clean build directory; no board attached:**
- Default env with the `-Wunused-*` flags: SUCCESS, 0 unused warnings, `firmware.bin` 1,175,488 B (matches the table above).
- Variant `-DBOARD_HAS_PCM5102A -DBOARD_HAS_BHI260AP`: SUCCESS.
- `T-Deck-Pro-modem` with `PLATFORMIO_SRC_DIR=examples/tethering` (as `make upload-modem` sets it): SUCCESS.
  Without that variable it compiles `examples/factory` and fails, which is expected.
- `tools/test_dict_lookup.py eng-pob.tsv --expect-old-failures`: 302 words; fixed 0 misses, indexed 0, old 108 (negative control).
- The six hardware checks above are still open: they need the board on USB and, for item 6, a power meter.

**Review fixes after `0af64f3` (build-verified, default + variant, 0 warnings, `firmware.bin` 1,175,696 B):**
- GPS: `gps_task_resume()` re-powers the module instead of assuming the rail is up, so the GPS screen works after a duty-cycle power-down. Resume respects the GPS setting. The resync window starts when the resync begins.
- Keypad: re-drain after 20 ms while INT stays low, capped at 10 tries, so a stuck line falls back to the 5 s safety net.
- Keys left in a burst after a screen switch go to the new screen straight away (`ui_deckpro_dispatch_keys`), not at the next timer wake.
- The lock screen does not light-sleep once the low-voltage countdown latches (`ui_low_voltage_latched`).
- The modem probe result is dropped if the modem was turned off during the probe. Deep sleep keeps RTC_PERIPH on so the keypad INT pull-up holds. `ui_dict_close()` takes `SpiGuard`. `bq25896_runtime_maintain` lost its redundant `millis()` gate.
- Still open: a failed modem probe cuts the rail, but the setting still shows ON. Auto-lock still skips the reader, because re-entering it restarts the book.

---

## Appendix — measurement commands

```bash
TC=~/.platformio/packages/toolchain-xtensa-esp32s3/bin
E=.pio/build/T-Deck-Pro/firmware.elf

$TC/xtensa-esp32s3-elf-size -A $E | sort -k2 -rn | head -20   # sections
$TC/xtensa-esp32s3-elf-nm -C --size-sort -r -S $E | head -45  # largest symbols
$TC/xtensa-esp32s3-elf-nm -C --size-sort -r -S \
    .pio/build/T-Deck-Pro/src/peri_gyroscope.cpp.o            # the gyro blob

# Why an archive got linked at all:
awk '/^Archive member included/,/^Discarded input sections/' \
    .pio/build/T-Deck-Pro/firmware.map | grep -A1 'libmesh.a('
```

Per-object totals in this document come from a map-file parser that sums
`.text*`, `.rodata*`, `.data*`, `.bss*`, `.iram*` and `.dram*` input sections per
contributing object, excluding all `.debug_*`, `.xt.prop` and `.xt.lit`
sections. Raw `ls -l` on `.o` files is **not** comparable — those are ~90% DWARF.

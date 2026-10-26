/**
 * @file      factory.ino
 * @author    ShallowGreen123
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2024-05-27
 *
 */


#include <Arduino.h>
#include "utilities.h"
#include <GxEPD2_BW.h>
#include "lvgl.h"
#include "ui_deckpro.h"
#include "factory.h"
#include "ui_deckpro_port.h"
#include "peripheral.h"
#include <freertos/semphr.h>
#include <WiFi.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// 0 restores the always-240 MHz, never-sleep behaviour (for current measurements).
#ifndef FACTORY_IDLE_POWER_SAVE
#define FACTORY_IDLE_POWER_SAVE 1
#endif

TinyGsm modem(SerialAT);
TaskHandle_t a7682_handle;

XPowersPPM PPM;
BQ27220 bq27220;
#ifdef BOARD_HAS_PCM5102A
Audio audio;
#endif

static constexpr uint16_t FACTORY_BATTERY_DESIGN_CAPACITY_MAH = 1400;
static constexpr uint16_t FACTORY_BQ25896_CHARGE_TARGET_MV = 4208;
static constexpr uint16_t FACTORY_BQ25896_FAST_CHARGE_MA = 512;
static constexpr uint16_t FACTORY_BQ25896_PRECHARGE_MA = 128;
static constexpr uint16_t FACTORY_BQ25896_TERMINATION_MA = 128;
static constexpr uint16_t FACTORY_BQ25896_INPUT_LIMIT_MA = 1000;
static constexpr uint16_t FACTORY_BQ25896_SYS_POWER_DOWN_MV = 3300;
static constexpr uint32_t FACTORY_BQ25896_RUNTIME_CHECK_MS = 5000;
static constexpr uint32_t FACTORY_BQ25896_RECOVERY_COOLDOWN_MS = 30000;
static constexpr uint32_t FACTORY_EPD_SPI_HZ = 20000000;
// Page height is HEIGHT/8 (40 rows, 1,200 B) rather than HEIGHT (9,600 B).
// The normal flush path bypasses the page buffer entirely — epd_push_frame()
// calls display.epd2.writeImage() directly. The paged buffer is only used by
// disp_hard_refresh() and disp_white_clear() (fillScreen loops), which iterate
// over 8 pages of 40 rows each with no visible difference to the user.
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT / 8> display(GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)); // GDEQ031T10 240x320, UC8253, (no inking, backside mark KEGMO 3100)

// Panel-native framebuffer geometry: one bit per pixel, row-major, stride
// rounded up to whole bytes. decodebuffer is a persistent full-panel mirror in
// this layout (EPD_BITMAP_BUF_SIZE bytes, allocated in lvgl_init): every flush
// packs its dirty area into it and the panel is refreshed once per LVGL frame
// from the union of those areas.
#define EPD_PANEL_STRIDE EPD_BITMAP_STRIDE(LCD_HOR_SIZE)

static uint8_t *decodebuffer = NULL;
static int disp_refr_mode = DISP_REFR_MODE_PART;
// A screen change refreshes the whole screen through the fast partial path;
// every Nth transition escalates to a full (flashing) refresh to clear the
// ghosting partial updates accumulate. Lower N if transitions ghost.
#define DISP_FULL_EVERY_N_TRANSITIONS 6  // tune on hardware: lower if transitions ghost
static uint8_t s_partial_transitions = 0;
static bool s_transition_pending = false;
// Tracks whether LVGL is rendering in landscape (320x240 logical). We do the
// 90° rotation ourselves during bitmap packing instead of using LVGL's
// sw_rotate, which would split a full-screen rotated flush into ~6-8 strips
// (lv_refr.c:1201, max_row = LV_DISP_ROT_MAX_BUF / area_w) — each strip
// triggering its own ~700 ms EPD refresh.
static bool s_landscape = false;

bool peri_init_st[E_PERI_NUM_MAX] = {0};
static SemaphoreHandle_t shared_spi_mutex = nullptr;
// Nesting depth for shared_spi_lock/unlock. Incremented after the recursive
// mutex is taken; decremented before it is given. shared_spi_release_all_cs()
// is called only when the outermost unlock runs (depth reaches 0).
static uint32_t s_spi_depth = 0;

// One-shot LVGL timer used to defer display.epd2.powerOff() by ~2 s after the
// last flush. Both this timer and epd_push_frame() run from lv_task_handler()
// on the Arduino loop task, so there is no concurrency between them.
static lv_timer_t *s_epd_poweroff_timer = NULL;

static void shared_spi_release_all_cs()
{
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS, HIGH);
    digitalWrite(BOARD_EPD_CS, HIGH);
}

void shared_spi_bus_init(void)
{
    if (shared_spi_mutex == nullptr) {
        shared_spi_mutex = xSemaphoreCreateRecursiveMutex();
        if (shared_spi_mutex == nullptr) {
            Serial.println("[SPI] Failed to create shared bus mutex");
            return;
        }
    }
    shared_spi_release_all_cs();
}

void shared_spi_lock(void)
{
    if (shared_spi_mutex != nullptr) {
        xSemaphoreTakeRecursive(shared_spi_mutex, portMAX_DELAY);
        // Depth is updated only after the mutex is acquired, so it is
        // always consistent with the recursive hold count.
        s_spi_depth++;
    }
}

void shared_spi_unlock(void)
{
    if (shared_spi_mutex != nullptr) {
        // Release all CS lines only when the outermost lock is unwound.
        // An inner unlock inside a nested critical section must not deassert
        // CS while the outer holder is still mid-transaction.
        if (--s_spi_depth == 0) {
            shared_spi_release_all_cs();
        }
        xSemaphoreGiveRecursive(shared_spi_mutex);
    }
}

// Power-rail and modem-powerkey pins held across deep sleep.
static const uint8_t k_rail_pins[] = {
    BOARD_6609_EN,
    BOARD_LORA_EN,
    BOARD_GPS_EN,
    BOARD_1V8_EN,
    BOARD_A7682E_PWRKEY,
};

void board_rails_hold(bool hold)
{
    for (uint8_t pin : k_rail_pins) {
        if (hold) {
            gpio_hold_en((gpio_num_t)pin);
        } else {
            gpio_hold_dis((gpio_num_t)pin);
        }
    }
    if (hold) {
        gpio_deep_sleep_hold_en();
    } else {
        gpio_deep_sleep_hold_dis();
    }
}

/*********************************************************************************
 *                              STATIC PROTOTYPES
 * *******************************************************************************/
// The panel refreshes for 0.7-1.1 s with its CS high and the bus idle. When
// this task holds the bus exactly once, lend it to LoRa / SD / USB-MSC for the
// duration of the BUSY wait instead of spinning with it locked.
static void epd_busy_cb(const void *)
{
    if (shared_spi_mutex != nullptr && s_spi_depth == 1 &&
        xSemaphoreGetMutexHolder(shared_spi_mutex) == xTaskGetCurrentTaskHandle()) {
        shared_spi_unlock();
        vTaskDelay(1);
        shared_spi_lock();
    } else {
        delay(1);
    }
}

static bool ink_screen_init()
{
    SpiGuard spi;

    display.epd2.selectSPI(SPI, SPISettings(FACTORY_EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
    display.init(115200, true, 2, false);
    display.epd2.setBusyCallback(epd_busy_cb);
    display.setRotation(0);
    display.powerOff();
    return true;
}

// Pack one LVGL area into the persistent panel-native mirror. In portrait mode
// the layout is a direct 1:1 row-major pack; in landscape we rotate 90° CW
// during the pack itself so a single writeImagePart can push the whole dirty
// area to the panel — avoiding LVGL's sw_rotate strip-splitting (and its
// multiple EPD refresh cycles).
//
// Polarity is panel-native: bit clear = black, bit set = white. Only the bytes
// covered by the area are written; the rest of the mirror keeps the pixels
// packed from earlier areas of the same frame.
static void pack_portrait(const lv_color_t *color_p, const lv_area_t *pa)
{
    const lv_coord_t width = lv_area_get_width(pa);
    const size_t stride = EPD_BITMAP_STRIDE(width);
    for (lv_coord_t y = 0; y < lv_area_get_height(pa); ++y) {
        const lv_color_t *row = color_p + size_t(y) * size_t(width);
        uint8_t *dst = decodebuffer + size_t(pa->y1 + y) * EPD_PANEL_STRIDE + (pa->x1 >> 3);
        lv_coord_t x = 0;
        for (size_t byte_idx = 0; byte_idx < stride; ++byte_idx) {
            uint8_t byte = 0xFF;
            uint8_t mask = 0x80;
            for (int b = 0; b < 8 && x < width; ++b, ++x) {
                if (lv_color_brightness(row[x]) < 128) {
                    byte &= ~mask;
                }
                mask >>= 1;
            }
            dst[byte_idx] = byte;
        }
    }
}

// 90° CW rotation: logical (lx, ly) maps to panel
// (pa->x1 + (lh - 1 - ly), pa->y1 + lx), matching the prior Adafruit_GFX
// setRotation(1) behaviour. Panel area dimensions swap: panel_w = lh,
// panel_h = lw. The rounder_cb guarantees lh is a multiple of 8, so pa->x1 —
// and therefore the byte range — stays 8-aligned for writeImagePart.
//
// Loop is transposed so color_p reads are sequential (good for PSRAM); each
// pixel writes one bit into the mirror, so we first set the area's bytes to
// 0xFF (all-white) and then clear bits for dark pixels.
static void pack_landscape(const lv_color_t *color_p, lv_coord_t lw, lv_coord_t lh,
                           const lv_area_t *pa)
{
    // White-out this area's slice of the mirror, one panel row at a time.
    const size_t first_byte = size_t(pa->x1) >> 3;
    const size_t last_byte  = size_t(pa->x2) >> 3;
    for (lv_coord_t py = pa->y1; py <= pa->y2; ++py) {
        memset(decodebuffer + size_t(py) * EPD_PANEL_STRIDE + first_byte, 0xFF,
               last_byte - first_byte + 1);
    }

    for (lv_coord_t ly = 0; ly < lh; ++ly) {
        // This logical row maps to panel column pa->x1 + (lh - 1 - ly); the
        // byte index and bit mask depend only on ly, so compute them once per
        // logical row instead of per pixel.
        const lv_coord_t panel_col = pa->x1 + (lh - 1 - ly);
        const size_t byte_idx = size_t(panel_col) >> 3;
        const uint8_t bit_mask = uint8_t(1u << (7 - (panel_col & 0x7)));
        const lv_color_t *row = color_p + size_t(ly) * size_t(lw);

        for (lv_coord_t lx = 0; lx < lw; ++lx) {
            if (lv_color_brightness(row[lx]) < 128) {
                // panel row == pa->y1 + lx (logical column maps to panel row)
                decodebuffer[size_t(pa->y1 + lx) * EPD_PANEL_STRIDE + byte_idx] &= ~bit_mask;
            }
        }
    }
}

// Convert a logical LVGL area to panel-native coordinates, applying 90° CW
// rotation in landscape mode. Output area always describes pixels in the
// physical 240x320 panel coordinate system.
static void logical_area_to_panel(const lv_area_t *logical, lv_area_t *panel)
{
    const lv_coord_t lw = lv_area_get_width(logical);
    const lv_coord_t lh = lv_area_get_height(logical);
    if (s_landscape) {
        panel->x1 = LCD_HOR_SIZE - logical->y1 - lh;   // = LCD_HOR_SIZE - y2 - 1
        panel->x2 = panel->x1 + lh - 1;
        panel->y1 = logical->x1;
        panel->y2 = panel->y1 + lw - 1;
    } else {
        *panel = *logical;
    }
}

// Timer callback that powers the EPD panel off after ~2 s of display
// idleness. Runs from lv_task_handler() — same task as flush_epd_bitmap()
// and ink_screen_prepare_shutdown() — so there is no concurrency.
static void epd_poweroff_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_epd_poweroff_timer = NULL;  /* mark as expired before the SPI call */
    SpiGuard spi;
    display.epd2.powerOff();
}

// Cancel the deferred power-down timer before explicitly powering off.
// Both this function and the timer callback run from the LVGL task so
// there is no concurrency, but we must not leave a stale timer that
// fires after the panel has already been powered off for sleep/shutdown.
static void epd_cancel_poweroff_timer(void)
{
    if (s_epd_poweroff_timer != NULL) {
        lv_timer_del(s_epd_poweroff_timer);
        s_epd_poweroff_timer = NULL;
    }
}

// Fill the whole panel with one colour. The caller owns setFullWindow().
static void epd_fill(uint16_t color)
{
    display.firstPage();
    do {
        display.fillScreen(color);
    } while (display.nextPage());
}

// Union of the panel areas packed during the current LVGL frame, and whether
// anything at all was packed.
static lv_area_t s_dirty;            // union of this frame's panel areas
static bool      s_dirty_valid = false;

// One e-paper refresh per LVGL frame. LVGL calls disp_flush() once per
// invalidated area — a single frame can have several (independent labels, a
// scrolled container, the status bar plus the page). Each area is packed into
// the persistent mirror as it arrives and the union is pushed here, once, when
// LVGL reports the last flush of the frame: N ~700 ms refreshes collapse into
// one, and FULL vs PART is decided once per frame.
static void epd_push_frame(void)
{
    {
        SpiGuard spi;

        // Direct write to controller memory + refresh, skipping the
        // firstPage()/nextPage() Adafruit_GFX raster path. decodebuffer is
        // already in panel-native polarity and orientation; the rounder_cb
        // enforces the 8-pixel x-alignment writeImage requires.
        //
        // writeImagePartAgain after refresh updates the controller's "previous"
        // buffer — UC8253 partial refresh diffs current vs. previous, so without
        // this step the next partial flush would overlay onto a stale baseline,
        // causing ghosting on page turns. writeImageForFullRefresh seeds both
        // buffers in one call, so the full path doesn't need the *Again variant.
        if (disp_refr_mode == DISP_REFR_MODE_FULL) {
            display.epd2.writeImageForFullRefresh(decodebuffer, 0, 0,
                                                  LCD_HOR_SIZE, LCD_VER_SIZE, false);
            display.epd2.refresh(false);
            s_partial_transitions = 0;
        } else if (s_dirty_valid) {
            const lv_coord_t w = lv_area_get_width(&s_dirty);
            const lv_coord_t h = lv_area_get_height(&s_dirty);
            display.epd2.writeImagePart(decodebuffer, s_dirty.x1, s_dirty.y1,
                                        LCD_HOR_SIZE, LCD_VER_SIZE,
                                        s_dirty.x1, s_dirty.y1, w, h, false);
            display.epd2.refresh(s_dirty.x1, s_dirty.y1, w, h);
            display.epd2.writeImagePartAgain(decodebuffer, s_dirty.x1, s_dirty.y1,
                                             LCD_HOR_SIZE, LCD_VER_SIZE,
                                             s_dirty.x1, s_dirty.y1, w, h, false);
        }
    }

    // Defer powerOff() rather than doing it on every flush. Rapid UI changes
    // (typing, settings navigation) pay the panel power-on cost only once
    // for the whole burst. Re-arm the timer on each flush so the 2 s window
    // restarts from the *last* flush, not the first.
    if (s_epd_poweroff_timer != NULL) {
        lv_timer_reset(s_epd_poweroff_timer);
    } else {
        s_epd_poweroff_timer = lv_timer_create(epd_poweroff_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_epd_poweroff_timer, 1);
    }

    s_dirty_valid = false;
    disp_refr_mode = DISP_REFR_MODE_PART;
    s_transition_pending = false;
}

// GDEQ031T10::writeImage requires the partial-update X window to start and
// span multiples of 8 pixels (one byte per 8 horizontal pixels). LVGL hands us
// arbitrary invalidate bounding boxes in *logical* coordinates before any
// rotation is applied.
//
// The flush path (logical_area_to_panel + pack_portrait/pack_landscape)
// rotates 90° CW in landscape: logical-X maps to panel-Y and logical-Y maps to
// panel-X. So the axis that must be 8-aligned in panel space is:
//   portrait  — logical X  (direct 1:1)
//   landscape — logical Y  (becomes panel X after 90° CW rotation)
//
// Rounding is conservative: x1/y1 round down (never shrink the dirty area),
// x2/y2 round up.
static void display_driver_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    (void)disp_drv;
    if (s_landscape) {
        /* logical Y → panel X; align logical Y to 8-pixel boundary */
        area->y1 &= ~0x7;
        area->y2 |= 0x7;
    } else {
        /* logical X → panel X; align logical X to 8-pixel boundary */
        area->x1 &= ~0x7;
        area->x2 |= 0x7;
    }
}

static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    // LVGL calls flush once per invalidated area, possibly several times within
    // one frame. Packing every area into the mirror and refreshing the union
    // once per frame turns N ~700 ms refreshes into one, and FULL vs PART is
    // decided once per frame (see epd_push_frame).
    const lv_coord_t lw = lv_area_get_width(area);
    const lv_coord_t lh = lv_area_get_height(area);

    lv_area_t panel_area;
    logical_area_to_panel(area, &panel_area);

    if (s_landscape) {
        pack_landscape(color_p, lw, lh, &panel_area);
    } else {
        pack_portrait(color_p, &panel_area);
    }

    if (!s_dirty_valid) {
        s_dirty = panel_area;
        s_dirty_valid = true;
    } else {
        _lv_area_join(&s_dirty, &s_dirty, &panel_area);
    }

    if (lv_disp_flush_is_last(disp_drv)) {
        epd_push_frame();
    }

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_disp_flush_ready(disp_drv);
}

static void lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf_dsc_1;
    lv_color_t *buf_1 = (lv_color_t *)ps_calloc(sizeof(lv_color_t), DISP_BUF_SIZE);
    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, NULL, DISP_BUF_SIZE);
    decodebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_BITMAP_BUF_SIZE);
    if (decodebuffer) memset(decodebuffer, 0xFF, EPD_BITMAP_BUF_SIZE);  // mirror starts white

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_HOR_SIZE;
    disp_drv.ver_res = LCD_VER_SIZE;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc_1;
    disp_drv.rounder_cb = display_driver_rounder_cb;
    // full_refresh=0 lets LVGL push only the invalidated bounding box per
    // flush instead of the whole framebuffer; partial e-paper updates are
    // ~10x cheaper than full ones, so small UI changes (battery icon, key
    // blink) no longer cost a full-screen redraw. Opt-in full refresh still
    // works via disp_refr_mode = DISP_REFR_MODE_FULL.
    disp_drv.full_refresh = 0;

    lv_disp_drv_register(&disp_drv);
}

void ink_screen_prepare_shutdown(void)
{
    if (!peri_init_st[E_PERI_INK_SCREEN]) {
        return;
    }

    epd_cancel_poweroff_timer();

    SpiGuard spi;
    display.powerOff();
}

static bool bq25896_apply_factory_profile(void)
{
    PPM.resetDefault();
    PPM.disableWatchdog();
    PPM.exitHizMode();
    PPM.disableOTG();
    PPM.enableBatterPowerPath();
    PPM.setInputCurrentLimit(FACTORY_BQ25896_INPUT_LIMIT_MA);
    PPM.setSysPowerDownVoltage(FACTORY_BQ25896_SYS_POWER_DOWN_MV);
    PPM.setChargeTargetVoltage(FACTORY_BQ25896_CHARGE_TARGET_MV);
    PPM.setChargerConstantCurr(FACTORY_BQ25896_FAST_CHARGE_MA);
    PPM.setPrechargeCurr(FACTORY_BQ25896_PRECHARGE_MA);
    PPM.setTerminationCurr(FACTORY_BQ25896_TERMINATION_MA);
    PPM.enableChargingTermination();
    PPM.enableCharge();
    PPM.disableStatPin();
    // One conversion at init; continuous mode (1 s conversions) only while the
    // BQ25896 screen is open. VBUS_GD / CHRG_STAT are status bits and update
    // without the ADC.
    return PPM.enableMeasure(XPowersPPM::ONE_SHORT);
}

static bool bq25896_init(void)
{
    // BQ25896 --- 0x6B
    // The keypad task runs at priority 19 and will preempt this init,
    // corrupting multi-byte I2C transactions. Hold i2c0_lock for the whole
    // probe + init path.
    I2C0Guard g;

    Wire.beginTransmission(BOARD_I2C_ADDR_BQ25896);
    if (Wire.endTransmission() != 0) {
        return false;
    }
    if (!PPM.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_ADDR_BQ25896)) {
        return false;
    }
    return bq25896_apply_factory_profile();
}

static bool bq27220_init(void)
{
    // BQ27220 init issues a long sequence of control subcommands (DEVICE_NUMBER
    // probe, unseal, CFGUPDATE entry, data-memory writes, CFGUPDATE exit). A
    // keypad preempt mid-sequence has been observed to return 0xFFFF on the
    // chip-ID read, aborting init and leaving battery_percent stuck at 0.
    bool ret;
    uint16_t soc = 0, vbat = 0;
    int16_t curr = 0;
    {
        I2C0Guard g;

        bq27220.setDefaultCapacity(FACTORY_BATTERY_DESIGN_CAPACITY_MAH);
        ret = bq27220.init();
        if (ret) {
            soc  = bq27220.getStateOfCharge();
            vbat = bq27220.getVoltage();
            curr = bq27220.getCurrent();
        }
    }
    Serial.printf("[BQ27220] init=%s soc=%u%% vbat=%umV curr=%dmA\n",
                  ret ? "ok" : "FAIL", soc, vbat, curr);
    return ret;
}

static void bq25896_runtime_maintain(void)
{
    static uint32_t last_check_ms = 0;
    static uint32_t last_recovery_ms = 0;

    if (!peri_init_st[E_PERI_BQ25896]) {
        return;
    }
    if (millis() - last_check_ms < FACTORY_BQ25896_RUNTIME_CHECK_MS) {
        return;
    }
    last_check_ms = millis();

    {
        I2C0Guard g;

        if (!PPM.isVbusIn()) {
            return;
        }

        bool need_recover = !PPM.isCharging();
        if (!need_recover && peri_init_st[E_PERI_BQ27220]) {
            need_recover = (bq27220.getAverageCurrent() < 0);
        }
        if (!need_recover) {
            return;
        }
        if (millis() - last_recovery_ms < FACTORY_BQ25896_RECOVERY_COOLDOWN_MS) {
            return;
        }

        Serial.println("[BQ25896] Restore charge path");
        bq25896_apply_factory_profile();
    }
    last_recovery_ms = millis();
}

bool sd_care_init(void)
{
    SpiGuard spi;

    // SD spec requires >=74 clocks with CS HIGH after power-up before the
    // first command. On a shared bus the card may also have seen unrelated
    // traffic (EPD/LoRa init) with its CS wiggling — pulse out a dozen
    // 0xFF bytes with CS deasserted to guarantee a clean idle state.
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(BOARD_SD_CS, HIGH);
    for (int i = 0; i < 12; ++i) {
        SPI.transfer(0xFF);
    }
    SPI.endTransaction();

    // Fastest first: SD.begin() does the 400 kHz card-identification phase
    // itself, then every transfer runs at the frequency passed here. The
    // shared-bus mutex serialises LoRa/EPD traffic, so there is no reason to
    // crawl at 1 MHz.
    const uint32_t freqs[] = { 20000000, 10000000, 4000000, 1000000 };
    bool ok = false;
    for (uint8_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); ++i) {
        if (i > 0) {
            SD.end();
            delay(50);
        }
        // at most 3 files are ever open; each slot costs ~550 B
        if (SD.begin(BOARD_SD_CS, SPI, freqs[i], "/sd", 3)) {
            ok = true;
            Serial.printf("[SD CARD] mounted at %u Hz (attempt %u)\n",
                          (unsigned)freqs[i], (unsigned)(i + 1));
            break;
        }
        Serial.printf("[SD CARD] mount attempt %u @ %u Hz failed\n",
                      (unsigned)(i + 1), (unsigned)freqs[i]);
    }
    if (!ok) {
        Serial.println("[SD CARD] Card Mount Failed");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD CARD] No card detected after mount");
        return false;
    }
    Serial.printf("[SD CARD] type=%s\n",
                  cardType == CARD_MMC  ? "MMC"  :
                  cardType == CARD_SD   ? "SDSC" :
                  cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);

    uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
    Serial.printf("SD Card Total: %lluMB\n", totalSize);

    return true;
}

static void a7682_task(void *param)
{
    while (1)
    {
        while (SerialAT.available())
        {
            SerialMon.write(SerialAT.read());
        }
        while (SerialMon.available())
        {
            SerialAT.write(SerialMon.read());
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // 10 ms is plenty for a serial bridge; 1 ms woke the CPU 1000x/s
    }
}

bool a7682_probe(void)
{
    // Set module baud rate and UART pins. Only once: SerialAT.begin() re-runs
    // the UART driver install, so a re-probe after a rail cut must reuse it.
    static bool uart_started = false;
    if (!uart_started) {
        SerialAT.begin(115200, SERIAL_8N1, BOARD_A7682E_TXD, BOARD_A7682E_RXD);
        uart_started = true;
    }

    if (!ui_setting_get_a7682_status()) {
        Serial.println("[A7682E] rail off, skipping probe");
        return false;
    }

    Serial.println("Start modem...");

    // power on
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
    delay(50);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);

    const int retries = 7;
    bool ok = false;
    for (int i = 0; i < retries && !ok; i++) {
        ok = modem.testAT(1000);
        if (!ok) Serial.println(".");
    }
    if (!ok) {
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
        delay(100);
        digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
        delay(1000);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);

        digitalWrite(BOARD_6609_EN, LOW);  // nothing answered: drop the rail instead of idling a dead modem

        Serial.println("[A7682E] Init Fail");
    }

    Serial.println();
    delay(200);

    return ok;
}

// The serial bridge only runs while the AT-test screen is open; created lazily
// so a modem-less session never pays for its stack.
void a7682_pump_resume(void)
{
    if (a7682_handle == NULL) {
        xTaskCreate(a7682_task, "a7682_handle", 1024 * 3, NULL, A7682E_PRIORITY, &a7682_handle);
    } else {
        vTaskResume(a7682_handle);
    }
}

void a7682_pump_suspend(void)
{
    if (a7682_handle != NULL) vTaskSuspend(a7682_handle);
}

#ifdef BOARD_HAS_PCM5102A
// Not static: the .ino prototype generator would otherwise emit a static
// declaration outside the #ifdef and warn on the 4G build.
bool pcm5102a_init(void)
{
    bool ret = audio.setPinout(BOARD_I2S_BCLK, BOARD_I2S_LRC, BOARD_I2S_DOUT);

    if (ret == false)
        Serial.printf("[%d] Execution error\n", __LINE__);

    audio.setVolume(21); // 0...21

    pinMode(BOARD_6609_EN, OUTPUT);
    digitalWrite(BOARD_6609_EN, HIGH);

    return true;
}
#endif /* BOARD_HAS_PCM5102A */

static void peripheral_init_task(void *param)
{
    // SPI must be initialized before this task starts
    if (ui_setting_get_lora_status()) {
        peri_init_st[E_PERI_LORA] = lora_init();
    }
    peri_init_st[E_PERI_BQ25896]    = bq25896_init();
    peri_init_st[E_PERI_BQ27220]    = bq27220_init();
    peri_init_st[E_PERI_SD]         = sd_care_init();
    // Honour the persisted GPS on/off preference. We need GPS up at boot so
    // the topbar clock can be set from satellite time when WiFi is off —
    // otherwise the clock would stay at "--:--" until the user manually
    // toggles GPS in settings. The baud-scan in gps_init walks several baud
    // rates with delays, but we're already on a background FreeRTOS task so
    // boot UX is not blocked. gps_task starts itself suspended; resume it
    // explicitly here so NMEA sentences actually flow.
    if (ui_setting_get_gps_status()) {
        // Keep the settings toggle from starting a second baud scan while
        // this one is in flight (and vice versa).
        gps_init_running = true;
        peri_init_st[E_PERI_GPS] = gps_init();
        if (peri_init_st[E_PERI_GPS]) {
            gps_task_resume();
            // GPS shares GPIO 1 with BOARD_RED_LED. gps_init() disables the
            // GPS time pulse (PPS) so it stops driving the line, but during
            // baud-scan the module was free to pulse the pin — re-assert the
            // saved LED state now that we own the line again.
            pinMode(BOARD_RED_LED, OUTPUT);
            digitalWrite(BOARD_RED_LED, ui_setting_get_red_led());
        }
        gps_init_running = false;
    }
    // Gyro — peripheral.h stubs this as a no-op when the BHI260AP driver isn't compiled in.
    peri_init_st[E_PERI_BHI260AP]   = BHI260AP_init();
    peri_init_st[E_PERI_LTR_553ALS] = LTR553_init();
    peri_init_st[E_PERI_A7682E]     = a7682_probe();

#ifdef BOARD_HAS_PCM5102A
    if(peri_init_st[E_PERI_A7682E] == false)
    {
        peri_init_st[E_PERI_PCM5102A] = pcm5102a_init();
    }
#endif

    Serial.println("Background peripheral initialization complete.");
    vTaskDelete(NULL);
}

void setup()
{
    board_rails_hold(false);

    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);  // never stall a task for 100 ms when no terminal is reading

    ui_settings_load();

    // No battery-backed RTC: every power cycle resets the clock to 1970. Pull
    // the last persisted epoch out of NVS so the topbar shows a sensible time
    // straight away; GPS or NTP will correct any drift once they're up.
    ui_time_persist_restore();

    WiFi.persistent(false);  // credentials live in our own NVS keys; stop the core re-writing its AP config on every connect

    // Restore the user's last WiFi on/off choice. We honour the persisted
    // intent (wifi_en pref) rather than just "SSID present" — otherwise
    // toggling WiFi off in the UI would silently come back on after reboot.
    if (ui_wifi_get_enabled()) {
        ui_wifi_radio_set(true);
    }

    // Output rails and indicators, driven from the persisted settings.
    struct { uint8_t pin; bool level; } outs[] = {
        {BOARD_KEYBOARD_LED,  ui_setting_get_keypad_light()},
        {BOARD_RED_LED,       ui_setting_get_red_led()},
        {BOARD_MOTOR_PIN,     ui_setting_get_motor_status()},
        {BOARD_6609_EN,       ui_setting_get_a7682_status()},
        {BOARD_LORA_EN,       ui_setting_get_lora_status()},
        {BOARD_GPS_EN,        ui_setting_get_gps_status()},
        // BOARD_1V8_EN powers both the BHI260AP gyro and the CST328 touch IC.
        {BOARD_1V8_EN,        HIGH},
        {BOARD_A7682E_PWRKEY, ui_setting_get_a7682_status()},
    };
    for (auto &out : outs) {
        pinMode(out.pin, OUTPUT);
        digitalWrite(out.pin, out.level);
    }

    // SPI Pins
    // CS lines idle HIGH from the start so no device sees a select before
    // shared_spi_bus_init() runs.
    for (uint8_t pin : {BOARD_LORA_CS, BOARD_LORA_RST, BOARD_SD_CS, BOARD_EPD_CS}) {
        pinMode(pin, OUTPUT);
    }
    shared_spi_release_all_cs();
    digitalWrite(BOARD_LORA_RST, HIGH);

    // Optimized I2C Check
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Wire.setClock(400000);  // every device on I2C-0 supports fast mode; 4x shorter bus hold per transaction
    Serial.printf(" ------------- I2C ------------- \n");
    static const struct { uint8_t addr; const char *name; } k_i2c_devs[] = {
        {BOARD_I2C_ADDR_TOUCH,      "TOUCH"},
        {BOARD_I2C_ADDR_LTR_553ALS, "LTR_553ALS"},
        {BOARD_I2C_ADDR_GYROSCOPDE, "GYROSCOPDE"},
        {BOARD_I2C_ADDR_KEYBOARD,   "KEYBOARD"},
        {BOARD_I2C_ADDR_BQ27220,    "BQ27220"},
        {BOARD_I2C_ADDR_BQ25896,    "BQ25896"},
    };

    for (const auto &dev : k_i2c_devs) {
        Wire.beginTransmission(dev.addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[0x%x] %s find!\n", dev.addr, dev.name);
        }
    }

    // Hardware version detection (V1.1 has DRV2605 at 0x5A)
    Wire.beginTransmission(0x5A);
    const bool is_v1_0 = (Wire.endTransmission() != 0);

    Serial.printf("[board] T-Deck-Pro %s detected\n", is_v1_0 ? "V1.0" : "V1.1");

#ifdef BOARD_HAS_PCM5102A
    // Only the audio build reads SPIFFS (the ringtone); books and the
    // dictionary live on SD. Format-on-fail would erase ~3,400 blocks
    // (minutes of blank screen) on an unformatted partition, so never format
    // implicitly.
    if(!SPIFFS.begin(false)){
        Serial.println("SPIFFS Mount Failed");
    }
#endif

    // SPI
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    shared_spi_bus_init();

    // Critical Peripheral Init (Required for UI/Input)
    peri_init_st[E_PERI_INK_SCREEN] = ink_screen_init();
    peri_init_st[E_PERI_KYEPAD]     = keypad_init(BOARD_I2C_ADDR_KEYBOARD);

    lvgl_init();
    ui_deckpro_entry();

    // Battery runtime maintenance: was polled from loop() 1000x/s.
    lv_timer_create([](lv_timer_t *) { bq25896_runtime_maintain(); },
                    FACTORY_BQ25896_RUNTIME_CHECK_MS, NULL);

    disp_full_refr();

    // Start background initialization for slow peripherals
    xTaskCreate(peripheral_init_task, "peri_init", 1024 * 4, NULL, 1, NULL);

    // Start keypad task
    if (peri_init_st[E_PERI_KYEPAD]) {
        keypad_task_create();
    }

    // CPU frequency is managed by loop(): it drops to 80 MHz only while loop()
    // is waiting with nothing to do, so rendering and bitmap packing still run
    // at 240 MHz.
}


// Light sleep on the lock screen: the panel holds the image with no power, so
// with nothing pending the chip sleeps until the next minute boundary (clock
// update) or a key (TCA8418 INT held low). Returns true if it slept.
static bool lock_screen_light_sleep(void)
{
    if (scr_mgr_get_curr_scr_id() != SCREEN_LOCK_ID) return false;
    lv_disp_t *disp = lv_disp_get_default();
    if (disp && disp->inv_p != 0) return false;          // a frame is still pending
    if (s_epd_poweroff_timer != NULL) return false;      // panel not powered down yet
    if (ui_settings_pending()) return false;             // debounced NVS write queued
    if (gps_module_active()) return false;               // GPS UART would drop NMEA
    if (keypad_get_val(NULL)) return false;              // key waiting for dispatch
    if (digitalRead(BOARD_KEYBOARD_INT) == LOW) return false;
    if (ui_battery_is_external_power_present()) return false; // on USB: keep USB-CDC up

    time_t now = time(NULL);
    uint32_t to_minute_s = 60 - (uint32_t)(now % 60);
    gpio_num_t kint = (gpio_num_t)BOARD_KEYBOARD_INT;
    gpio_intr_disable(kint);
    gpio_wakeup_enable(kint, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)to_minute_s * 1000000ULL + 200000ULL);
    esp_light_sleep_start();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable(kint);
    gpio_set_intr_type(kint, GPIO_INTR_NEGEDGE);   // restore the keypad ISR's edge trigger
    gpio_intr_enable(kint);
    if (cause == ESP_SLEEP_WAKEUP_GPIO) keypad_kick();  // INT already low: no edge for the ISR
    else ui_lock_clock_refresh();
    return true;
}

void loop()
{
    // Event-driven: the keypad task notifies this task after every key burst,
    // and lv_timer_handler() reports when the next LVGL timer is due. The old
    // loop ran lv_task_handler() + vTaskDelay(1) at 1 kHz.
    ui_deckpro_dispatch_keys();
    uint32_t next_ms = lv_timer_handler();

#ifdef BOARD_HAS_PCM5102A
    if (peri_init_st[E_PERI_PCM5102A] == true) {
        audio.loop();      // the decoder needs a tight loop
        vTaskDelay(1);
        return;
    }
#endif /* BOARD_HAS_PCM5102A */

    if (next_ms > 500) next_ms = 500;   // also clamps LV_NO_TIMER_READY
    if (next_ms == 0) next_ms = 1;
#if FACTORY_IDLE_POWER_SAVE
    if (lock_screen_light_sleep()) return;
    // Long idle gaps run at 80 MHz; any work (flush, key) runs at 240.
    const bool slow = next_ms >= 100;
    if (slow) setCpuFrequencyMhz(80);
#endif
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next_ms));
#if FACTORY_IDLE_POWER_SAVE
    if (slow) setCpuFrequencyMhz(240);
#endif
}

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void disp_full_refr(void)
{
    disp_refr_mode = DISP_REFR_MODE_FULL;
}

void disp_transition_refr(void)
{
    // A screen change: refresh the whole screen with a partial update (fast,
    // no flash) and only every Nth transition with a full one to clear the
    // ghosting partial updates accumulate. entry() and exit() both call this
    // for one transition, so count once per frame.
    if (s_transition_pending) return;
    s_transition_pending = true;
    if (++s_partial_transitions >= DISP_FULL_EVERY_N_TRANSITIONS) {
        disp_refr_mode = DISP_REFR_MODE_FULL;
    }
}

void disp_hard_refresh(void)
{
    // Multiple B/W flashes are needed to fully clear residual charge on this
    // panel — a single pair leaves visible ghosting of the prior frame.
    const int cycles = 3;

    display.setFullWindow();

    // The panel needs neither the bus nor CS while it waits on BUSY (see
    // epd_busy_cb), so take the bus per fill instead of across the sequence.
    for (int i = 0; i < cycles; i++) {
        {
            SpiGuard spi;
            epd_fill(GxEPD_BLACK);
        }
        {
            SpiGuard spi;
            epd_fill(GxEPD_WHITE);
        }
    }

    // Flag next LVGL flush as FULL so it draws the UI on a clean slate
    disp_full_refr();
}

void disp_white_clear(void)
{
    // Cancel the deferred power-down timer — we are about to power off
    // explicitly at the end of this function.
    epd_cancel_poweroff_timer();

    SpiGuard spi;
    display.setFullWindow();
    epd_fill(GxEPD_WHITE);
    display.powerOff();
}

// Switch the display+LVGL into landscape (320x240 logical) or portrait
// (240x320 logical). LVGL stays unaware of any rotation — we present a
// display whose logical dimensions match the requested orientation, and the
// disp_flush path rotates the rendered buffer to panel-native coordinates
// during the bitmap pack. GxEPD2 stays pinned to setRotation(0) so writeImage
// goes straight to controller memory without extra coordinate translation.
//
// This avoids LVGL's sw_rotate, which would slice a rotated full-screen flush
// into ~6-8 strips (LV_DISP_ROT_MAX_BUF / area_w rows each, lv_refr.c:1201),
// each strip costing its own ~700 ms EPD refresh cycle.
void factory_set_landscape(bool landscape)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp || !disp->driver) return;

    lv_coord_t want_w = landscape ? LCD_VER_SIZE : LCD_HOR_SIZE; // 320 vs 240
    lv_coord_t want_h = landscape ? LCD_HOR_SIZE : LCD_VER_SIZE; // 240 vs 320
    if (disp->driver->hor_res == want_w &&
        disp->driver->ver_res == want_h &&
        s_landscape == landscape) return;

    disp->driver->hor_res = want_w;
    disp->driver->ver_res = want_h;
    s_landscape = landscape;

    {
        SpiGuard spi;
        display.setRotation(0);
    }

    lv_disp_drv_update(disp, disp->driver);

    disp_full_refr();
}



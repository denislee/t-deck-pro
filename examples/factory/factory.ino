/**
 * @file      test_touchpad.h
 * @author    ShallowGreen123
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2024-05-27
 *
 */


#include <Arduino.h>
#include "utilities.h"
#include <GxEPD2_BW.h>
#include <TouchDrvCSTXXX.hpp>
#include <TinyGPS++.h>
#include "lvgl.h"
#include "ui_deckpro.h"
#include <Fonts/FreeMonoBold9pt7b.h>
#include "factory.h"
#include "ui_deckpro_port.h"
#include "peripheral.h"
#include <Preferences.h>
#include <freertos/semphr.h>
#include "esp_pm.h"

Preferences preferences;

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
TouchDrvCSTXXX touch;
// Page height is HEIGHT/8 (40 rows, 1,200 B) rather than HEIGHT (9,600 B).
// The normal flush path bypasses the page buffer entirely — flush_epd_bitmap()
// calls display.epd2.writeImage() directly. The paged buffer is only used by
// disp_hard_refresh() and disp_white_clear() (fillScreen loops), which iterate
// over 8 pages of 40 rows each with no visible difference to the user.
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT / 8> display(GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)); // GDEQ031T10 240x320, UC8253, (no inking, backside mark KEGMO 3100)

uint8_t *decodebuffer = NULL;
int disp_refr_mode = DISP_REFR_MODE_PART;
// Tracks whether LVGL is rendering in landscape (320x240 logical). We do the
// 90° rotation ourselves during bitmap packing instead of using LVGL's
// sw_rotate, which would split a full-screen rotated flush into ~6-8 strips
// (lv_refr.c:1201, max_row = LV_DISP_ROT_MAX_BUF / area_w) — each strip
// triggering its own ~700 ms EPD refresh.
static bool s_landscape = false;

bool isT_Deck_Pro_v1_0 = false;
const char Version_str1[] = "T-Deck-Pro V1.0";
const char Version_str2[] = "T-Deck-Pro V1.1";

bool peri_init_st[E_PERI_NUM_MAX] = {0};
static SemaphoreHandle_t shared_spi_mutex = nullptr;
// Nesting depth for shared_spi_lock/unlock. Incremented after the recursive
// mutex is taken; decremented before it is given. shared_spi_release_all_cs()
// is called only when the outermost unlock runs (depth reaches 0).
static uint32_t s_spi_depth = 0;

// One-shot LVGL timer used to defer display.epd2.powerOff() by ~2 s after the
// last flush. Both this timer and flush_epd_bitmap() run from lv_task_handler()
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
    if (shared_spi_mutex == nullptr) {
        shared_spi_bus_init();
    }
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

// Deasserts all CS lines so each driver can assert only its own pin before
// beginning a transaction. Does NOT assert cs_pin LOW — drivers manage their
// own chip-select timing. The cs_pin argument is retained for call-site
// clarity (callers pass the pin they are about to use) but is not driven here.
// Rename to shared_spi_deselect_all() is a follow-up: callers in other files
// would need updating and are owned by a different agent.
void shared_spi_prepare_device(int cs_pin)
{
    (void)cs_pin;  /* see comment above — pin is not driven here */
    shared_spi_release_all_cs();
}

/*********************************************************************************
 *                              STATIC PROTOTYPES
 * *******************************************************************************/
static bool ink_screen_init()
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);

    // SPI.begin(BOARD_SPI_SCK, -1, BOARD_SPI_MOSI, BOARD_EPD_CS);
    display.epd2.selectSPI(SPI, SPISettings(FACTORY_EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
    display.init(115200, true, 2, false);
    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    if (display.epd2.WIDTH < 104) display.setFont(0);
    display.setTextColor(GxEPD_BLACK);
    display.powerOff();
    shared_spi_unlock();
    return true;
}

// Pack the LVGL render buffer into decodebuffer in panel-native (portrait)
// orientation. In portrait mode the layout is a direct 1:1 row-major pack;
// in landscape we rotate 90° CW during the pack itself so a single writeImage
// can push the whole dirty area to the panel — avoiding LVGL's sw_rotate
// strip-splitting (and its multiple EPD refresh cycles).
//
// Polarity is panel-native: bit clear = black, bit set = white.
static void convert_lvgl_buf_to_epd_bitmap_portrait(const lv_color_t *color_p,
                                                    lv_coord_t width, lv_coord_t height)
{
    const size_t stride = EPD_BITMAP_STRIDE(width);
    for (lv_coord_t y = 0; y < height; ++y) {
        const lv_color_t *row = color_p + size_t(y) * size_t(width);
        uint8_t *dst = decodebuffer + size_t(y) * stride;
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

// 90° CW rotation: logical (lx, ly) maps to panel (LCD_HOR_SIZE - 1 - ly, lx),
// matching the prior Adafruit_GFX setRotation(1) behaviour. Panel area
// dimensions swap: panel_w = lh, panel_h = lw. The rounder_cb guarantees lh
// is a multiple of 8 so panel_x_min stays 8-aligned for writeImage.
//
// Loop is transposed so color_p reads are sequential (good for PSRAM); each
// pixel writes one bit into decodebuffer, so we memset the affected region
// to 0xFF (all-white) first and then clear bits for dark pixels.
static void convert_lvgl_buf_to_epd_bitmap_landscape(const lv_color_t *color_p,
                                                     lv_coord_t lw, lv_coord_t lh)
{
    const lv_coord_t panel_w = lh; // panel columns spanned
    const lv_coord_t panel_h = lw; // panel rows spanned
    const size_t stride = EPD_BITMAP_STRIDE(panel_w);
    memset(decodebuffer, 0xFF, size_t(panel_h) * stride);

    for (lv_coord_t ly = 0; ly < lh; ++ly) {
        // For this logical row, the corresponding panel column offset within
        // the area is (lh - 1 - ly). Compute byte index + bit mask once per
        // logical row instead of per pixel.
        const lv_coord_t panel_col_off = lh - 1 - ly;
        const size_t byte_idx = size_t(panel_col_off) >> 3;
        const uint8_t bit_mask = uint8_t(1u << (7 - (panel_col_off & 0x7)));
        const lv_color_t *row = color_p + size_t(ly) * size_t(lw);

        for (lv_coord_t lx = 0; lx < lw; ++lx) {
            if (lv_color_brightness(row[lx]) < 128) {
                // panel row offset == lx (logical column maps to panel row)
                decodebuffer[size_t(lx) * stride + byte_idx] &= ~bit_mask;
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
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display.epd2.powerOff();
    shared_spi_unlock();
}

static void flush_epd_bitmap(const lv_area_t *panel_area)
{
    const lv_coord_t width = lv_area_get_width(panel_area);
    const lv_coord_t height = lv_area_get_height(panel_area);

    if ((width <= 0) || (height <= 0)) {
        return;
    }

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);

    // Direct write to controller memory + refresh, skipping the
    // firstPage()/nextPage() Adafruit_GFX raster path. decodebuffer is already
    // in panel-native polarity and orientation; the rounder_cb enforces the
    // 8-pixel x-alignment writeImage requires.
    //
    // writeImageAgain after refresh updates the controller's "previous"
    // buffer — UC8253 partial refresh diffs current vs. previous, so without
    // this step the next partial flush would overlay onto a stale baseline,
    // causing ghosting on page turns. writeImageForFullRefresh seeds both
    // buffers in one call, so the full path doesn't need writeImageAgain.
    if (disp_refr_mode == DISP_REFR_MODE_PART) {
        display.epd2.writeImage(decodebuffer, panel_area->x1, panel_area->y1, width, height, false);
        display.epd2.refresh(panel_area->x1, panel_area->y1, width, height);
        display.epd2.writeImageAgain(decodebuffer, panel_area->x1, panel_area->y1, width, height, false);
    } else {
        display.epd2.writeImageForFullRefresh(decodebuffer, panel_area->x1, panel_area->y1, width, height, false);
        display.epd2.refresh(false);
    }

    shared_spi_unlock();

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
}

// GDEQ031T10::writeImage requires the partial-update X window to start and
// span multiples of 8 pixels (one byte per 8 horizontal pixels). LVGL hands us
// arbitrary invalidate bounding boxes in *logical* coordinates before any
// rotation is applied.
//
// The flush path (logical_area_to_panel + flush_epd_bitmap) rotates 90° CW in
// landscape: logical-X maps to panel-Y and logical-Y maps to panel-X. So the
// axis that must be 8-aligned in panel space is:
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
    const lv_coord_t lw = lv_area_get_width(area);
    const lv_coord_t lh = lv_area_get_height(area);

    if (s_landscape) {
        convert_lvgl_buf_to_epd_bitmap_landscape(color_p, lw, lh);
    } else {
        convert_lvgl_buf_to_epd_bitmap_portrait(color_p, lw, lh);
    }

    lv_area_t panel_area;
    logical_area_to_panel(area, &panel_area);
    flush_epd_bitmap(&panel_area);

    disp_refr_mode = DISP_REFR_MODE_PART;

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_disp_flush_ready(disp_drv);
}

static void touchpad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    if (!ui_setting_get_touch_status()) {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = last_x;
        data->point.y = last_y;
        return;
    }

    // uint8_t touched = touch.getPoint(&last_x, &last_y, 1);
    uint8_t touched = hyn_touch_get_point(&last_x, &last_y, 1);
    if(touched) {
        data->state = LV_INDEV_STATE_PR;

        Serial.printf("x = %d, y = %d\n", last_x, last_y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    // Serial.printf("touch=%d, x = %d, y = %d\n", touched, last_x, last_y);
    /*Set the last pressed coordinates*/
    data->point.x = last_x;
    data->point.y = last_y;
}

static void lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf_dsc_1;
    lv_color_t *buf_1 = (lv_color_t *)ps_calloc(sizeof(lv_color_t), DISP_BUF_SIZE);
    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, NULL, LCD_HOR_SIZE * LCD_VER_SIZE);
    decodebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_BITMAP_BUF_SIZE);
    // lv_disp_draw_buf_init(&draw_buf, lv_disp_buf_p, NULL, DISP_BUF_SIZE);

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

    /*------------------
     * Touchpad
     * -----------------*/
    /*Register a touchpad input device*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);
}

void ink_screen_prepare_shutdown(void)
{
    if (!peri_init_st[E_PERI_INK_SCREEN]) {
        return;
    }

    // Cancel the deferred power-down timer before explicitly powering off.
    // Both this function and the timer callback run from the LVGL task so
    // there is no concurrency, but we must not leave a stale timer that
    // fires after the panel has already been powered off for sleep/shutdown.
    if (s_epd_poweroff_timer != NULL) {
        lv_timer_del(s_epd_poweroff_timer);
        s_epd_poweroff_timer = NULL;
    }

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display.powerOff();
    shared_spi_unlock();
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
    return PPM.enableMeasure();
}

static bool bq25896_init(void)
{
    // BQ25896 --- 0x6B
    // The keypad task runs at priority 19 and will preempt this init,
    // corrupting multi-byte I2C transactions. Hold i2c0_lock for the whole
    // probe + init path.
    i2c0_lock();
    Wire.beginTransmission(BOARD_I2C_ADDR_BQ25896);
    if (Wire.endTransmission() != 0) {
        i2c0_unlock();
        return false;
    }
    if (!PPM.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_ADDR_BQ25896)) {
        i2c0_unlock();
        return false;
    }
    bool ok = bq25896_apply_factory_profile();
    i2c0_unlock();
    return ok;
}

static bool bq27220_init(void)
{
    // BQ27220 init issues a long sequence of control subcommands (DEVICE_NUMBER
    // probe, unseal, CFGUPDATE entry, data-memory writes, CFGUPDATE exit). A
    // keypad preempt mid-sequence has been observed to return 0xFFFF on the
    // chip-ID read, aborting init and leaving battery_percent stuck at 0.
    i2c0_lock();
    bq27220.setDefaultCapacity(FACTORY_BATTERY_DESIGN_CAPACITY_MAH);
    bool ret = bq27220.init();
    uint16_t soc = 0, vbat = 0;
    int16_t curr = 0;
    if (ret) {
        soc  = bq27220.getStateOfCharge();
        vbat = bq27220.getVoltage();
        curr = bq27220.getCurrent();
    }
    i2c0_unlock();
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

    i2c0_lock();
    if (!PPM.isVbusIn()) {
        i2c0_unlock();
        return;
    }

    bool need_recover = !PPM.isCharging();
    if (!need_recover && peri_init_st[E_PERI_BQ27220]) {
        need_recover = (bq27220.getAverageCurrent() < 0);
    }
    if (!need_recover) {
        i2c0_unlock();
        return;
    }
    if (millis() - last_recovery_ms < FACTORY_BQ25896_RECOVERY_COOLDOWN_MS) {
        i2c0_unlock();
        return;
    }

    Serial.println("[BQ25896] Restore charge path");
    bq25896_apply_factory_profile();
    i2c0_unlock();
    last_recovery_ms = millis();
}

static bool sd_care_init(void)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

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

    // SD.begin default is 4 MHz. Retry at a lower frequency on the first
    // few tries — some cards are flaky on the very first init right after
    // power-up, especially with the LoRa radio mid-transmit on the same bus.
    const uint32_t freqs[] = { 1000000, 4000000, 4000000 };
    bool ok = false;
    for (uint8_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); ++i) {
        if (i > 0) {
            SD.end();
            delay(50);
        }
        if (SD.begin(BOARD_SD_CS, SPI, freqs[i])) {
            ok = true;
            Serial.printf("[SD CARD] mounted at %u Hz (attempt %u)\n",
                          (unsigned)freqs[i], (unsigned)(i + 1));
            break;
        }
        Serial.printf("[SD CARD] mount attempt %u @ %u Hz failed\n",
                      (unsigned)(i + 1), (unsigned)freqs[i]);
    }
    if (!ok) {
        shared_spi_unlock();
        Serial.println("[SD CARD] Card Mount Failed");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD CARD] No card detected after mount");
        shared_spi_unlock();
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

    uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
    Serial.printf("SD Card Used: %lluMB\n", usedSize);
    shared_spi_unlock();
    return true;
}

static void a7682_task(void *param)
{
    // Suspend the calling task. NULL means "self" in FreeRTOS and is correct
    // here: this task runs at priority 20, above its creator (priority 1), so
    // FreeRTOS switches to it before xTaskCreate() returns and assigns
    // a7682_handle. Using the global at this point would read an uninitialised
    // handle (NULL at boot), which would accidentally suspend the idle task.
    vTaskSuspend(NULL);
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
        vTaskDelay(1);
    }
}

static bool A7682E_init(void)
{
    Serial.println("Place your board outside to catch satelite signal");

    // Set module baud rate and UART pins
    SerialAT.begin(115200, SERIAL_8N1, BOARD_A7682E_TXD, BOARD_A7682E_RXD);

    Serial.println("Start modem...");

    // power on
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
    delay(50);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);

    int retry_cnt = 5;
    int retry = 0;
    while (!modem.testAT(1000)) {
        Serial.println(".");
        if (retry++ > retry_cnt) {
            digitalWrite(BOARD_A7682E_PWRKEY, LOW);
            delay(100);
            digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
            delay(1000);
            digitalWrite(BOARD_A7682E_PWRKEY, LOW);

            Serial.println("[A7682E] Init Fail");
            break;
        }
    }
    
    Serial.println();
    delay(200);

    xTaskCreate(a7682_task, "a7682_handle", 1024 * 3, NULL, A7682E_PRIORITY, &a7682_handle);

    return (retry < retry_cnt);
}

#ifdef BOARD_HAS_PCM5102A
static bool pcm5102a_init(void)
{
    bool ret = audio.setPinout(BOARD_I2S_BCLK, BOARD_I2S_LRC, BOARD_I2S_DOUT);

    if (ret == false)
        Serial.printf("[%d] Execution error\n", __LINE__);

    audio.setVolume(21); // 0...21

    pinMode(BOARD_6609_EN, OUTPUT);
    digitalWrite(BOARD_6609_EN, HIGH);

    // audio_paly_flag = audio.connecttoFS(SD, "/voice_time/BBIBBI.mp3");

    return true;
}
#endif /* BOARD_HAS_PCM5102A */

static void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing spiffs directory: %s\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

static void peripheral_init_task(void *param)
{
    // SPI must be initialized before this task starts
    peri_init_st[E_PERI_LORA]       = lora_init();
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
    } else {
        peri_init_st[E_PERI_GPS] = false;
    }
    // Gyro — only attempt init when the BHI260AP driver is compiled in.
    // On this V1.1 board the chip-ID readback returns 0xFFFF and SensorLib
    // retries with 1 s delays generating Wire.cpp:499 Error 263 spam.
    // BOARD_1V8_EN (IO38) powers BOTH the gyro AND the CST328 touch IC; it
    // stays HIGH at boot (setup()) regardless of this flag.
#ifdef BOARD_HAS_BHI260AP
    peri_init_st[E_PERI_BHI260AP]   = BHI260AP_init();
#else
    peri_init_st[E_PERI_BHI260AP]   = false;
#endif
    peri_init_st[E_PERI_LTR_553ALS] = LTR553_init();
    peri_init_st[E_PERI_A7682E]     = A7682E_init();

#ifdef BOARD_HAS_PCM5102A
    if(peri_init_st[E_PERI_A7682E] == false)
    {
        peri_init_st[E_PERI_PCM5102A] = pcm5102a_init();
    }
#else
    // Audio hardware not compiled in; PCM5102A init is skipped.
    peri_init_st[E_PERI_PCM5102A] = false;
#endif

    Serial.println("Background peripheral initialization complete.");
    vTaskDelete(NULL);
}

void setup()
{
    gpio_hold_dis((gpio_num_t)BOARD_6609_EN);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_EN);
    gpio_hold_dis((gpio_num_t)BOARD_GPS_EN);
    gpio_hold_dis((gpio_num_t)BOARD_1V8_EN);
    gpio_hold_dis((gpio_num_t)BOARD_A7682E_PWRKEY);

    gpio_deep_sleep_hold_dis();

    Serial.begin(115200);

    ui_settings_load();

    // No battery-backed RTC: every power cycle resets the clock to 1970. Pull
    // the last persisted epoch out of NVS so the topbar shows a sensible time
    // straight away; GPS or NTP will correct any drift once they're up.
    ui_time_persist_restore();

    // Restore the user's last WiFi on/off choice. We honour the persisted
    // intent (wifi_en pref) rather than just "SSID present" — otherwise
    // toggling WiFi off in the UI would silently come back on after reboot.
    if (ui_wifi_get_enabled()) {
        ui_wifi_set_enabled(true);
    }

    // IO
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    pinMode(BOARD_RED_LED, OUTPUT);
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    pinMode(BOARD_6609_EN, OUTPUT);         
    pinMode(BOARD_LORA_EN, OUTPUT);         
    pinMode(BOARD_GPS_EN, OUTPUT);          
    pinMode(BOARD_1V8_EN, OUTPUT);          
    pinMode(BOARD_A7682E_PWRKEY, OUTPUT);

    digitalWrite(BOARD_KEYBOARD_LED, ui_setting_get_keypad_light());
    digitalWrite(BOARD_RED_LED, ui_setting_get_red_led());
    digitalWrite(BOARD_MOTOR_PIN, ui_setting_get_motor_status());
    digitalWrite(BOARD_6609_EN, ui_setting_get_a7682_status());
    digitalWrite(BOARD_LORA_EN, ui_setting_get_lora_status());
    digitalWrite(BOARD_GPS_EN, ui_setting_get_gps_status());
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_A7682E_PWRKEY, ui_setting_get_a7682_status());

    // SPI Pins
    pinMode(BOARD_LORA_CS, OUTPUT); 
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT); 
    digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT); 
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT); 
    digitalWrite(BOARD_EPD_CS, HIGH);

    // Optimized I2C Check
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Serial.printf(" ------------- I2C ------------- \n");
    const uint8_t known_addresses[] = {
        BOARD_I2C_ADDR_TOUCH,
        BOARD_I2C_ADDR_LTR_553ALS,
        BOARD_I2C_ADDR_GYROSCOPDE,
        BOARD_I2C_ADDR_KEYBOARD,
        BOARD_I2C_ADDR_BQ27220,
        BOARD_I2C_ADDR_BQ25896
    };

    for (uint8_t address : known_addresses) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            if (address == BOARD_I2C_ADDR_TOUCH) Serial.printf("[0x%x] TOUCH find!\n", address);
            else if (address == BOARD_I2C_ADDR_LTR_553ALS) Serial.printf("[0x%x] LTR_553ALS find!\n", address);
            else if (address == BOARD_I2C_ADDR_GYROSCOPDE) Serial.printf("[0x%x] GYROSCOPDE find!\n", address);
            else if (address == BOARD_I2C_ADDR_KEYBOARD) Serial.printf("[0x%x] KEYBOARD find!\n", address);
            else if (address == BOARD_I2C_ADDR_BQ27220) Serial.printf("[0x%x] BQ27220 find!\n", address);
            else if (address == BOARD_I2C_ADDR_BQ25896) Serial.printf("[0x%x] BQ25896 find!\n", address);
        }
    }

    // Hardware version detection (V1.1 has DRV2605 at 0x5A)
    Wire.beginTransmission(0x5A);
    isT_Deck_Pro_v1_0 = (Wire.endTransmission() != 0);

#ifdef T_DECK_PRO_V1_0
    if(!isT_Deck_Pro_v1_0){
        Serial.printf(" ------------- ERROR ------------- \n");
        Serial.printf("Firmware mismatch\n");
        Serial.printf("Your hardware might be the T-Deck-Pro V1.1, but please download the H693_factory_v1.x.bin firmware.\n");
        Serial.printf("T-Deck-Pro V1.0   ---   H693_factory_v1.x.bin \n");
        Serial.printf("T-Deck-Pro V1.1   ---   H693_factory_v2.x.bin\n");
    }
#endif

    if(!SPIFFS.begin(true)){
        Serial.println("SPIFFS Mount Failed");
        return;
    }

    // SPI
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    shared_spi_bus_init();

    // Critical Peripheral Init (Required for UI/Input)
    peri_init_st[E_PERI_INK_SCREEN] = ink_screen_init();
    peri_init_st[E_PERI_KYEPAD]     = keypad_init(BOARD_I2C_ADDR_KEYBOARD);
    // Touch driver disabled — the CST328 isn't responding on this V1.1 board
    // and its init/probe was driving I2C error spam. Keypad input only.
    peri_init_st[E_PERI_TOUCH]      = false;

    lvgl_init();
    ui_deckpro_entry();
    disp_full_refr();

    // Start background initialization for slow peripherals
    xTaskCreate(peripheral_init_task, "peri_init", 1024 * 4, NULL, 1, NULL);

    // Start keypad task
    if (peri_init_st[E_PERI_KYEPAD]) {
        keypad_task_create();
    }

    // CPU runs at the Arduino default (240 MHz) — earlier code capped it at
    // 80 MHz with light sleep, which was the dominant cause of UI stutter
    // (LVGL render and bitmap-pack loops are CPU-bound). Restore aggressive
    // power management here only when the user explicitly trades perf for
    // battery (e.g. lock screen / standby).
}


uint32_t tick = 0;

void loop()
{
    lv_task_handler();
    // keypad is drained by a dedicated FreeRTOS task — see keypad_task_create().
    bq25896_runtime_maintain();

#ifdef BOARD_HAS_PCM5102A
    if(peri_init_st[E_PERI_PCM5102A] == true)
    {
        audio.loop();
    }
#endif /* BOARD_HAS_PCM5102A */
    
    vTaskDelay(1);


    if(millis() - tick > 3000) {
        tick = millis();
        // printf("BOARD_LORA_CS=%d\n", digitalRead(BOARD_LORA_CS));
        // printf("BOARD_LORA_RST=%d\n", digitalRead(BOARD_LORA_RST));
        // printf("BOARD_LORA_BUSY=%d\n", digitalRead(BOARD_LORA_BUSY));
        // printf("BOARD_LORA_EN=%d\n", digitalRead(BOARD_LORA_EN));

        // printf("BOARD_EPD_CS=%d\n", digitalRead(BOARD_EPD_CS));
    }
}

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void disp_full_refr(void)
{
    disp_refr_mode = DISP_REFR_MODE_FULL;
}

void disp_hard_refresh(void)
{
    // Multiple B/W flashes are needed to fully clear residual charge on this
    // panel — a single pair leaves visible ghosting of the prior frame.
    const int cycles = 3;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display.setFullWindow();

    for (int i = 0; i < cycles; i++) {
        display.firstPage();
        do {
            display.fillScreen(GxEPD_BLACK);
        } while (display.nextPage());

        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
        } while (display.nextPage());
    }

    shared_spi_unlock();

    // Flag next LVGL flush as FULL so it draws the UI on a clean slate
    disp_full_refr();
}

void disp_white_clear(void)
{
    // Cancel the deferred power-down timer — we are about to power off
    // explicitly at the end of this function.
    if (s_epd_poweroff_timer != NULL) {
        lv_timer_del(s_epd_poweroff_timer);
        s_epd_poweroff_timer = NULL;
    }

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
    display.powerOff();
    shared_spi_unlock();
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

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display.setRotation(0);
    shared_spi_unlock();

    lv_disp_drv_update(disp, disp->driver);

    disp_full_refr();
}



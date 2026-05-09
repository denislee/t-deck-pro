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

Preferences preferences;

TinyGsm modem(SerialAT);
TaskHandle_t a7682_handle;

XPowersPPM PPM;
BQ27220 bq27220;
Audio audio;

static constexpr uint16_t FACTORY_BATTERY_DESIGN_CAPACITY_MAH = 1400;
static constexpr uint16_t FACTORY_BQ25896_CHARGE_TARGET_MV = 4208;
static constexpr uint16_t FACTORY_BQ25896_FAST_CHARGE_MA = 512;
static constexpr uint16_t FACTORY_BQ25896_PRECHARGE_MA = 128;
static constexpr uint16_t FACTORY_BQ25896_TERMINATION_MA = 128;
static constexpr uint16_t FACTORY_BQ25896_INPUT_LIMIT_MA = 1000;
static constexpr uint16_t FACTORY_BQ25896_SYS_POWER_DOWN_MV = 3300;
static constexpr uint32_t FACTORY_BQ25896_RUNTIME_CHECK_MS = 5000;
static constexpr uint32_t FACTORY_BQ25896_RECOVERY_COOLDOWN_MS = 30000;
static constexpr uint32_t FACTORY_EPD_SPI_HZ = 2000000;
TouchDrvCSTXXX touch;
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)); // GDEQ031T10 240x320, UC8253, (no inking, backside mark KEGMO 3100)

uint8_t *decodebuffer = NULL;
lv_timer_t *flush_timer = NULL;
int disp_refr_mode = DISP_REFR_MODE_PART;

bool isT_Deck_Pro_v1_0 = false;
const char Version_str1[] = "T-Deck-Pro V1.0";
const char Version_str2[] = "T-Deck-Pro V1.1";

bool peri_init_st[E_PERI_NUM_MAX] = {0};
static SemaphoreHandle_t shared_spi_mutex = nullptr;

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
    }
}

void shared_spi_unlock(void)
{
    if (shared_spi_mutex != nullptr) {
        shared_spi_release_all_cs();
        xSemaphoreGiveRecursive(shared_spi_mutex);
    }
}

void shared_spi_prepare_device(int cs_pin)
{
    shared_spi_release_all_cs();
    if (cs_pin >= 0) {
        digitalWrite(cs_pin, HIGH);
    }
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

static void convert_lvgl_buf_to_epd_bitmap(const lv_color_t *color_p, lv_coord_t width, lv_coord_t height)
{
    const size_t stride = EPD_BITMAP_STRIDE(width);
    const size_t bitmap_size = stride * size_t(height);

    memset(decodebuffer, 0xFF, bitmap_size);

    for (lv_coord_t y = 0; y < height; ++y) {
        size_t row_offset = size_t(y) * stride;
        for (lv_coord_t x = 0; x < width; ++x) {
            const size_t pixel_index = size_t(y) * size_t(width) + size_t(x);
            if (lv_color_brightness(color_p[pixel_index]) < 128) {
                decodebuffer[row_offset + size_t(x / 8)] &= ~(0x80 >> (x & 0x7));
            }
        }
    }
}

static void flush_epd_bitmap(const lv_area_t *area)
{
    const lv_coord_t width = lv_area_get_width(area);
    const lv_coord_t height = lv_area_get_height(area);

    if ((width <= 0) || (height <= 0)) {
        return;
    }

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);

    if (disp_refr_mode == DISP_REFR_MODE_PART) {
        display.setPartialWindow(area->x1, area->y1, width, height);
    } else {
        display.setFullWindow();
    }

    display.firstPage();
    do {
        if (disp_refr_mode == DISP_REFR_MODE_FULL) {
            display.fillScreen(GxEPD_WHITE);
        }
        display.drawInvertedBitmap(area->x1, area->y1, decodebuffer, width, height, GxEPD_BLACK);
    }
    while (display.nextPage());

    display.powerOff();
    shared_spi_unlock();
}

static void flush_timer_cb(lv_timer_t *t)
{
    static int idx = 0;
    lv_disp_t *disp = lv_disp_get_default();
    if(disp->rendering_in_progress == false) {
        // Use the live driver dimensions, not LCD_HOR_SIZE/LCD_VER_SIZE,
        // so the forced full refresh tracks the current rotation
        // (factory_set_landscape swaps hor_res/ver_res).
        lv_area_t full_area;
        full_area.x1 = 0;
        full_area.y1 = 0;
        full_area.x2 = disp->driver->hor_res - 1;
        full_area.y2 = disp->driver->ver_res - 1;

        flush_epd_bitmap(&full_area);
        
        Serial.printf("flush_timer_cb:%d, %s\n", idx++, (disp_refr_mode == DISP_REFR_MODE_FULL ? "full" : "part"));

        disp_refr_mode = DISP_REFR_MODE_PART;
        lv_timer_pause(flush_timer);
    }
}

static void dips_render_start_cb(struct _lv_disp_drv_t * disp_drv)
{
    if(flush_timer == NULL) {
        flush_timer = lv_timer_create(flush_timer_cb, 10, NULL);
    } else {
        lv_timer_resume(flush_timer);
    }
}

static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    convert_lvgl_buf_to_epd_bitmap(color_p, lv_area_get_width(area), lv_area_get_height(area));
    flush_epd_bitmap(area);
    
    static int idx = 0;
    Serial.printf("disp_flush:%d, %s, area=(%d,%d)-(%d,%d)\n",
                  idx++,
                  (disp_refr_mode == DISP_REFR_MODE_FULL ? "full" : "part"),
                  area->x1, area->y1, area->x2, area->y2);

    disp_refr_mode = DISP_REFR_MODE_PART;

    // Serial.printf("x1=%d, y1=%d, x2=%d, y2=%d\n", area->x1, area->y1, area->x2, area->y2);

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
    disp_drv.render_start_cb = dips_render_start_cb;
    disp_drv.draw_buf = &draw_buf_dsc_1;
    // disp_drv.rounder_cb = display_driver_rounder_cb;
    disp_drv.full_refresh = 1;
    // sw_rotate intentionally NOT set: LVGL 8.3 silently bails out of the
    // rotation path when full_refresh && sw_rotate are both on
    // (lib/lvgl/src/core/lv_refr.c:1186 — "cannot rotate a full refreshed
    // display!"), so flushes never reach the EPD when rotated. We rotate at
    // the GxEPD2 level instead via factory_set_landscape().

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
    Wire.beginTransmission(BOARD_I2C_ADDR_BQ25896);
    if (Wire.endTransmission() == 0)
    {
        if (!PPM.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_ADDR_BQ25896)) {
            return false;
        }

        return bq25896_apply_factory_profile();
    }
    return false;
}

static bool bq27220_init(void)
{
    bq27220.setDefaultCapacity(FACTORY_BATTERY_DESIGN_CAPACITY_MAH);
    bool ret = bq27220.init();
    // if(ret) 
    //     bq27220.reset();
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

    if(!SD.begin(BOARD_SD_CS, SPI)){
        shared_spi_unlock();
        Serial.println("[SD CARD] Card Mount Failed");
        return false;
    }

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
    vTaskSuspend(a7682_handle);
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

    // If a WiFi SSID is configured, kick off an asynchronous connect now so
    // NTP can finish before the user notices. Connect runs on its own task,
    // so it does not delay the rest of setup().
    {
        char saved_ssid[33] = {0};
        ui_wifi_get_ssid(saved_ssid, sizeof(saved_ssid));
        if (saved_ssid[0] != '\0') {
            ui_wifi_set_enabled(true);
        }
    }

    // delay(3000);

    // // frist startup
    // preferences.begin("my-app", false);
    // bool start = preferences.getBool("counter", false);
    // Serial.printf("start = %d\n", start);
    // if(start == false)
    // {
    //     Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    //     bool ret = bq25896_init();
    //     if(ret == true)
    //     {
    //         preferences.putBool("counter", true);
    //         Serial.printf("bq25896 init success\n");
    //     }else{
    //         Serial.printf("bq25896 init failure\n");
    //     }

    //     while (PPM.isVbusIn())
    //     {
    //         delay(1000);
    //         Serial.println("Unplug the USB");
    //     }
    //     PPM.shutdown();
    // }

    // IO
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    pinMode(BOARD_6609_EN, OUTPUT);         // enable 7682 module
    pinMode(BOARD_LORA_EN, OUTPUT);         // enable LORA module
    pinMode(BOARD_GPS_EN, OUTPUT);          // enable GPS module
    pinMode(BOARD_1V8_EN, OUTPUT);          // 1.8V rail (gyro AND touch IC)
    pinMode(BOARD_A7682E_PWRKEY, OUTPUT);

    digitalWrite(BOARD_KEYBOARD_LED, ui_setting_get_keypad_light());
    digitalWrite(BOARD_MOTOR_PIN, ui_setting_get_motor_status());
    digitalWrite(BOARD_6609_EN, ui_setting_get_a7682_status());
    digitalWrite(BOARD_LORA_EN, ui_setting_get_lora_status());
    digitalWrite(BOARD_GPS_EN, ui_setting_get_gps_status());
    // VDD1V8 powers the CST328 touch controller as well as the gyro,
    // so this rail must stay on regardless of the user's gyro toggle.
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_A7682E_PWRKEY, ui_setting_get_a7682_status());

    // LORA、SD、EPD use the same SPI, in order to avoid mutual influence;
    // before powering on, all CS signals should be pulled high and in an unselected state;
    pinMode(BOARD_LORA_CS, OUTPUT); 
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT); 
    digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT); 
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT); 
    digitalWrite(BOARD_EPD_CS, HIGH);


    // i2c devices
    byte error, address;
    int nDevices = 0;
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Serial.printf(" ------------- I2C ------------- \n");
    for(address = 0x01; address < 0x7F; address++){
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if(error == 0){ // 0: success.
            nDevices++;
            if(address == BOARD_I2C_ADDR_TOUCH){
                // flag_Touch_init = true;
                Serial.printf("[0x%x] TOUCH find!\n", address);
            } else if (address == BOARD_I2C_ADDR_LTR_553ALS) {
                Serial.printf("[0x%x] LTR_553ALS find!\n", address);
            } else if (address == BOARD_I2C_ADDR_GYROSCOPDE) {
                Serial.printf("[0x%x] GYROSCOPDE find!\n", address);
            } else if (address == BOARD_I2C_ADDR_KEYBOARD) {
                Serial.printf("[0x%x] KEYBOARD find!\n", address);
            } else if (address == BOARD_I2C_ADDR_BQ27220) {
                Serial.printf("[0x%x] BQ27220 find!\n", address);
            } else if (address == BOARD_I2C_ADDR_BQ25896) {
                Serial.printf("[0x%x] BQ25896 find!\n", address);
            }
        }
    }

    for(int i = 0; i < 3; i++) {
        Wire.beginTransmission(0x5A);
        error = Wire.endTransmission();
        if(error == 0) {
            isT_Deck_Pro_v1_0 = 0;
        } else {
            isT_Deck_Pro_v1_0 = 1;
        }
    }

#ifdef T_DECK_PRO_V1_0
    if(!isT_Deck_Pro_v1_0){
        Serial.printf(" ------------- ERROR ------------- \n");
        Serial.printf("Firmware mismatch\n");
        Serial.printf("Your hardware might be the T-Deck-Pro V1.1, but please download the H693_factory_v1.x.bin firmware.\n");
        Serial.printf("T-Deck-Pro V1.0   ---   H693_factory_v1.x.bin \n");
        Serial.printf("T-Deck-Pro V1.1   ---   H693_factory_v2.x.bin\n");
    }
#endif

    Serial.printf(" ------------- SPIFFS ------------- \n");

    if(!SPIFFS.begin(true)){
        Serial.println("SPIFFS Mount Failed");
        return;
    }

    listDir(SPIFFS, "/", 0);
    Serial.println(" ------------- PERI ------------- ");

    // SPI
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);

    // init peripheral
    // touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    peri_init_st[E_PERI_INK_SCREEN] = ink_screen_init();
    peri_init_st[E_PERI_LORA]       = lora_init();
    // peri_init_st[E_PERI_TOUCH]      = touch.begin(Wire, BOARD_I2C_ADDR_TOUCH, BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
    peri_init_st[E_PERI_KYEPAD]     = keypad_init(BOARD_I2C_ADDR_KEYBOARD);
    peri_init_st[E_PERI_BQ25896]    = bq25896_init();
    peri_init_st[E_PERI_BQ27220]    = bq27220_init();
    peri_init_st[E_PERI_SD]         = sd_care_init();
    peri_init_st[E_PERI_GPS]        = gps_init();
    peri_init_st[E_PERI_BHI260AP]   = BHI260AP_init();
    peri_init_st[E_PERI_LTR_553ALS] = LTR553_init();
    peri_init_st[E_PERI_A7682E]     = A7682E_init();

    if(peri_init_st[E_PERI_A7682E] == false)
    {
        peri_init_st[E_PERI_PCM5102A] = pcm5102a_init();
    }

    peri_init_st[E_PERI_TOUCH] = hyn_touch_init();

    lvgl_init();

    ui_deckpro_entry();

    disp_full_refr();

    // Start the keypad drain task last so the rest of the I2C bus init has
    // settled. From here on, every Wire user on bus 0 must serialize via
    // i2c0_lock() / i2c0_unlock() — see peripheral.h.
    if (peri_init_st[E_PERI_KYEPAD]) {
        keypad_task_create();
    }
}


uint32_t tick = 0;

void loop()
{
    lv_task_handler();
    // keypad is drained by a dedicated FreeRTOS task — see keypad_task_create().
    bq25896_runtime_maintain();

    if(peri_init_st[E_PERI_PCM5102A] == true) 
    {
        audio.loop();
    }
    
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

// Switch the display+LVGL into landscape (320x240) or portrait (240x320).
// Workaround for the LVGL 8.3 full_refresh+sw_rotate bug: instead of asking
// LVGL to rotate the buffer (which silently no-ops), we swap LVGL's logical
// resolution and let GxEPD2's setRotation handle the actual pixel rotation
// when the bitmap is drawn to the panel.
void factory_set_landscape(bool landscape)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp || !disp->driver) return;

    lv_coord_t want_w = landscape ? LCD_VER_SIZE : LCD_HOR_SIZE; // 320 vs 240
    lv_coord_t want_h = landscape ? LCD_HOR_SIZE : LCD_VER_SIZE; // 240 vs 320
    if (disp->driver->hor_res == want_w && disp->driver->ver_res == want_h) return;

    disp->driver->hor_res = want_w;
    disp->driver->ver_res = want_h;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_EPD_CS);
    display.setRotation(landscape ? 1 : 0);
    shared_spi_unlock();

    lv_disp_drv_update(disp, disp->driver);

    disp_full_refr();
}




#include "Arduino.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "utilities.h"

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <TinyGPS++.h>
#include "peripheral.h"
#include "WiFi.h"
#include <ctype.h>
#include <TouchDrvCSTXXX.hpp>
#include <Preferences.h>

// extern 
extern TouchDrvCSTXXX touch;


volatile int default_language = DEFAULT_LANGUAGE_EN;
volatile bool default_keypad_light = false;
volatile bool default_motor_status = false;
volatile bool default_gps_status = true;
volatile bool default_lora_status = true;
volatile bool default_gyro_status = true;
volatile bool default_a7682_status = true;

void ui_settings_save(void)
{
    Preferences prefs;
    prefs.begin("t-deck-pro", false);
    prefs.putInt("lang", default_language);
    prefs.putBool("kplight", default_keypad_light);
    prefs.putBool("motor", default_motor_status);
    prefs.putBool("gps", default_gps_status);
    prefs.putBool("lora", default_lora_status);
    prefs.putBool("gyro", default_gyro_status);
    prefs.putBool("a7682", default_a7682_status);
    prefs.end();
}

void ui_settings_load(void)
{
    Preferences prefs;
    prefs.begin("t-deck-pro", true);
    default_language = prefs.getInt("lang", DEFAULT_LANGUAGE_EN);
    default_keypad_light = prefs.getBool("kplight", false);
    default_motor_status = prefs.getBool("motor", false);
    default_gps_status = prefs.getBool("gps", true);
    default_lora_status = prefs.getBool("lora", true);
    default_gyro_status = prefs.getBool("gyro", true);
    default_a7682_status = prefs.getBool("a7682", true);
    prefs.end();
}
// ----

void ui_disp_full_refr(void)
{
    disp_full_refr();
}
//************************************[ screen 0 ]****************************************** menu
//************************************[ screen 1 ]****************************************** lora

static float lora_default_freq = 850.0;
static int lora_default_band = 125;
static int lora_default_power = 22;

float ui_lora_get_freq(void) { return lora_default_freq; }
void ui_lora_set_freq(float freq) { lora_default_freq = freq; ui_settings_save(); }
int ui_lora_get_bandwidth(void) { return lora_default_band; }
void ui_lora_set_bandwidth(float bd) { lora_default_band = bd; ui_settings_save(); }
int ui_lora_get_power(void) { return lora_default_power; }
void ui_lora_set_power(float po) { lora_default_power = po; ui_settings_save(); }

void ui_lora_param_set(void)
{
    lora_param_set();
}

int ui_lora_get_mode(void)
{
    return lora_get_mode();
}
void ui_lora_set_mode(int mode)
{
    lora_set_mode(mode);
}
void ui_lora_send(const char *str)
{
    lora_transmit(str);
}
void ui_lora_recv_loop(void)
{
    lora_receive_loop();
}
bool ui_lora_get_recv(const char **str, int *rssi)
{
    return lora_get_recv(str, rssi);
}
void ui_lora_set_recv_flag(void)
{
    lora_set_recv_flag();
}
//************************************[ screen 2 ]****************************************** setting
#if 1
// set function
// DEFAULT_LANGUAGE_CN、DEFAULT_LANGUAGE_EN
void ui_setting_set_language(int language)
{
    default_language = language;
    ui_settings_save();
}
void ui_setting_set_keypad_light(bool on)
{
    digitalWrite(BOARD_KEYBOARD_LED, on);
    default_keypad_light = on;
    ui_settings_save();
}
void ui_setting_set_motor_status(bool on)
{
    digitalWrite(BOARD_MOTOR_PIN, on);
    default_motor_status = on;
    ui_settings_save();
}
void ui_setting_set_gps_status(bool on)
{
    // enable GPS module power
    digitalWrite(BOARD_GPS_EN, on);
    default_gps_status = on;
    ui_settings_save();
}
void ui_setting_set_lora_status(bool on)
{
    // enable LORA module power
    digitalWrite(BOARD_LORA_EN, on);
    default_lora_status = on;
    ui_settings_save();
}
void ui_setting_set_gyro_status(bool on)
{
    // VDD1V8 also powers the CST328 touch controller, so we must not cut
    // it from the gyro toggle. Track the preference; the gyro driver itself
    // can honor it at the software level.
    default_gyro_status = on;
    ui_settings_save();
}
void ui_setting_set_a7682_status(bool on)
{
    // enable 7682 module power
    digitalWrite(BOARD_6609_EN, on);
    digitalWrite(BOARD_A7682E_PWRKEY, on);
    default_a7682_status = on;
    ui_settings_save();
}

// get function
int ui_setting_get_language(void)
{
    return default_language;
}
bool ui_setting_get_keypad_light(void)
{
    return default_keypad_light;
}
bool ui_setting_get_motor_status(void)
{
    return default_motor_status;
}
bool ui_setting_get_gps_status(void)
{
    return default_gps_status;
}
bool ui_setting_get_lora_status(void)
{
    return default_lora_status;
}
bool ui_setting_get_gyro_status(void)
{
    return default_gyro_status;
}
bool ui_setting_get_a7682_status(void)
{
    return default_a7682_status;
}

// About System
const char *ui_setting_get_sf_ver(void)
{
    return UI_T_DECK_PRO_VERSION;
}
const char *ui_setting_get_hd_ver(void)
{
    return BOARD_T_DECK_PRO_VERSION;
}

void ui_setting_get_sd_capacity(uint64_t *total, uint64_t *used)
{
    if(ui_test_sd_card())
    {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);

        if(total)
            *total = SD.totalBytes() / (1024 * 1024);
        if(used)
            *used = SD.usedBytes() / (1024 * 1024);

        printf("total=%lluMB, used=%lluMB\n", *total, *used);

        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("SD Card Size: %lluMB\n", cardSize);

        uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
        Serial.printf("SD Card Total: %lluMB\n", totalSize);

        uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
        Serial.printf("SD Card Used: %lluMB\n", usedSize);
        shared_spi_unlock();
    }
}

#endif
//************************************[ screen 3 ]****************************************** GPS
void ui_gps_task_suspend(void)
{
    gps_task_suspend();
}
void ui_gps_task_resume(void)
{
    gps_task_resume();
}
void ui_gps_get_coord(double *lat, double *lng)
{
    gps_get_coord(lat, lng);
}
void ui_gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day)
{
    gps_get_data(year, month, day);
}
void ui_gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    gps_get_time(hour, minute, second);
}

void ui_gps_get_satellites(uint32_t *vsat)
{
    gps_get_satellites(vsat);
}
void ui_gps_get_speed(double *speed)
{
    gps_get_speed(speed);
}
//************************************[ screen 4 ]****************************************** Wifi Scan
int is_chinese_utf8(const char *str) {
    unsigned char c = (unsigned char)str[0];
    return (c >= 0xE0 && c <= 0xEF);  // 检查第一个字节是否在 UTF-8 的中文字符范围内
}

void ui_wifi_get_scan_info(ui_wifi_scan_info_t *list, int list_len)
{
    int n = WiFi.scanNetworks();
    if(n > list_len)
        n = list_len;
    
    memset(list, 0, (sizeof(*list) * list_len));
    for(int i = 0; i < n; i++)
    {
        const char *str = WiFi.SSID(i).c_str();
        if(is_chinese_utf8(str))
            continue;
        strncpy(list[i].name, WiFi.SSID(i).c_str(), 16);
        list[i].rssi = WiFi.RSSI(i);
    }
}
//************************************[ screen 5 ]****************************************** Test
bool ui_test_get(int peri_id)
{
    return peri_init_st[peri_id];
}
bool ui_test_sd_card(void) 
{
    return peri_init_st[E_PERI_SD];
}
bool ui_test_a7682e(void) 
{
    return peri_init_st[E_PERI_A7682E];
}
bool ui_test_pcm5102a(void)
{
    return peri_init_st[E_PERI_PCM5102A];
}

//************************************[ screen 6 ]****************************************** Battery
#if 1

// BQ25896
bool ui_battery_25896_is_vbus_in(void)
{
    return PPM.isVbusIn();
}

bool ui_batt_25896_is_chg(void)
{
    if(PPM.isCharging() == false) {
        return false;
    } else {
        return true;
    }
    // return true;
}
float ui_batt_25896_get_vbus(void)
{
    return (PPM.getVbusVoltage() *1.0 / 1000.0 );
    // return 4.5;
}
float ui_batt_25896_get_vsys(void)
{
    return (PPM.getSystemVoltage() * 1.0 / 1000.0);
    // return 4.5;
}
float ui_batt_25896_get_vbat(void)
{
    return (PPM.getBattVoltage() * 1.0 / 1000.0);
    // return 4.5;
}
float ui_batt_25896_get_volt_targ(void)
{
    return (PPM.getChargeTargetVoltage() * 1.0 / 1000.0);
    // return 4.5; 
}
float ui_batt_25896_get_chg_curr(void)
{
    return (PPM.getChargeCurrent());
    // return 4.5;
}
float ui_batt_25896_get_pre_curr(void)
{
    return (PPM.getPrechargeCurr());;
    // return 4.5;
}
const char * ui_batt_25896_get_chg_st(void)
{
    return PPM.getChargeStatusString();
    // return "hello";
}
const char * ui_batt_25896_get_vbus_st(void)
{
    return PPM.getBusStatusString();
    // return "hello";
}
const char * ui_batt_25896_get_ntc_st(void)
{
    return PPM.getNTCStatusString();
    // return "hello";
}
/* 27220 */
// All these helpers touch the primary I2C bus (BQ25896 PMU and BQ27220 fuel
// gauge). They are called from LVGL UI tasks (battery indicator, etc.) which
// race with the keypad drain task — guard each call with the bus mutex.
bool ui_battery_27220_is_vaild(void) {return peri_init_st[E_PERI_BQ27220]; }
bool ui_battery_is_external_power_present(void)
{
    if (peri_init_st[E_PERI_BQ25896]) {
        i2c0_lock();
        bool v = PPM.isVbusIn();
        i2c0_unlock();
        return v;
    }
    if (peri_init_st[E_PERI_BQ27220]) {
        i2c0_lock();
        bool v = bq27220.getAverageCurrent() > 0;
        i2c0_unlock();
        return v;
    }
    return false;
}
bool ui_battery_27220_get_input(void) { return ui_battery_is_external_power_present(); }
bool ui_battery_27220_get_charge_finish(void) { i2c0_lock(); bool v = bq27220.getCharingFinish(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_status(void)
{
    BQ27220BatteryStatus batt;
    i2c0_lock();
    bq27220.getBatteryStatus(&batt);
    i2c0_unlock();
    return batt.full;
}
uint16_t ui_battery_27220_get_voltage(void) { i2c0_lock(); uint16_t v = bq27220.getVoltage(); i2c0_unlock(); return v; }
int16_t ui_battery_27220_get_current(void) { i2c0_lock(); int16_t v = bq27220.getCurrent(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_temperature(void) { i2c0_lock(); uint16_t v = bq27220.getTemperature(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_full_capacity(void) { i2c0_lock(); uint16_t v = bq27220.getFullChargeCapacity(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_design_capacity(void) { i2c0_lock(); uint16_t v = bq27220.getDesignCapacity(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_remain_capacity(void) { i2c0_lock(); uint16_t v = bq27220.getRemainingCapacity(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_percent(void) { i2c0_lock(); uint16_t v = bq27220.getStateOfCharge(); i2c0_unlock(); return v; }
uint16_t ui_battery_27220_get_health(void) { i2c0_lock(); uint16_t v = bq27220.getStateOfHealth(); i2c0_unlock(); return v; }
bool ui_battery_27220_is_low_alarm(void)
{
    if (!peri_init_st[E_PERI_BQ27220]) {
        return false;
    }

    BQ27220BatteryStatus batt = {0};
    BQ27220OperationStatus oper = {0};
    BQ27220GaugingStatus gauging = {0};

    i2c0_lock();
    bq27220.getBatteryStatus(&batt);
    bq27220.getGaugingStatus(&gauging);
    i2c0_unlock();

    return batt.reg.SYSDWN || batt.reg.TDA || gauging.reg.EDV;
}
const char * ui_battert_27220_get_percent_level(void)
{
    i2c0_lock();
    int percent = bq27220.getStateOfCharge();
    i2c0_unlock();
    const char * str = NULL;
    if(percent < 20)      str =  LV_SYMBOL_BATTERY_EMPTY;
    else if(percent < 40) str =  LV_SYMBOL_BATTERY_1;
    else if(percent < 65) str =  LV_SYMBOL_BATTERY_2;
    else if(percent < 90) str =  LV_SYMBOL_BATTERY_3;
    else                  str =  LV_SYMBOL_BATTERY_FULL;
    return str;
}
#endif
//************************************[ screen 7 ]****************************************** Input
int ui_input_get_touch_coord(int *x, int *y)
{
    int16_t last_x = 0;
    int16_t last_y = 0;
    // int ret = touch.getPoint(&last_x, &last_y);
    int ret = hyn_touch_get_point(&last_x, &last_y, 1);

    *x = last_x;
    *y = last_y;
    return ret;
}

int ui_input_get_keypad_val(char *v)
{
    return keypad_get_val(v);
}

void ui_input_set_keypad_flag(void)
{
    keypad_set_flag();
}

int ui_other_get_LTR(int *ch0, int *ch1, int *ps)
{
    // if(ch0 != NULL) *ch0 = lv_rand(0, LCD_VER_SIZE);
    // if(ch1 != NULL) *ch1 = lv_rand(0, LCD_VER_SIZE);
    // if(ps  != NULL) *ps  = lv_rand(0, LCD_VER_SIZE);

    if((ch0 != NULL) && (ch1 != NULL) && (ps != NULL))
    {
        *ch0 = LTR_553ALS_get_channel(0);
        *ch1 = LTR_553ALS_get_channel(1);
        *ps  = LTR_553ALS_get_ps();
    }
    else
    {
        Serial.printf("[%d] %s : Argument cannot be empty", __LINE__, __FILE__);
    }
    return 1;
}

int ui_other_get_gyro(float *gyro_x, float *gyro_y, float *gyro_z)
{
    // if(gyro_x != NULL) *gyro_x = lv_rand(0, LCD_VER_SIZE);
    // if(gyro_y != NULL) *gyro_y = lv_rand(0, LCD_VER_SIZE);
    // if(gyro_z != NULL) *gyro_z = lv_rand(0, LCD_VER_SIZE);

    if((gyro_x != NULL) && (gyro_x != NULL) && (gyro_x != NULL))
    {
        BHI260AP_get_val(2, gyro_x, gyro_y, gyro_z);
    }
    else
    {
        Serial.printf("[%d] %s : Argument cannot be empty", __LINE__, __FILE__);
    }
    return 1;
}

//************************************[ screen 8 ]****************************************** A7682E
bool ui_a7682_at_cb(const char *at_cmd)
{
    printf("[A7682E] at cmd: %s\n", at_cmd);

    modem.sendAT("+CTTSPARAM=1,3,0,1,1");

    delay(100);

    modem.sendAT("+CTTS=2,\"1234567890\"");

    return false;
}

void ui_a7682_call(const char *number)
{
    char buf[32];
    lv_snprintf(buf, 32, "D%s;", number);
    printf("[A7682E] at cmd: %s\n", buf);

    modem.sendAT(buf);
    delay(100);
}

void ui_a7682_hang_up(void)
{
    modem.sendAT("+CHUP");
    delay(100);
}

void ui_a7682_loop_resume(void)
{
    vTaskResume(a7682_handle);
}

void ui_a7682_loop_suspend(void)
{
    vTaskSuspend(a7682_handle);
}

//************************************[ screen 9 ]****************************************** Input

void ui_shutdown_on(void)
{
    ink_screen_prepare_shutdown();
    PPM.shutdown();
    Serial.println("Shutdown .....");
}

//************************************[ screen 10 ]****************************************** PCM5102
bool ui_pcm5102_cb(const char *at_cmd)
{
    audio.connecttoFS(SPIFFS, "/iphone_call.mp3");
    return true;
}

void ui_pcm5102_stop(void)
{
    audio.stopSong();
}

// optional
void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
// void audio_id3data(const char *info){  //id3 metadata
//     Serial.print("id3data     ");Serial.println(info);
// }
// void audio_eof_mp3(const char *info){  //end of file
//     Serial.print("eof_mp3     ");Serial.println(info);
// }
// void audio_showstation(const char *info){
//     Serial.print("station     ");Serial.println(info);
// }
// void audio_showstreamtitle(const char *info){
//     Serial.print("streamtitle ");Serial.println(info);
// }
// void audio_bitrate(const char *info){
//     Serial.print("bitrate     ");Serial.println(info);
// }
// void audio_commercial(const char *info){  //duration in sec
//     Serial.print("commercial  ");Serial.println(info);
// }
// void audio_icyurl(const char *info){  //homepage
//     Serial.print("icyurl      ");Serial.println(info);
// }
// void audio_lasthost(const char *info){  //stream URL played
//     Serial.print("lasthost    ");Serial.println(info);
// }

//************************************[ screen 12 ]****************************************** Notes
#include <SPIFFS.h>

void ui_notes_get_list(bool is_sd, char list[UI_NOTES_MAX_COUNT][32], int *count)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    *count = 0;
    
    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    if (!fs.exists("/notes")) {
        fs.mkdir("/notes");
    }

    File root = fs.open("/notes");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file && *count < UI_NOTES_MAX_COUNT) {
            if (!file.isDirectory()) {
                const char* name = file.name();
                // file.name() might return full path or just name depending on version
                const char* lastSlash = strrchr(name, '/');
                if (lastSlash) {
                    strncpy(list[*count], lastSlash + 1, 31);
                } else {
                    strncpy(list[*count], name, 31);
                }
                list[*count][31] = '\0';
                (*count)++;
            }
            file = root.openNextFile();
        }
    }

    if (is_sd) {
        shared_spi_unlock();
    }
}

char* ui_notes_read(bool is_sd, const char *filename)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    char path[64];
    snprintf(path, sizeof(path), "/notes/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        if (is_sd) shared_spi_unlock();
        return NULL;
    }

    size_t size = file.size();
    char *content = (char *)malloc(size + 1);
    if (content) {
        file.readBytes(content, size);
        content[size] = '\0';
    }
    file.close();

    if (is_sd) shared_spi_unlock();
    return content;
}

bool ui_notes_write(bool is_sd, const char *filename, const char *content)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;

    // Strip leading/trailing whitespace and any embedded control chars from
    // the filename — the on-screen keypad sends '\n' on Enter, which would
    // otherwise leak into the path and make fs.open() fail.
    char clean_name[32] = {0};
    size_t out = 0;
    for (size_t i = 0; filename[i] && out < sizeof(clean_name) - 1; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (c >= 32 && c < 127) clean_name[out++] = (char)c;
    }
    while (out > 0 && clean_name[out - 1] == ' ') clean_name[--out] = '\0';
    if (out == 0) return false;

    char path[64];
    snprintf(path, sizeof(path), "/notes/%s", clean_name);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    if (!fs.exists("/notes")) {
        fs.mkdir("/notes");
    }

    File file = fs.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("ui_notes_write: open(%s) failed\n", path);
        if (is_sd) shared_spi_unlock();
        return false;
    }

    size_t content_len = strlen(content);
    size_t written = content_len ? file.print(content) : 0;
    file.close();

    if (is_sd) shared_spi_unlock();

    // Empty content writes a zero-byte file, which is a valid save.
    return written == content_len;
}

bool ui_notes_delete(bool is_sd, const char *filename)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    char path[64];
    snprintf(path, sizeof(path), "/notes/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    bool ret = fs.remove(path);

    if (is_sd) shared_spi_unlock();
    return ret;
}

//************************************[ screen 13 ]****************************************** Ebook Reader

void ui_reader_get_list(bool is_sd, char list[UI_READER_MAX_COUNT][32], int *count)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    *count = 0;
    
    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    if (!fs.exists("/reader")) {
        fs.mkdir("/reader");
    }

    File root = fs.open("/reader");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file && *count < UI_READER_MAX_COUNT) {
            if (!file.isDirectory()) {
                const char* name = file.name();
                // file.name() might return full path or just name depending on version
                const char* lastSlash = strrchr(name, '/');
                if (lastSlash) {
                    strncpy(list[*count], lastSlash + 1, 31);
                } else {
                    strncpy(list[*count], name, 31);
                }
                list[*count][31] = '\0';
                (*count)++;
            }
            file = root.openNextFile();
        }
    }

    if (is_sd) {
        shared_spi_unlock();
    }
}

char* ui_reader_read(bool is_sd, const char *filename)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    char path[64];
    snprintf(path, sizeof(path), "/reader/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        if (is_sd) shared_spi_unlock();
        return NULL;
    }

    size_t size = file.size();
    char *content = (char *)malloc(size + 1);
    if (content) {
        file.readBytes(content, size);
        content[size] = '\0';
    }
    file.close();

    if (is_sd) shared_spi_unlock();
    return content;
}


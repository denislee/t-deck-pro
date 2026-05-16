
#include "Arduino.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "utilities.h"

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "USB.h"
#include "USBMSC.h"
#include <TinyGPS++.h>
#include "peripheral.h"
#include "WiFi.h"
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <TouchDrvCSTXXX.hpp>
#include <Preferences.h>
#include "esp_sntp.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// extern 
extern TouchDrvCSTXXX touch;


volatile int default_language = DEFAULT_LANGUAGE_EN;
volatile bool default_keypad_light = false;
volatile bool default_red_led_status = false;
volatile bool default_motor_status = false;
volatile bool default_gps_status = true;
volatile bool default_lora_status = true;
volatile bool default_gyro_status = true;
volatile bool default_a7682_status = true;
volatile bool default_touch_status = false;

// System font preferences. face: 0=Mono Bold (built-in), 1=Sans (Montserrat),
// 2=Serif, 3=Pixel, 4=Spleen, 5=Tamzen, 6=Tiny — see reader_faces[] in
// ui_deckpro.cpp. The size index picks from the per-face size table there.
//
// One (face, size) pair per slot. Defaults: General/Topbar use Mono Bold 16
// (size index 1); reader body uses size 2 (18 pt) for comfortable reading;
// reader footer uses the smallest available size for a discreet status line.
static int default_font_face[UI_FONT_SLOT_COUNT] = {0, 0, 0, 0};
static int default_font_size[UI_FONT_SLOT_COUNT] = {1, 1, 2, 0};
static int default_reader_rotation  = 0; // 0=portrait, 1=landscape
static int default_reader_line_space = 4; // px between text lines in reader
static bool default_topbar_show_battery = true; // show battery icon+% on topbar
static bool default_reader_bars_hidden  = false; // reader view: hide top/bottom bars

static int reader_line_space_clamp(int px)
{
    if (px < 0)  return 0;
    if (px > 16) return 16;
    return px;
}

static int font_slot_clamp(int slot)
{
    if (slot < 0 || slot >= UI_FONT_SLOT_COUNT) return UI_FONT_SLOT_GENERAL;
    return slot;
}

// WiFi credentials and timezone. Persisted via Preferences. Keep buffers
// generously sized so callers can pass through without truncating spec-legal
// values (32 bytes SSID, 63 bytes WPA2 password).
static char default_wifi_ssid[33] = {0};
static char default_wifi_password[65] = {0};
static char default_wifi_tz[48] = "UTC0";

// Live WiFi state. Bumped from the connect/disconnect helpers and the
// asynchronous WiFi event handler so the UI can poll without blocking.
static volatile ui_wifi_status_t wifi_status = UI_WIFI_STATUS_DISABLED;
static volatile bool ntp_synced = false;

int  ui_font_face_get(int slot)         { return default_font_face[font_slot_clamp(slot)]; }
void ui_font_face_set(int slot, int f)  { default_font_face[font_slot_clamp(slot)] = f; ui_settings_save(); }
int  ui_font_size_get(int slot)         { return default_font_size[font_slot_clamp(slot)]; }
void ui_font_size_set(int slot, int s)  { default_font_size[font_slot_clamp(slot)] = s; ui_settings_save(); }
int  ui_reader_rotation_get(void)         { return default_reader_rotation; }
void ui_reader_rotation_set(int r)        { default_reader_rotation = r ? 1 : 0; ui_settings_save(); }
int  ui_reader_line_space_get(void)       { return default_reader_line_space; }
void ui_reader_line_space_set(int px)     { default_reader_line_space = reader_line_space_clamp(px); ui_settings_save(); }
bool ui_topbar_show_battery_get(void)     { return default_topbar_show_battery; }
void ui_topbar_show_battery_set(bool on)  { default_topbar_show_battery = on; ui_settings_save(); }
bool ui_reader_bars_hidden_get(void)         { return default_reader_bars_hidden; }
void ui_reader_bars_hidden_set(bool hidden)  { default_reader_bars_hidden = hidden; ui_settings_save(); }

// FNV-1a 32-bit hash of the filename, formatted as "b_xxxxxxxx" (10 chars).
// Per-file bookmarks live under a per-filename NVS key derived this way
// because NVS keys are capped at 15 chars but filenames can be up to 32.
// Output buffer must be at least 11 bytes.
static void reader_bookmark_key(const char *filename, char out[11])
{
    uint32_t h = 2166136261u;
    if (filename) {
        for (const unsigned char *p = (const unsigned char *)filename; *p; ++p) {
            h ^= *p;
            h *= 16777619u;
        }
    }
    snprintf(out, 11, "b_%08x", (unsigned)h);
}

bool ui_reader_resume_get(char *filename, size_t fn_size, size_t *offset)
{
    if (!filename || fn_size == 0) return false;
    Preferences prefs;
    prefs.begin("t-deck-pro", true);
    String s = prefs.getString("rd_last", "");
    uint32_t off = prefs.getUInt("rd_off", 0);
    prefs.end();
    if (s.length() == 0) { filename[0] = '\0'; return false; }
    strncpy(filename, s.c_str(), fn_size - 1);
    filename[fn_size - 1] = '\0';
    if (offset) *offset = off;
    return true;
}

void ui_reader_resume_set(const char *filename, size_t offset)
{
    Preferences prefs;
    prefs.begin("t-deck-pro", false);
    prefs.putString("rd_last", filename ? filename : "");
    prefs.putUInt("rd_off", (uint32_t)offset);
    if (filename && filename[0]) {
        char key[11];
        reader_bookmark_key(filename, key);
        prefs.putUInt(key, (uint32_t)offset);
    }
    prefs.end();
}

bool ui_reader_bookmark_get(const char *filename, size_t *offset)
{
    if (!filename || !filename[0]) return false;
    char key[11];
    reader_bookmark_key(filename, key);
    Preferences prefs;
    prefs.begin("t-deck-pro", true);
    bool present = prefs.isKey(key);
    uint32_t off = prefs.getUInt(key, 0);
    prefs.end();
    if (!present) return false;
    if (offset) *offset = off;
    return true;
}

void ui_settings_save(void)
{
    Preferences prefs;
    prefs.begin("t-deck-pro", false);
    prefs.putInt("lang", default_language);
    prefs.putBool("kplight", default_keypad_light);
    prefs.putBool("redled", default_red_led_status);
    prefs.putBool("motor", default_motor_status);
    prefs.putBool("gps", default_gps_status);
    prefs.putBool("lora", default_lora_status);
    prefs.putBool("gyro", default_gyro_status);
    prefs.putBool("a7682", default_a7682_status);
    prefs.putBool("touch", default_touch_status);
    // sys_face/sys_size remain the General slot (back-compat with older builds
    // that only knew about one font pair). The other three slots get their
    // own keys.
    prefs.putInt("sys_face",  default_font_face[UI_FONT_SLOT_GENERAL]);
    prefs.putInt("sys_size",  default_font_size[UI_FONT_SLOT_GENERAL]);
    prefs.putInt("tb_face",   default_font_face[UI_FONT_SLOT_TOPBAR]);
    prefs.putInt("tb_size",   default_font_size[UI_FONT_SLOT_TOPBAR]);
    prefs.putInt("rdb_face",  default_font_face[UI_FONT_SLOT_READER_BODY]);
    prefs.putInt("rdb_size",  default_font_size[UI_FONT_SLOT_READER_BODY]);
    prefs.putInt("rdf_face",  default_font_face[UI_FONT_SLOT_READER_FOOTER]);
    prefs.putInt("rdf_size",  default_font_size[UI_FONT_SLOT_READER_FOOTER]);
    prefs.putInt("rd_rot",  default_reader_rotation);
    prefs.putInt("rd_lsp",  default_reader_line_space);
    prefs.putBool("tb_batt", default_topbar_show_battery);
    prefs.putBool("rd_bars", default_reader_bars_hidden);
    prefs.putString("wifi_ssid", default_wifi_ssid);
    prefs.putString("wifi_pass", default_wifi_password);
    prefs.putString("wifi_tz",   default_wifi_tz);
    prefs.end();
}

void ui_settings_load(void)
{
    Preferences prefs;
    prefs.begin("t-deck-pro", true);
    default_language = prefs.getInt("lang", DEFAULT_LANGUAGE_EN);
    default_keypad_light = prefs.getBool("kplight", false);
    default_red_led_status = prefs.getBool("redled", false);
    default_motor_status = prefs.getBool("motor", false);
    default_gps_status = prefs.getBool("gps", true);
    default_lora_status = prefs.getBool("lora", true);
    default_gyro_status = prefs.getBool("gyro", true);
    default_a7682_status = prefs.getBool("a7682", true);
    default_touch_status = prefs.getBool("touch", false);
    int gen_face = prefs.getInt("sys_face", 0);
    int gen_size = prefs.getInt("sys_size", 1);
    default_font_face[UI_FONT_SLOT_GENERAL]       = gen_face;
    default_font_size[UI_FONT_SLOT_GENERAL]       = gen_size;
    // Each non-general slot defaults to the General slot's pair when its own
    // pref is missing — that way upgrading from an older build leaves the
    // device looking identical until the user customises a slot.
    default_font_face[UI_FONT_SLOT_TOPBAR]        = prefs.getInt("tb_face",  gen_face);
    default_font_size[UI_FONT_SLOT_TOPBAR]        = prefs.getInt("tb_size",  gen_size);
    default_font_face[UI_FONT_SLOT_READER_BODY]   = prefs.getInt("rdb_face", gen_face);
    default_font_size[UI_FONT_SLOT_READER_BODY]   = prefs.getInt("rdb_size", 2);
    default_font_face[UI_FONT_SLOT_READER_FOOTER] = prefs.getInt("rdf_face", gen_face);
    default_font_size[UI_FONT_SLOT_READER_FOOTER] = prefs.getInt("rdf_size", 0);
    default_reader_rotation  = prefs.getInt("rd_rot",  0);
    default_reader_line_space = reader_line_space_clamp(prefs.getInt("rd_lsp", 4));
    default_topbar_show_battery = prefs.getBool("tb_batt", true);
    default_reader_bars_hidden  = prefs.getBool("rd_bars", false);

    String s = prefs.getString("wifi_ssid", "");
    strncpy(default_wifi_ssid, s.c_str(), sizeof(default_wifi_ssid) - 1);
    default_wifi_ssid[sizeof(default_wifi_ssid) - 1] = '\0';

    s = prefs.getString("wifi_pass", "");
    strncpy(default_wifi_password, s.c_str(), sizeof(default_wifi_password) - 1);
    default_wifi_password[sizeof(default_wifi_password) - 1] = '\0';

    s = prefs.getString("wifi_tz", "UTC0");
    strncpy(default_wifi_tz, s.c_str(), sizeof(default_wifi_tz) - 1);
    default_wifi_tz[sizeof(default_wifi_tz) - 1] = '\0';

    prefs.end();

    // Apply TZ now so any later localtime() call (before NTP completes) at
    // least uses the correct offset once the clock is set.
    setenv("TZ", default_wifi_tz, 1);
    tzset();
}
// ----

void ui_disp_full_refr(void)
{
    disp_full_refr();
    lv_obj_invalidate(lv_scr_act());
}

void ui_disp_hard_refr(void)
{
    disp_hard_refresh();
    lv_obj_invalidate(lv_scr_act());
}

void ui_disp_white_clear(void)
{
    disp_white_clear();
}

void ui_set_reader_landscape(bool landscape)
{
    factory_set_landscape(landscape);
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
void ui_setting_apply_keypad_light(bool on)
{
    digitalWrite(BOARD_KEYBOARD_LED, on);
}
void ui_setting_set_red_led(bool on)
{
    digitalWrite(BOARD_RED_LED, on);
    default_red_led_status = on;
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
    if (on) {
        digitalWrite(BOARD_GPS_EN, HIGH);
        gps_task_resume();
    } else {
        gps_task_suspend();
        digitalWrite(BOARD_GPS_EN, LOW);
    }
    default_gps_status = on;
    ui_settings_save();
}
void ui_setting_set_lora_status(bool on)
{
    if (on) {
        digitalWrite(BOARD_LORA_EN, HIGH);
    } else {
        lora_sleep();
        digitalWrite(BOARD_LORA_EN, LOW);
    }
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
    if (on) {
        digitalWrite(BOARD_6609_EN, HIGH);
        digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
        if (a7682_handle) vTaskResume(a7682_handle);
    } else {
        if (a7682_handle) vTaskSuspend(a7682_handle);
        digitalWrite(BOARD_6609_EN, LOW);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    }
    default_a7682_status = on;
    ui_settings_save();
}
void ui_setting_set_touch_status(bool on)
{
    // Soft toggle: the CST328 IC stays powered (its 1V8 rail is shared with
    // the gyro and must stay up), but the LVGL touchpad_read callback honors
    // this flag and stops reporting points when off.
    default_touch_status = on;
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
bool ui_setting_get_red_led(void)
{
    return default_red_led_status;
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
bool ui_setting_get_touch_status(void)
{
    return default_touch_status;
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
    memset(list, 0, (sizeof(*list) * list_len));

    // Force STA mode and tear down any in-flight association first. An active
    // WiFi.begin() (e.g. from boot autoconnect) blocks scanNetworks() and
    // returns -1, leaving the user staring at an empty list. Settling for a
    // moment after mode/disconnect avoids racing the WiFi event loop.
    WiFi.mode(WIFI_STA);
    if (WiFi.status() == WL_CONNECTED || WiFi.status() == WL_IDLE_STATUS) {
        WiFi.disconnect(false, false);
    }
    // Drain a previously-running scan if any: scanNetworks(async=true,...) can
    // be called from another path; we wait for it to settle so the new scan
    // doesn't clobber state.
    int prev = WiFi.scanComplete();
    if (prev == WIFI_SCAN_RUNNING) {
        Serial.println("[wifi] previous scan still running, waiting...");
        for (int i = 0; i < 50 && WiFi.scanComplete() == WIFI_SCAN_RUNNING; i++) {
            delay(100);
        }
    }
    delay(100);

    // Synchronous scan with hidden networks revealed.
    int n = WiFi.scanNetworks(/*async*/ false, /*show_hidden*/ true);
    Serial.printf("[wifi] scanNetworks returned %d\n", n);
    if (n < 0) {
        // Common case after a failed begin(): radio is in a half-state. Reset
        // it and try once more before giving up.
        Serial.println("[wifi] scan failed, resetting radio and retrying");
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.mode(WIFI_STA);
        delay(200);
        n = WiFi.scanNetworks(false, true);
        Serial.printf("[wifi] retry scanNetworks returned %d\n", n);
    }
    if (n < 0) n = 0;
    if (n > list_len) n = list_len;

    int dst = 0;
    for (int i = 0; i < n && dst < list_len; i++)
    {
        String s = WiFi.SSID(i);
        const char *str = s.c_str();
        if (!str) continue;
        if (str[0] == '\0') {
            // Hidden SSID — surface it as "<hidden>" so the user can still
            // pick it (they'll need the SSID to actually connect though).
            strncpy(list[dst].name, "<hidden>", sizeof(list[dst].name) - 1);
        } else if (is_chinese_utf8(str)) {
            // Skip CJK SSIDs — our font has no glyphs for them.
            Serial.printf("[wifi] skipping non-latin SSID '%s'\n", str);
            continue;
        } else {
            strncpy(list[dst].name, str, sizeof(list[dst].name) - 1);
        }
        list[dst].name[sizeof(list[dst].name) - 1] = '\0';
        list[dst].rssi = WiFi.RSSI(i);
        list[dst].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        Serial.printf("[wifi]   %2d: %-32s %4d dBm enc=%d\n",
                      i, list[dst].name, list[dst].rssi,
                      (int)WiFi.encryptionType(i));
        dst++;
    }
    // Free the scan-result buffer the WiFi driver allocated. Otherwise
    // repeated scans (e.g. via 'r') leak memory.
    WiFi.scanDelete();
}

static bool wifi_enabled = false;

void ui_wifi_get_ssid(char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    strncpy(out, default_wifi_ssid, out_len - 1);
    out[out_len - 1] = '\0';
}

void ui_wifi_set_ssid(const char *ssid)
{
    if (!ssid) return;
    strncpy(default_wifi_ssid, ssid, sizeof(default_wifi_ssid) - 1);
    default_wifi_ssid[sizeof(default_wifi_ssid) - 1] = '\0';
    ui_settings_save();
}

void ui_wifi_get_password(char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    strncpy(out, default_wifi_password, out_len - 1);
    out[out_len - 1] = '\0';
}

void ui_wifi_set_password(const char *password)
{
    if (!password) return;
    strncpy(default_wifi_password, password, sizeof(default_wifi_password) - 1);
    default_wifi_password[sizeof(default_wifi_password) - 1] = '\0';
    ui_settings_save();
}

void ui_wifi_get_tz(char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    strncpy(out, default_wifi_tz, out_len - 1);
    out[out_len - 1] = '\0';
}

void ui_wifi_set_tz(const char *tz)
{
    if (!tz) return;
    strncpy(default_wifi_tz, tz, sizeof(default_wifi_tz) - 1);
    default_wifi_tz[sizeof(default_wifi_tz) - 1] = '\0';
    setenv("TZ", default_wifi_tz, 1);
    tzset();
    ui_settings_save();
}

int ui_wifi_get_status(void)
{
    return (int)wifi_status;
}

void ui_wifi_get_ip(char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    if (WiFi.status() == WL_CONNECTED) {
        strncpy(out, WiFi.localIP().toString().c_str(), out_len - 1);
    } else {
        out[0] = '\0';
        return;
    }
    out[out_len - 1] = '\0';
}

bool ui_time_is_synced(void)
{
    return ntp_synced;
}

bool ui_time_get_local(struct tm *out)
{
    if (!out) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    if (!ntp_synced && (out->tm_year + 1900) >= 2024) {
        ntp_synced = true;
    }
    return ntp_synced;
}

void ui_ntp_resync(void)
{
    if (WiFi.status() != WL_CONNECTED) return;
    setenv("TZ", default_wifi_tz, 1);
    tzset();
    // configTzTime applies the TZ string and starts SNTP polling. Multiple
    // servers give us a fallback if pool.ntp.org is unreachable.
    configTzTime(default_wifi_tz, "pool.ntp.org", "time.nist.gov", "time.google.com");
}

// ----- ICMP ping -----
//
// esp_ping runs the actual ICMP exchange on its own internal task and signals
// us via callbacks. The caller blocks on a semaphore until the session ends
// (success, timeout, or hard failure), so the calling task can treat ping as
// a simple synchronous "did it reply" boolean.

typedef struct {
    volatile bool replied;
    volatile uint32_t rtt_ms;
    SemaphoreHandle_t done;
} ui_ping_result_t;

static void ui_ping_on_success(esp_ping_handle_t hdl, void *args)
{
    ui_ping_result_t *r = (ui_ping_result_t *)args;
    uint32_t elapsed = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    r->rtt_ms = elapsed;
    r->replied = true;
}

static void ui_ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    // No-op — replied stays false, on_end will signal completion.
    (void)hdl; (void)args;
}

static void ui_ping_on_end(esp_ping_handle_t hdl, void *args)
{
    ui_ping_result_t *r = (ui_ping_result_t *)args;
    if (r && r->done) xSemaphoreGive(r->done);
}

bool ui_ping(const char *host_or_ip, int timeout_ms, int *rtt_ms_out)
{
    if (rtt_ms_out) *rtt_ms_out = -1;
    if (!host_or_ip || !*host_or_ip) return false;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ping] not connected");
        return false;
    }

    IPAddress addr;
    if (!addr.fromString(host_or_ip)) {
        if (!WiFi.hostByName(host_or_ip, addr)) {
            Serial.printf("[ping] DNS lookup failed for %s\n", host_or_ip);
            return false;
        }
    }
    ip_addr_t target = {0};
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = (uint32_t)addr;

    ui_ping_result_t result = {};
    result.done = xSemaphoreCreateBinary();
    if (!result.done) return false;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 1;
    cfg.interval_ms = 0;
    cfg.timeout_ms = (uint32_t)timeout_ms;
    cfg.target_addr = target;
    cfg.task_stack_size = 4096;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args = &result;
    cbs.on_ping_success = ui_ping_on_success;
    cbs.on_ping_timeout = ui_ping_on_timeout;
    cbs.on_ping_end     = ui_ping_on_end;

    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        Serial.println("[ping] esp_ping_new_session failed");
        vSemaphoreDelete(result.done);
        return false;
    }

    Serial.printf("[ping] -> %s\n", addr.toString().c_str());
    esp_ping_start(hdl);

    // Wait a bit beyond the timeout to give on_end a chance to fire.
    if (xSemaphoreTake(result.done, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) {
        Serial.println("[ping] semaphore timeout");
    }

    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    vSemaphoreDelete(result.done);

    if (result.replied) {
        if (rtt_ms_out) *rtt_ms_out = (int)result.rtt_ms;
        Serial.printf("[ping] reply rtt=%u ms\n", (unsigned)result.rtt_ms);
        return true;
    }
    Serial.println("[ping] no reply");
    return false;
}

// Background task: WiFi.begin() can take many seconds; we don't want to stall
// the LVGL/main loop. The task self-deletes when done.
static void wifi_connect_task(void *arg)
{
    wifi_status = UI_WIFI_STATUS_CONNECTING;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    WiFi.begin(default_wifi_ssid, default_wifi_password);

    const uint32_t t0 = millis();
    while (millis() - t0 < 20000) {
        if (WiFi.status() == WL_CONNECTED) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifi_status = UI_WIFI_STATUS_CONNECTED;
        wifi_enabled = true;
        ui_ntp_resync();
        Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
    } else {
        wifi_status = UI_WIFI_STATUS_FAILED;
        Serial.printf("[wifi] connect failed (ssid=%s)\n", default_wifi_ssid);
    }

    vTaskDelete(NULL);
}

bool ui_wifi_connect(void)
{
    if (default_wifi_ssid[0] == '\0') {
        wifi_status = UI_WIFI_STATUS_IDLE;
        return false;
    }
    if (wifi_status == UI_WIFI_STATUS_CONNECTING) return true;
    // Stack of 4096 is enough for WiFi.begin + a String op or two; lower
    // priority than the keypad task so we never preempt I2C transactions.
    xTaskCreate(wifi_connect_task, "wifi_conn", 4096, NULL, 1, NULL);
    return true;
}

void ui_wifi_disconnect(void)
{
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    wifi_enabled = false;
    wifi_status = UI_WIFI_STATUS_DISABLED;
    ntp_synced = false;
}

void ui_wifi_set_enabled(bool en)
{
    if (en) {
        WiFi.mode(WIFI_STA);
        wifi_enabled = true;
        if (default_wifi_ssid[0] != '\0') {
            ui_wifi_connect();
        } else {
            wifi_status = UI_WIFI_STATUS_IDLE;
        }
    } else {
        ui_wifi_disconnect();
    }
}

bool ui_wifi_get_enabled(void)
{
    return wifi_enabled;
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
//
// They also gate on peri_init_st[E_PERI_BQ27220]: when bq27220.init() fails
// (e.g. Unseal failed), the chip is on the bus but won't reply to register
// reads, so every unconditional getter would generate a ~1 s Wire timeout
// and a Wire.cpp:499 Error 263. With the gate, periodic taskbar/low-voltage
// pollers short-circuit to a safe default and the LVGL task stops stalling.
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
bool ui_battery_27220_get_charge_finish(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return false;
    i2c0_lock(); bool v = bq27220.getCharingFinish(); i2c0_unlock(); return v;
}
uint16_t ui_battery_27220_get_status(void)
{
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    BQ27220BatteryStatus batt;
    i2c0_lock();
    bq27220.getBatteryStatus(&batt);
    i2c0_unlock();
    return batt.full;
}
uint16_t ui_battery_27220_get_voltage(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); uint16_t v = bq27220.getVoltage(); i2c0_unlock(); return v;
}
int16_t ui_battery_27220_get_current(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); int16_t v = bq27220.getCurrent(); i2c0_unlock(); return v;
}
uint16_t ui_battery_27220_get_temperature(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); uint16_t v = bq27220.getTemperature(); i2c0_unlock(); return v;
}
uint16_t ui_battery_27220_get_full_capacity(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); uint16_t v = bq27220.getFullChargeCapacity(); i2c0_unlock(); return v;
}
uint16_t ui_battery_27220_get_design_capacity(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); uint16_t v = bq27220.getDesignCapacity(); i2c0_unlock(); return v;
}
uint16_t ui_battery_27220_get_remain_capacity(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); uint16_t v = bq27220.getRemainingCapacity(); i2c0_unlock(); return v;
}
// State of charge changes slowly; the UI refreshes it from multiple sites per
// taskbar tick and several screens chain back-to-back calls. Cache for 500ms
// to coalesce these into a single I2C transaction without changing any
// callers' API.
static uint16_t battery_percent_cache = 0;
static uint32_t battery_percent_cache_ms = 0;
#define BATTERY_PERCENT_CACHE_MS 500
uint16_t ui_battery_27220_get_percent(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    uint32_t now = millis();
    if (battery_percent_cache_ms != 0 && (now - battery_percent_cache_ms) < BATTERY_PERCENT_CACHE_MS) {
        return battery_percent_cache;
    }
    i2c0_lock();
    battery_percent_cache = bq27220.getStateOfCharge();
    i2c0_unlock();
    battery_percent_cache_ms = now;
    return battery_percent_cache;
}
uint16_t ui_battery_27220_get_health(void) {
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    i2c0_lock(); uint16_t v = bq27220.getStateOfHealth(); i2c0_unlock(); return v;
}
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
    // Reuse the cached percent getter — callers consistently pair this with
    // ui_battery_27220_get_percent(), so going direct would double the I2C
    // traffic to the fuel gauge.
    int percent = ui_battery_27220_get_percent();
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

#if CONFIG_TINYUSB_MSC_ENABLED
static USBMSC msc;
static bool msc_active = false;

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    uint32_t count = bufsize / 512;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.readRAW((uint8_t*)buffer + (i * 512), lba + i)) {
            shared_spi_unlock();
            return -1;
        }
    }
    shared_spi_unlock();
    return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    uint32_t count = bufsize / 512;
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.writeRAW(buffer + (i * 512), lba + i)) {
            shared_spi_unlock();
            return -1;
        }
    }
    shared_spi_unlock();
    return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
    return true;
}

void ui_usb_msc_begin(void) {
    if (msc_active) return;
    
    if (!ui_test_sd_card()) return;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    uint32_t sectorCount = SD.cardSize() / 512;
    shared_spi_unlock();

    msc.vendorID("LilyGo");
    msc.productID("T-Deck Pro");
    msc.productRevision("1.0");
    msc.onRead(onRead);
    msc.onWrite(onWrite);
    msc.onStartStop(onStartStop);
    msc.mediaPresent(true);
    msc.begin(sectorCount, 512);
    USB.begin();
    msc_active = true;
}

void ui_usb_msc_end(void) {
    if (!msc_active) return;
    Serial.println("[usb_msc] disabling mass storage");
    // Tell the host the medium has gone away. Give it a polling cycle to
    // notice via TEST UNIT READY before we tear down the LUN — otherwise
    // some hosts keep the drive cached as if still mounted.
    msc.mediaPresent(false);
    delay(200);
    msc.end();
    msc_active = false;
}

bool ui_usb_msc_is_active(void) {
    return msc_active;
}
#else
void ui_usb_msc_begin(void) {}
void ui_usb_msc_end(void) {}
bool ui_usb_msc_is_active(void) { return false; }
#endif

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

//************************************[ dictionary ]***************************************** Dictionary
//
// Sorted-TSV binary search on SD. The file lives at /dict/eng-pob.tsv and is
// expected to be sorted ASCII-case-insensitively by the headword column. We
// keep this on SD (not SPIFFS) because real bilingual dictionaries are several
// MB and would not fit in flash; the rest of the app's storage assumptions
// (notes, books) already require SD anyway.
//
// Algorithm: classic byte-offset binary search.
//   1. Pick mid = (lo+hi)/2, seek there, discard the partial line we landed
//      in by reading until '\n' (unless mid==0). Note line_start.
//   2. Read a line, parse out the headword (text before first '\t').
//   3. If headword < query: lo = position after this line.
//      Else:                hi = line_start.
//   4. Stop once the window is small (<=512 bytes) and linear-scan the
//      remainder. Linear scan also handles the case where multiple entries
//      share a headword (we return the first match).

static int dict_line_read(File &f, char *buf, size_t buf_size)
{
    // Returns bytes read (excluding '\n'), -1 on EOF before any data. The
    // line is null-terminated. Lines longer than buf_size-1 are truncated;
    // the file pointer is still advanced past the '\n'.
    size_t n = 0;
    bool any = false;
    while (true) {
        int c = f.read();
        if (c < 0) break;
        any = true;
        if (c == '\n') break;
        if (n + 1 < buf_size) buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return any ? (int)n : -1;
}

static int dict_strcasecmp_word(const char *line, const char *query)
{
    // Compare just the headword portion of `line` (up to '\t' or end) to
    // `query`, case-insensitive. Returns <0 / 0 / >0 like strcasecmp.
    const unsigned char *a = (const unsigned char *)line;
    const unsigned char *b = (const unsigned char *)query;
    while (*a && *a != '\t' && *b) {
        int ca = tolower(*a);
        int cb = tolower(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    bool a_end = (*a == '\0' || *a == '\t');
    bool b_end = (*b == '\0');
    if (a_end && b_end) return 0;
    if (a_end) return -1;
    return 1;
}

static char* dict_decode_definition(const char *src)
{
    // Convert literal "\\n" (backslash + 'n') sequences to real newlines so
    // multi-line definitions render correctly in the result label. Allocates
    // a fresh buffer; caller frees.
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == '\\' && src[i + 1] == 'n') {
            out[w++] = '\n';
            i++;
        } else {
            out[w++] = src[i];
        }
    }
    out[w] = '\0';
    return out;
}

char* ui_dict_lookup(const char *word)
{
    if (!word || !*word) return NULL;

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);

    File f = SD.open("/dict/eng-pob.tsv", FILE_READ);
    if (!f) {
        shared_spi_unlock();
        return NULL;
    }

    long lo = 0;
    long hi = (long)f.size();
    char line[512];

    while (hi - lo > 512) {
        long mid = lo + (hi - lo) / 2;
        f.seek(mid);
        if (mid != 0) {
            // Discard partial line at the seek point.
            while (true) {
                int c = f.read();
                if (c < 0 || c == '\n') break;
            }
        }
        long line_start = f.position();
        int len = dict_line_read(f, line, sizeof(line));
        if (len < 0) { hi = line_start; continue; }
        int cmp = dict_strcasecmp_word(line, word);
        if (cmp < 0) lo = f.position();
        else         hi = line_start;
    }

    // Linear scan of the remaining window.
    f.seek(lo);
    if (lo != 0) {
        while (true) {
            int c = f.read();
            if (c < 0 || c == '\n') break;
        }
    }

    char *result = NULL;
    while ((long)f.position() < hi) {
        int len = dict_line_read(f, line, sizeof(line));
        if (len < 0) break;
        int cmp = dict_strcasecmp_word(line, word);
        if (cmp == 0) {
            const char *tab = strchr(line, '\t');
            const char *def = tab ? tab + 1 : "";
            result = dict_decode_definition(def);
            break;
        }
        if (cmp > 0) break; // sorted, so we've passed it
    }

    f.close();
    shared_spi_unlock();
    return result;
}

bool ui_dict_available(void)
{
    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool ok = SD.exists("/dict/eng-pob.tsv");
    shared_spi_unlock();
    return ok;
}

//************************************[ screen 13 ]****************************************** Ebook Reader

// Cached file handle for the active reader view. While open, every
// ui_reader_read_range call that matches (is_sd, filename) reuses this handle
// instead of reopening — opening a file on SD walks the directory over SPI,
// which used to dominate cold-load and resume-seek time.
static File   s_reader_file;
static bool   s_reader_file_open = false;
static bool   s_reader_file_is_sd = true;
static char   s_reader_file_name[32] = {0};

size_t ui_reader_open_view(bool is_sd, const char *filename)
{
    ui_reader_close_view();
    if (!filename || !*filename) return 0;

    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    char path[64];
    snprintf(path, sizeof(path), "/books/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }
    s_reader_file = fs.open(path, FILE_READ);
    size_t size = 0;
    if (s_reader_file) {
        size = s_reader_file.size();
        s_reader_file_open = true;
        s_reader_file_is_sd = is_sd;
        strncpy(s_reader_file_name, filename, sizeof(s_reader_file_name) - 1);
        s_reader_file_name[sizeof(s_reader_file_name) - 1] = '\0';
    } else {
        log_e("reader_open_view: open failed: %s", path);
    }
    if (is_sd) shared_spi_unlock();
    return size;
}

void ui_reader_close_view(void)
{
    if (!s_reader_file_open) return;
    if (s_reader_file_is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }
    s_reader_file.close();
    if (s_reader_file_is_sd) shared_spi_unlock();
    s_reader_file_open = false;
    s_reader_file_name[0] = '\0';
}

void ui_reader_get_list(bool is_sd, char list[UI_READER_MAX_COUNT][32], int *count)
{
    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    *count = 0;
    
    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    if (!fs.exists("/books")) {
        fs.mkdir("/books");
    }

    File root = fs.open("/books");
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
    snprintf(path, sizeof(path), "/books/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        log_e("reader: open failed: %s", path);
        if (is_sd) shared_spi_unlock();
        return NULL;
    }

    size_t size = file.size();
    log_i("reader: opened %s size=%u psram=%u heap=%u",
          path, (unsigned)size,
          (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());

    // Books can be large; prefer PSRAM, fall back to internal heap.
    char *content = (char *)ps_malloc(size + 1);
    if (!content) content = (char *)malloc(size + 1);
    if (!content) {
        log_e("reader: alloc %u failed", (unsigned)(size + 1));
        file.close();
        if (is_sd) shared_spi_unlock();
        return NULL;
    }

    size_t got = file.readBytes(content, size);
    content[got] = '\0';
    if (got != size) log_w("reader: short read %u/%u", (unsigned)got, (unsigned)size);
    file.close();

    if (is_sd) shared_spi_unlock();
    return content;
}

// Returns true if the cached handle is open and matches the requested file —
// in which case ui_reader_read_range / ui_reader_size short-circuit the
// per-call open/close (the SD directory walk used to dominate read time).
static bool reader_cached_handle_matches(bool is_sd, const char *filename)
{
    return s_reader_file_open &&
           s_reader_file_is_sd == is_sd &&
           filename != NULL &&
           strncmp(s_reader_file_name, filename, sizeof(s_reader_file_name)) == 0;
}

size_t ui_reader_size(bool is_sd, const char *filename)
{
    if (reader_cached_handle_matches(is_sd, filename)) {
        // The handle is already open from ui_reader_open_view; just read size.
        if (is_sd) {
            shared_spi_lock();
            shared_spi_prepare_device(BOARD_SD_CS);
        }
        size_t size = s_reader_file.size();
        if (is_sd) shared_spi_unlock();
        return size;
    }

    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    char path[64];
    snprintf(path, sizeof(path), "/books/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }
    File file = fs.open(path, FILE_READ);
    size_t size = file ? file.size() : 0;
    if (file) file.close();
    if (is_sd) shared_spi_unlock();
    return size;
}

size_t ui_reader_read_range(bool is_sd, const char *filename, size_t offset,
                            char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return 0;
    buf[0] = '\0';

    // Fast path: reader view is active and has the file already open.
    if (reader_cached_handle_matches(is_sd, filename)) {
        if (is_sd) {
            shared_spi_lock();
            shared_spi_prepare_device(BOARD_SD_CS);
        }
        s_reader_file.seek(offset);
        size_t got = s_reader_file.readBytes(buf, buf_size - 1);
        buf[got] = '\0';
        if (is_sd) shared_spi_unlock();
        return got;
    }

    fs::FS &fs = is_sd ? (fs::FS &)SD : (fs::FS &)SPIFFS;
    char path[64];
    snprintf(path, sizeof(path), "/books/%s", filename);

    if (is_sd) {
        shared_spi_lock();
        shared_spi_prepare_device(BOARD_SD_CS);
    }
    File file = fs.open(path, FILE_READ);
    size_t got = 0;
    if (file) {
        if (offset > 0) file.seek(offset);
        got = file.readBytes(buf, buf_size - 1);
        buf[got] = '\0';
        file.close();
    } else {
        log_e("reader_range: open failed: %s", path);
    }
    if (is_sd) shared_spi_unlock();
    return got;
}


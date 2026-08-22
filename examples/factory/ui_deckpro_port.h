/************************************************************************
 * FilePath     : ui_base.h
 * Author       : GX.Duan
 * LastEditors  : ShallowGreen123 2608653986@qq.com
 * Copyright (c): 2022 by GX.Duan, All Rights Reserved.
 * Github       : https://github.com/ShallowGreen123/lvgl_examples.git
 ************************************************************************/
#ifndef __UI_PORT_DECKPOR_H__
#define __UI_PORT_DECKPOR_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include "lvgl.h"
#include "peripheral.h"
#include "ui_deckpro.h"
#include "utilities.h"
#include "factory.h"

/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
#ifndef DEFAULT_LANGUAGE_EN
#define DEFAULT_LANGUAGE_EN 1
#endif
#ifndef DEFAULT_LANGUAGE_CN
#define DEFAULT_LANGUAGE_CN 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

void ui_disp_full_refr(void);
void ui_disp_hard_refr(void);
void ui_disp_white_clear(void);
void ui_set_reader_landscape(bool landscape);

// DEFAULT_LANGUAGE_CN、DEFAULT_LANGUAGE_EN
void ui_setting_set_language(int language);
int ui_setting_get_language(void);

// settings persistence
void ui_settings_load(void);
void ui_settings_save(void);

bool ui_setting_get_keypad_light(void);
bool ui_setting_get_red_led(void);
bool ui_setting_get_motor_status(void);
bool ui_setting_get_gps_status(void);
bool ui_setting_get_lora_status(void);
bool ui_setting_get_gyro_status(void);
bool ui_setting_get_a7682_status(void);
bool ui_setting_get_touch_status(void);

void ui_setting_set_keypad_light(bool on);
// Drive the keypad LED without touching the persisted user preference.
// Used by the lock screen to turn the LED off while locked and restore it
// to the user's setting on unlock.
void ui_setting_apply_keypad_light(bool on);
void ui_setting_set_red_led(bool on);
void ui_setting_set_motor_status(bool on);
void ui_setting_set_gps_status(bool on);
void ui_setting_set_lora_status(bool on);
void ui_setting_set_gyro_status(bool on);
void ui_setting_set_a7682_status(bool on);
void ui_setting_set_touch_status(bool on);

// setting - > About System
const char *ui_setting_get_sf_ver(void);
const char *ui_setting_get_hd_ver(void);
void ui_setting_get_sd_capacity(uint64_t *total, uint64_t *used);

// [ screen 3 ] --- GPS
void ui_gps_task_suspend(void);
void ui_gps_task_resume(void);
void ui_gps_get_coord(double *lat, double *lng);
void ui_gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day);
void ui_gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second);
void ui_gps_get_satellites(uint32_t *vsat);
void ui_gps_get_speed(double *speed);

// [ screen 4 ] --- Wifi Scan
void ui_wifi_get_scan_info(ui_wifi_scan_info_t *list, int list_len);
// Async variants used by screen4 so an Esc press can interrupt without
// having to wait for the (multi-second) WiFi.scanNetworks() to return.
// _start_async kicks off WiFi.scanNetworks(async=true, hidden=true).
// _poll returns -1 if still running, -2 if the scan failed, otherwise the
// number of results copied into `list` (0..list_len).
// _cancel aborts a running scan and frees the result buffer.
void ui_wifi_scan_start_async(void);
int  ui_wifi_scan_poll(ui_wifi_scan_info_t *list, int list_len);
void ui_wifi_scan_cancel(void);
void ui_wifi_set_enabled(bool en);
bool ui_wifi_get_enabled(void);

// [ screen 4.1 ] --- Wifi Config + NTP
// Stored credentials. Buffers must be at least 33 bytes for SSID and 65 for password.
void ui_wifi_get_ssid(char *out, int out_len);
void ui_wifi_set_ssid(const char *ssid);
void ui_wifi_get_password(char *out, int out_len);
void ui_wifi_set_password(const char *password);
// Timezone string in POSIX TZ format (e.g. "UTC0", "EST5EDT,M3.2.0,M11.1.0").
void ui_wifi_get_tz(char *out, int out_len);
void ui_wifi_set_tz(const char *tz);

typedef enum {
    UI_WIFI_STATUS_DISABLED = 0,
    UI_WIFI_STATUS_IDLE,
    UI_WIFI_STATUS_CONNECTING,
    UI_WIFI_STATUS_CONNECTED,
    UI_WIFI_STATUS_FAILED,
} ui_wifi_status_t;
int ui_wifi_get_status(void);
void ui_wifi_get_ip(char *out, int out_len);
// Kicks off an asynchronous connect attempt using stored creds. Returns false
// immediately if creds are missing.
bool ui_wifi_connect(void);
void ui_wifi_disconnect(void);

// True once SNTP has produced a plausible local time (year >= 2024).
bool ui_time_is_synced(void);
// Fills 'out' with current local time. Returns true when the clock is synced.
bool ui_time_get_local(struct tm *out);
// Triggers a manual NTP refresh (no-op if WiFi is not connected).
void ui_ntp_resync(void);

// Persisted-clock fallback. This board has no battery-backed RTC, so on every
// power cycle the system clock resets to 1970. ui_time_persist_save() stamps
// the current time into NVS; ui_time_persist_restore() reads it back at boot.
// Restored time is wrong by however long the device was off, but it's good
// enough to render a sensible "HH:MM" on the topbar — and gets corrected the
// moment GPS gets a fix or WiFi runs NTP.
void ui_time_persist_save(void);
void ui_time_persist_restore(void);

// ICMP ping. Blocks for up to (timeout_ms + ~500ms) on the calling task.
// Returns true on reply; rtt_ms_out (if non-NULL) gets the RTT in ms on
// success or -1 on failure. Returns false immediately if not associated.
bool ui_ping(const char *host_or_ip, int timeout_ms, int *rtt_ms_out);

// [ screen 5 ] --- State
bool ui_test_get(int peri_id);
bool ui_test_sd_card(void);
bool ui_test_a7682e(void);
bool ui_test_pcm5102(void);

// [ screen 6 ] --- Battery
/* 25896 */
bool ui_battery_25896_is_vbus_in(void);
bool ui_batt_25896_is_chg(void);
float ui_batt_25896_get_vbus(void);
float ui_batt_25896_get_vsys(void);
float ui_batt_25896_get_vbat(void);
float ui_batt_25896_get_volt_targ(void);
float ui_batt_25896_get_chg_curr(void);
float ui_batt_25896_get_pre_curr(void);
const char * ui_batt_25896_get_chg_st(void);
const char * ui_batt_25896_get_vbus_st(void);
const char * ui_batt_25896_get_ntc_st(void);

/* 27220 */
bool ui_battery_27220_is_vaild(void);
bool ui_battery_27220_get_input(void);
bool ui_battery_27220_get_charge_finish(void);
uint16_t ui_battery_27220_get_status(void);
uint16_t ui_battery_27220_get_voltage(void);
int16_t ui_battery_27220_get_current(void);
uint16_t ui_battery_27220_get_temperature(void);
uint16_t ui_battery_27220_get_full_capacity(void);
uint16_t ui_battery_27220_get_design_capacity(void);
uint16_t ui_battery_27220_get_remaining_capacity(void);
uint16_t ui_battery_27220_get_remain_capacity(void);
uint16_t ui_battery_27220_get_percent(void);
uint16_t ui_battery_27220_get_health(void);
const char * ui_battert_27220_get_percent_level(void);
bool ui_battery_27220_is_low_alarm(void);

/* status bar info */
bool ui_battery_is_external_power_present(void);

// [ screen 7 ] --- Input
int ui_input_get_touch_coord(int *x, int *y);
int ui_input_get_keypad_val(char *val);
void ui_input_set_keypad_flag(void);
int ui_other_get_gyro(float *x, float *y, float *z);

// [ screen 8 ] --- Lora
float ui_lora_get_freq(void);
void ui_lora_set_freq(float freq);
int ui_lora_get_bandwidth(void);
void ui_lora_set_bandwidth(float bd);
int ui_lora_get_power(void);
void ui_lora_set_power(float po);
void ui_lora_param_set(void);
int ui_lora_get_mode(void);
void ui_lora_set_mode(int mode);
void ui_lora_send(const char *str);
void ui_lora_recv_loop(void);
bool ui_lora_get_recv(const char **str, int *rssi);
void ui_lora_set_recv_flag(void);

// shutdown
void ui_shutdown_on(void);

// [ screen 10 ] --- A7682E
void ui_a7682_call(const char *num);
void ui_a7682_hang_up(void);
void ui_a7682_loop_resume(void);
void ui_a7682_loop_suspend(void);
bool ui_a7682_at_cb(const char *at_cmd);
bool ui_pcm5102_cb(const char *at_cmd);
void ui_pcm5102_stop(void);

// [ screen 12 ] --- Notes
#define UI_NOTES_MAX_COUNT 20
void ui_notes_get_list(bool is_sd, char list[UI_NOTES_MAX_COUNT][32], int *count);
char* ui_notes_read(bool is_sd, const char *filename);
bool ui_notes_write(bool is_sd, const char *filename, const char *content);
bool ui_notes_delete(bool is_sd, const char *filename);

// [ screen 13 ] --- Ebook Reader
#define UI_READER_MAX_COUNT 20
void ui_reader_get_list(bool is_sd, char list[UI_READER_MAX_COUNT][32], int *count);
char* ui_reader_read(bool is_sd, const char *filename);
size_t ui_reader_size(bool is_sd, const char *filename);
// Reads up to buf_size-1 bytes starting at `offset`, null-terminates buf,
// returns bytes actually read (0 on failure / EOF).
size_t ui_reader_read_range(bool is_sd, const char *filename, size_t offset, char *buf, size_t buf_size);

// Opens the named book file and caches the File handle so subsequent
// ui_reader_read_range calls (with the same is_sd/filename) reuse it instead
// of reopening the file from scratch each time. Returns the file size, or 0
// on open failure. Call ui_reader_close_view() when leaving the reader.
//
// Why: opening a file on the SD card walks FAT directory entries over SPI;
// for a long book the resume-seek and per-page-flip read paths used to do
// that on every 1 KB page, making cold-load and big-book resumes slow.
size_t ui_reader_open_view(bool is_sd, const char *filename);
void   ui_reader_close_view(void);

// [ screen dict ] --- Dictionary
//
// Looks up `word` (case-insensitively) in /dict/eng-pob.tsv on SD. The file
// is a tab-separated, ASCII-sorted-by-headword list — one entry per line:
//
//   headword<TAB>definition\n
//
// Definitions may use the literal sequence "\\n" (backslash + 'n') to encode
// line breaks since real newlines would break the line-per-entry format.
//
// Returns a malloc'd, null-terminated string with the definition (real
// newlines decoded), or NULL if the word is not found / SD is unavailable.
// Caller frees.
char* ui_dict_lookup(const char *word);

// Returns true if /dict/eng-pob.tsv exists and is readable on the SD card.
// Used to distinguish "word not in dictionary" from "dictionary file missing"
// when ui_dict_lookup returns NULL.
bool ui_dict_available(void);

// System font selection (persisted via Preferences).
//
// Each slot holds an independent (face, size) pair so the four broad font
// regions can be styled separately:
//   GENERAL       — every UI label that goes through ui_get_font(pt, false)
//   TOPBAR        — clock + battery + wifi/charge symbols in the status bar
//   READER_BODY   — the reader page content
//   READER_FOOTER — the small status line at the bottom of the reader view
#define UI_FONT_SLOT_GENERAL       0
#define UI_FONT_SLOT_TOPBAR        1
#define UI_FONT_SLOT_READER_BODY   2
#define UI_FONT_SLOT_READER_FOOTER 3
#define UI_FONT_SLOT_COUNT         4

int  ui_font_face_get(int slot);
void ui_font_face_set(int slot, int f);
int  ui_font_size_get(int slot);
void ui_font_size_set(int slot, int s);
int  ui_reader_rotation_get(void);   // 0=portrait, 1=landscape
void ui_reader_rotation_set(int r);

// Lock screen orientation: persisted across reboots so the lock screen
// comes back up in whichever orientation the user last toggled it to
// (via 'r' on the lock screen). 0=portrait, 1=landscape.
int  ui_lock_landscape_get(void);
void ui_lock_landscape_set(int r);

// Vertical pixel gap between rendered text lines in the reader body label.
// Persisted via Preferences. Clamped to a sensible range by the setter.
int  ui_reader_line_space_get(void);
void ui_reader_line_space_set(int px);

// Toggle: show/hide the battery icon and percent labels on the top status bar.
// Persisted via Preferences.
bool ui_topbar_show_battery_get(void);
void ui_topbar_show_battery_set(bool on);

// Toggle: hide top/bottom bars in the reader view for distraction-free reading.
// Persisted via Preferences so the user's last choice survives a reboot.
bool ui_reader_bars_hidden_get(void);
void ui_reader_bars_hidden_set(bool hidden);

// Last-read bookmark: filename + byte offset of the page the reader was
// showing when the user left the view. Returns true if a saved entry exists
// and `filename` is non-empty.
bool ui_reader_resume_get(char *filename, size_t fn_size, size_t *offset);
void ui_reader_resume_set(const char *filename, size_t offset);

// Per-file bookmark: returns the saved page offset for `filename` (the byte
// offset of the page the user was viewing when they last exited that book),
// or false if no bookmark exists for it.
bool ui_reader_bookmark_get(const char *filename, size_t *offset);

// Per-file page-index bookmark (§3.5).
//
// Stores and retrieves the LVGL reader page index (i.e. the index into
// reader_page_offsets[]) alongside the byte-offset bookmark so that
// ui_reader_resume can start rendering at the saved page directly instead
// of walking the full O(n) offset array from page 0.
//
// Call ui_reader_bookmark_page_set() whenever ui_reader_resume_set() is
// called, passing the current page index. On resume, call
// ui_reader_bookmark_page_get() first; if it returns true, seed
// reader_page_offsets[0] = saved_byte_offset and start at saved_page_index
// (invalidate on geometry/font change by clearing both keys when those
// settings change).
void ui_reader_bookmark_page_set(const char *filename, int page_index);
bool ui_reader_bookmark_page_get(const char *filename, int *page_index_out);

// USB MSC
void ui_usb_msc_begin(void);
void ui_usb_msc_end(void);
bool ui_usb_msc_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_PORT_DECKPOR_H__ */


#include "Arduino.h"
#include "ui_deckpro_port.h"
#include "factory.h"
#include "utilities.h"

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "USB.h"
#include "USBMSC.h"
#include "peripheral.h"
#include "WiFi.h"
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <Preferences.h>
#include "esp_sntp.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Defaults live in k_settings[] below; ui_settings_load() runs first thing in
// setup() and fills every one of these.
static bool default_keypad_light;
static bool default_red_led_status;
static bool default_motor_status;
static bool default_gps_status;
static bool default_lora_status;
#ifdef BOARD_HAS_BHI260AP
static bool default_gyro_status;
#endif
static bool default_a7682_status;

// System font preferences. face: 0=Mono Bold (built-in), 1=Sans (Montserrat),
// 2=Serif, 3=Pixel, 4=Spleen, 5=Tamzen, 6=Tiny — see reader_faces[] in
// ui_reader.cpp. The size index picks from the per-face size table there.
//
// One (face, size) pair per slot. Defaults: General/Topbar use Mono Bold 16
// (size index 1); reader body uses size 2 (18 pt) for comfortable reading;
// reader footer uses the smallest available size for a discreet status line.
static int default_font_face[UI_FONT_SLOT_COUNT] = {0, 0, 0, 0};
static int default_font_size[UI_FONT_SLOT_COUNT] = {1, 1, 2, 0};
static int default_reader_rotation; // 0=portrait, 1=landscape
static int default_reader_line_space; // px between text lines in reader
static bool default_topbar_show_battery; // show battery icon+% on topbar
static bool default_reader_bars_hidden;  // reader view: hide top/bottom bars
static int default_lock_landscape;   // lock screen: 0=portrait, 1=landscape

// True while the SD card's raw sectors are exported to a USB host. Defined
// unconditionally (the MSC code itself is compiled in only with TinyUSB MSC)
// so the FatFs callers below can always fence themselves off.
static bool msc_active = false;

// FatFs may only touch the card when it is mounted and not exported over USB.
static bool sd_fs_ready(void) { return peri_init_st[E_PERI_SD] && !msc_active; }

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
static bool default_wifi_enabled;
// Live radio state: true while the WiFi stack is up (or coming up). Defined
// up here (rather than next to the connect helpers) so ui_settings_load() can
// seed it from the persisted intent during boot.
static bool wifi_enabled = false;

// Live WiFi state. Bumped from the connect/disconnect helpers and the
// asynchronous WiFi event handler so the UI can poll without blocking.
static volatile ui_wifi_status_t wifi_status = UI_WIFI_STATUS_DISABLED;
static volatile bool ntp_synced = false;
// "Local clock is plausible" (year >= 2024). Independent of ntp_synced: a
// persisted / compile-time-seeded clock makes this true with no network I/O.
static volatile bool clock_valid = false;

// LoRa modem defaults, persisted via Settings (lr_freq / lr_bw / lr_pwr).
static float lora_default_freq;
static int   lora_default_band;
static int   lora_default_power;

// Forward declaration — defined after ui_settings_save() below.
static void settings_mark_dirty(void);

int  ui_font_face_get(int slot)         { return default_font_face[font_slot_clamp(slot)]; }
void ui_font_face_set(int slot, int f)  { default_font_face[font_slot_clamp(slot)] = f; settings_mark_dirty(); }
int  ui_font_size_get(int slot)         { return default_font_size[font_slot_clamp(slot)]; }
void ui_font_size_set(int slot, int s)  { default_font_size[font_slot_clamp(slot)] = s; settings_mark_dirty(); }
int  ui_reader_rotation_get(void)         { return default_reader_rotation; }
void ui_reader_rotation_set(int r)        { default_reader_rotation = r ? 1 : 0; settings_mark_dirty(); }
int  ui_reader_line_space_get(void)       { return default_reader_line_space; }
void ui_reader_line_space_set(int px)     { default_reader_line_space = reader_line_space_clamp(px); settings_mark_dirty(); }
bool ui_topbar_show_battery_get(void)     { return default_topbar_show_battery; }
void ui_topbar_show_battery_set(bool on)  { default_topbar_show_battery = on; settings_mark_dirty(); }
bool ui_reader_bars_hidden_get(void)         { return default_reader_bars_hidden; }
void ui_reader_bars_hidden_set(bool hidden)  { default_reader_bars_hidden = hidden; settings_mark_dirty(); }
int  ui_lock_landscape_get(void)             { return default_lock_landscape; }
void ui_lock_landscape_set(int r)            { default_lock_landscape = r ? 1 : 0; settings_mark_dirty(); }

// NVS namespace shared by every persisted setting and bookmark.
static const char kNvsNs[] = "t-deck-pro";

// Opens the shared NVS namespace for the enclosing scope.
struct NvsPrefs {
    Preferences p;
    explicit NvsPrefs(bool read_only) { p.begin(kNvsNs, read_only); }
    ~NvsPrefs() { p.end(); }
};

// strncpy() + explicit NUL terminator, so dst is always terminated.
static void copy_str(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0) return;
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = '\0';
}

// printf-free fixed-point: '%.<decimals>f<suffix>' without pulling in
// newlib's float printf (~35 KB). decimals 0..3.
int ui_fmt_fixed(char *buf, size_t n, float v, int decimals, const char *suffix)
{
    static const long k_scale[] = {1, 10, 100, 1000};
    if (decimals < 0) decimals = 0;
    if (decimals > 3) decimals = 3;
    long scale = k_scale[decimals];
    long x = (long)(v * scale + (v < 0 ? -0.5f : 0.5f));
    const char *sign = (x < 0) ? "-" : "";
    if (x < 0) x = -x;
    if (!suffix) suffix = "";
    if (decimals == 0) return snprintf(buf, n, "%s%ld%s", sign, x, suffix);
    return snprintf(buf, n, "%s%ld.%0*ld%s", sign, x / scale, decimals, x % scale, suffix);
}

static uint32_t fnv_hash(const char *filename)
{
    uint32_t h = 2166136261u;
    if (filename) {
        for (const unsigned char *p = (const unsigned char *)filename; *p; ++p) {
            h ^= *p;
            h *= 16777619u;
        }
    }
    return h;
}

// Formats a precomputed hash as "<prefix>_xxxxxxxx" (10 chars).
// Output buffer must be at least 11 bytes.
static void fnv_key_h(char prefix, uint32_t h, char out[11])
{
    snprintf(out, 11, "%c_%08x", prefix, (unsigned)h);
}

// FNV-1a 32-bit hash of the filename, formatted as "<prefix>_xxxxxxxx"
// (10 chars). Per-file bookmarks live under a per-filename NVS key derived
// this way because NVS keys are capped at 15 chars but filenames can be up
// to UI_READER_NAME_MAX. 'b' = byte-offset bookmark, 'p' = page-index
// bookmark, 'c' = geometry cookie.
static void fnv_key(char prefix, const char *filename, char out[11])
{
    fnv_key_h(prefix, fnv_hash(filename), out);
}

// Per-book keys (b_/p_/c_) are pruned LRU-style so NVS (~500 entries in
// 20 KB) cannot fill up after a few hundred books.
#define BOOKMARK_LRU_MAX 48
static void bookmark_touch(Preferences &prefs, const char *filename)
{
    if (!filename || !filename[0]) return;

    uint32_t lru[BOOKMARK_LRU_MAX + 1];
    size_t   n = 0;

    size_t blob_len = prefs.getBytesLength("bm_lru");
    if (blob_len >= sizeof(uint32_t)) {
        if (blob_len > BOOKMARK_LRU_MAX * sizeof(uint32_t)) {
            blob_len = BOOKMARK_LRU_MAX * sizeof(uint32_t);
        }
        prefs.getBytes("bm_lru", lru, blob_len);
        n = blob_len / sizeof(uint32_t);
        if (n > BOOKMARK_LRU_MAX) n = BOOKMARK_LRU_MAX;
    }

    // Drop the touched book from wherever it was (MRU order after this pass).
    uint32_t h = fnv_hash(filename);
    size_t   w = 0;
    for (size_t i = 0; i < n; i++) {
        if (lru[i] != h) lru[w++] = lru[i];
    }
    n = w;

    // Insert it at index 0 = most recently used.
    for (size_t i = n; i > 0; i--) lru[i] = lru[i - 1];
    lru[0] = h;
    n++;

    // Over the cap: the last entry is the least recently used — evict it.
    if (n > BOOKMARK_LRU_MAX) {
        uint32_t evict = lru[n - 1];
        char key[11];
        fnv_key_h('b', evict, key); prefs.remove(key);
        fnv_key_h('p', evict, key); prefs.remove(key);
        fnv_key_h('c', evict, key); prefs.remove(key);
        n--;
    }

    prefs.putBytes("bm_lru", lru, n * sizeof(uint32_t));
}

bool ui_reader_resume_get(char *filename, size_t fn_size, size_t *offset)
{
    if (!filename || fn_size == 0) return false;
    NvsPrefs nvs(true);
    Preferences &prefs = nvs.p;
    String s = prefs.getString("rd_last", "");
    uint32_t off = prefs.getUInt("rd_off", 0);
    if (s.length() == 0) { filename[0] = '\0'; return false; }
    copy_str(filename, fn_size, s.c_str());
    if (offset) *offset = off;
    return true;
}

void ui_reader_resume_set(const char *filename, size_t offset)
{
    NvsPrefs nvs(false);
    Preferences &prefs = nvs.p;
    prefs.putString("rd_last", filename ? filename : "");
    prefs.putUInt("rd_off", (uint32_t)offset);
    if (filename && filename[0]) {
        char key[11];
        fnv_key('b', filename, key);
        prefs.putUInt(key, (uint32_t)offset);
        bookmark_touch(prefs, filename);
    }
}

bool ui_reader_bookmark_get(const char *filename, size_t *offset)
{
    if (!filename || !filename[0]) return false;
    char key[11];
    fnv_key('b', filename, key);
    NvsPrefs nvs(true);
    Preferences &prefs = nvs.p;
    bool present = prefs.isKey(key);
    uint32_t off = prefs.getUInt(key, 0);
    if (!present) return false;
    if (offset) *offset = off;
    return true;
}

// Per-book page index persistence (§3.5 companion API).
//
// The page index is stored under "p_xxxxxxxx" — the same FNV-1a hash of
// the filename as the byte-offset key ("b_xxxxxxxx"), but with a different
// prefix. Both keys fit within NVS's 15-character key limit.
//
// Usage pattern (from ui_deckpro.cpp):
//   Save:    ui_reader_bookmark_page_set(filename, current_page_index);
//   Restore: ui_reader_bookmark_page_get(filename, &saved_page_index);
//            then seed reader_page_offsets[0] = saved_byte_offset and start
//            rendering from saved_page_index instead of walking from 0.

void ui_reader_bookmark_page_set(const char *filename, int page_index)
{
    if (!filename || !filename[0]) return;
    char key[11];
    fnv_key('p', filename, key);
    NvsPrefs nvs(false);
    Preferences &prefs = nvs.p;
    prefs.putInt(key, page_index);
    bookmark_touch(prefs, filename);
}

// Returns true and fills *page_index_out if a page bookmark was previously
// saved for filename. Returns false if no page bookmark exists (e.g. the
// book was bookmarked by an older firmware that didn't store the page index,
// or has never been opened).
bool ui_reader_bookmark_page_get(const char *filename, int *page_index_out)
{
    if (!filename || !filename[0]) return false;
    char key[11];
    fnv_key('p', filename, key);
    NvsPrefs nvs(true);
    Preferences &prefs = nvs.p;
    bool present = prefs.isKey(key);
    int  page    = prefs.getInt(key, 0);
    if (!present) return false;
    if (page_index_out) *page_index_out = page;
    return true;
}

// Per-book geometry cookie persistence (companion to the page-index key).
//
// The cookie captures every setting that affects page wrapping (font face,
// font size, rotation, bar visibility, line spacing). Stored under
// "c_xxxxxxxx" — the same FNV-1a hash used by the "b_"/"p_" keys — so the
// O(1) resume fast path still validates after a reboot.
void ui_reader_geo_cookie_set(const char *filename, uint32_t cookie)
{
    if (!filename || !filename[0]) return;
    char key[11];
    fnv_key('c', filename, key);
    NvsPrefs nvs(false);
    Preferences &prefs = nvs.p;
    prefs.putUInt(key, cookie);
}

bool ui_reader_geo_cookie_get(const char *filename, uint32_t *cookie)
{
    if (!filename || !filename[0]) return false;
    char key[11];
    fnv_key('c', filename, key);
    NvsPrefs nvs(true);
    Preferences &prefs = nvs.p;
    bool     present = prefs.isKey(key);
    uint32_t c       = prefs.getUInt(key, 0);
    if (!present) return false;
    if (cookie) *cookie = c;
    return true;
}

// ---------------------------------------------------------------------------
// Debounced NVS settings persistence (§3.9)
//
// ui_settings_save() is the immediate / forced form — it writes every
// persisted key and clears the pending timer.  Setters inside this file call
// settings_mark_dirty() instead, which arms a one-shot LVGL timer that flushes
// a second later; rapid toggles therefore produce a single NVS round-trip.
//
// Paths that power the device down (ui_shutdown_on) call ui_settings_save()
// directly so the most recent state is never lost.
// ---------------------------------------------------------------------------
static bool        s_settings_dirty = false;
static lv_timer_t *s_settings_timer = NULL;

// Every scalar setting lives in one table so save/load stay in lockstep: a new
// preference is one row, not a put + a get in two places.
enum { ST_BOOL, ST_INT, ST_FLOAT };
typedef struct { const char *key; uint8_t type; void *var; float dflt; } setting_def_t;
static const setting_def_t k_settings[] = {
    {"kplight", ST_BOOL, &default_keypad_light, 0}, {"redled", ST_BOOL, &default_red_led_status, 0},
    {"motor", ST_BOOL, &default_motor_status, 0},   {"gps", ST_BOOL, &default_gps_status, 1},
    {"lora", ST_BOOL, &default_lora_status, 1},
#ifdef BOARD_HAS_BHI260AP
    {"gyro", ST_BOOL, &default_gyro_status, 1},
#endif
    // Off by default; an idle registered modem draws 10-20 mA.
    {"a7682", ST_BOOL, &default_a7682_status, 0},
    {"rd_rot", ST_INT, &default_reader_rotation, 0}, {"rd_lsp", ST_INT, &default_reader_line_space, 4},
    {"tb_batt", ST_BOOL, &default_topbar_show_battery, 1}, {"rd_bars", ST_BOOL, &default_reader_bars_hidden, 0},
    {"lk_rot", ST_INT, &default_lock_landscape, 0}, {"wifi_en", ST_BOOL, &default_wifi_enabled, 0},
    {"lr_freq", ST_FLOAT, &lora_default_freq, 850}, {"lr_bw", ST_INT, &lora_default_band, 125},
    {"lr_pwr", ST_INT, &lora_default_power, 22},
};

static void settings_deferred_flush(lv_timer_t * /*t*/)
{
    s_settings_timer = NULL; // one-shot; LVGL won't reschedule it
    if (s_settings_dirty) {
        ui_settings_save(); // clears s_settings_dirty
    }
}

// Mark settings as changed and (re)arm the debounce timer. Must be called
// from the LVGL task only (same thread as lv_timer_create).
static void settings_mark_dirty(void)
{
    s_settings_dirty = true;
    if (s_settings_timer == NULL) {
        s_settings_timer = lv_timer_create(settings_deferred_flush, 1000, NULL);
        if (s_settings_timer) lv_timer_set_repeat_count(s_settings_timer, 1);
    } else {
        // Extend the deadline from this moment so rapid changes collapse.
        lv_timer_reset(s_settings_timer);
    }
}

// True while a debounced NVS write is still queued.
bool ui_settings_pending(void)
{
    return s_settings_dirty;
}

void ui_settings_save(void)
{
    // Cancel any pending deferred write — we are doing the flush right now.
    if (s_settings_timer != NULL) {
        lv_timer_del(s_settings_timer);
        s_settings_timer = NULL;
    }
    s_settings_dirty = false;

    NvsPrefs nvs(false);
    Preferences &prefs = nvs.p;
    for (size_t i = 0; i < sizeof(k_settings) / sizeof(k_settings[0]); i++) {
        const setting_def_t *s = &k_settings[i];
        switch (s->type) {
        case ST_BOOL:  prefs.putBool(s->key, *(bool *)s->var); break;
        case ST_INT:   prefs.putInt(s->key, *(int *)s->var); break;
        default:       prefs.putFloat(s->key, *(float *)s->var); break;
        }
    }
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
    prefs.putString("wifi_ssid", default_wifi_ssid);
    prefs.putString("wifi_pass", default_wifi_password);
    prefs.putString("wifi_tz",   default_wifi_tz);
}

void ui_settings_load(void)
{
    NvsPrefs nvs(true);
    Preferences &prefs = nvs.p;
    for (size_t i = 0; i < sizeof(k_settings) / sizeof(k_settings[0]); i++) {
        const setting_def_t *s = &k_settings[i];
        switch (s->type) {
        case ST_BOOL:  *(bool *)s->var  = prefs.getBool(s->key, s->dflt != 0); break;
        case ST_INT:   *(int *)s->var   = prefs.getInt(s->key, (int)s->dflt); break;
        default:       *(float *)s->var = prefs.getFloat(s->key, s->dflt); break;
        }
    }
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
    default_reader_line_space = reader_line_space_clamp(default_reader_line_space);
    default_lock_landscape    = default_lock_landscape ? 1 : 0;

    String s = prefs.getString("wifi_ssid", "");
    copy_str(default_wifi_ssid, sizeof(default_wifi_ssid), s.c_str());

    s = prefs.getString("wifi_pass", "");
    copy_str(default_wifi_password, sizeof(default_wifi_password), s.c_str());

    // Default to America/Sao_Paulo (UTC-3, no DST since 2019) — this matches
    // the TZ the GPS time-sync and NTP-sync paths hardcode elsewhere. Leaving
    // this at "UTC0" caused the topbar to render UTC after every reboot
    // because the GPS-sync setenv only lives in the env for that session and
    // is never persisted. Also overwrite a stale "UTC0" left over from devices
    // that booted on the old default — otherwise NVS pins them to UTC forever.
    s = prefs.getString("wifi_tz", UI_DEFAULT_TZ);
    if (s.length() == 0 || s == "UTC0") s = UI_DEFAULT_TZ;
    copy_str(default_wifi_tz, sizeof(default_wifi_tz), s.c_str());

    // Seed the live state from the persisted intent so ui_wifi_get_enabled()
    // returns the right value before factory_setup() actually brings WiFi up.
    wifi_enabled = default_wifi_enabled;

    // Apply TZ now so any later localtime() call (before NTP completes) at
    // least uses the correct offset once the clock is set.
    setenv("TZ", default_wifi_tz, 1);
    tzset();
}
// ----

void ui_disp_full_refr(void)
{
    disp_transition_refr();
    lv_obj_invalidate(lv_scr_act());
}

// Always a full (flashing) refresh — for ghost clean-up points. The plain
// ui_disp_full_refr() is the transition refresh: fast partial update, with a
// full one only every Nth screen change.
void ui_disp_force_full_refr(void)
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

float ui_lora_get_freq(void) { return lora_default_freq; }
void ui_lora_set_freq(float freq) { lora_default_freq = freq; settings_mark_dirty(); }
int ui_lora_get_bandwidth(void) { return lora_default_band; }
void ui_lora_set_bandwidth(int bd) { lora_default_band = bd; settings_mark_dirty(); }
int ui_lora_get_power(void) { return lora_default_power; }
void ui_lora_set_power(int po) { lora_default_power = po; settings_mark_dirty(); }

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
void ui_lora_sleep(void) { lora_sleep(); }
//************************************[ screen 2 ]****************************************** setting
// set function
void ui_setting_set_keypad_light(bool on)
{
    digitalWrite(BOARD_KEYBOARD_LED, on);
    default_keypad_light = on;
    settings_mark_dirty();
}
void ui_setting_apply_keypad_light(bool on)
{
    digitalWrite(BOARD_KEYBOARD_LED, on);
}
void ui_setting_set_red_led(bool on)
{
    digitalWrite(BOARD_RED_LED, on);
    default_red_led_status = on;
    settings_mark_dirty();
}
void ui_setting_set_motor_status(bool on)
{
    digitalWrite(BOARD_MOTOR_PIN, on);
    default_motor_status = on;
    settings_mark_dirty();
}
// Lazy GPS bring-up. gps_init() walks several baud rates with delays, so it
// must not run on the LVGL task that called ui_setting_set_gps_status().
volatile bool gps_init_running = false;

static void gps_lazy_init_task(void *)
{
    peri_init_st[E_PERI_GPS] = gps_init();
    if (peri_init_st[E_PERI_GPS]) {
        if (default_gps_status) {
            gps_task_resume();
            // GPS shares GPIO 1 with BOARD_RED_LED; re-assert the saved LED state (same as boot path).
            pinMode(BOARD_RED_LED, OUTPUT);
            digitalWrite(BOARD_RED_LED, default_red_led_status);
        } else {
            // User switched GPS off while we were scanning: keep it down.
            gps_task_suspend();
            digitalWrite(BOARD_GPS_EN, LOW);
        }
    }
    gps_init_running = false;
    vTaskDelete(NULL);
}

void ui_setting_set_gps_status(bool on)
{
    if (on) {
        digitalWrite(BOARD_GPS_EN, HIGH);
        if (!peri_init_st[E_PERI_GPS]) {
            // GPS was off at boot, so no gps_task exists yet and
            // gps_task_resume() would be a no-op: init the module first.
            if (!gps_init_running) {
                gps_init_running = true;
                xTaskCreate(gps_lazy_init_task, "gps_init", 4096, NULL, 1, NULL);
            }
        } else {
            gps_task_resume();
        }
    } else {
        gps_task_suspend();
        digitalWrite(BOARD_GPS_EN, LOW);
    }
    default_gps_status = on;
    settings_mark_dirty();
}
// Lazy LoRa bring-up. lora_init() talks to the radio over SPI and must not
// run on the LVGL task that called ui_setting_set_lora_status().
static volatile bool s_lora_init_running = false;

static void lora_lazy_init_task(void *)
{
    delay(10);
    peri_init_st[E_PERI_LORA] = lora_init();
    s_lora_init_running = false;
    vTaskDelete(NULL);
}

void ui_setting_set_lora_status(bool on)
{
    if (on) {
        digitalWrite(BOARD_LORA_EN, HIGH);
        if (!peri_init_st[E_PERI_LORA] && !s_lora_init_running) {
            // lora_init() blocks on SPI + RadioLib, so run it off the LVGL
            // task; a second toggle while it is in flight is a no-op.
            s_lora_init_running = true;
            xTaskCreate(lora_lazy_init_task, "lora_init", 4096, NULL, 1, NULL);
        }
    } else {
        lora_power_off();
        digitalWrite(BOARD_LORA_EN, LOW);
        // rail cut loses the radio config; re-init on next power-on
        peri_init_st[E_PERI_LORA] = false;
    }
    default_lora_status = on;
    settings_mark_dirty();
}
#ifdef BOARD_HAS_BHI260AP
void ui_setting_set_gyro_status(bool on)
{
    // VDD1V8 also powers the CST328 touch controller, so we must not cut
    // it from the gyro toggle. Track the preference; the gyro driver itself
    // can honor it at the software level.
    default_gyro_status = on;
    settings_mark_dirty();
}
#endif
// Lazy A7682E bring-up. The AT probe blocks for up to ~7 s, so it must not
// run on the LVGL task that called ui_setting_set_a7682_status().
static volatile bool s_a7682_probe_running = false;

static void a7682_lazy_probe_task(void *)
{
    bool ok = a7682_probe();
    // The user may have cut the rail while the probe was running; a stale
    // "ok" would mark an unpowered modem as present.
    peri_init_st[E_PERI_A7682E] = ok && default_a7682_status;
    s_a7682_probe_running = false;
    vTaskDelete(NULL);
}

void ui_setting_set_a7682_status(bool on)
{
    if (on) {
        // a7682_probe() reads ui_setting_get_a7682_status() to decide whether
        // the rail should be up, so publish the new intent before the probe
        // task can start.
        default_a7682_status = on;
        digitalWrite(BOARD_6609_EN, HIGH);
        digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
        if (!peri_init_st[E_PERI_A7682E] && !s_a7682_probe_running) {
            s_a7682_probe_running = true;
            xTaskCreate(a7682_lazy_probe_task, "a7682_probe", 4096, NULL, 1, NULL);
        }
    } else {
        ui_a7682_loop_suspend();
        digitalWrite(BOARD_6609_EN, LOW);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
        peri_init_st[E_PERI_A7682E] = false; // rail cut, so the next power-on must probe again
    }
    default_a7682_status = on;
    settings_mark_dirty();
}

// get function
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
#ifdef BOARD_HAS_BHI260AP
bool ui_setting_get_gyro_status(void)
{
    return default_gyro_status;
}
#endif
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

// SD card initialised and usable (also gates the USB mass-storage screens).
static bool ui_test_sd_card(void)
{
    return peri_init_st[E_PERI_SD];
}

void ui_setting_get_sd_capacity(uint64_t *total, uint64_t *used)
{
    if (!sd_fs_ready()) return;

    uint64_t totalBytes = 0;
    uint64_t usedBytes  = 0;
    {
        SpiGuard spi;
        totalBytes = SD.totalBytes();
        usedBytes  = SD.usedBytes();
    }

    if (total) *total = totalBytes / (1024 * 1024);
    if (used)  *used  = usedBytes / (1024 * 1024);

    printf("[SD] total=%lluMB used=%lluMB\n",
           (unsigned long long)(totalBytes / (1024 * 1024)),
           (unsigned long long)(usedBytes / (1024 * 1024)));
}

//************************************[ screen 3 ]****************************************** GPS
void ui_gps_task_resume(void)
{
    // Resuming re-powers the module; respect the GPS on/off setting.
    if (default_gps_status) gps_task_resume();
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
void ui_gps_set_continuous(bool on)
{
    gps_set_continuous(on);
}
//************************************[ screen 4 ]****************************************** Wifi Scan
static int is_chinese_utf8(const char *str) {
    unsigned char c = (unsigned char)str[0];
    return (c >= 0xE0 && c <= 0xEF);  // 检查第一个字节是否在 UTF-8 的中文字符范围内
}

// Copy the results of an already-completed WiFi.scanNetworks() pass into the
// caller's list. This is the only collector now — the poll-style API below
// owns every scan.
static int wifi_scan_collect(int n, ui_wifi_scan_info_t *list, int list_len)
{
    memset(list, 0, (sizeof(*list) * list_len));
    if (n < 0) n = 0;
    if (n > list_len) n = list_len;

    int dst = 0;
    for (int i = 0; i < n && dst < list_len; i++) {
        String s = WiFi.SSID(i);
        const char *str = s.c_str();
        if (!str) continue;
        if (str[0] == '\0') {
            // Hidden SSID — surface it as "<hidden>" so the user can still
            // pick it (they'll need the SSID to actually connect though).
            copy_str(list[dst].name, sizeof(list[dst].name), "<hidden>");
        } else if (is_chinese_utf8(str)) {
            // Skip CJK SSIDs — our font has no glyphs for them.
            Serial.printf("[wifi] skipping non-latin SSID '%s'\n", str);
            continue;
        } else {
            copy_str(list[dst].name, sizeof(list[dst].name), str);
        }
        list[dst].rssi = WiFi.RSSI(i);
        list[dst].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        dst++;
    }
    WiFi.scanDelete();
    return dst;
}

void ui_wifi_scan_start_async(void)
{
    WiFi.mode(WIFI_STA);
    if (WiFi.status() == WL_CONNECTED || WiFi.status() == WL_IDLE_STATUS) {
        WiFi.disconnect(false, false);
    }
    int prev = WiFi.scanComplete();
    if (prev == WIFI_SCAN_RUNNING) {
        // Don't kick off a second one — wifi_scan_poll will pick up the
        // existing scan when it finishes.
        return;
    }
    if (prev >= 0) {
        WiFi.scanDelete();
    }
    WiFi.scanNetworks(/*async*/ true, /*show_hidden*/ true);
}

int ui_wifi_scan_poll(ui_wifi_scan_info_t *list, int list_len)
{
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return -1;
    if (n == WIFI_SCAN_FAILED || n < 0) {
        WiFi.scanDelete();
        return -2;
    }
    return wifi_scan_collect(n, list, list_len);
}

void ui_wifi_scan_cancel(void)
{
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        // ESP32 Arduino has no "abort scan" call; the cheap way to drop the
        // running scan is to flip the radio mode, which the driver
        // implements by tearing down the WiFi task. The deletion call
        // afterwards frees any partial result buffer.
        WiFi.disconnect(true, true);
    }
    WiFi.scanDelete();
}

void ui_wifi_set_ssid(const char *ssid)
{
    if (!ssid) return;
    copy_str(default_wifi_ssid, sizeof(default_wifi_ssid), ssid);
    settings_mark_dirty();
}

void ui_wifi_set_password(const char *password)
{
    if (!password) return;
    copy_str(default_wifi_password, sizeof(default_wifi_password), password);
    settings_mark_dirty();
}

void ui_wifi_set_tz(const char *tz)
{
    if (!tz) return;
    copy_str(default_wifi_tz, sizeof(default_wifi_tz), tz);
    setenv("TZ", default_wifi_tz, 1);
    tzset();
    settings_mark_dirty();
}

int ui_wifi_get_status(void)
{
    return (int)wifi_status;
}

void ui_wifi_get_ip(char *out, int out_len)
{
    if (!out || out_len <= 0) return;
    if (WiFi.status() != WL_CONNECTED) {
        out[0] = '\0';
        return;
    }
    copy_str(out, out_len, WiFi.localIP().toString().c_str());
}

bool ui_time_is_synced(void)
{
    if (!ntp_synced && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ntp_synced = true;
    }
    return ntp_synced;
}

bool ui_time_get_local(struct tm *out)
{
    if (!out) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    if ((out->tm_year + 1900) >= 2024) {
        clock_valid = true;
    }
    return clock_valid;
}

void ui_ntp_resync(void)
{
    if (WiFi.status() != WL_CONNECTED) return;
    setenv("TZ", default_wifi_tz, 1);
    tzset();
    // Arms a fresh SNTP exchange: ui_time_is_synced() must not keep reporting
    // the previous answer while this one is still in flight.
    ntp_synced = false;
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    // configTzTime applies the TZ string and starts SNTP polling. Multiple
    // servers give us a fallback if pool.ntp.org is unreachable.
    configTzTime(default_wifi_tz, "pool.ntp.org", "time.nist.gov", "time.google.com");
}

void ui_time_persist_save(void)
{
    time_t now = time(NULL);
    // Only stamp once the clock holds a plausible year — otherwise we'd
    // overwrite a previously-saved good time with a fresh-boot 1970 epoch.
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if ((tm_now.tm_year + 1900) < 2024) return;

    NvsPrefs nvs(false);
    Preferences &prefs = nvs.p;
    prefs.putULong("tlast", (uint32_t)now);
}

// Compile-time epoch derived from __DATE__/__TIME__. Acts as a floor for the
// system clock on a virgin device that has never seen GPS or NTP: real-world
// time is always >= compile time, so even an uncorrected clock seeded here is
// a better starting point than the 1970 epoch (the topbar gate requires year
// >= 2024, so 1970 means "--:--" forever).
//
// __DATE__/__TIME__ give the build host's LOCAL time. We assume the build
// host runs in the same TZ as the device (true for the typical dev setup
// here) and let mktime() interpret the struct in the current TZ — which
// ui_settings_load() has already pinned to the target TZ before this runs.
// Forcing TZ=UTC0 for the mktime call (the previous approach) made the saved
// epoch off by the build-host TZ offset, which the topbar then rendered as a
// constant-offset error.
static time_t compile_time_epoch(void)
{
    const char *d = __DATE__; // "Mmm dd yyyy"
    const char *t = __TIME__; // "HH:MM:SS"
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = { d[0], d[1], d[2], 0 };
    const char *p = strstr(months, m);
    int month = p ? (int)((p - months) / 3) : 0;

    struct tm tm = {};
    tm.tm_year = atoi(d + 7) - 1900;
    tm.tm_mon  = month;
    tm.tm_mday = atoi(d + 4);
    tm.tm_hour = atoi(t);
    tm.tm_min  = atoi(t + 3);
    tm.tm_sec  = atoi(t + 6);
    tm.tm_isdst = -1; // let mktime resolve DST under the current TZ

    return mktime(&tm);
}

void ui_time_persist_restore(void)
{
    uint32_t saved = 0;
    {
        NvsPrefs nvs(true);
        Preferences &prefs = nvs.p;
        saved = prefs.getULong("tlast", 0);
    }

    time_t floor_t = compile_time_epoch();
    // Pick the more recent of (NVS stamp) and (compile time). Compile time
    // covers the first-ever boot before anything has been persisted; NVS wins
    // on every subsequent boot. Either way we never regress the clock.
    time_t target = ((time_t)saved > floor_t) ? (time_t)saved : floor_t;
    if (target <= 0) return;

    struct timeval tv = { .tv_sec = target, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return;
    // The clock now holds a plausible date, so the topbar can paint
    // immediately instead of waiting for the first GPS/NTP refresh. This is
    // not an SNTP sync — ntp_synced stays false until SNTP actually answers.
    clock_valid = true;
    Serial.printf("[time] restored: epoch=%u (nvs=%u, compile=%u)\n",
                  (unsigned)target, (unsigned)saved, (unsigned)floor_t);
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
        WiFi.setSleep(WIFI_PS_MAX_MODEM);  // WiFi only serves NTP/ping: doze between beacons at the listen interval instead of waking for every DTIM
        ui_ntp_resync();
        Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
    } else {
        wifi_status = UI_WIFI_STATUS_FAILED;
        Serial.printf("[wifi] connect failed (ssid=%s)\n", default_wifi_ssid);
    }

    vTaskDelete(NULL);
}

static bool ui_wifi_connect(void)
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

// Bring the radio up or down without touching the persisted user intent.
// Used by the lock screen, which drops the radio while locked and restores it
// on unlock: the user's saved on/off choice must survive that.
void ui_wifi_radio_set(bool en)
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

void ui_wifi_set_enabled(bool en)
{
    ui_wifi_radio_set(en);
    // Remember the user's on/off intent so it survives reboot. Only the
    // explicit toggle path lands here — a transient disconnect from
    // wifi_connect_task or the lock screen must not flip the saved intent.
    if (default_wifi_enabled != en) {
        default_wifi_enabled = en;
        settings_mark_dirty();
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

//************************************[ screen 6 ]****************************************** Battery

// BQ25896 / BQ27220 helpers.
//
// Every helper below is bus-guarded and init-gated: they are called from LVGL
// UI tasks (battery indicator, low-voltage poller, ...) which race with the
// keypad drain task — I2C0Guard serialises each transaction so a preemption
// mid-call can't corrupt TwoWire's txBuffer.
//
// The init gate matters too: when bq27220.init() fails (e.g. Unseal failed) or
// the PMU is absent, the chip won't reply and every ungated register read
// would generate a ~1 s Wire timeout and a Wire.cpp:499 Error 263. Gated
// getters short-circuit to a safe default so the LVGL task stops stalling.
template <class T, class F> static T bq25896_read(T dflt, F f)
{
    if (!peri_init_st[E_PERI_BQ25896]) return dflt;
    I2C0Guard g;
    return f();
}
template <class T, class F> static T bq27220_read(T dflt, F f)
{
    if (!peri_init_st[E_PERI_BQ27220]) return dflt;
    I2C0Guard g;
    return f();
}

// BQ25896
bool ui_battery_25896_is_vbus_in(void)
{
    return bq25896_read(false, [] { return PPM.isVbusIn(); });
}

bool ui_batt_25896_is_chg(void)
{
    return bq25896_read(false, [] { return PPM.isCharging(); });
}

// One conversion at init; continuous mode (1 s conversions) only while the
// BQ25896 screen is open.
void ui_batt_25896_set_adc_continuous(bool on)
{
    bq25896_read(false, [on] { return PPM.enableMeasure(on ? XPowersPPM::CONTINUOUS : XPowersPPM::ONE_SHORT); });
}
float ui_batt_25896_get_vbus(void)
{
    return bq25896_read(0.0f, [] { return PPM.getVbusVoltage() * 1.0 / 1000.0; });
}
float ui_batt_25896_get_vsys(void)
{
    return bq25896_read(0.0f, [] { return PPM.getSystemVoltage() * 1.0 / 1000.0; });
}
float ui_batt_25896_get_vbat(void)
{
    return bq25896_read(0.0f, [] { return PPM.getBattVoltage() * 1.0 / 1000.0; });
}
float ui_batt_25896_get_volt_targ(void)
{
    return bq25896_read(0.0f, [] { return PPM.getChargeTargetVoltage() * 1.0 / 1000.0; });
}
float ui_batt_25896_get_chg_curr(void)
{
    return bq25896_read(0.0f, [] { return (float)PPM.getChargeCurrent(); });
}
float ui_batt_25896_get_pre_curr(void)
{
    return bq25896_read(0.0f, [] { return (float)PPM.getPrechargeCurr(); });
}
const char * ui_batt_25896_get_chg_st(void)
{
    return bq25896_read("N/A", [] { return PPM.getChargeStatusString(); });
}
const char * ui_batt_25896_get_vbus_st(void)
{
    return bq25896_read("N/A", [] { return PPM.getBusStatusString(); });
}
const char * ui_batt_25896_get_ntc_st(void)
{
    return bq25896_read("N/A", [] { return PPM.getNTCStatusString(); });
}
/* 27220 */
bool ui_battery_27220_is_vaild(void) {return peri_init_st[E_PERI_BQ27220]; }
bool ui_battery_is_external_power_present(void)
{
    if (peri_init_st[E_PERI_BQ25896]) {
        I2C0Guard g;
        bool v = PPM.isVbusIn();
        return v;
    }
    if (peri_init_st[E_PERI_BQ27220]) {
        I2C0Guard g;
        bool v = bq27220.getAverageCurrent() > 0;
        return v;
    }
    return false;
}
bool ui_battery_27220_get_charge_finish(void) {
    return bq27220_read(false, [] { return bq27220.getCharingFinish(); });
}
uint16_t ui_battery_27220_get_status(void)
{
    if (!peri_init_st[E_PERI_BQ27220]) return 0;
    BQ27220BatteryStatus batt;
    I2C0Guard g;
    bq27220.getBatteryStatus(&batt);
    return batt.full;
}
uint16_t ui_battery_27220_get_voltage(void) {
    return bq27220_read((uint16_t)0, [] { return bq27220.getVoltage(); });
}
int16_t ui_battery_27220_get_current(void) {
    return bq27220_read((int16_t)0, [] { return bq27220.getCurrent(); });
}
uint16_t ui_battery_27220_get_temperature(void) {
    return bq27220_read((uint16_t)0, [] { return bq27220.getTemperature(); });
}
uint16_t ui_battery_27220_get_full_capacity(void) {
    return bq27220_read((uint16_t)0, [] { return bq27220.getFullChargeCapacity(); });
}
uint16_t ui_battery_27220_get_remain_capacity(void) {
    return bq27220_read((uint16_t)0, [] { return bq27220.getRemainingCapacity(); });
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
    I2C0Guard g;
    battery_percent_cache = bq27220.getStateOfCharge();
    battery_percent_cache_ms = now;
    return battery_percent_cache;
}
uint16_t ui_battery_27220_get_health(void) {
    return bq27220_read((uint16_t)0, [] { return bq27220.getStateOfHealth(); });
}
bool ui_battery_27220_is_low_alarm(void)
{
    if (!peri_init_st[E_PERI_BQ27220]) {
        return false;
    }

    BQ27220BatteryStatus batt = {0};
    BQ27220GaugingStatus gauging = {0};

    I2C0Guard g;
    bq27220.getBatteryStatus(&batt);
    bq27220.getGaugingStatus(&gauging);

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
//************************************[ screen 7 ]****************************************** Input
int ui_input_get_keypad_val(char *v)
{
    return keypad_get_val(v);
}

void ui_input_set_keypad_flag(void)
{
    keypad_set_flag();
}

// Milliseconds since the keypad last produced a key (grows past start-up).
uint32_t ui_input_idle_ms(void)
{
    return millis() - keypad_last_activity_ms();
}

#ifdef BOARD_HAS_BHI260AP
int ui_other_get_gyro(float *gyro_x, float *gyro_y, float *gyro_z)
{
    if((gyro_x != NULL) && (gyro_y != NULL) && (gyro_z != NULL))
    {
        BHI260AP_get_val(2, gyro_x, gyro_y, gyro_z);
    }
    else
    {
        Serial.printf("[%d] %s : Argument cannot be empty", __LINE__, __FILE__);
    }
    return 1;
}
#endif

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
    a7682_pump_resume();
}

void ui_a7682_loop_suspend(void)
{
    a7682_pump_suspend();
}

//************************************[ screen 9 ]****************************************** Shutdown

void ui_shutdown_on(void)
{
    // Stamp the clock before power drops: the next cold boot has no
    // battery-backed RTC and restores whatever is in NVS.
    ui_time_persist_save();
    // Flush any pending debounced settings write before we cut power.
    // The deferred timer will never fire once the device is off.
    ui_settings_save();
    ink_screen_prepare_shutdown();
    { if (peri_init_st[E_PERI_BQ25896]) { I2C0Guard g; PPM.shutdown(); } }
    Serial.println("Shutdown .....");
}

//************************************[ screen 10 ]****************************************** PCM5102
#ifdef BOARD_HAS_PCM5102A
bool ui_pcm5102_cb(const char *at_cmd)
{
    audio.connecttoFS(SPIFFS, "/iphone_call.mp3");
    return true;
}

void ui_pcm5102_stop(void)
{
    audio.stopSong();
}

// optional Audio library event callback — only present when the audio
// stack is compiled in, since the Audio object that calls it won't exist
// on the 4G / no-DAC variant.
void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
#endif /* BOARD_HAS_PCM5102A */

//************************************[ screen 12 ]****************************************** Notes
#include <SPIFFS.h>

#if CONFIG_TINYUSB_MSC_ENABLED
static USBMSC msc;

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    uint32_t count = bufsize / 512;
    SpiGuard spi;
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.readRAW((uint8_t*)buffer + (i * 512), lba + i)) {
            return -1;
        }
    }
    return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    uint32_t count = bufsize / 512;
    SpiGuard spi;
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.writeRAW(buffer + (i * 512), lba + i)) {
            return -1;
        }
    }
    return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
    return true;
}

void ui_usb_msc_begin(void) {
    if (msc_active) return;
    
    if (!ui_test_sd_card()) return;

    // From here on the host owns the raw sectors; FatFs is fenced off by
    // sd_fs_ready() and remounted fresh in ui_usb_msc_end(). Drop the only
    // long-lived File handles (the reader's and the dictionary's) before
    // handing the card over.
    ui_reader_close_view();
    ui_dict_close();

    uint32_t sectorCount = 0;
    {
        SpiGuard spi;
        sectorCount = SD.cardSize() / 512;
    }

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

    // The host may have rewritten the FAT; drop FatFs's cached window/FSINFO
    // and mount again.
    {
        SpiGuard spi;
        SD.end();
    }
    peri_init_st[E_PERI_SD] = sd_care_init();
}
#else
void ui_usb_msc_begin(void) {}
void ui_usb_msc_end(void) {}
#endif

// Strip non-ASCII / control bytes (the on-screen keypad sends '\n' on Enter)
// and trailing spaces. Shared by the notes filename cleanup and the
// dictionary lookup, which both read raw textarea content.
size_t ui_ascii_trim(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0 || !src) return 0;
    size_t out = 0;
    for (size_t i = 0; src[i] && out < n - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) dst[out++] = (char)c;
    }
    while (out > 0 && dst[out - 1] == ' ') out--;
    dst[out] = '\0';
    return out;
}

bool ui_notes_write(const char *filename, const char *content)
{
    if (!sd_fs_ready()) return false;

    // Strip leading/trailing whitespace and any embedded control chars from
    // the filename — the on-screen keypad sends '\n' on Enter, which would
    // otherwise leak into the path and make SD.open() fail.
    char clean_name[32];
    size_t out = ui_ascii_trim(clean_name, sizeof(clean_name), filename);
    if (out == 0) return false;

    char path[64];
    snprintf(path, sizeof(path), "/notes/%s", clean_name);

    SpiGuard spi;

    if (!SD.exists("/notes")) {
        SD.mkdir("/notes");
    }

    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("ui_notes_write: open(%s) failed\n", path);
        return false;
    }

    size_t content_len = strlen(content);
    size_t written = content_len ? file.print(content) : 0;
    file.close();

    // Empty content writes a zero-byte file, which is a valid save.
    return written == content_len;
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

// ---------------------------------------------------------------------------
// Buffered reader for the dictionary binary search (§3.4)
//
// Each dict_buf_read_line / dict_buf_skip_line call issues one
// f.read(chunk, 512) per 512-byte window instead of one f.read() per byte,
// dropping ~20,000 single-byte VFS/FATFS/SPI round-trips to ~40 per lookup.
//
// The virtual file position tracked by dict_buf_tell() is byte-for-byte
// identical to what f.position() would return after the equivalent single-byte
// reads, so the binary-search algorithm and its convergence bounds are
// unchanged.
// ---------------------------------------------------------------------------
#define DICT_CHUNK_SIZE 512

typedef struct {
    File    *f;
    uint8_t  chunk[DICT_CHUNK_SIZE];
    int      chunk_len;   // valid bytes in chunk[]
    int      chunk_pos;   // next byte to consume
    long     chunk_start; // file offset of chunk[0]
} dict_buf_t;

static void dict_buf_init(dict_buf_t *b, File *f)
{
    b->f          = f;
    b->chunk_len  = 0;
    b->chunk_pos  = 0;
    b->chunk_start = 0;
}

// Advance the chunk window by reading the next DICT_CHUNK_SIZE bytes from
// the underlying file. Must only be called when the current chunk is fully
// consumed (chunk_pos >= chunk_len).
static bool dict_buf_refill(dict_buf_t *b)
{
    b->chunk_start += b->chunk_len; // previous chunk fully consumed
    b->chunk_pos    = 0;
    b->chunk_len    = (int)b->f->read(b->chunk, DICT_CHUNK_SIZE);
    return b->chunk_len > 0;
}

// Virtual file position: equals f.position() of the equivalent unbuffered
// reader at every point the binary-search algorithm samples it.
static long dict_buf_tell(const dict_buf_t *b)
{
    return b->chunk_start + b->chunk_pos;
}

// Seek the underlying file and reset the buffer. After this call
// dict_buf_tell(b) == pos.
static void dict_buf_seek(dict_buf_t *b, long pos)
{
    b->f->seek((uint32_t)pos);
    b->chunk_start = pos;
    b->chunk_pos   = 0;
    b->chunk_len   = 0;
}

// Consume bytes up to and including the next '\n' (or EOF). Used to discard
// the partial line at a binary-search seek point.
static void dict_buf_skip_line(dict_buf_t *b)
{
    while (true) {
        if (b->chunk_pos >= b->chunk_len) {
            if (!dict_buf_refill(b)) return; // EOF
        }
        uint8_t c = b->chunk[b->chunk_pos++];
        if (c == '\n') return;
    }
}

// Read one line into buf (up to buf_size-1 bytes), null-terminated.
// '\r' is stripped (handles CRLF files transparently). Lines longer than
// buf_size-1 are truncated; the reader still consumes through to '\n'.
// Returns bytes written (excluding '\0'), or -1 on EOF before any data.
static int dict_buf_read_line(dict_buf_t *b, char *buf, size_t buf_size)
{
    size_t n   = 0;
    bool   any = false;
    while (true) {
        if (b->chunk_pos >= b->chunk_len) {
            if (!dict_buf_refill(b)) break; // EOF
        }
        uint8_t c = b->chunk[b->chunk_pos++];
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue; // strip CR so CRLF files work
        if (n + 1 < buf_size) buf[n++] = (char)c;
        // even when buf is full keep consuming until '\n' so the file
        // position stays consistent for the binary-search accounting
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

static uint32_t dict_le32(const uint8_t *p)
{
    // Index records are little-endian; read explicitly rather than aliasing a
    // uint32_t so the layout stays defined even if the struct changes.
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Layout of one /dict/eng-pob.idx record: u32 LE line offset followed by a
// 28-byte NUL-padded key (see tools/build_dict_index.py).
#define DICT_IDX_KEY_LEN 28

// Compare a 28-byte index key (NUL-padded) to a NUL-terminated query:
// unsigned byte-wise, shorter prefix is less.
static int dict_key_cmp(const uint8_t *key, const char *query)
{
    for (int i = 0; i < DICT_IDX_KEY_LEN; i++) {
        int ca = key[i];
        int cb = (unsigned char)query[i];
        if (ca != cb) return ca - cb;  // key's NUL pad sorts below any byte
        if (ca == 0) return 0;
    }
    return 0;
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

// ---------------------------------------------------------------------------
// Long-lived dictionary handle
//
// The 108 MB TSV used to be opened and closed around every lookup, and the
// bisection seeked ~18 times inside it. Both are FAT/SPI round-trips. The
// dictionary screen now keeps the file open while it is on screen, plus the
// sparse sidecar index (/dict/eng-pob.idx, built by tools/build_dict_index.py)
// in PSRAM, so a lookup is a handful of block reads.
//
// Callers that are not the dictionary screen still work: ui_dict_lookup()
// opens the file itself when nobody has, and closes it again before returning.
// ---------------------------------------------------------------------------
static File     s_dict_file;
static bool     s_dict_open = false;
static uint8_t *s_dict_idx = NULL;      // raw records: u32 LE offset + 28-byte key
static uint32_t s_dict_idx_count = 0;

#define DICT_TSV_PATH  "/dict/eng-pob.tsv"
#define DICT_IDX_PATH  "/dict/eng-pob.idx"
#define DICT_IDX_REC   (4 + DICT_IDX_KEY_LEN)  // u32 offset + key
#define DICT_IDX_KEY   4                       // key offset inside a record

void ui_dict_open(void)
{
    if (s_dict_open) return;
    if (!sd_fs_ready()) return;

    SpiGuard spi;

    s_dict_file = SD.open(DICT_TSV_PATH, FILE_READ);
    if (!s_dict_file) return;
    s_dict_file.setBufferSize(0);  // dict_buf_t already reads 512-B chunks

    s_dict_idx = NULL;
    s_dict_idx_count = 0;

    File idx = SD.open(DICT_IDX_PATH, FILE_READ);
    if (idx) {
        uint8_t hdr[16];
        if (idx.read(hdr, sizeof(hdr)) == sizeof(hdr) && memcmp(hdr, "DIX1", 4) == 0) {
            uint32_t stride  = dict_le32(hdr + 4);  // informational: offsets are absolute
            uint32_t count   = dict_le32(hdr + 8);
            uint32_t tsv_sz  = dict_le32(hdr + 12);
            (void)stride;
            // Only trust the index if it was built from this exact file; a
            // stale index would send the scan to the wrong line entirely.
            if (count > 0 && count <= (0xFFFFFFFFu / DICT_IDX_REC) &&
                tsv_sz == (uint32_t)s_dict_file.size()) {
                uint32_t bytes = count * DICT_IDX_REC;
                uint8_t *recs = (uint8_t *)ps_malloc(bytes);
                if (recs && idx.read(recs, bytes) == bytes) {
                    s_dict_idx = recs;
                    s_dict_idx_count = count;
                } else {
                    free(recs);  // short read or no PSRAM: fall back to bisection
                }
            }
        }
        idx.close();
    }

    s_dict_open = true;
    if (s_dict_idx) Serial.printf("[dict] index: %u records\n", s_dict_idx_count);
    else           Serial.println("[dict] no index, bisection");
}

void ui_dict_close(void)
{
    if (s_dict_file) {
        SpiGuard spi;  // SD shares the bus with LoRa and the e-paper
        s_dict_file.close();
    }
    if (s_dict_idx) {
        free(s_dict_idx);
        s_dict_idx = NULL;
    }
    s_dict_idx_count = 0;
    s_dict_open = false;
}

// Linear scan from the reader's current position. `hi` is the bisection's
// upper bound (inclusive, -1 = unbounded for the indexed path): the bisection
// stops once tell() has passed hi, the indexed path just runs to EOF.
static char* dict_scan(dict_buf_t *buf, const char *word, long hi)
{
    char line[512];
    while (hi < 0 || dict_buf_tell(buf) <= hi) {
        int len = dict_buf_read_line(buf, line, sizeof(line));
        if (len < 0) break;
        int cmp = dict_strcasecmp_word(line, word);
        if (cmp == 0) {
            const char *tab = strchr(line, '\t');
            const char *def = tab ? tab + 1 : "";
            return dict_decode_definition(def);
        }
        if (cmp > 0) break; // file is sorted — we've passed the target
    }
    return NULL;
}

char* ui_dict_lookup(const char *word)
{
    if (!word || !*word) return NULL;
    if (!sd_fs_ready()) return NULL;

    // Out of the dictionary screen the file is normally closed between
    // lookups; open it here and close it before returning.
    bool opened_here = false;
    if (!s_dict_open) {
        ui_dict_open();
        if (!s_dict_open) return NULL;
        opened_here = true;
    }

    char *result = NULL;
    {
        SpiGuard spi;

        long hi = (long)s_dict_file.size();

        // dict_buf provides ~512-byte block reads so the bisection issues
        // ~40 SD transactions instead of ~20,000 single-byte VFS calls.
        dict_buf_t buf;
        dict_buf_init(&buf, &s_dict_file);

        if (s_dict_idx) {
            // Indexed path. The keys were built from the ASCII-lowercased
            // headword truncated to 27 bytes, so truncate the query the same way.
            char q[DICT_IDX_KEY_LEN];
            size_t qn = 0;
            while (word[qn] && qn < sizeof(q) - 1) {
                q[qn] = (char)tolower((unsigned char)word[qn]);
                qn++;
            }
            q[qn] = '\0';

            // First record whose key is >= q; the LAST record strictly less
            // than q is the one before it (or the file start if there is none).
            uint32_t lo_rec = 0, hi_rec = s_dict_idx_count;
            while (lo_rec < hi_rec) {
                uint32_t mid = lo_rec + (hi_rec - lo_rec) / 2;
                if (dict_key_cmp(s_dict_idx + (size_t)mid * DICT_IDX_REC + DICT_IDX_KEY, q) < 0)
                    lo_rec = mid + 1;
                else
                    hi_rec = mid;
            }
            long start = 0;
            if (lo_rec > 0)
                start = (long)dict_le32(s_dict_idx + (size_t)(lo_rec - 1) * DICT_IDX_REC);

            dict_buf_seek(&buf, start);
            result = dict_scan(&buf, word, -1);
        } else {
            long lo = 0;

            while (hi - lo > 512) {
                long mid = lo + (hi - lo) / 2;
                dict_buf_seek(&buf, mid);
                if (mid != 0) {
                    // Discard the partial line we landed in the middle of.
                    dict_buf_skip_line(&buf);
                }
                long line_start = dict_buf_tell(&buf);
                // A long line straddling mid can push line_start to or past hi;
                // never let hi grow — fall through to the linear scan instead.
                if (line_start >= hi) break;
                char line[512];
                int len = dict_buf_read_line(&buf, line, sizeof(line));
                if (len < 0) { hi = line_start; continue; }
                int cmp = dict_strcasecmp_word(line, word);
                if (cmp < 0) lo = dict_buf_tell(&buf);
                else         hi = line_start;
            }

            // Invariant: lo and hi are both line starts, and the first line
            // whose headword is >= word starts in [lo, hi] — inclusive, since
            // an exact hit sets hi to the matching line's start. So scan from
            // lo without skipping and include the line that starts at hi.
            dict_buf_seek(&buf, lo);
            result = dict_scan(&buf, word, hi);
        }
    }

    if (opened_here) ui_dict_close();
    return result;
}

bool ui_dict_available(void)
{
    if (!sd_fs_ready()) return false;
    SpiGuard spi;
    return SD.exists("/dict/eng-pob.tsv");
}

//************************************[ screen 13 ]****************************************** Ebook Reader

// Cached file handle for the active reader view. While open, every
// ui_reader_read_range call that matches filename reuses this handle
// instead of reopening — opening a file on SD walks the directory over SPI,
// which used to dominate cold-load and resume-seek time.
static File   s_reader_file;
static bool   s_reader_file_open = false;
static char   s_reader_file_name[UI_READER_NAME_MAX] = {0};

size_t ui_reader_open_view(const char *filename)
{
    ui_reader_close_view();
    if (!sd_fs_ready()) return 0;
    if (!filename || !*filename) return 0;

    char path[UI_READER_NAME_MAX + 16];
    snprintf(path, sizeof(path), "/books/%s", filename);

    size_t size = 0;
    {
        SpiGuard spi;
        s_reader_file = SD.open(path, FILE_READ);
        if (s_reader_file) {
            size = s_reader_file.size();
            s_reader_file_open = true;
            s_reader_file.setBufferSize(1024);  // stdio buffer lives in internal DRAM for the whole reader session
            copy_str(s_reader_file_name, sizeof(s_reader_file_name), filename);
        } else {
            log_e("reader_open_view: open failed: %s", path);
        }
    }
    return size;
}

void ui_reader_close_view(void)
{
    if (!s_reader_file_open) return;
    {
        SpiGuard spi;
        s_reader_file.close();
    }
    s_reader_file_open = false;
    s_reader_file_name[0] = '\0';
}

// List the plain files in `dir` (creating it when missing) into `list` as
// basenames. The caller owns the SD/SPI bus — wrap this in a SpiGuard.
static void fs_list_dir(const char *dir, char list[][UI_READER_NAME_MAX], int max, int *count)
{
    *count = 0;

    if (!SD.exists(dir)) {
        SD.mkdir(dir);
    }

    File root = SD.open(dir);
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file && *count < max) {
            if (!file.isDirectory()) {
                const char* name = file.name();
                // file.name() might return full path or just name depending on version
                const char* lastSlash = strrchr(name, '/');
                copy_str(list[*count], UI_READER_NAME_MAX, lastSlash ? lastSlash + 1 : name);
                (*count)++;
            }
            file = root.openNextFile();
        }
    }
}

void ui_reader_get_list(char list[UI_READER_MAX_COUNT][UI_READER_NAME_MAX], int *count)
{
    if (!sd_fs_ready()) {
        if (count) *count = 0;
        return;
    }
    SpiGuard spi;
    fs_list_dir("/books", list, UI_READER_MAX_COUNT, count);
}

// Returns true if the cached handle is open and matches the requested file —
// in which case ui_reader_read_range short-circuits the per-call open/close
// (the SD directory walk used to dominate read time).
static bool reader_cached_handle_matches(const char *filename)
{
    return s_reader_file_open &&
           filename != NULL &&
           strncmp(s_reader_file_name, filename, sizeof(s_reader_file_name)) == 0;
}

size_t ui_reader_read_range(const char *filename, size_t offset,
                            char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return 0;
    buf[0] = '\0';
    if (!sd_fs_ready()) return 0;

    // Fast path: reader view is active and has the file already open.
    if (reader_cached_handle_matches(filename)) {
        SpiGuard spi;
        s_reader_file.seek(offset);
        size_t got = s_reader_file.readBytes(buf, buf_size - 1);
        buf[got] = '\0';
        return got;
    }

    char path[UI_READER_NAME_MAX + 16];
    snprintf(path, sizeof(path), "/books/%s", filename);

    SpiGuard spi;
    File file = SD.open(path, FILE_READ);
    size_t got = 0;
    if (file) {
        if (offset > 0) file.seek(offset);
        got = file.readBytes(buf, buf_size - 1);
        buf[got] = '\0';
        file.close();
    } else {
        log_e("reader_range: open failed: %s", path);
    }
    return got;
}


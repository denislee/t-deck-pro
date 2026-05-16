
#include "ui_deckpro.h"
#include "src/assets.h"
#include "stdio.h"
#include "ui_deckpro_port.h"
#include "Arduino.h"
#include <time.h>

#define SETTING_PAGE_MAX_ITEM 15
#define GET_BUFF_LEN(a) sizeof(a)/sizeof(a[0])

#define FONT_BOLD_SIZE_14 ui_get_font(14, false)
#define FONT_BOLD_SIZE_15 ui_get_font(15, false)
#define FONT_BOLD_SIZE_16 ui_get_font(16, false)
#define FONT_BOLD_SIZE_17 ui_get_font(17, false)
#define FONT_BOLD_SIZE_18 ui_get_font(18, false)
#define FONT_BOLD_SIZE_19 ui_get_font(19, false)

#define FONT_BOLD_MONO_SIZE_14 ui_get_font(14, true)
#define FONT_BOLD_MONO_SIZE_15 ui_get_font(15, true)
#define FONT_BOLD_MONO_SIZE_16 ui_get_font(16, true)
#define FONT_BOLD_MONO_SIZE_17 ui_get_font(17, true)
#define FONT_BOLD_MONO_SIZE_18 ui_get_font(18, true)
#define FONT_BOLD_MONO_SIZE_19 ui_get_font(19, true)

// Forward declarations for the font helpers (defined alongside the reader
// font catalog further down). The taskbar/menu code that references the
// per-slot accessors lives above the catalog, so we need these visible early.
static const lv_font_t* ui_get_font(int pt, bool force_mono);
static const lv_font_t* topbar_font_get(void);
static const lv_font_t* reader_body_font_get(void);
static const lv_font_t* reader_footer_font_get(void);

#define GLOBAL_BUF_LEN 30
#define LOW_VOLTAGE_THRESHOLD_MV 3300
#define LOW_VOLTAGE_SOC_THRESHOLD 5
#define LOW_VOLTAGE_SHUTDOWN_DELAY_MS 20000
#define LOW_VOLTAGE_POLL_MS 2000
static char global_buf[GLOBAL_BUF_LEN];

static lv_obj_t *notes_list_obj;
static bool notes_use_sd = true;
static char notes_selected_file[32] = {0};

static lv_timer_t *touch_chk_timer = NULL;
static lv_timer_t *taskbar_update_timer = NULL;
static lv_timer_t *low_voltage_timer = NULL;
static lv_obj_t *label_list[10] = {0};
uint16_t taskbar_statue[TASKBAR_ID_MAX] = {0};
static lv_obj_t *low_voltage_popup = NULL;
static lv_obj_t *low_voltage_countdown_label = NULL;
static bool low_voltage_latched = false;
static bool low_voltage_shutdown_requested = false;
static uint32_t low_voltage_shutdown_deadline_ms = 0;
static int low_voltage_last_countdown_sec = -1;

static void low_voltage_popup_set_visible(bool visible)
{
    if (!low_voltage_popup) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(low_voltage_popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(low_voltage_popup);
    } else {
        lv_obj_add_flag(low_voltage_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void low_voltage_popup_update(int countdown_sec)
{
    if (!low_voltage_countdown_label) {
        return;
    }

    lv_label_set_text_fmt(low_voltage_countdown_label,
                          "Battery voltage is too low.\nPlease charge now.\nAuto shutdown in %ds.",
                          countdown_sec);
}

static void low_voltage_popup_create(void)
{
    if (low_voltage_popup) {
        return;
    }

    low_voltage_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_width(low_voltage_popup, 220);
    lv_obj_set_height(low_voltage_popup, LV_SIZE_CONTENT);
    lv_obj_center(low_voltage_popup);
    lv_obj_clear_flag(low_voltage_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_voltage_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(low_voltage_popup, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(low_voltage_popup, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(low_voltage_popup, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(low_voltage_popup, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(low_voltage_popup, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(low_voltage_popup, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(low_voltage_popup, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(low_voltage_popup, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(low_voltage_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(low_voltage_popup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(low_voltage_popup);
    lv_obj_set_style_text_font(title, FONT_BOLD_SIZE_17, LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(title, "LOW VOLTAGE");

    low_voltage_countdown_label = lv_label_create(low_voltage_popup);
    lv_obj_set_width(low_voltage_countdown_label, lv_pct(100));
    lv_obj_set_style_text_font(low_voltage_countdown_label, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_align(low_voltage_countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(low_voltage_countdown_label, LV_LABEL_LONG_WRAP);
    low_voltage_popup_update(LOW_VOLTAGE_SHUTDOWN_DELAY_MS / 1000);

    low_voltage_popup_set_visible(false);
}

static bool low_voltage_should_latch(void)
{
    bool voltage_low = false;
    bool gauge_low = false;
    bool soc_low = false;

    if (ui_test_get(E_PERI_BQ27220)) {
        uint16_t vbat_mv = (uint16_t)ui_battery_27220_get_voltage();
        voltage_low = (vbat_mv <= LOW_VOLTAGE_THRESHOLD_MV);
    }

    if (ui_battery_27220_is_vaild()) {
        gauge_low = ui_battery_27220_is_low_alarm();
        // soc_low = (ui_battery_27220_get_percent() <= LOW_VOLTAGE_SOC_THRESHOLD);
    }

    return voltage_low || gauge_low || soc_low;
}

static void low_voltage_reset_state(void)
{
    low_voltage_latched = false;
    low_voltage_shutdown_requested = false;
    low_voltage_shutdown_deadline_ms = 0;
    low_voltage_last_countdown_sec = -1;
    low_voltage_popup_set_visible(false);
}

static void low_voltage_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!low_voltage_popup) {
        low_voltage_popup_create();
    }

    if (ui_battery_is_external_power_present()) {
        low_voltage_reset_state();
        return;
    }

    // Latch until external power is inserted so the popup does not flicker near 3.3V.
    if (!low_voltage_latched && low_voltage_should_latch()) {
        low_voltage_latched = true;
        low_voltage_shutdown_requested = false;
        low_voltage_shutdown_deadline_ms = lv_tick_get() + LOW_VOLTAGE_SHUTDOWN_DELAY_MS;
        low_voltage_last_countdown_sec = -1;
    }

    if (!low_voltage_latched) {
        return;
    }

    low_voltage_popup_set_visible(true);

    int32_t remaining_ms = (int32_t)(low_voltage_shutdown_deadline_ms - lv_tick_get());
    int countdown_sec = remaining_ms > 0 ? (int)((remaining_ms + 999) / 1000) : 0;
    if (countdown_sec != low_voltage_last_countdown_sec) {
        low_voltage_popup_update(countdown_sec);
        low_voltage_last_countdown_sec = countdown_sec;
    }

    if (remaining_ms <= 0 && !low_voltage_shutdown_requested) {
        low_voltage_shutdown_requested = true;
        ui_shutdown_on();
    }
}

//************************************[ Other fun ]******************************************
#if 1
static lv_obj_t *scr_back_btn_create(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_height(btn, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 3, 3);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label2 = lv_label_create(btn);
    lv_obj_align(label2, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(label2, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label2, LV_SYMBOL_LEFT);

    lv_obj_t *label = lv_label_create(parent);
    lv_obj_align_to(label, label2, LV_ALIGN_OUT_RIGHT_MID, 5, -1);
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(label, 20);

    return label;
}

// ******************************** [ screen USB MSC ] ********************************
#if 1
static lv_obj_t *usb_msc_label;

static void usb_msc_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create_usb_msc(lv_obj_t *parent)
{
    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_color(info, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "\n\nUSB Mass Storage Mode\n\nSD Card is now mounted\non your computer.\n\nDO NOT unplug while\ntransferring files!");
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 40);

    usb_msc_label = lv_label_create(parent);
    lv_obj_set_style_text_font(usb_msc_label, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_align(usb_msc_label, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(usb_msc_label, "Status: Active");

    scr_back_btn_create(parent, "USB SD Mount", usb_msc_btn_event_cb);
}

static void entry_usb_msc(void)
{
    ui_usb_msc_begin();
    ui_disp_full_refr();
}

static void exit_usb_msc(void)
{
    ui_usb_msc_end();
    ui_disp_full_refr();
}

static void destroy_usb_msc(void) {}

static scr_lifecycle_t screen_usb_msc = {
    .create = create_usb_msc,
    .entry = entry_usb_msc,
    .exit  = exit_usb_msc,
    .destroy = destroy_usb_msc,
};
#endif

// ******************************** [ screen LOCK ] ********************************
#if 1
#define LOCK_UNLOCK_REQUIRED 3

static lv_obj_t *lock_time_label = NULL;
static lv_obj_t *lock_date_label = NULL;
static lv_obj_t *lock_month_label = NULL;
static lv_obj_t *lock_cal_label = NULL;
static lv_obj_t *lock_today_box = NULL;
static lv_obj_t *lock_today_label = NULL;
static lv_obj_t *lock_dots_label = NULL;
static lv_obj_t *lock_hint_label = NULL;
static lv_timer_t *lock_clock_timer = NULL;
static int lock_unlock_progress = 0;
static bool lock_saved_wifi_was_enabled = false;
static int lock_last_minute = -1;
static int lock_last_yday = -1;

// Calendar grid geometry — must match positions used in create_lock().
#define LOCK_CAL_X     36   // left edge of grid (7 cols × 24 = 168, centered in 240)
#define LOCK_CAL_Y    184   // top edge of grid
#define LOCK_CAL_CELL_W 24
#define LOCK_CAL_CELL_H 16

static void lock_render_dots(void)
{
    if (!lock_dots_label) return;
    char buf[32];
    char *p = buf;
    for (int i = 0; i < LOCK_UNLOCK_REQUIRED; i++) {
        if (i) *p++ = ' ';
        *p++ = '[';
        *p++ = (i < lock_unlock_progress) ? '#' : ' ';
        *p++ = ']';
    }
    *p = '\0';
    lv_label_set_text(lock_dots_label, buf);
}

static void lock_render_calendar(const struct tm *tm_now)
{
    if (!lock_cal_label || !lock_month_label) return;

    static const char *months_full[] = {
        "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
        "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
    };
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    int year  = tm_now->tm_year + 1900;
    int mon   = tm_now->tm_mon;        // 0..11
    int today = tm_now->tm_mday;       // 1..31
    int wday  = tm_now->tm_wday;       // 0..6 (Sun..Sat)
    if (mon < 0 || mon > 11) mon = 0;
    if (wday < 0 || wday > 6) wday = 0;

    int days_in_month = dim[mon];
    if (mon == 1) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (leap) days_in_month = 29;
    }

    // Weekday of day-1: walk back from today's known weekday.
    int first_wday = ((wday - (today - 1)) % 7 + 7) % 7;

    char header[24];
    lv_snprintf(header, sizeof(header), "%s %d", months_full[mon], year);
    lv_label_set_text(lock_month_label, header);

    // Build the grid as a single multi-line label, %2d + space per cell so
    // each cell is exactly 3 chars wide (24 px in 8x16 font).
    char buf[7 * 6 * 4 + 8] = {0};
    char *p = buf;
    int col = 0, row = 0;
    int today_col = -1, today_row = -1;
    for (int i = 0; i < first_wday; i++) {
        *p++ = ' '; *p++ = ' '; *p++ = ' ';
        col++;
    }
    for (int d = 1; d <= days_in_month; d++) {
        if (d == today) { today_col = col; today_row = row; }
        char tmp[8];
        lv_snprintf(tmp, sizeof(tmp), "%2d ", d);
        for (int j = 0; tmp[j]; j++) *p++ = tmp[j];
        col++;
        if (col == 7) {
            col = 0;
            row++;
            if (d != days_in_month) *p++ = '\n';
        }
    }
    *p = '\0';

    lv_label_set_text(lock_cal_label, buf);

    // Position today's inverse cell + white number on top of the grid.
    if (lock_today_box && lock_today_label && today_col >= 0) {
        int x = LOCK_CAL_X + today_col * LOCK_CAL_CELL_W;
        int y = LOCK_CAL_Y + today_row * LOCK_CAL_CELL_H;
        lv_obj_set_pos(lock_today_box, x, y);
        lv_obj_clear_flag(lock_today_box, LV_OBJ_FLAG_HIDDEN);

        char tnum[8];
        lv_snprintf(tnum, sizeof(tnum), "%2d", today);
        lv_label_set_text(lock_today_label, tnum);
        lv_obj_set_pos(lock_today_label, x, y);
        lv_obj_clear_flag(lock_today_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lock_update_clock(bool force_full)
{
    struct tm tm_now;
    if (!ui_time_get_local(&tm_now)) {
        if (lock_time_label) lv_label_set_text(lock_time_label, "--:--");
        if (lock_date_label) lv_label_set_text(lock_date_label, "no time sync");
        if (lock_month_label) lv_label_set_text(lock_month_label, "");
        if (lock_cal_label)  lv_label_set_text(lock_cal_label, "");
        if (lock_today_box)  lv_obj_add_flag(lock_today_box, LV_OBJ_FLAG_HIDDEN);
        if (lock_today_label) lv_obj_add_flag(lock_today_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    bool minute_changed = force_full || tm_now.tm_min != lock_last_minute;
    bool day_changed    = force_full || tm_now.tm_yday != lock_last_yday;
    if (!minute_changed && !day_changed) return;
    lock_last_minute = tm_now.tm_min;
    lock_last_yday   = tm_now.tm_yday;

    static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    int wday = tm_now.tm_wday;
    int mon  = tm_now.tm_mon;
    if (wday < 0 || wday > 6) wday = 0;
    if (mon  < 0 || mon  > 11) mon = 0;

    if (minute_changed && lock_time_label) {
        char tbuf[8];
        lv_snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        lv_label_set_text(lock_time_label, tbuf);
    }
    if (day_changed) {
        char dbuf[32];
        lv_snprintf(dbuf, sizeof(dbuf), "%s, %s %d",
                    days[wday], months[mon], tm_now.tm_mday);
        if (lock_date_label) lv_label_set_text(lock_date_label, dbuf);
        lock_render_calendar(&tm_now);
    }
}

static void lock_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    lock_update_clock(false);
}

static void create_lock(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top inverse banner ──
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, LV_HOR_RES, 24);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(band, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *banner = lv_label_create(band);
    lv_label_set_text(banner, "L  O  C  K  E  D");
    lv_obj_set_style_text_color(banner, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(banner, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(banner, 1, LV_PART_MAIN);
    lv_obj_center(banner);

    // ── Huge clock (32×64 pixel font) ──
    lock_time_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_time_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_time_label, &lv_font_spleen_32x64, LV_PART_MAIN);
    lv_label_set_text(lock_time_label, "--:--");
    lv_obj_align(lock_time_label, LV_ALIGN_TOP_MID, 0, 40);

    // ── Date subtitle ──
    lock_date_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_date_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_date_label, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_label_set_text(lock_date_label, " ");
    lv_obj_align(lock_date_label, LV_ALIGN_TOP_MID, 0, 112);

    // ── Divider above the calendar ──
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 200, 1);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_style_bg_color(rule, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);

    // ── Calendar: month name ──
    lock_month_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_month_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_month_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(lock_month_label, 1, LV_PART_MAIN);
    lv_label_set_text(lock_month_label, "");
    lv_obj_align(lock_month_label, LV_ALIGN_TOP_MID, 0, 144);

    // ── Calendar: weekday header (Su Mo Tu We Th Fr Sa) ──
    lv_obj_t *wk = lv_label_create(parent);
    lv_obj_set_style_text_color(wk, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(wk, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_label_set_text(wk, "Su Mo Tu We Th Fr Sa");
    lv_obj_set_pos(wk, LOCK_CAL_X, LOCK_CAL_Y - 18);

    // ── Today highlight: black box (drawn before label so label sits above) ──
    lock_today_box = lv_obj_create(parent);
    lv_obj_remove_style_all(lock_today_box);
    lv_obj_set_size(lock_today_box, LOCK_CAL_CELL_W - 4, LOCK_CAL_CELL_H);
    lv_obj_set_style_bg_color(lock_today_box, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lock_today_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(lock_today_box, LV_OBJ_FLAG_HIDDEN);

    // ── Calendar: date grid ──
    lock_cal_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_cal_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_cal_label, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_label_set_text(lock_cal_label, "");
    lv_obj_set_pos(lock_cal_label, LOCK_CAL_X, LOCK_CAL_Y);

    // White today number (drawn above the grid + box)
    lock_today_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_today_label, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_today_label, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_label_set_text(lock_today_label, "");
    lv_obj_add_flag(lock_today_label, LV_OBJ_FLAG_HIDDEN);

    // ── Progress dots ──
    lock_dots_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_dots_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_dots_label, &lv_font_tamzen_6x12, LV_PART_MAIN);
    lv_obj_align(lock_dots_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lock_render_dots();
}

static void entry_lock(void)
{
    lock_unlock_progress = 0;
    lock_last_minute = -1;
    lock_last_yday = -1;
    lock_render_dots();
    lock_update_clock(true);
    if (!lock_clock_timer) {
        lock_clock_timer = lv_timer_create(lock_clock_timer_cb, 15000, NULL);
    } else {
        lv_timer_resume(lock_clock_timer);
    }

    lock_saved_wifi_was_enabled = ui_wifi_get_enabled();
    if (lock_saved_wifi_was_enabled) {
        ui_wifi_set_enabled(false);
    }
    ui_setting_apply_keypad_light(false);

    ui_disp_full_refr();
}

static void exit_lock(void)
{
    if (lock_clock_timer) {
        lv_timer_pause(lock_clock_timer);
    }

    if (lock_saved_wifi_was_enabled) {
        ui_wifi_set_enabled(true);
        lock_saved_wifi_was_enabled = false;
    }
    ui_setting_apply_keypad_light(ui_setting_get_keypad_light());

    ui_disp_full_refr();
}

static void destroy_lock(void)
{
    if (lock_clock_timer) {
        lv_timer_del(lock_clock_timer);
        lock_clock_timer = NULL;
    }
    lock_time_label = NULL;
    lock_date_label = NULL;
    lock_month_label = NULL;
    lock_cal_label = NULL;
    lock_today_box = NULL;
    lock_today_label = NULL;
    lock_dots_label = NULL;
    lock_hint_label = NULL;
}

static scr_lifecycle_t screen_lock = {
    .create = create_lock,
    .entry = entry_lock,
    .exit = exit_lock,
    .destroy = destroy_lock,
};

// Called by the home-screen keypad timer while the lock screen is active.
static void lock_handle_key(char key)
{
    if (key == 'u' || key == 'U') {
        lock_unlock_progress++;
        if (lock_unlock_progress >= LOCK_UNLOCK_REQUIRED) {
            lock_unlock_progress = 0;
            scr_mgr_pop(false);
            return;
        }
        lock_render_dots();
    } else if (lock_unlock_progress != 0) {
        lock_unlock_progress = 0;
        lock_render_dots();
    }
}
#endif

static const char *line_full_format(int max_c, const char *str1, const char *str2)
{
    int len1 = 0, len2 = 0;
    int j;

    len1 = strlen(str1);

    strncpy(global_buf, str1, len1);

    len2 = strlen(str2);
    for(j = len1; j < max_c -1 - len2; j++){
        global_buf[j] = ' ';
    }
    strncpy(global_buf + j, str2, len2);
    j = j + len2;
    
    global_buf[j] = '\0'; 

    printf("[%d] buf: %s\n", __LINE__, global_buf);

    return (const char *)global_buf;
}

#endif
//************************************[ screen 0 ]****************************************** menu
#if 1
#define MENU_BTN_NUM (sizeof(menu_btn_list) / sizeof(menu_btn_list[0]))

static ui_indev_read_cb ui_get_gesture_dir = NULL;

static lv_obj_t *menu_screen1;
static lv_obj_t *menu_screen2;
static lv_obj_t *ui_Panel4;

static lv_obj_t * menu_taskbar = NULL;
static lv_obj_t * menu_taskbar_time = NULL;
static lv_obj_t * menu_taskbar_charge = NULL;
static lv_obj_t * menu_taskbar_battery = NULL;
static lv_obj_t * menu_taskbar_battery_percent = NULL;
static lv_obj_t * menu_taskbar_wifi = NULL;

// Each screen's create() calls ui_taskbar_create() which clobbers the globals
// above to point at the new screen's taskbar children. When we pop back to
// SCREEN0 the previous screen's destroy() has already torn its taskbar down,
// leaving those globals dangling — entry0 then crashed touching them. Capture
// SCREEN0's taskbar nodes when it's first built so entry0 can re-point the
// globals at SCREEN0's still-alive taskbar before using them.
static lv_obj_t * scr0_taskbar = NULL;
static lv_obj_t * scr0_taskbar_time = NULL;
static lv_obj_t * scr0_taskbar_charge = NULL;
static lv_obj_t * scr0_taskbar_battery = NULL;
static lv_obj_t * scr0_taskbar_battery_percent = NULL;
static lv_obj_t * scr0_taskbar_wifi = NULL;

static int page_num = 0;
static int page_curr = 0;

static struct menu_btn menu_btn_list[] = 
{
    {SCREEN2_ID,  &img_setting, "Setting"},
    {SCREEN13_ID, &img_SD,    "Reader"},
    {SCREEN12_1_ID, &img_SD,    "Notes"},
    {SCREEN_DICT_ID, &img_SD,   "Dict"},
    {SCREEN_USB_MSC_ID, &img_SD, "USB Mount"},
};

#ifndef APP_HIDDEN_STATUS_DEFINED
#define APP_HIDDEN_STATUS_DEFINED
static bool app_hidden_status[MENU_BTN_NUM] = {0};
#endif

static void menu_btn_event_cb(lv_event_t *e)
{
    struct menu_btn *tgr = (struct menu_btn *)e->user_data;
    scr_mgr_push(tgr->idx, false);
}

static void menu_get_gesture_dir(int dir)
{
    if(MENU_BTN_NUM <= 9) return;

    if(dir == LV_DIR_LEFT) {
        if(page_curr < page_num){
            page_curr++;
            // ui_disp_full_refr();
        }
        else{
            return ;
        }
    } else if(dir == LV_DIR_RIGHT) {
        if(page_curr > 0){
            page_curr--;
        }
        else{
            return ;
        }
    }   

    if(page_curr == 1) {
        lv_obj_clear_flag(menu_screen2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_screen1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 0), lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 1), lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);

    } else if(page_curr == 0) {
        lv_obj_clear_flag(menu_screen1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_screen2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 0), lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 1), lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void menu_btn_create(lv_obj_t *parent, struct menu_btn *info, int x, int y)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, 50);
    lv_obj_set_height(btn, 50);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(btn, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(btn, 3, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_14, LV_PART_MAIN);
    lv_obj_set_width(label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(label, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(label, 0);
    lv_obj_set_y(label, 20);
    lv_obj_set_align(label, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_x(btn, x);
    lv_obj_set_y(btn, y);
    lv_obj_set_style_bg_img_src(btn, info->icon, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, (info->name));
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, menu_btn_event_cb, LV_EVENT_CLICKED, (void *)info);
}

static void ui_taskbar_apply_battery_visibility(void);

static void ui_taskbar_create(lv_obj_t *parent)
{
    int status_bar_height = 25;
    menu_taskbar = lv_obj_create(parent);
    lv_obj_set_size(menu_taskbar, LV_HOR_RES, status_bar_height);
    lv_obj_set_style_pad_all(menu_taskbar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_taskbar, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(menu_taskbar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(menu_taskbar, LV_OBJ_FLAG_SCROLLABLE);
    
    // Top bar elements (clock, battery icon+%, wifi/charge symbols) all share
    // the TOPBAR slot's font so the row scales together when the user changes
    // the top-bar font in System Font settings.
    const lv_font_t *tb_font = topbar_font_get();

    menu_taskbar_time = lv_label_create(menu_taskbar);
    lv_obj_set_style_border_width(menu_taskbar_time, 0, 0);
    // If the clock is already synced, paint the current time straight away —
    // otherwise the user would see "--:--" until the next whole-minute tick
    // when entering any app (the update timer only redraws on minute change).
    // Before NTP we show a dash placeholder; an actual digit string like
    // "10:19" lets the e-paper settle full glyphs that a partial refresh
    // can't cleanly overwrite when the real time finally arrives — the
    // dash form keeps that first transition clean, and the first real-time
    // update also forces a full refresh (see menu_taskbar_update_timer_cb).
    {
        struct tm tm_now;
        if (ui_time_get_local(&tm_now)) {
            lv_label_set_text_fmt(menu_taskbar_time, "%02d:%02d",
                                  tm_now.tm_hour, tm_now.tm_min);
        } else {
            lv_label_set_text(menu_taskbar_time, "--:--");
        }
    }
    lv_obj_set_style_text_font(menu_taskbar_time, tb_font, LV_PART_MAIN);
    lv_obj_align(menu_taskbar_time, LV_ALIGN_LEFT_MID, 10, 3);

    lv_obj_t *status_parent = lv_obj_create(menu_taskbar);
    lv_obj_set_size(status_parent, lv_pct(80)-2, status_bar_height-2);
    lv_obj_set_style_pad_all(status_parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_parent, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_parent, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(status_parent, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(status_parent, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(status_parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(status_parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(status_parent, LV_ALIGN_RIGHT_MID, 0, 3);

    menu_taskbar_wifi = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_wifi, "%s", LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(menu_taskbar_wifi, tb_font, LV_PART_MAIN);
    lv_obj_add_flag(menu_taskbar_wifi, LV_OBJ_FLAG_HIDDEN);

    menu_taskbar_charge = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(menu_taskbar_charge, tb_font, LV_PART_MAIN);
    lv_obj_add_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);

    if(taskbar_statue[TASKBAR_ID_WIFI])
        lv_obj_clear_flag(menu_taskbar_wifi, LV_OBJ_FLAG_HIDDEN);

    if(taskbar_statue[TASKBAR_ID_CHARGE])
        lv_obj_clear_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);

    menu_taskbar_battery = lv_label_create(status_parent);
    lv_obj_set_style_text_font(menu_taskbar_battery, tb_font, LV_PART_MAIN);

    menu_taskbar_battery_percent = lv_label_create(status_parent);
    lv_obj_set_style_text_font(menu_taskbar_battery_percent, tb_font, LV_PART_MAIN);

    ui_taskbar_apply_battery_visibility();
}

static void ui_taskbar_apply_battery_visibility(void)
{
    bool show = ui_topbar_show_battery_get();
    if (menu_taskbar_battery) {
        if (show) lv_obj_clear_flag(menu_taskbar_battery, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(menu_taskbar_battery,   LV_OBJ_FLAG_HIDDEN);
    }
    if (menu_taskbar_battery_percent) {
        if (show) lv_obj_clear_flag(menu_taskbar_battery_percent, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(menu_taskbar_battery_percent,   LV_OBJ_FLAG_HIDDEN);
    }
}

static void create0(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    scr0_taskbar = menu_taskbar;
    scr0_taskbar_time = menu_taskbar_time;
    scr0_taskbar_charge = menu_taskbar_charge;
    scr0_taskbar_battery = menu_taskbar_battery;
    scr0_taskbar_battery_percent = menu_taskbar_battery_percent;
    scr0_taskbar_wifi = menu_taskbar_wifi;

    page_num = 0;

    // === Top divider line under the taskbar ===
    lv_obj_t *top_rule = lv_obj_create(parent);
    lv_obj_remove_style_all(top_rule);
    lv_obj_set_size(top_rule, LV_HOR_RES, 1);
    lv_obj_align(top_rule, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_set_style_bg_color(top_rule, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top_rule, LV_OPA_COVER, LV_PART_MAIN);

    // === Inverse "SHORTCUTS" header strip ===
    lv_obj_t *sc_header = lv_obj_create(parent);
    lv_obj_remove_style_all(sc_header);
    lv_obj_set_size(sc_header, 200, 18);
    lv_obj_align(sc_header, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(sc_header, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sc_header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sc_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sc_title = lv_label_create(sc_header);
    lv_label_set_text(sc_title, "SHORTCUTS");
    lv_obj_set_style_text_color(sc_title, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(sc_title, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(sc_title, 2, LV_PART_MAIN);
    lv_obj_center(sc_title);

    // === Two-column shortcut list ===
    lv_obj_t *sc_left = lv_label_create(parent);
    lv_label_set_text(sc_left,
        "s   Setting\n"
        "n   Notes\n"
        "t   Dict\n"
        "w   WiFi\n"
        "z   Time\n"
        "esc Refresh");
    lv_obj_set_style_text_font(sc_left, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sc_left, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_align(sc_left, LV_ALIGN_TOP_LEFT, 28, 60);

    lv_obj_t *sc_right = lv_label_create(parent);
    lv_label_set_text(sc_right,
        "r   Reader\n"
        "c   Resume\n"
        "q   USB\n"
        "p   Ping\n"
        "l   Lock\n"
        "d   Sleep");
    lv_obj_set_style_text_font(sc_right, &lv_font_tamzen_8x16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sc_right, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_align(sc_right, LV_ALIGN_TOP_LEFT, 132, 60);

    // === Tip line at the bottom ===
    lv_obj_t *tip = lv_label_create(parent);
    lv_label_set_text(tip, "press a key to launch");
    lv_obj_set_style_text_font(tip, &lv_font_tamzen_6x12, LV_PART_MAIN);
    lv_obj_set_style_text_color(tip, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void entry0(void) {
    ui_get_gesture_dir = menu_get_gesture_dir;

    // Re-point the taskbar globals at SCREEN0's still-living taskbar children.
    // A pushed screen's create() overwrote them, and that screen's destroy()
    // freed those nodes — without this restore the labels below would touch
    // freed memory.
    menu_taskbar = scr0_taskbar;
    menu_taskbar_time = scr0_taskbar_time;
    menu_taskbar_charge = scr0_taskbar_charge;
    menu_taskbar_battery = scr0_taskbar_battery;
    menu_taskbar_battery_percent = scr0_taskbar_battery_percent;
    menu_taskbar_wifi = scr0_taskbar_wifi;

    lv_timer_resume(touch_chk_timer);
    lv_timer_resume(taskbar_update_timer);

    if (menu_taskbar_battery) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
    }
    if (menu_taskbar_battery_percent) {
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_taskbar_apply_battery_visibility();
}
static void exit0(void) {
    ui_get_gesture_dir = NULL;
    lv_timer_pause(touch_chk_timer);
    lv_timer_pause(taskbar_update_timer);
}
static void destroy0(void) {
    if(menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
    scr0_taskbar = NULL;
    scr0_taskbar_time = NULL;
    scr0_taskbar_charge = NULL;
    scr0_taskbar_battery = NULL;
    scr0_taskbar_battery_percent = NULL;
    scr0_taskbar_wifi = NULL;
}



static scr_lifecycle_t screen0 = {
    .create = create0,
    .entry = entry0,
    .exit  = exit0,
    .destroy = destroy0,
};
#endif
//************************************[ screen 1 ]****************************************** lora
// --------------------- screen 1 --------------------- lora
#if 1
lv_obj_t * scr1_list;
static lv_obj_t *scr1_lab_buf[20];

static void scr1_list_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    for(int i = 0; i < lv_obj_get_child_cnt(obj); i++) 
    {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if(lv_obj_check_type(child, &lv_label_class)) {
            char *str = lv_label_get_text(child);

            if(strcmp("- Auto Test", str) == 0)
            {
                scr_mgr_push(SCREEN1_1_ID, false);
            }
            if(strcmp("- Lora Setting", str) == 0)
            {
                scr_mgr_push(SCREEN1_2_ID, false);
            }
            printf("%s\n", str);
        }
    }
}

static void scr1_item_create(const char *name, lv_event_cb_t cb)
{
    lv_obj_t * obj = lv_obj_class_create_obj(&lv_list_btn_class, scr1_list);
    lv_obj_class_init_obj(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_set_height(obj, LV_VER_RES / 6);
    lv_obj_set_style_text_font(obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(obj, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    // lv_obj_set_style_text_color(obj, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL); 
}

static void scr1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        // ui_full_refresh();
        scr_mgr_pop(false);
    }
}

static void create1(lv_obj_t *parent) 
{
    scr1_list = lv_list_create(parent);
    lv_obj_set_size(scr1_list, lv_pct(93), lv_pct(91));
    lv_obj_align(scr1_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_obj_set_style_bg_color(scr1_list, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr1_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr1_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr1_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(scr1_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr1_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_border_color(scr1_list, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr1_list, 0, LV_PART_MAIN);

    scr1_item_create("- Auto Test", scr1_list_event);
    scr1_item_create("- Lora Setting", scr1_list_event);

    // back
    scr_back_btn_create(parent, "Lora", scr1_btn_event_cb);
}

static void entry1(void) 
{
    ui_disp_full_refr();
}
static void exit1(void) {
    ui_disp_full_refr();
}
static void destroy1(void) { }

static scr_lifecycle_t screen1 = {
    .create = create1,
    .entry = entry1,
    .exit  = exit1,
    .destroy = destroy1,
};
#endif
// --------------------- screen 1.1 --------------------- Auto Send
#if 1
static lv_obj_t *scr1_1_cont;
static lv_obj_t *lora_lab_buf[11] = {0};
static lv_obj_t *lora_sw_btn;
static lv_obj_t *lora_sw_btn_info;
static lv_timer_t *lora_RT_timer = NULL;
static lv_timer_t *lora_recv_timer = NULL;
static int lora_cnt = 0;

static void scr1_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void lora_mode_sw_event(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        if(ui_lora_get_mode() == LORA_MODE_SEND) {
            ui_lora_set_mode(LORA_MODE_RECV);
            lv_label_set_text(lora_sw_btn_info, "Recv");
            for(int i = 0; i < GET_BUFF_LEN(lora_lab_buf); i++){
                lv_label_set_text(lora_lab_buf[i], " ");
            }
            lora_cnt = 0;
        } else if(ui_lora_get_mode() == LORA_MODE_RECV) {
            ui_lora_set_mode(LORA_MODE_SEND);
            lv_label_set_text(lora_sw_btn_info, "Send");
            for(int i = 0; i < GET_BUFF_LEN(lora_lab_buf); i++){
                lv_label_set_text(lora_lab_buf[i], " ");
            }
            lora_cnt = 0;
        }
    }
}

static void lora_recv_loop_event(lv_timer_t *t)
{
    ui_lora_recv_loop();
}

static void lora_RT_timer_event(lv_timer_t *t)
{
    static int data = 0;
    char buf[32];
    const char *recv_info = NULL;
    int recv_rssi = 0;
    
    if(ui_lora_get_mode() == LORA_MODE_SEND) 

    {
        lv_snprintf(buf, 32, "DeckPro #%d", data++);
        lv_label_set_text_fmt(lora_lab_buf[lora_cnt], "send-> %s", buf);
        ui_lora_send(buf);

        lora_cnt++;
        if(lora_cnt >= GET_BUFF_LEN(lora_lab_buf)) {
            lora_cnt = 0;
        }
    }
    else if(ui_lora_get_mode() == LORA_MODE_RECV)
    {
        if(ui_lora_get_recv(&recv_info, &recv_rssi))
        {
            ui_lora_set_recv_flag();
            lv_label_set_text_fmt(lora_lab_buf[lora_cnt], "recv-> %s [%d]", recv_info, recv_rssi);

            lora_cnt++;
            if(lora_cnt >= GET_BUFF_LEN(lora_lab_buf)) {
                lora_cnt = 0;
            }
        }
    }
}

static lv_obj_t * scr2_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_HOR_RES - 26);
    lv_obj_set_style_text_font(label, FONT_BOLD_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}
static void create1_1(lv_obj_t *parent) 
{
    scr1_1_cont = lv_obj_create(parent);
    lv_obj_set_size(scr1_1_cont, lv_pct(100), lv_pct(85));
    lv_obj_set_style_bg_color(scr1_1_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr1_1_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr1_1_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr1_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr1_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(scr1_1_cont, 13, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr1_1_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr1_1_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr1_1_cont, 5, LV_PART_MAIN);
    lv_obj_set_align(scr1_1_cont, LV_ALIGN_BOTTOM_MID);

    for(int i = 0; i < GET_BUFF_LEN(lora_lab_buf); i++){
        lora_lab_buf[i] = scr2_create_label(scr1_1_cont);
        lv_label_set_text(lora_lab_buf[i], " ");
    }

    lora_sw_btn = lv_btn_create(parent);
    lv_obj_set_size(lora_sw_btn, 70, 25);
    lv_obj_set_style_radius(lora_sw_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_sw_btn, 2, LV_PART_MAIN);
    lora_sw_btn_info = lv_label_create(lora_sw_btn);
    lv_obj_set_style_text_font(lora_sw_btn_info, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_align(lora_sw_btn_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lora_sw_btn_info, "Send");
    lv_obj_center(lora_sw_btn_info);
    lv_obj_align(lora_sw_btn, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_add_event_cb(lora_sw_btn, lora_mode_sw_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_style_text_font(lab, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_text_fmt(lab, "%.1fM", ui_lora_get_freq());
    lv_obj_align(lab, LV_ALIGN_TOP_RIGHT, -10, 10);

    ui_lora_set_mode(LORA_MODE_SEND);
    lora_cnt = 0;

    // back
    scr_back_btn_create(parent, ("Lora"), scr1_1_btn_event_cb);
}
static void entry1_1(void) 
{
    ui_disp_full_refr();
    lora_RT_timer = lv_timer_create(lora_RT_timer_event, 2000, NULL);
    lora_recv_timer = lv_timer_create(lora_recv_loop_event, 400, NULL);
}
static void exit1_1(void) {
    ui_disp_full_refr();
    if(lora_RT_timer) {
        lv_timer_del(lora_RT_timer);
        lora_RT_timer = NULL;
    }
    if(lora_recv_timer) {
        lv_timer_del(lora_recv_timer);
        lora_recv_timer = NULL;
    }
}
static void destroy1_1(void) { }

static scr_lifecycle_t screen1_1 = {
    .create = create1_1,
    .entry = entry1_1,
    .exit  = exit1_1,
    .destroy = destroy1_1,
};
#endif
// --------------------- screen 1.2 --------------------- Lora Setting
#if 1

#define RADIO_FREQUENCY_LIST "433MHz\n 850MHz\n 868MHz\n 915MHz\n 920MHz"
#define RADIO_BANDWIDTH "125KHz\n 250KHz\n 500KHz"
#define RADIO_TX_POWER "10dBm\n 22dBm"

static float lora_freq_list[] = {433.0, 850.0, 868.0, 915.0, 920.0};
static int lora_band_list[] = {125, 250, 500};
static int lora_power_list[] = {10, 22};

static lv_obj_t *scr1_2_cont;
static lv_obj_t *dropdown_freq;
static lv_obj_t *dropdown_band;
static lv_obj_t *dropdown_power;

static void scr1_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void lora_setting_event_handler(lv_event_t * e)
{
    char buf[32]={0};
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    const char *flag = ( const char *)lv_event_get_user_data(e);
    int select = lv_dropdown_get_selected(obj);

    lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
    switch (*flag)
    {
    case 'f': 
        for(int i = 0; i < GET_BUFF_LEN(lora_freq_list); i++) {
            if(lora_freq_list[select] == lora_freq_list[i]) {
                printf("set freq %.1fMHz\n", lora_freq_list[i]);
                ui_lora_set_freq(lora_freq_list[i]);
            }
        }
        break;
    case 'b': 
        for(int i = 0; i < GET_BUFF_LEN(lora_band_list); i++) {
            if(lora_band_list[select] == lora_band_list[i]) {
                printf("set bandwidth %dKhz\n", lora_band_list[i]);
                ui_lora_set_bandwidth(lora_band_list[i]);
            }
        }
        break;
    case 'p': 
        for(int i = 0; i < GET_BUFF_LEN(lora_power_list); i++) {
            if(lora_power_list[select] == lora_power_list[i]) {
                printf("set power %ddBm\n", lora_power_list[i]);
                ui_lora_set_power(lora_power_list[i]);
            }
        }
        break;
    
    default:
        break;
    }
}

static lv_obj_t * scr1_2_lora_setting_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *ui_Container1 = lv_obj_create(parent);
    lv_obj_remove_style_all(ui_Container1);
    lv_obj_set_height(ui_Container1, 42);
    lv_obj_set_width(ui_Container1, lv_pct(100));
    lv_obj_set_x(ui_Container1, 35);
    lv_obj_set_y(ui_Container1, -16);
    lv_obj_set_align(ui_Container1, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(ui_Container1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_Container1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(ui_Container1, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_pad_row(ui_Container1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ui_Container1, 20, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ui_Label14 = lv_label_create(ui_Container1);
    lv_obj_set_width(ui_Label14, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label14, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Label14, -60);
    lv_obj_set_y(ui_Label14, -42);
    lv_obj_set_align(ui_Label14, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label14, text);
    lv_obj_set_style_text_font(ui_Label14, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   

    lv_obj_t *ui_Dropdown1 = lv_dropdown_create(ui_Container1);
    lv_obj_set_width(ui_Dropdown1, lv_pct(60));
    lv_obj_set_height(ui_Dropdown1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Dropdown1, 19);
    lv_obj_set_y(ui_Dropdown1, -1);
    lv_obj_add_flag(ui_Dropdown1, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags

    // lv_obj_set_style_bg_opa(ui_Dropdown1, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ui_Dropdown1, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    // lv_obj_set_style_shadow_width(ui_Dropdown1, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_PRESSED);

    return ui_Dropdown1;
}

static void create1_2(lv_obj_t *parent) 
{
    scr1_2_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(scr1_2_cont);
    lv_obj_set_width(scr1_2_cont, lv_pct(100));
    lv_obj_set_height(scr1_2_cont, lv_pct(85));
    lv_obj_set_align(scr1_2_cont, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(scr1_2_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr1_2_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(scr1_2_cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_pad_row(scr1_2_cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(scr1_2_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // lv_obj_set_style_border_width(scr1_2_cont, 3, LV_PART_MAIN);
    lv_obj_set_align(scr1_2_cont, LV_ALIGN_BOTTOM_MID);

    dropdown_freq = scr1_2_lora_setting_create(scr1_2_cont, "Freq: ");
    lv_dropdown_set_options(dropdown_freq, RADIO_FREQUENCY_LIST);
    for(int i = 0; i < GET_BUFF_LEN(lora_freq_list); i++) {
        if(ui_lora_get_freq() == lora_freq_list[i]) {
            lv_dropdown_set_selected(dropdown_freq, i);
        }
    }

    dropdown_band = scr1_2_lora_setting_create(scr1_2_cont, "Band: ");
    lv_dropdown_set_options(dropdown_band, RADIO_BANDWIDTH);
    for(int i = 0; i < GET_BUFF_LEN(lora_band_list); i++) {
        if(ui_lora_get_bandwidth() == lora_band_list[i]) {
            lv_dropdown_set_selected(dropdown_band, i);
        }
    }

    dropdown_power = scr1_2_lora_setting_create(scr1_2_cont, "Power:");
    lv_dropdown_set_options(dropdown_power, RADIO_TX_POWER);
    for(int i = 0; i < GET_BUFF_LEN(lora_power_list); i++) {
        if(ui_lora_get_power() == lora_power_list[i]) {
            lv_dropdown_set_selected(dropdown_power, i);
        }
    }
    static const char freq_flag = 'f';
    static const char band_flag = 'b';
    static const char power_flag = 'p';
    lv_obj_add_event_cb(dropdown_freq, lora_setting_event_handler, LV_EVENT_VALUE_CHANGED, (void *)&freq_flag);
    lv_obj_add_event_cb(dropdown_band, lora_setting_event_handler, LV_EVENT_VALUE_CHANGED, (void *)&band_flag);
    lv_obj_add_event_cb(dropdown_power,   lora_setting_event_handler, LV_EVENT_VALUE_CHANGED, (void *)&power_flag);
    // back
    scr_back_btn_create(parent, ("Lora Setting"), scr1_2_btn_event_cb);
}
static void entry1_2(void) 
{
    ui_disp_full_refr();
}
static void exit1_2(void) {
    ui_disp_full_refr();
    ui_lora_param_set();
}
static void destroy1_2(void) { }

static scr_lifecycle_t screen1_2 = {
    .create = create1_2,
    .entry = entry1_2,
    .exit  = exit1_2,
    .destroy = destroy1_2,
};
#endif
//************************************[ screen 2 ]****************************************** Setting
// --------------------- screen 2.1 --------------------- About System
#if 1
static lv_obj_t *scr2_1_cont;

static void scr2_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create2_1(lv_obj_t *parent) 
{
    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_color(info, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    // Build into a fixed stack buffer instead of an Arduino String — String +=
    // reallocs on every concatenation, fragmenting the heap. Note that
    // line_full_format() returns a pointer into a single shared static buffer,
    // so its result must be consumed (snprintf'd) before the next call.
    char str[512];
    char buf[32];
    uint64_t total = 0, used = 0;
    ui_setting_get_sd_capacity(&total, &used);

    size_t off = 0;
    off += lv_snprintf(str + off, sizeof(str) - off, "                           \n");
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n",
                       line_full_format(28, "SF Version:", ui_setting_get_sf_ver()));
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n",
                       line_full_format(28, "HD Version:", ui_setting_get_hd_ver()));

    lv_snprintf(buf, sizeof(buf), "%lluMB", total);
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n",
                       line_full_format(28, "SD total:", buf));

    lv_snprintf(buf, sizeof(buf), "%lluMB", used);
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n",
                       line_full_format(28, "SD used:", buf));

    lv_label_set_text_fmt(info, "%s", str);
    
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 35);
    
    lv_obj_t *back2_1_label = scr_back_btn_create(parent, ("About System"), scr2_1_btn_event_cb);
}
static void entry2_1(void) 
{
    ui_disp_full_refr();
}
static void exit2_1(void) {
    ui_disp_full_refr();
}
static void destroy2_1(void) { }

static scr_lifecycle_t screen2_1 = {
    .create = create2_1,
    .entry = entry2_1,
    .exit  = exit2_1,
    .destroy = destroy2_1,
};
#endif
// --------------------- screen 2.2 --------------------- Hidden Apps
#if 1
static void hidden_app_event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    if(code == LV_EVENT_VALUE_CHANGED) {
        int app_index = (int)lv_event_get_user_data(e);
        if(lv_obj_has_state(obj, LV_STATE_CHECKED)) {
            app_hidden_status[app_index] = true;
        } else {
            app_hidden_status[app_index] = false;
        }
    }
}

static void scr2_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create2_2(lv_obj_t *parent)
{
    lv_obj_t * list = lv_list_create(parent);
    lv_obj_set_size(list, LV_HOR_RES, lv_pct(88));
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);

    for (int i = 0; i < MENU_BTN_NUM; i++)
    {
        lv_obj_t * cb = lv_checkbox_create(list);
        lv_checkbox_set_text(cb, menu_btn_list[i].name);
        if (app_hidden_status[i])
        {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(cb, hidden_app_event_handler, LV_EVENT_VALUE_CHANGED, (void*)i);
    }

    scr_back_btn_create(parent, ("Hidden Apps"), scr2_2_btn_event_cb);
}
static void entry2_2(void)
{
}
static void exit2_2(void)
{
}
static void destroy2_2(void)
{
}

static scr_lifecycle_t screen2_2 = {
    .create = create2_2,
    .entry = entry2_2,
    .exit  = exit2_2,
    .destroy = destroy2_2,
};
#endif
// --------------------- screen 2 --------------------- Setting
#if 1
static lv_obj_t *setting_list;
static lv_group_t *setting_group = NULL;
static lv_obj_t *setting_page;
static int setting_num = 0;
static int setting_page_num = 0;
static int setting_curr_page = 0;
static ui_setting_handle setting_handle_list[] = {
    {.name = "- WIFI Setup",     .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN4_ID, .shortcut = 'w'},
    {.name = "- System Font",    .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN13_2_ID, .shortcut = 'f'},
    {.name = "Red LED",          .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_red_led,      .get_cb = ui_setting_get_red_led, .shortcut = 'r'},
    {.name = "Keypad Backlight", .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_keypad_light, .get_cb = ui_setting_get_keypad_light, .shortcut = 'b'},
    {.name = "Motor Status",     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_motor_status, .get_cb = ui_setting_get_motor_status, .shortcut = 'm'},
    {.name = "Power GPS",        .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gps_status,   .get_cb = ui_setting_get_gps_status, .shortcut = 'g'},
    {.name = "Power Lora",       .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_lora_status,  .get_cb = ui_setting_get_lora_status, .shortcut = 'l'},
    {.name = "Power Gyro",       .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gyro_status,  .get_cb = ui_setting_get_gyro_status, .shortcut = 'y'},
    {.name = "Power A7682",      .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_a7682_status, .get_cb = ui_setting_get_a7682_status, .shortcut = 'a'},
    {.name = "Touchscreen",      .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_touch_status, .get_cb = ui_setting_get_touch_status, .shortcut = 't'},
    {.name = "- Lora",           .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN1_ID, .shortcut = 's'},
    {.name = "- GPS",            .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN3_ID, .shortcut = 'p'},
    {.name = "- Test",           .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN5_ID, .shortcut = 'x'},
    {.name = "- Battery",        .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN6_ID, .shortcut = 'u'},
    {.name = "- Input",          .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN7_ID, .shortcut = 'i'},
    {.name = "- A7682E",         .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN8_ID, .shortcut = 'e'},
    {.name = "- PCM5102",        .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN10_ID, .shortcut = 'c'},
    {.name = "- USB SD Mount",   .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN_USB_MSC_ID, .shortcut = 'v'},
    {.name = "- Shutdown",       .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN9_ID, .shortcut = 'h'},
    {.name = "- Sleep",          .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN11_ID, .shortcut = 'd'},
    {.name = "- About System",   .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_1_ID, .shortcut = 'z'},
};

static void setting_item_create(int curr_apge);

static void scr2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void setting_scr_event(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)e->target;
    ui_setting_handle *h = (ui_setting_handle *)e->user_data;

    if(e->code == LV_EVENT_CLICKED) {
        switch (h->type)
        {
        case UI_SETTING_TYPE_SW:
            h->set_cb(!h->get_cb());
            lv_label_set_text_fmt(h->st, "%s", (h->get_cb() ? "ON" : "OFF"));
            break;
        case UI_SETTING_TYPE_SUB:
            scr_mgr_push(h->sub_id, false);
            break;
        default:
            break;
        }
    }
}

static void setting_apply_focus_style(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, DECKPRO_COLOR_BG, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
}

static void setting_page_switch_internal(int target_page)
{
    if(setting_num <= SETTING_PAGE_MAX_ITEM && target_page != 0) return;

    if (setting_group) lv_group_remove_all_objs(setting_group);

    int child_cnt = lv_obj_get_child_cnt(setting_list);

    for(int i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(setting_list, 0);
        if(child)
            lv_obj_del(child);
    }

    setting_curr_page = target_page;

    setting_item_create(setting_curr_page);
    if (setting_page) {
        lv_label_set_text_fmt(setting_page, "%d / %d", setting_curr_page + 1, setting_page_num + 1);
    }
}

static void setting_page_switch_relative(char opt)
{
    int target = setting_curr_page;
    if(opt == 'p')
    {
        target = (setting_curr_page < setting_page_num) ? setting_curr_page + 1 : 0;
    }
    else if(opt == 'n')
    {
        target = (setting_curr_page > 0) ? setting_curr_page - 1 : setting_page_num;
    }
    setting_page_switch_internal(target);
}

static void setting_page_switch_cb(lv_event_t *e)
{
    char opt = (int)e->user_data;
    setting_page_switch_relative(opt);
}

static void setting_item_create(int curr_apge)
{
    printf("setting_curr_page = %d\n", setting_curr_page);
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > setting_num) end = setting_num;

    printf("start=%d, end=%d\n", start, end);

    for(int i = start; i < end; i++) {
        ui_setting_handle *h = &setting_handle_list[i];
        
        char full_name[64];
        if (h->shortcut) {
            snprintf(full_name, sizeof(full_name), "[%c] %s", h->shortcut, h->name);
        } else {
            strncpy(full_name, h->name, sizeof(full_name));
        }

        switch (h->type)
        {
        case UI_SETTING_TYPE_SW:
            h->obj = lv_list_add_btn(setting_list, NULL, full_name);
            h->st = lv_label_create(h->obj);
            lv_obj_set_style_text_font(h->st, FONT_BOLD_SIZE_14, LV_PART_MAIN);
            lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_label_set_text_fmt(h->st, "%s", (h->get_cb() ? "ON" : "OFF"));
            // Ensure status label also changes color on focus
            lv_obj_set_style_text_color(h->st, DECKPRO_COLOR_BG, LV_PART_MAIN | LV_STATE_FOCUSED);
            break;
        case UI_SETTING_TYPE_SUB:
            h->obj = lv_list_add_btn(setting_list, NULL, full_name);
            break;
        default:
            break;
        }

        // style
        lv_obj_set_height(h->obj, 18); // Ultra-compact height
        lv_obj_set_style_pad_left(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_right(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_text_font(h->obj, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(h->obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(h->obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT); // No border for cleaner look
        lv_obj_set_style_radius(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT); // Square edges for list items
        lv_obj_add_event_cb(h->obj, setting_scr_event, LV_EVENT_CLICKED, (void *)h);

        setting_apply_focus_style(h->obj);
        if (setting_group) lv_group_add_obj(setting_group, h->obj);
    }
    if (setting_group) {
        lv_obj_t *first = lv_obj_get_child(setting_list, 0);
        if (first) lv_group_focus_obj(first);
    }
}

static void create2(lv_obj_t *parent) 
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;

    if (!setting_group) {
        setting_group = lv_group_create();
        lv_group_set_wrap(setting_group, false); // Disable wrap to detect boundaries
    }

    setting_list = lv_list_create(parent);
    lv_obj_set_size(setting_list, LV_HOR_RES, LV_VER_RES - status_bar_height);
    lv_obj_align(setting_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(setting_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(setting_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(setting_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(setting_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(setting_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(setting_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(setting_list, 0, LV_PART_MAIN);

    setting_num = sizeof(setting_handle_list) / sizeof(setting_handle_list[0]);
    setting_page_num = setting_num / SETTING_PAGE_MAX_ITEM;

    setting_item_create(setting_curr_page);
}
static lv_timer_t *settings_kb_timer = NULL;
static void settings_kb_timer_cb(lv_timer_t *t) {
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to home
            scr_mgr_pop(false);
            return;
        } else if (key == 'j') {
            if (setting_group) {
                lv_obj_t *old_f = lv_group_get_focused(setting_group);
                lv_group_focus_next(setting_group);
                lv_obj_t *new_f = lv_group_get_focused(setting_group);
                if (old_f == new_f) { // At the bottom
                    setting_page_switch_relative('p');
                    return; // Exit: list was rebuilt
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'k') {
            if (setting_group) {
                lv_obj_t *old_f = lv_group_get_focused(setting_group);
                lv_group_focus_prev(setting_group);
                lv_obj_t *new_f = lv_group_get_focused(setting_group);
                if (old_f == new_f) { // At the top
                    setting_page_switch_relative('n');
                    // Invert: focus the last item of the previous page
                    lv_obj_t *last = lv_obj_get_child(setting_list, lv_obj_get_child_cnt(setting_list) - 1);
                    if (last) lv_group_focus_obj(last);
                    return; // Exit: list was rebuilt
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'E') {
            if (setting_group) {
                lv_obj_t *f = lv_group_get_focused(setting_group);
                if (f) lv_event_send(f, LV_EVENT_CLICKED, NULL);
            }
        } else {
            // Check for shortcuts
            for (int i = 0; i < setting_num; i++) {
                if (setting_handle_list[i].shortcut == key) {
                    int target_page = i / SETTING_PAGE_MAX_ITEM;
                    if (target_page != setting_curr_page) {
                        setting_page_switch_internal(target_page);
                    }
                    // Find the object in the list
                    int idx_on_page = i % SETTING_PAGE_MAX_ITEM;
                    lv_obj_t *obj = lv_obj_get_child(setting_list, idx_on_page);
                    if (obj) {
                        lv_group_focus_obj(obj);
                        lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                        lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
                    }
                    return;
                }
            }
        }
    }
}
static void entry2(void) {
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    if (!settings_kb_timer) settings_kb_timer = lv_timer_create(settings_kb_timer_cb, 20, NULL);
    ui_disp_full_refr();
}
static void exit2(void) {
    lv_timer_pause(taskbar_update_timer);
    if (settings_kb_timer) {
        lv_timer_del(settings_kb_timer);
        settings_kb_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy2(void) {
    if (setting_group) {
        lv_group_del(setting_group);
        setting_group = NULL;
    }
}

static scr_lifecycle_t screen2 = {
    .create = create2,
    .entry = entry2,
    .exit  = exit2,
    .destroy = destroy2,
};
#endif
//************************************[ screen 3 ]****************************************** GPS
#if 1
#define line_max 23
static lv_obj_t *scr3_cont;
static lv_obj_t *scr3_cnt_lab;
static lv_timer_t *GPS_loop_timer = NULL;

static void gps_set_line(lv_obj_t *label, const char *str1, const char *str2)
{
    int w2 = strlen(str2);
    int w1 = line_max - w2;
    lv_label_set_text_fmt(label, "%-*s%-*s", w1, str1, w2, str2);
}

static lv_obj_t * scr3_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

static void scr3_GPS_updata(void)
{
    double lat      = 0; // Latitude
    double lon      = 0; // Longitude
    double speed    = 0; // Speed over ground
    float alt      = 0; // Altitude
    float accuracy = 0; // Accuracy
    uint32_t   vsat     = 0; // Visible Satellites
    int   usat     = 0; // Used Satellites
    uint16_t   year     = 0; // 
    uint8_t   month    = 0; // 
    uint8_t   day      = 0; // 
    uint8_t   hour     = 0; // 
    uint8_t   min      = 0; // 
    uint8_t   sec      = 0; // 

    static int cnt = 0;

    lv_label_set_text_fmt(scr3_cnt_lab, " %05d ", ++cnt);

    ui_gps_get_coord(&lat, &lon);
    ui_gps_get_data(&year, &month, &day);
    ui_gps_get_time(&hour, &min, &sec);
    ui_gps_get_satellites(&vsat);
    ui_gps_get_speed(&speed);

    char buf[32];

    lv_snprintf(buf, 16, "%0.1f", lat);
    gps_set_line(label_list[0], "Latitude:", buf);

    lv_snprintf(buf, 16, "%0.1f", lon);
    gps_set_line(label_list[1], "Longitude:", buf);

    lv_snprintf(buf, 16, "%0.3fkmph", speed);
    gps_set_line(label_list[2], "Speed:", buf);

    lv_snprintf(buf, 16, "%d", vsat);
    gps_set_line(label_list[3], "vsat:", buf);
    
    lv_snprintf(buf, 16, "%d", year);
    gps_set_line(label_list[4], "year:", buf);

    lv_snprintf(buf, 16, "%d", month);
    gps_set_line(label_list[5], "month:", buf);

    lv_snprintf(buf, 16, "%d", day);
    gps_set_line(label_list[6], "day:", buf);

    lv_snprintf(buf, 16, "%02d:%02d:%02d", hour, min, sec);
    gps_set_line(label_list[7], "time:", buf);

    // lv_snprintf(buf, 16, "%0.1f", alt);
    // gps_set_line(label_list[3], "alt:", buf);

    // lv_snprintf(buf, 16, "%d", usat);
    // gps_set_line(label_list[5], "usat:", buf);

}

static void GPS_loop_timer_event(lv_timer_t * t)
{
    scr3_GPS_updata();
}

static void scr3_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create3(lv_obj_t *parent) 
{   
    scr3_cont = lv_obj_create(parent);
    lv_obj_set_size(scr3_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr3_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr3_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr3_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr3_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_align(scr3_cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(scr3_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr3_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = scr3_create_label(scr3_cont);
        lv_label_set_text(label_list[i], " ");
    }

    scr3_cnt_lab = lv_label_create(parent);
    lv_obj_set_style_text_font(scr3_cnt_lab, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr3_cnt_lab, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr3_cnt_lab, 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr3_cnt_lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(scr3_cnt_lab, " %05d ", 0);
    lv_obj_center(scr3_cnt_lab);
    lv_obj_align(scr3_cnt_lab, LV_ALIGN_TOP_RIGHT, -10, 10);

    lv_obj_t *back3_label = scr_back_btn_create(parent, ("GPS"), scr3_btn_event_cb);
}
static void entry3(void) 
{
    scr3_GPS_updata();

    ui_gps_task_resume();

    GPS_loop_timer = lv_timer_create(GPS_loop_timer_event, 3000, NULL);
    ui_disp_full_refr();
}
static void exit3(void) {
    ui_gps_task_suspend();
    if(GPS_loop_timer) {
        lv_timer_del(GPS_loop_timer);
        GPS_loop_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy3(void) { }

static scr_lifecycle_t screen3 = {
    .create = create3,
    .entry = entry3,
    .exit  = exit3,
    .destroy = destroy3,
};

#undef line_max

#endif
//************************************[ screen 4 ]****************************************** Wifi Scan + Pick
// --------------------- screen 4 --------------------- WIFI: scan-and-select picker
//
// Flow: settings -> WIFI -> SCREEN4 (this screen) shows scanned networks.
// j/k cycles selection, E selects:
//   - open network: connect immediately, pop back to settings.
//   - encrypted network: push SCREEN4_1 (password prompt).
// Esc backs out. The list is rescanned when the screen entry runs (with a
// brief "Scanning..." placeholder while the blocking call returns).
#if 1
static lv_obj_t *scr4_list = NULL;
static lv_obj_t *scr4_status_lab = NULL;
static lv_timer_t *scr4_kb_timer = NULL;
static lv_timer_t *scr4_scan_timer = NULL;
static int scr4_focus = 0;
static int scr4_count = 0;
static ui_wifi_scan_info_t scr4_results[UI_WIFI_SCAN_ITEM_MAX];

// Selection handed off to SCREEN4_1 when the user picks an encrypted SSID.
static char scr4_selected_ssid[33] = {0};

static void scr4_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void scr4_render_list(void)
{
    if (!scr4_list) return;

    // Drop and rebuild — simpler than reconciling per-row state across rescans.
    int child_cnt = lv_obj_get_child_cnt(scr4_list);
    for (int i = 0; i < child_cnt; i++) {
        lv_obj_t *c = lv_obj_get_child(scr4_list, 0);
        if (c) lv_obj_del(c);
    }

    if (scr4_count == 0) {
        lv_obj_t *btn = lv_list_add_btn(scr4_list, NULL, "(no networks)");
        lv_obj_set_style_text_font(btn, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < scr4_count; i++) {
        char buf[48];
        lv_snprintf(buf, sizeof(buf), "%s%-20.20s %4d",
                    scr4_results[i].open ? " " : "*",
                    scr4_results[i].name, scr4_results[i].rssi);
        lv_obj_t *btn = lv_list_add_btn(scr4_list, NULL, buf);
        lv_obj_set_style_text_font(btn, FONT_BOLD_MONO_SIZE_14, LV_PART_MAIN);
        lv_obj_set_height(btn, 28);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_width(btn, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(btn, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
    }
}

static void scr4_apply_focus(void)
{
    if (!scr4_list) return;
    int n = lv_obj_get_child_cnt(scr4_list);
    if (n == 0) return;
    if (scr4_focus < 0)  scr4_focus = 0;
    if (scr4_focus >= n) scr4_focus = n - 1;
    for (int i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr4_list, i);
        if (i == scr4_focus) {
            lv_obj_add_state(c, LV_STATE_FOCUSED);
            lv_obj_scroll_to_view(c, LV_ANIM_OFF);
        } else {
            lv_obj_clear_state(c, LV_STATE_FOCUSED);
        }
    }
}

static void scr4_set_status(const char *text)
{
    if (scr4_status_lab) lv_label_set_text(scr4_status_lab, text);
}

static void scr4_select_focused(void)
{
    if (scr4_count == 0) return;
    if (scr4_focus < 0 || scr4_focus >= scr4_count) return;

    const ui_wifi_scan_info_t *r = &scr4_results[scr4_focus];
    strncpy(scr4_selected_ssid, r->name, sizeof(scr4_selected_ssid) - 1);
    scr4_selected_ssid[sizeof(scr4_selected_ssid) - 1] = '\0';

    if (r->open) {
        // Open network: save SSID, blank password, connect, pop back.
        ui_wifi_set_ssid(scr4_selected_ssid);
        ui_wifi_set_password("");
        ui_wifi_set_enabled(true);
        scr_mgr_pop(false);
    } else {
        scr_mgr_push(SCREEN4_1_ID, false);
    }
}

static void scr4_scan_timer_cb(lv_timer_t *t);  // forward decl

static void scr4_start_scan(void)
{
    scr4_count = 0;
    scr4_focus = 0;
    scr4_render_list();
    scr4_set_status("Scanning...");
    ui_disp_full_refr();
    if (!scr4_scan_timer) scr4_scan_timer = lv_timer_create(scr4_scan_timer_cb, 50, NULL);
}

static void scr4_kb_timer_cb(lv_timer_t *t)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { scr_mgr_pop(false); return; }
        if (key == 'r') {
            scr4_start_scan();
            return; // list rebuilt async; abandon this tick
        }
        if (scr4_count == 0) continue;
        if (key == 'j') {
            scr4_focus = (scr4_focus + 1) % scr4_count;
            scr4_apply_focus();
        } else if (key == 'k') {
            scr4_focus = (scr4_focus - 1 + scr4_count) % scr4_count;
            scr4_apply_focus();
        } else if (key == 'E') {
            scr4_select_focused();
            return;
        }
    }
}

// Run the (blocking) scan one tick after entry so the "Scanning..." label
// actually paints first.
static void scr4_scan_timer_cb(lv_timer_t *t)
{
    lv_timer_del(scr4_scan_timer);
    scr4_scan_timer = NULL;

    memset(scr4_results, 0, sizeof(scr4_results));
    ui_wifi_get_scan_info(scr4_results, UI_WIFI_SCAN_ITEM_MAX);

    scr4_count = 0;
    for (int i = 0; i < UI_WIFI_SCAN_ITEM_MAX; i++) {
        if (scr4_results[i].name[0] != '\0') scr4_count++;
        else break;
    }

    char status[64];
    lv_snprintf(status, sizeof(status), "%d nets  j/k move  E pick  r rescan", scr4_count);
    scr4_set_status(status);

    scr4_focus = 0;
    scr4_render_list();
    scr4_apply_focus();
    ui_disp_full_refr();
}

static void create4(lv_obj_t *parent)
{
    scr4_list = lv_list_create(parent);
    lv_obj_set_size(scr4_list, lv_pct(100), lv_pct(80));
    lv_obj_align(scr4_list, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_pad_top(scr4_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr4_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(scr4_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr4_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr4_list, 0, LV_PART_MAIN);

    scr4_status_lab = lv_label_create(parent);
    lv_obj_set_width(scr4_status_lab, lv_pct(100));
    lv_obj_set_style_text_font(scr4_status_lab, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_align(scr4_status_lab, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_label_set_text(scr4_status_lab, "Scanning...");

    scr_back_btn_create(parent, "WiFi", scr4_btn_event_cb);
}

static void entry4(void)
{
    scr4_count = 0;
    scr4_focus = 0;
    scr4_render_list();
    scr4_set_status("Scanning...");
    ui_disp_full_refr();
    if (!scr4_kb_timer)   scr4_kb_timer   = lv_timer_create(scr4_kb_timer_cb, 30, NULL);
    if (!scr4_scan_timer) scr4_scan_timer = lv_timer_create(scr4_scan_timer_cb, 50, NULL);
}

static void exit4(void)
{
    if (scr4_kb_timer)   { lv_timer_del(scr4_kb_timer);   scr4_kb_timer = NULL; }
    if (scr4_scan_timer) { lv_timer_del(scr4_scan_timer); scr4_scan_timer = NULL; }
    ui_disp_full_refr();
}

static void destroy4(void)
{
    scr4_list = NULL;
    scr4_status_lab = NULL;
}

static scr_lifecycle_t screen4 = {
    .create = create4,
    .entry = entry4,
    .exit  = exit4,
    .destroy = destroy4,
};

const char *ui_wifi_get_selected_ssid(void) { return scr4_selected_ssid; }
#endif
// --------------------- screen 4.1 --------------------- Wifi password prompt
//
// Reached only from the scan picker (SCREEN4) when the user picks an
// encrypted network. SCREEN4 stashes the chosen SSID in scr4_selected_ssid
// before pushing here. We just collect the password and connect.
//
// Layout: SSID readout (top), password textarea, status line, Connect button.
// 'E' from the password field or button triggers connect-and-pop.
// Esc backs out to the scan list.
#if 1
static lv_obj_t *scr4_1_ssid_lab = NULL;
static lv_obj_t *scr4_1_pass_ta = NULL;
static lv_obj_t *scr4_1_conn_btn = NULL;
static lv_obj_t *scr4_1_conn_btn_lab = NULL;
static lv_obj_t *scr4_1_status_lab = NULL;
static lv_timer_t *scr4_1_kb_timer = NULL;
static lv_timer_t *scr4_1_status_timer = NULL;
// 0 = password textarea, 1 = connect button.
static int scr4_1_focus = 0;

// Local buffer is the source of truth for the typed password — we re-render
// the textarea after each mutation so the displayed text can never disagree
// with what's actually being saved.
static char scr4_1_pass_buf[65] = {0};

static void scr4_1_render_pass(void)
{
    if (scr4_1_pass_ta) lv_textarea_set_text(scr4_1_pass_ta, scr4_1_pass_buf);
}

static void scr4_1_apply_focus(void)
{
    lv_obj_t *items[2] = { scr4_1_pass_ta, scr4_1_conn_btn };
    for (int i = 0; i < 2; i++) {
        if (!items[i]) continue;
        lv_obj_set_style_border_width(items[i], (i == scr4_1_focus) ? 3 : 1, LV_PART_MAIN);
        if (i == scr4_1_focus) lv_obj_add_state(items[i], LV_STATE_FOCUSED);
        else                    lv_obj_clear_state(items[i], LV_STATE_FOCUSED);
    }
}

static void scr4_1_refresh_status(void)
{
    if (!scr4_1_status_lab) return;
    int st = ui_wifi_get_status();
    char ip[32] = {0};
    ui_wifi_get_ip(ip, sizeof(ip));

    const char *label;
    switch (st) {
        case UI_WIFI_STATUS_DISABLED:   label = "off";        break;
        case UI_WIFI_STATUS_IDLE:       label = "idle";       break;
        case UI_WIFI_STATUS_CONNECTING: label = "connecting"; break;
        case UI_WIFI_STATUS_CONNECTED:  label = "connected";  break;
        case UI_WIFI_STATUS_FAILED:     label = "failed";     break;
        default:                        label = "?";          break;
    }

    char buf[80];
    if (ip[0]) lv_snprintf(buf, sizeof(buf), "%s   %s", label, ip);
    else       lv_snprintf(buf, sizeof(buf), "%s", label);
    lv_label_set_text(scr4_1_status_lab, buf);

    if (scr4_1_conn_btn_lab) {
        lv_label_set_text(scr4_1_conn_btn_lab,
                          (st == UI_WIFI_STATUS_CONNECTED) ? "Disconnect" : "Connect");
    }
}

static void scr4_1_status_timer_cb(lv_timer_t *t)
{
    scr4_1_refresh_status();
    // Once we land on connected, drop back to settings so the flow ends
    // naturally. Stay put on failure so the user can retry the password.
    if (ui_wifi_get_status() == UI_WIFI_STATUS_CONNECTED) {
        scr_mgr_pop(false);
    }
}

static void scr4_1_save_and_connect(void)
{
    extern const char *ui_wifi_get_selected_ssid(void);
    const char *ssid = ui_wifi_get_selected_ssid();
    if (ssid && ssid[0]) ui_wifi_set_ssid(ssid);
    ui_wifi_set_password(scr4_1_pass_buf);
    if (ui_wifi_get_status() == UI_WIFI_STATUS_CONNECTED) {
        ui_wifi_disconnect();
    } else {
        ui_wifi_set_enabled(true);
    }
    scr4_1_refresh_status();
}

static void scr4_1_kb_timer_cb(lv_timer_t *t)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();

        if (key == 0x1B) { // Esc -> back to scan list
            scr_mgr_pop(false);
            return;
        }

        // 'E' on the password field commits and connects (single-line UX so
        // we treat enter as submit). 'E' on the button does the same.
        if (key == 'E') {
            scr4_1_save_and_connect();
            continue;
        }

        if (scr4_1_focus == 0) {
            int len = (int)strlen(scr4_1_pass_buf);
            int max_len = (int)sizeof(scr4_1_pass_buf) - 1;
            if (key == 0x08) {
                if (len > 0) scr4_1_pass_buf[len - 1] = '\0';
                scr4_1_render_pass();
            } else if (key >= 32 && key <= 126) {
                if (len < max_len) {
                    scr4_1_pass_buf[len] = key;
                    scr4_1_pass_buf[len + 1] = '\0';
                    scr4_1_render_pass();
                }
            }
        }
    }
}

static void scr4_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void scr4_1_conn_btn_event_cb(lv_event_t *e)
{
    if (e->code == LV_EVENT_CLICKED) {
        scr4_1_save_and_connect();
    }
}

static void create4_1(lv_obj_t *parent)
{
    // Reset password buffer — never inherit a stale value from a previous
    // attempt; the user is responding to a fresh SSID pick.
    memset(scr4_1_pass_buf, 0, sizeof(scr4_1_pass_buf));

    extern const char *ui_wifi_get_selected_ssid(void);
    const char *ssid = ui_wifi_get_selected_ssid();

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(88));
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 6, LV_PART_MAIN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // SSID readout (read-only)
    scr4_1_ssid_lab = lv_label_create(cont);
    lv_obj_set_width(scr4_1_ssid_lab, lv_pct(100));
    lv_obj_set_style_text_font(scr4_1_ssid_lab, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_label_set_long_mode(scr4_1_ssid_lab, LV_LABEL_LONG_DOT);
    char ssid_buf[64];
    lv_snprintf(ssid_buf, sizeof(ssid_buf), "SSID: %s", (ssid && ssid[0]) ? ssid : "(none)");
    lv_label_set_text(scr4_1_ssid_lab, ssid_buf);

    // Password textarea
    scr4_1_pass_ta = lv_textarea_create(cont);
    lv_textarea_set_one_line(scr4_1_pass_ta, true);
    lv_textarea_set_password_mode(scr4_1_pass_ta, true);
    lv_obj_set_width(scr4_1_pass_ta, lv_pct(100));
    lv_obj_set_height(scr4_1_pass_ta, 32);
    lv_obj_set_style_text_font(scr4_1_pass_ta, FONT_BOLD_MONO_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr4_1_pass_ta, 4, LV_PART_MAIN);
    lv_obj_clear_flag(scr4_1_pass_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    // Connect button
    scr4_1_conn_btn = lv_btn_create(cont);
    lv_obj_set_size(scr4_1_conn_btn, lv_pct(100), 36);
    lv_obj_set_style_bg_color(scr4_1_conn_btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr4_1_conn_btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr4_1_conn_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr4_1_conn_btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr4_1_conn_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(scr4_1_conn_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(scr4_1_conn_btn, scr4_1_conn_btn_event_cb, LV_EVENT_CLICKED, NULL);
    scr4_1_conn_btn_lab = lv_label_create(scr4_1_conn_btn);
    lv_label_set_text(scr4_1_conn_btn_lab, "Connect");
    lv_obj_center(scr4_1_conn_btn_lab);

    // Status panel below
    scr4_1_status_lab = lv_label_create(cont);
    lv_obj_set_width(scr4_1_status_lab, lv_pct(100));
    lv_obj_set_style_text_font(scr4_1_status_lab, FONT_BOLD_MONO_SIZE_14, LV_PART_MAIN);
    lv_label_set_long_mode(scr4_1_status_lab, LV_LABEL_LONG_WRAP);
    lv_label_set_text(scr4_1_status_lab, "");

    scr_back_btn_create(parent, "Password", scr4_1_btn_event_cb);
}

static void entry4_1(void)
{
    ui_disp_full_refr();
    scr4_1_focus = 0;
    scr4_1_apply_focus();
    scr4_1_refresh_status();
    if (!scr4_1_kb_timer)     scr4_1_kb_timer     = lv_timer_create(scr4_1_kb_timer_cb,    20,   NULL);
    if (!scr4_1_status_timer) scr4_1_status_timer = lv_timer_create(scr4_1_status_timer_cb, 1000, NULL);
}
static void exit4_1(void) {
    if (scr4_1_kb_timer)     { lv_timer_del(scr4_1_kb_timer);     scr4_1_kb_timer     = NULL; }
    if (scr4_1_status_timer) { lv_timer_del(scr4_1_status_timer); scr4_1_status_timer = NULL; }
    ui_disp_full_refr();
}
static void destroy4_1(void) {
    scr4_1_ssid_lab = NULL;
    scr4_1_pass_ta = NULL;
    scr4_1_conn_btn = NULL;
    scr4_1_conn_btn_lab = NULL;
    scr4_1_status_lab = NULL;
}

static scr_lifecycle_t screen4_1 = {
    .create = create4_1,
    .entry = entry4_1,
    .exit  = exit4_1,
    .destroy = destroy4_1,
};
#endif
// --------------------- screen 4.2 --------------------- Wifi Scan
#if 1
static lv_obj_t *scr4_2_cont;
static lv_obj_t *wifi_scan_lab;
static lv_timer_t *wifi_scan_timer = NULL;

static ui_wifi_scan_info_t wifi_info_list[UI_WIFI_SCAN_ITEM_MAX];

static void scr4_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void show_wifi_scan(void)
{
#define BUFF_LEN 400
    char buf[BUFF_LEN];
    int ret = 0, offs = 0;

    ret = lv_snprintf(buf + offs, BUFF_LEN, "       NAME      | RSSI\n");
    offs = offs + ret;
    ret = lv_snprintf(buf + offs, BUFF_LEN, "-----------------------\n");
    offs = offs + ret;

    for(int i = 0; i < UI_WIFI_SCAN_ITEM_MAX; i++) {
        if(strcmp(wifi_info_list[i].name, "") == 0 && wifi_info_list[i].rssi == 0)
        {
            break;
        }
        if(i == UI_WIFI_SCAN_ITEM_MAX - 1) {
            ret = lv_snprintf(buf + offs, BUFF_LEN, "%-16.16s | %4d", wifi_info_list[i].name, wifi_info_list[i].rssi);
            break;
        }

        ret = lv_snprintf(buf + offs, BUFF_LEN, "%-16.16s | %4d\n", wifi_info_list[i].name, wifi_info_list[i].rssi);
        offs = offs + ret;
    }
    lv_label_set_text(wifi_scan_lab, buf);
#undef BUFF_LEN
}

static void wifi_scan_timer_event(lv_timer_t *t)
{
    ui_wifi_get_scan_info(wifi_info_list, UI_WIFI_SCAN_ITEM_MAX);
    show_wifi_scan();
}

static void create4_2(lv_obj_t *parent) 
{
    scr4_2_cont = lv_obj_create(parent);
    lv_obj_set_size(scr4_2_cont, lv_pct(100), lv_pct(90));
    lv_obj_set_style_bg_color(scr4_2_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr4_2_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr4_2_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr4_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr4_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(scr4_2_cont, 13, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr4_2_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr4_2_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr4_2_cont, 5, LV_PART_MAIN);
    lv_obj_set_align(scr4_2_cont, LV_ALIGN_BOTTOM_MID);

    wifi_scan_lab = lv_label_create(scr4_2_cont);
    lv_obj_set_width(wifi_scan_lab, lv_pct(95));
    lv_obj_set_style_pad_all(wifi_scan_lab, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi_scan_lab, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(wifi_scan_lab, 0, LV_PART_MAIN);
    lv_label_set_long_mode(wifi_scan_lab, LV_LABEL_LONG_WRAP);

    lv_obj_t *back4_label = scr_back_btn_create(parent, ("Wifi"), scr4_2_btn_event_cb);
}
static void entry4_2(void) 
{
    ui_disp_full_refr();
    wifi_scan_timer = lv_timer_create(wifi_scan_timer_event, 10000, NULL);
    lv_timer_ready(wifi_scan_timer);
}
static void exit4_2(void) {
    ui_disp_full_refr();
    if(wifi_scan_timer) {
        lv_timer_del(wifi_scan_timer);
        wifi_scan_timer = NULL;
    }
}

static void destroy4_2(void) { }

static scr_lifecycle_t screen4_2 = {
    .create = create4_2,
    .entry = entry4_2,
    .exit  = exit4_2,
    .destroy = destroy4_2,
};
#endif
//************************************[ screen 5 ]****************************************** Test
#if 1
static lv_obj_t *test_list;
static lv_obj_t *test_page;
static int test_num = 0;
static int test_page_num = 0;
static int test_curr_page = 0;

static ui_test_handle test_handle_list[] = {
    { .name="Lora",       .peri_id=E_PERI_LORA       , .cb=ui_test_get },
    { .name="Touch",      .peri_id=E_PERI_TOUCH      , .cb=ui_test_get },
    { .name="BQ25896",    .peri_id=E_PERI_BQ25896    , .cb=ui_test_get },
    { .name="BQ27220",    .peri_id=E_PERI_BQ27220    , .cb=ui_test_get },
    { .name="SD Card",    .peri_id=E_PERI_SD         , .cb=ui_test_get },
    { .name="A7682E",     .peri_id=E_PERI_A7682E     , .cb=ui_test_get },
    { .name="PCM5102A",   .peri_id=E_PERI_PCM5102A   , .cb=ui_test_get },
    { .name="Keypad",     .peri_id=E_PERI_KYEPAD     , .cb=ui_test_get },
    { .name="GPS",        .peri_id=E_PERI_GPS        , .cb=ui_test_get },
    { .name="BHI260AP",   .peri_id=E_PERI_BHI260AP   , .cb=ui_test_get },
    { .name="LTR_553ALS", .peri_id=E_PERI_LTR_553ALS , .cb=ui_test_get },
    { .name="INK_SCREEN", .peri_id=E_PERI_INK_SCREEN , .cb=ui_test_get },
};

static void test_item_create(int curr_apge);

static void scr5_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static lv_group_t *test_group = NULL;
static lv_timer_t *test_kb_timer = NULL;

static void test_page_switch_internal(char opt)
{
    if(test_num <= SETTING_PAGE_MAX_ITEM) return;

    if (test_group) lv_group_remove_all_objs(test_group);

    int child_cnt = lv_obj_get_child_cnt(test_list);

    for(int i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(test_list, 0);
        if(child)
            lv_obj_del(child);
    }

    if(opt == 'p')
    {
        test_curr_page = (test_curr_page < test_page_num) ? test_curr_page + 1 : 0;
    }
    else if(opt == 'n')
    {
        test_curr_page = (test_curr_page > 0) ? test_curr_page - 1 : test_page_num;
    }

    test_item_create(test_curr_page);
    lv_label_set_text_fmt(test_page, "%d / %d", test_curr_page + 1, test_page_num + 1);
}

static void test_page_switch_cb(lv_event_t *e)
{
    char opt = (int)e->user_data;
    test_page_switch_internal(opt);
}

static void test_item_create(int curr_apge)
{
    printf("test_curr_page = %d\n", test_curr_page);
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > test_num) end = test_num;

    printf("start=%d, end=%d\n", start, end);

    for(int i = start; i < end; i++) {
        ui_test_handle *h = &test_handle_list[i];
        h->obj = lv_list_add_btn(test_list, NULL, h->name);
        h->st = lv_label_create(h->obj);
        lv_obj_set_style_text_font(h->st, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_label_set_text_fmt(h->st, "%s", (h->cb(h->peri_id) ? "PASS" : "----"));
        // Ensure status label also changes color on focus
        lv_obj_set_style_text_color(h->st, DECKPRO_COLOR_BG, LV_PART_MAIN | LV_STATE_FOCUSED);
        // style
        lv_obj_set_style_text_font(h->obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_obj_set_height(h->obj, 18);
        lv_obj_set_style_pad_left(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_right(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(h->obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(h->obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        setting_apply_focus_style(h->obj);
        if (test_group) lv_group_add_obj(test_group, h->obj);
    }
    if (test_group) {
        lv_obj_t *first = lv_obj_get_child(test_list, 0);
        if (first) lv_group_focus_obj(first);
    }
}

static void create5(lv_obj_t *parent) 
{
    if (!test_group) {
        test_group = lv_group_create();
        lv_group_set_wrap(test_group, false);
    }
    test_list = lv_list_create(parent);
    lv_obj_set_size(test_list, LV_HOR_RES, lv_pct(88));
    lv_obj_align(test_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(test_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(test_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(test_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(test_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(test_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(test_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(test_list, 0, LV_PART_MAIN);

    test_num = sizeof(test_handle_list) / sizeof(test_handle_list[0]);
    test_page_num = test_num / SETTING_PAGE_MAX_ITEM;
    test_item_create(test_curr_page);

    lv_obj_t * ui_Button2 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button2, 71);
    lv_obj_set_height(ui_Button2, 40);
    lv_obj_set_x(ui_Button2, -70);
    lv_obj_set_y(ui_Button2, 130);
    lv_obj_set_align(ui_Button2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button2, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label1 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label1, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label1, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label1, "Back");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Button14 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button14, 71);
    lv_obj_set_height(ui_Button14, 40);
    lv_obj_set_x(ui_Button14, 70);
    lv_obj_set_y(ui_Button14, 130);
    lv_obj_set_align(ui_Button14, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button14, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button14, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button14, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button14, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button14, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label15 = lv_label_create(ui_Button14);
    lv_obj_set_width(ui_Label15, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label15, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label15, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label15, "Next");
    lv_obj_set_style_text_color(ui_Label15, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label15, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_Button2, test_page_switch_cb, LV_EVENT_CLICKED, (void*)'n');
    lv_obj_add_event_cb(ui_Button14, test_page_switch_cb, LV_EVENT_CLICKED, (void*)'p');

    test_page = lv_label_create(parent);
    lv_obj_set_width(test_page, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(test_page, LV_SIZE_CONTENT);    /// 1
    lv_obj_align(test_page, LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_label_set_text_fmt(test_page, "%d / %d", test_curr_page + 1, test_page_num + 1);
    lv_obj_set_style_text_color(test_page, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(test_page, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back5_label = scr_back_btn_create(parent, ("Test"), scr5_btn_event_cb);
}
static void test_kb_timer_cb(lv_timer_t *t) {
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to settings
            scr_mgr_pop(false);
            return;
        } else if (key == 'j') {
            if (test_group) {
                lv_obj_t *old_f = lv_group_get_focused(test_group);
                lv_group_focus_next(test_group);
                lv_obj_t *new_f = lv_group_get_focused(test_group);
                if (old_f == new_f) { 
                    test_page_switch_internal('p');
                    return;
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'k') {
            if (test_group) {
                lv_obj_t *old_f = lv_group_get_focused(test_group);
                lv_group_focus_prev(test_group);
                lv_obj_t *new_f = lv_group_get_focused(test_group);
                if (old_f == new_f) {
                    test_page_switch_internal('n');
                    lv_obj_t *last = lv_obj_get_child(test_list, lv_obj_get_child_cnt(test_list) - 1);
                    if (last) lv_group_focus_obj(last);
                    return;
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'E') {
            if (test_group) {
                lv_obj_t *f = lv_group_get_focused(test_group);
                if (f) lv_event_send(f, LV_EVENT_CLICKED, NULL);
            }
        }
    }
}
static void entry5(void)
{
    if (!test_kb_timer) test_kb_timer = lv_timer_create(test_kb_timer_cb, 20, NULL);
    ui_disp_full_refr();
}
static void exit5(void) {
    if (test_kb_timer) {
        lv_timer_del(test_kb_timer);
        test_kb_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy5(void) {
    if (test_group) {
        lv_group_del(test_group);
        test_group = NULL;
    }
}

static scr_lifecycle_t screen5 = {
    .create = create5,
    .entry = entry5,
    .exit  = exit5,
    .destroy = destroy5,
};
#endif
//************************************[ screen 6 ]****************************************** Battery
// --------------------- screen 6 --------------------- Battery
#if 1
lv_obj_t * scr6_list;
static lv_obj_t *scr6_lab_buf[20];

static void scr6_list_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    for(int i = 0; i < lv_obj_get_child_cnt(obj); i++) 
    {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if(lv_obj_check_type(child, &lv_label_class)) {
            char *str = lv_label_get_text(child);

            if(strcmp("- BQ25896", str) == 0)
            {
                scr_mgr_push(SCREEN6_1_ID, false);
            }
            if(strcmp("- BQ27220", str) == 0)
            {
                scr_mgr_push(SCREEN6_2_ID, false);
            }
            printf("%s\n", str);
        }
    }
}

static void scr6_item_create(const char *name, lv_event_cb_t cb)
{
    lv_obj_t * obj = lv_obj_class_create_obj(&lv_list_btn_class, scr6_list);
    lv_obj_class_init_obj(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_set_height(obj, LV_VER_RES / 6);
    lv_obj_set_style_text_font(obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(obj, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    // lv_obj_set_style_text_color(obj, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL); 
}

static void scr6_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        // ui_full_refresh();
        scr_mgr_pop(false);
    }
}

static void create6(lv_obj_t *parent) 
{
    scr6_list = lv_list_create(parent);
    lv_obj_set_size(scr6_list, lv_pct(93), lv_pct(91));
    lv_obj_align(scr6_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_obj_set_style_bg_color(scr6_list, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr6_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr6_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(scr6_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr6_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_border_color(scr6_list, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr6_list, 0, LV_PART_MAIN);

    scr6_item_create("- BQ25896", scr6_list_event);
    scr6_item_create("- BQ27220", scr6_list_event);

    // back
    scr_back_btn_create(parent, "Battery", scr6_btn_event_cb);
}

static void entry6(void) 
{
    ui_disp_full_refr();
}
static void exit6(void) {
    ui_disp_full_refr();
}
static void destroy6(void) { }

static scr_lifecycle_t screen6 = {
    .create = create6,
    .entry = entry6,
    .exit  = exit6,
    .destroy = destroy6,
};
#endif
// --------------------- screen 6.1 --------------------- BQ25896
#if 1
#define line_max 23

static lv_timer_t *batt_6_1_timer = NULL;

static void battery_set_line(lv_obj_t *label, const char *str1, const char *str2)
{
    int w2 = strlen(str2);
    int w1 = line_max - w2;
    lv_label_set_text_fmt(label, "%-*s%-*s", w1, str1, w2, str2);
}

static lv_obj_t * scr6_1_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

static void scr6_1_battert_updata(void)
{
    char buf[line_max];

    battery_set_line(label_list[0], "Charging:", (ui_batt_25896_is_chg() == true ? "Charging" : "Not charged"));

    lv_snprintf(buf, line_max, "%.2fV", ui_batt_25896_get_vbus());
    battery_set_line(label_list[1], "VBUS:", buf);

    lv_snprintf(buf, line_max, "%.2fV", ui_batt_25896_get_vsys());
    battery_set_line(label_list[2], "VSYS:", buf);

    lv_snprintf(buf, line_max, "%.2fV", ui_batt_25896_get_vbat());
    battery_set_line(label_list[3], "VBAT:", buf);

    lv_snprintf(buf, line_max, "%.2fv", ui_batt_25896_get_volt_targ());
    battery_set_line(label_list[4], "VOLT Target:", buf);

    lv_snprintf(buf, line_max, "%.2fmA", ui_batt_25896_get_chg_curr());
    battery_set_line(label_list[5], "Charge Curr:", buf);

    lv_snprintf(buf, line_max, "%.2fmA", ui_batt_25896_get_pre_curr());
    battery_set_line(label_list[6], "Prechg Curr:", buf);

    lv_snprintf(buf, line_max, "%s", ui_batt_25896_get_chg_st());
    battery_set_line(label_list[7], "CHG ST:", buf);

    lv_snprintf(buf, line_max, "%s", ui_batt_25896_get_vbus_st());
    battery_set_line(label_list[8], "VBUS Status:", buf);

    lv_snprintf(buf, line_max, "%s", ui_batt_25896_get_ntc_st());
    battery_set_line(label_list[9], " ", buf);
}

static void batt_6_1_updata_timer_event(lv_timer_t *t) 
{
    scr6_1_battert_updata();
}

static void scr6_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create6_1(lv_obj_t *parent) 
{
    lv_obj_t *scr6_1_cont = lv_obj_create(parent);
    lv_obj_set_size(scr6_1_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr6_1_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr6_1_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr6_1_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_1_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_align(scr6_1_cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(scr6_1_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr6_1_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = scr6_1_create_label(scr6_1_cont);
    }

    scr_back_btn_create(parent, ("BQ25896"), scr6_1_btn_event_cb);
}
static void entry6_1(void) 
{
    scr6_1_battert_updata();
    ui_disp_full_refr();
    batt_6_1_timer = lv_timer_create(batt_6_1_updata_timer_event, 5000, NULL);
}
static void exit6_1(void) {
    if(batt_6_1_timer) {
        lv_timer_del(batt_6_1_timer);
        batt_6_1_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy6_1(void) { }

static scr_lifecycle_t screen6_1 = {
    .create = create6_1,
    .entry = entry6_1,
    .exit  = exit6_1,
    .destroy = destroy6_1,
};
#undef line_max

#endif
// --------------------- screen 6.2 --------------------- BQ27220
#if 1

#define line_max 23

static lv_timer_t *batt_6_2_timer = NULL;

static lv_obj_t * scr6_2_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

static void scr6_2_battert_updata(void)
{
    char buf[line_max];

    battery_set_line(label_list[0],"VBUS ST::", (ui_battery_27220_get_input() == true ? "Connected" : "Disonnected"));

    if(ui_battery_27220_get_input() == true ){
        lv_snprintf(buf, line_max, "%s", (ui_battery_27220_get_charge_finish()? "Finsish":"Charging"));
    } else {
        lv_snprintf(buf, line_max, "%s", "Discharge");
    }
    battery_set_line(label_list[1],"Charing ST:", buf);

    lv_snprintf(buf, line_max, "0x%x", ui_battery_27220_get_status());
    battery_set_line(label_list[2],"Battery ST:", buf);

    lv_snprintf(buf, line_max, "%dmV", ui_battery_27220_get_voltage());
    battery_set_line(label_list[3], "Voltage:", buf);

    lv_snprintf(buf, line_max, "%dmA", ui_battery_27220_get_current());
    battery_set_line(label_list[4], "Current:", buf);

    lv_snprintf(buf, line_max, "%.2fC", (float)(ui_battery_27220_get_temperature() / 10.0 - 273.0));
    battery_set_line(label_list[5], "Temperature:", buf);

    lv_snprintf(buf, line_max, "%dmAh", ui_battery_27220_get_remain_capacity());
    battery_set_line(label_list[6], "Cap Remain:", buf);

    lv_snprintf(buf, line_max, "%dmAh", ui_battery_27220_get_full_capacity());
    battery_set_line(label_list[7], "Cap Full:", buf);

    lv_snprintf(buf, line_max, "%d%%", ui_battery_27220_get_percent());
    battery_set_line(label_list[8], "Cap Percent:", buf);

    lv_snprintf(buf, line_max, "%d%%", ui_battery_27220_get_health());
    battery_set_line(label_list[9], "CapHealth:", buf);
}

static void batt_6_2_updata_timer_event(lv_timer_t *t) 
{
    scr6_2_battert_updata();
}

static void scr6_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create6_2(lv_obj_t *parent) 
{   
    lv_obj_t *scr6_2_cont = lv_obj_create(parent);
    lv_obj_set_size(scr6_2_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr6_2_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr6_2_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr6_2_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_2_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_align(scr6_2_cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(scr6_2_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr6_2_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = scr6_2_create_label(scr6_2_cont);
    }
    // back
    scr_back_btn_create(parent, ("BQ27220"), scr6_btn_event_cb);
}

static void entry6_2(void) 
{
    scr6_2_battert_updata();
    ui_disp_full_refr();
    batt_6_2_timer = lv_timer_create(batt_6_2_updata_timer_event, 5000, NULL);
}
static void exit6_2(void) {
    if(batt_6_2_timer) {
        lv_timer_del(batt_6_2_timer);
        batt_6_2_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy6_2(void) { }

static scr_lifecycle_t screen6_2 = {
    .create = create6_2,
    .entry = entry6_2,
    .exit  = exit6_2,
    .destroy = destroy6_2,
};
#undef line_max
#endif
//************************************[ screen 7 ]****************************************** Other
#if 1
static lv_obj_t *scr7_cont;
static lv_obj_t *input_touch;
static lv_obj_t *input_keypad;
static lv_obj_t *gyroscope;
static lv_timer_t *input_timer;
// Fixed buffer so each keypress is an O(1) append instead of a String realloc.
// 128 bytes holds the "Keypad: \n" header plus ~118 typed chars before
// rolling.
#define KEYPAD_STR_CAP 128
static char keypad_str[KEYPAD_STR_CAP] = "Keypad: \n";
static size_t keypad_str_len = 9; // strlen("Keypad: \n")

static void scr7_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void input_timer_event(lv_timer_t *t)
{
    int touch_x, touch_y;
    static int sec = 0;
    float gyro_x, gyro_y, gyro_z;
    char keypay_v;

    int ret = ui_input_get_touch_coord(&touch_x, &touch_y);

    if(ret > 0)
    {
        lv_label_set_text_fmt(input_touch,  "Touch: x: %03d | y: %03d", touch_x, touch_y);

        sec = 0;
    }

    ret = ui_input_get_keypad_val(&keypay_v);
    if(ret == 1)
    {
        ui_input_set_keypad_flag();
        if (keypad_str_len + 1 >= KEYPAD_STR_CAP) {
            // Roll back to just the header to avoid an unbounded label.
            memcpy(keypad_str, "Keypad: \n", 9);
            keypad_str_len = 9;
        }
        keypad_str[keypad_str_len++] = keypay_v;
        keypad_str[keypad_str_len] = '\0';
        lv_label_set_text_fmt(input_keypad, "%s", keypad_str);

        sec = 0;
    }

    sec++;
    if(sec > 60) // 2s
    {
        sec = 0;

        ui_other_get_gyro(&gyro_x, &gyro_y, &gyro_z);
        // Format as fixed-point thousandths instead of %.3f — avoids pulling
        // the heavy floating-point printf path on every gyro refresh.
        int32_t gx_m = (int32_t)(gyro_x * 1000.0f);
        int32_t gy_m = (int32_t)(gyro_y * 1000.0f);
        int32_t gz_m = (int32_t)(gyro_z * 1000.0f);
        lv_label_set_text_fmt(gyroscope,    "   gyros_x: %d.%03d\n"
                                            "   gyros_y: %d.%03d\n"
                                            "   gyros_z: %d.%03d",
                              gx_m / 1000, abs(gx_m % 1000),
                              gy_m / 1000, abs(gy_m % 1000),
                              gz_m / 1000, abs(gz_m % 1000));
    }
}

static void create7(lv_obj_t *parent) 
{
    scr7_cont = lv_obj_create(parent);
    lv_obj_set_size(scr7_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr7_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr7_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr7_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr7_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr7_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(scr7_cont, 13, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr7_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr7_cont, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr7_cont, 5, LV_PART_MAIN);
    lv_obj_set_align(scr7_cont, LV_ALIGN_BOTTOM_MID);

    input_touch = lv_label_create(scr7_cont);
    // lv_obj_set_height(input_touch, 90);
    lv_obj_set_width(input_touch, lv_pct(95));
    lv_obj_set_style_pad_all(input_touch, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(input_touch, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_border_width(input_touch, 1, LV_PART_MAIN);
    lv_label_set_long_mode(input_touch, LV_LABEL_LONG_WRAP);
    lv_label_set_text(input_touch,  "Touch: x:     | y:    ");

    input_keypad = lv_label_create(scr7_cont);
    // lv_obj_set_height(input_keypad, 100);
    lv_obj_set_width(input_keypad, lv_pct(95));
    lv_obj_set_style_pad_all(input_keypad, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(input_keypad, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_border_width(input_keypad, 1, LV_PART_MAIN);
    lv_label_set_long_mode(input_keypad, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(input_keypad, "Keypad: ");

    lv_obj_t *lab2 = lv_label_create(scr7_cont);
    lv_obj_set_style_text_font(lab2, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_label_set_text(lab2, "gyroscope");

    gyroscope = lv_label_create(scr7_cont);
    // lv_obj_set_height(input_keypad, 100);
    lv_obj_set_width(gyroscope, lv_pct(95));
    lv_obj_set_style_pad_all(gyroscope, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(gyroscope, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_border_width(gyroscope, 1, LV_PART_MAIN);
    lv_label_set_long_mode(gyroscope, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(gyroscope,    "   gyros_x: 000\n"
                                        "   gyros_y: 000\n"
                                        "   gyros_z: 000");

    lv_obj_t *back7_label = scr_back_btn_create(parent, ("Other"), scr7_btn_event_cb);
}
static void entry7(void)
{
    memcpy(keypad_str, "Keypad: \n", 9);
    keypad_str[9] = '\0';
    keypad_str_len = 9;
    ui_disp_full_refr();
    input_timer = lv_timer_create(input_timer_event, 50, NULL);
}
static void exit7(void) {
    if(input_timer)
    {
        lv_timer_del(input_timer);
        input_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy7(void) { }

static scr_lifecycle_t screen7 = {
    .create = create7,
    .entry = entry7,
    .exit  = exit7,
    .destroy = destroy7,
};
#endif
//************************************[ screen 8 ]****************************************** A7682E
// --------------------- screen 8 --------------------- A7682E
#if 1
static lv_obj_t *a7682_list;
static lv_obj_t *a7682_page;
static int a7682_num = 0;
static int a7682_page_num = 0;
static int a7682_curr_page = 0;

bool ui_a7682_call_test(const char *param)
{
    scr_mgr_push(SCREEN8_1_ID, false);
    return true;
}

bool ui_a7682_at_test(const char *param)
{
    scr_mgr_push(SCREEN8_2_ID, false);
    return true;
}

static ui_a7682_handle a7682_handle_list[] = 
{
    {"A7682 Audio", NULL, NULL, ui_a7682_at_cb},
    {"Call test", NULL, NULL, ui_a7682_call_test},
    {"AT test", NULL, NULL, ui_a7682_at_test},
};

static void a7682_item_create(int curr_apge);

static void a7682_scr_event(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)e->target;
    ui_a7682_handle *h = (ui_a7682_handle *)e->user_data;

    if(e->code == LV_EVENT_CLICKED) {
        if(h->cb)
            h->cb(h->name);
    }
}

static lv_group_t *a7682_group = NULL;
static lv_timer_t *a7682_kb_timer = NULL;

static void a7682_page_switch_internal(char opt)
{
    if(a7682_num <= SETTING_PAGE_MAX_ITEM) return;

    if (a7682_group) lv_group_remove_all_objs(a7682_group);

    int child_cnt = lv_obj_get_child_cnt(a7682_list);

    for(int i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(a7682_list, 0);
        if(child)
            lv_obj_del(child);
    }

    if(opt == 'p')
    {
        a7682_curr_page = (a7682_curr_page < a7682_page_num) ? a7682_curr_page + 1 : 0;
    }
    else if(opt == 'n')
    {
        a7682_curr_page = (a7682_curr_page > 0) ? a7682_curr_page - 1 : a7682_page_num;
    }

    a7682_item_create(a7682_curr_page);
    lv_label_set_text_fmt(a7682_page, "%d / %d", a7682_curr_page + 1, a7682_page_num + 1);
}

static void a7682_page_switch_cb(lv_event_t *e)
{
    char opt = (int)e->user_data;
    a7682_page_switch_internal(opt);
}

static void a7682_item_create(int curr_apge)
{
    printf("a7682_curr_page = %d\n", a7682_curr_page);
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > a7682_num) end = a7682_num;

    printf("start=%d, end=%d\n", start, end);

    for(int i = start; i < end; i++) {
        ui_a7682_handle *h = &a7682_handle_list[i];
        h->obj = lv_list_add_btn(a7682_list, NULL, h->name);
        // style
        lv_obj_set_height(h->obj, 18);
        lv_obj_set_style_pad_left(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_right(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_text_font(h->obj, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(h->obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(h->obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(h->obj, a7682_scr_event, LV_EVENT_CLICKED, (void *)h);
        
        setting_apply_focus_style(h->obj);
        if (a7682_group) lv_group_add_obj(a7682_group, h->obj);
    }
    if (a7682_group) {
        lv_obj_t *first = lv_obj_get_child(a7682_list, 0);
        if (first) lv_group_focus_obj(first);
    }
}

static void a7682_kb_timer_cb(lv_timer_t *t) {
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to settings
            scr_mgr_pop(false);
            return;
        } else if (key == 'j') {
            if (a7682_group) {
                lv_obj_t *old_f = lv_group_get_focused(a7682_group);
                lv_group_focus_next(a7682_group);
                lv_obj_t *new_f = lv_group_get_focused(a7682_group);
                if (old_f == new_f) { 
                    a7682_page_switch_internal('p');
                    return;
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'k') {
            if (a7682_group) {
                lv_obj_t *old_f = lv_group_get_focused(a7682_group);
                lv_group_focus_prev(a7682_group);
                lv_obj_t *new_f = lv_group_get_focused(a7682_group);
                if (old_f == new_f) {
                    a7682_page_switch_internal('n');
                    lv_obj_t *last = lv_obj_get_child(a7682_list, lv_obj_get_child_cnt(a7682_list) - 1);
                    if (last) lv_group_focus_obj(last);
                    return;
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'E') {
            if (a7682_group) {
                lv_obj_t *f = lv_group_get_focused(a7682_group);
                if (f) lv_event_send(f, LV_EVENT_CLICKED, NULL);
            }
        }
    }
}

static void scr8_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create8(lv_obj_t *parent) 
{
    if (!a7682_group) {
        a7682_group = lv_group_create();
        lv_group_set_wrap(a7682_group, false);
    }
    a7682_list = lv_list_create(parent);
    lv_obj_set_size(a7682_list, LV_HOR_RES, lv_pct(88));
    lv_obj_align(a7682_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(a7682_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(a7682_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(a7682_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(a7682_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(a7682_list, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(a7682_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(a7682_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(a7682_list, 0, LV_PART_MAIN);

    a7682_num = sizeof(a7682_handle_list) / sizeof(a7682_handle_list[0]);
    a7682_page_num = a7682_num / SETTING_PAGE_MAX_ITEM;
    a7682_item_create(a7682_curr_page);

    lv_obj_t * ui_Button2 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button2, 71);
    lv_obj_set_height(ui_Button2, 40);
    lv_obj_set_x(ui_Button2, -70);
    lv_obj_set_y(ui_Button2, 130);
    lv_obj_set_align(ui_Button2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button2, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label1 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label1, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label1, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label1, "Back");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Button14 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button14, 71);
    lv_obj_set_height(ui_Button14, 40);
    lv_obj_set_x(ui_Button14, 70);
    lv_obj_set_y(ui_Button14, 130);
    lv_obj_set_align(ui_Button14, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button14, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button14, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button14, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button14, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button14, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label15 = lv_label_create(ui_Button14);
    lv_obj_set_width(ui_Label15, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label15, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label15, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label15, "Next");
    lv_obj_set_style_text_color(ui_Label15, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label15, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_Button2, a7682_page_switch_cb, LV_EVENT_CLICKED, (void*)'n');
    lv_obj_add_event_cb(ui_Button14, a7682_page_switch_cb, LV_EVENT_CLICKED, (void*)'p');

    a7682_page = lv_label_create(parent);
    lv_obj_set_width(a7682_page, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(a7682_page, LV_SIZE_CONTENT);    /// 1
    lv_obj_align(a7682_page, LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_label_set_text_fmt(a7682_page, "%d / %d", a7682_curr_page + 1, a7682_page_num + 1);
    lv_obj_set_style_text_color(a7682_page, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(a7682_page, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back8_label = scr_back_btn_create(parent, ("A7682E"), scr8_btn_event_cb);
}
static void entry8(void)
{
    if (!a7682_kb_timer) a7682_kb_timer = lv_timer_create(a7682_kb_timer_cb, 20, NULL);
    ui_disp_full_refr();
}
static void exit8(void) {
    if (a7682_kb_timer) {
        lv_timer_del(a7682_kb_timer);
        a7682_kb_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy8(void) {
    if (a7682_group) {
        lv_group_del(a7682_group);
        a7682_group = NULL;
    }
}
static scr_lifecycle_t screen8 = {
    .create = create8,
    .entry = entry8,
    .exit  = exit8,
    .destroy = destroy8,
};
#endif
// --------------------- screen 8.1 --------------------- Call test
#if 1
static void event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * ta =  (lv_obj_t *)lv_event_get_user_data(e);

    if(code == LV_EVENT_VALUE_CHANGED) {
        uint32_t id = lv_btnmatrix_get_selected_btn(obj);
        const char * txt = lv_btnmatrix_get_btn_text(obj, id);
        int len = strlen(txt);
 
        if(!strcmp(txt, LV_SYMBOL_CALL)) {
            ui_a7682_call(lv_textarea_get_text(ta));
        } else if(!strcmp(txt, "Hang up"))
        {
            ui_a7682_hang_up();
        } else if(!strcmp(txt, LV_SYMBOL_BACKSPACE))
        {
            lv_textarea_del_char(ta);
        }else{
            lv_textarea_add_text(ta, txt);
        }
    }
}

static const char * btnm_map[] = {  "1", "2", "3", "\n",
                                    "4", "5", "6", "\n",
                                    "7", "8", "9", "\n",
                                    "*", "0", "#", "\n",
                                    LV_SYMBOL_CALL, "Hang up", LV_SYMBOL_BACKSPACE,""
                                 };


static void scr8_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create8_1(lv_obj_t *parent) 
{
    lv_obj_t * ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, lv_pct(98));
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, lv_pct(20));
    lv_obj_set_style_text_font(ta, &Font_Mono_Bold_20, LV_PART_MAIN);
    // lv_obj_add_state(ta, LV_STATE_FOCUSED); /*To be sure the cursor is visible*/
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_letter_space(ta, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * btnm1 = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(btnm1, btnm_map);
    lv_obj_set_size(btnm1, lv_pct(100)-2, lv_pct(60));
    lv_obj_set_style_border_width(btnm1, 0, 0);
    // lv_btnmatrix_set_btn_width(btnm1, 10, 2);        /*Make "Action1" twice as wide as "Action2"*/
    // lv_btnmatrix_set_btn_ctrl(btnm1, 10, LV_BTNMATRIX_CTRL_CHECKABLE);
    // lv_btnmatrix_set_btn_ctrl(btnm1, 11, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_align(btnm1, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(btnm1, event_handler, LV_EVENT_VALUE_CHANGED, ta);
    
    lv_obj_t *back8_1_label = scr_back_btn_create(parent, ("Call"), scr8_1_btn_event_cb);
}
static void entry8_1(void) 
{
    ui_a7682_loop_resume();
    ui_disp_full_refr();
}
static void exit8_1(void) {
    ui_a7682_loop_suspend();
    ui_disp_full_refr();
}
static void destroy8_1(void) { }

static scr_lifecycle_t screen8_1 = {
    .create = create8_1,
    .entry = entry8_1,
    .exit  = exit8_1,
    .destroy = destroy8_1,
};
#endif
// --------------------- screen 8.2 --------------------- AT test
#if 1
static void scr8_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create8_2(lv_obj_t *parent) 
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_width(lab, lv_pct(95));
    lv_obj_set_style_text_font(lab, FONT_BOLD_SIZE_17, LV_PART_MAIN);
    lv_label_set_text(lab, "Open the serial port, set the baud rate to 115200, "
                            "and send the AT command of A7682E to test the function.");
    lv_obj_center(lab);
    
    lv_obj_t *back8_2_label = scr_back_btn_create(parent, ("AT test"), scr8_2_btn_event_cb);
}
static void entry8_2(void) 
{
    ui_a7682_loop_resume();
    ui_disp_full_refr();
}
static void exit8_2(void) {
    ui_a7682_loop_suspend();
    ui_disp_full_refr();
}
static void destroy8_2(void) { }

static scr_lifecycle_t screen8_2 = {
    .create = create8_2,
    .entry = entry8_2,
    .exit  = exit8_2,
    .destroy = destroy8_2,
};
#endif
//************************************[ screen 9 ]****************************************** Shutdown
#if 1
static lv_timer_t *shutdown_timer = NULL;

static void scr9_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void shutdown_timer_event(lv_timer_t* t)
{
    ui_shutdown_on();
    lv_timer_del(t);
}

static void create9(lv_obj_t *parent)
{
    if(ui_battery_25896_is_vbus_in()) 
    {
        lv_obj_t * label = lv_label_create(parent);
        lv_obj_set_width(label, lv_pct(95));
        lv_obj_set_style_text_font(label, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, "The shutdown function can only be used when the "
                            "battery is connected alone, and cannot be shut down when connected to USB.");
        lv_obj_center(label);

        // back 
        scr_back_btn_create(parent, "Shutdown", scr8_btn_event_cb);
    } 
    else 
    {
        lv_obj_t * img = lv_img_create(parent);
        lv_img_set_src(img, &img_start);
        lv_obj_center(img);

        lv_timer_create(shutdown_timer_event, 2000, (void *)parent);
    }
}
static void entry9(void) 
{
    ui_disp_full_refr();
}
static void exit9(void) {
    ui_disp_full_refr();
}
static void destroy9(void) { }

static scr_lifecycle_t screen9 = {
    .create = create9,
    .entry = entry9,
    .exit  = exit9,
    .destroy = destroy9,
};
#endif
//************************************[ screen 10 ]***************************************** pcm5102
#if 1
static lv_obj_t *pcm5102_list;
static lv_obj_t *pcm5102_page;
static int pcm5102_num = 0;
static int pcm5102_page_num = 0;
static int pcm5102_curr_page = 0;

static ui_pcm5102_handle pcm5102_handle_list[] = 
{
    {"PCM5102 Audio", NULL, NULL, ui_pcm5102_cb},
};

static void pcm5102_item_create(int curr_apge);

static void pcm5102_scr_event(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)e->target;
    ui_pcm5102_handle *h = (ui_pcm5102_handle *)e->user_data;

    if(e->code == LV_EVENT_CLICKED) {
        if(h->cb)
            h->cb(h->name);
    }
}

static lv_group_t *pcm5102_group = NULL;
static lv_timer_t *pcm5102_kb_timer = NULL;

static void pcm5102_page_switch_internal(char opt)
{
    if(pcm5102_num <= SETTING_PAGE_MAX_ITEM) return;

    if (pcm5102_group) lv_group_remove_all_objs(pcm5102_group);

    int child_cnt = lv_obj_get_child_cnt(pcm5102_list);

    for(int i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(pcm5102_list, 0);
        if(child)
            lv_obj_del(child);
    }

    if(opt == 'p')
    {
        pcm5102_curr_page = (pcm5102_curr_page < pcm5102_page_num) ? pcm5102_curr_page + 1 : 0;
    }
    else if(opt == 'n')
    {
        pcm5102_curr_page = (pcm5102_curr_page > 0) ? pcm5102_curr_page - 1 : pcm5102_page_num;
    }

    pcm5102_item_create(pcm5102_curr_page);
    lv_label_set_text_fmt(pcm5102_page, "%d / %d", pcm5102_curr_page + 1, pcm5102_page_num + 1);
}

static void pcm5102_page_switch_cb(lv_event_t *e)
{
    char opt = (int)e->user_data;
    pcm5102_page_switch_internal(opt);
}

static void pcm5102_item_create(int curr_apge)
{
    printf("pcm5102_curr_page = %d\n", pcm5102_curr_page);
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > pcm5102_num) end = pcm5102_num;

    printf("start=%d, end=%d\n", start, end);

    for(int i = start; i < end; i++) {
        ui_pcm5102_handle *h = &pcm5102_handle_list[i];
        h->obj = lv_list_add_btn(pcm5102_list, NULL, h->name);
        // style
        lv_obj_set_height(h->obj, 18);
        lv_obj_set_style_pad_left(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_right(h->obj, 5, LV_PART_MAIN);
        lv_obj_set_style_text_font(h->obj, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(h->obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(h->obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(h->obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(h->obj, pcm5102_scr_event, LV_EVENT_CLICKED, (void *)h);
        
        setting_apply_focus_style(h->obj);
        if (pcm5102_group) lv_group_add_obj(pcm5102_group, h->obj);
    }
    if (pcm5102_group) {
        lv_obj_t *first = lv_obj_get_child(pcm5102_list, 0);
        if (first) lv_group_focus_obj(first);
    }
}

static void pcm5102_kb_timer_cb(lv_timer_t *t) {
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to settings
            scr_mgr_pop(false);
            return;
        } else if (key == 'j') {
            if (pcm5102_group) {
                lv_obj_t *old_f = lv_group_get_focused(pcm5102_group);
                lv_group_focus_next(pcm5102_group);
                lv_obj_t *new_f = lv_group_get_focused(pcm5102_group);
                if (old_f == new_f) { 
                    pcm5102_page_switch_internal('p');
                    return;
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'k') {
            if (pcm5102_group) {
                lv_obj_t *old_f = lv_group_get_focused(pcm5102_group);
                lv_group_focus_prev(pcm5102_group);
                lv_obj_t *new_f = lv_group_get_focused(pcm5102_group);
                if (old_f == new_f) {
                    pcm5102_page_switch_internal('n');
                    lv_obj_t *last = lv_obj_get_child(pcm5102_list, lv_obj_get_child_cnt(pcm5102_list) - 1);
                    if (last) lv_group_focus_obj(last);
                    return;
                } else if (new_f) {
                    lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
                }
            }
        } else if (key == 'E') {
            if (pcm5102_group) {
                lv_obj_t *f = lv_group_get_focused(pcm5102_group);
                if (f) lv_event_send(f, LV_EVENT_CLICKED, NULL);
            }
        }
    }
}


static void scr10_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create10(lv_obj_t *parent) 
{
    if (!pcm5102_group) {
        pcm5102_group = lv_group_create();
        lv_group_set_wrap(pcm5102_group, false);
    }
    pcm5102_list = lv_list_create(parent);
    lv_obj_set_size(pcm5102_list, LV_HOR_RES, lv_pct(88));
    lv_obj_align(pcm5102_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(pcm5102_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(pcm5102_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pcm5102_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(pcm5102_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(pcm5102_list, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(pcm5102_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(pcm5102_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(pcm5102_list, 0, LV_PART_MAIN);

    pcm5102_num = sizeof(pcm5102_handle_list) / sizeof(pcm5102_handle_list[0]);
    pcm5102_page_num = pcm5102_num / SETTING_PAGE_MAX_ITEM;
    pcm5102_item_create(pcm5102_curr_page);

    lv_obj_t * ui_Button2 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button2, 71);
    lv_obj_set_height(ui_Button2, 40);
    lv_obj_set_x(ui_Button2, -70);
    lv_obj_set_y(ui_Button2, 130);
    lv_obj_set_align(ui_Button2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button2, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label1 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label1, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label1, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label1, "Back");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Button14 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button14, 71);
    lv_obj_set_height(ui_Button14, 40);
    lv_obj_set_x(ui_Button14, 70);
    lv_obj_set_y(ui_Button14, 130);
    lv_obj_set_align(ui_Button14, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button14, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button14, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button14, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button14, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button14, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label15 = lv_label_create(ui_Button14);
    lv_obj_set_width(ui_Label15, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label15, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label15, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label15, "Next");
    lv_obj_set_style_text_color(ui_Label15, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label15, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_Button2, pcm5102_page_switch_cb, LV_EVENT_CLICKED, (void*)'n');
    lv_obj_add_event_cb(ui_Button14, pcm5102_page_switch_cb, LV_EVENT_CLICKED, (void*)'p');

    pcm5102_page = lv_label_create(parent);
    lv_obj_set_width(pcm5102_page, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(pcm5102_page, LV_SIZE_CONTENT);    /// 1
    lv_obj_align(pcm5102_page, LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_label_set_text_fmt(pcm5102_page, "%d / %d", pcm5102_curr_page + 1, pcm5102_page_num + 1);
    lv_obj_set_style_text_color(pcm5102_page, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(pcm5102_page, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back10_label = scr_back_btn_create(parent, ("PCM5102"), scr10_btn_event_cb);
}
static void entry10(void) 
{
    if (!pcm5102_kb_timer) pcm5102_kb_timer = lv_timer_create(pcm5102_kb_timer_cb, 20, NULL);
    ui_disp_full_refr();
}
static void exit10(void) 
{
    ui_pcm5102_stop();
    if (pcm5102_kb_timer) {
        lv_timer_del(pcm5102_kb_timer);
        pcm5102_kb_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy10(void) {
    if (pcm5102_group) {
        lv_group_del(pcm5102_group);
        pcm5102_group = NULL;
    }
}

static scr_lifecycle_t screen10 = {
    .create = create10,
    .entry = entry10,
    .exit  = exit10,
    .destroy = destroy10,
};
#endif
//************************************[ screen 11 ]****************************************** Sleep
#if 1
#include <TouchDrvCSTXXX.hpp>
static void scr11_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create11(lv_obj_t *parent)
{
    extern TouchDrvCSTXXX touch;

    touch.sleep();

    lora_sleep();

    SerialGPS.end();
    
    // pinMode(BOARD_GPS_PPS, OUTPUT);
    // pinMode(BOARD_GPS_RXD, OUTPUT);
    // pinMode(BOARD_GPS_TXD, OUTPUT);
    // pinMode(BOARD_LORA_RST, OUTPUT);
    // pinMode(BOARD_TOUCH_RST, OUTPUT);
    // pinMode(BOARD_LORA_BUSY, OUTPUT);

    // digitalWrite(BOARD_GPS_PPS, LOW);
    // digitalWrite(BOARD_GPS_RXD, LOW);
    // digitalWrite(BOARD_GPS_TXD, LOW);
    // digitalWrite(BOARD_LORA_RST, LOW);
    // digitalWrite(BOARD_TOUCH_RST, LOW);
    // digitalWrite(BOARD_LORA_BUSY, LOW);

    gpio_reset_pin((gpio_num_t)BOARD_GPS_PPS);
    gpio_reset_pin((gpio_num_t)BOARD_GPS_RXD);
    gpio_reset_pin((gpio_num_t)BOARD_GPS_TXD);
    gpio_reset_pin((gpio_num_t)BOARD_LORA_RST);
    gpio_reset_pin((gpio_num_t)BOARD_TOUCH_RST);
    gpio_reset_pin((gpio_num_t)BOARD_LORA_BUSY);

    digitalWrite(BOARD_6609_EN, LOW);
    digitalWrite(BOARD_LORA_EN, LOW);
    digitalWrite(BOARD_GPS_EN, LOW);
    
    digitalWrite(BOARD_1V8_EN, LOW);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);

    // gpio_hold_en((gpio_num_t)BOARD_GPS_PPS);
    // gpio_hold_en((gpio_num_t)BOARD_TOUCH_RST);
    // gpio_hold_en((gpio_num_t)BOARD_GPS_RXD);
    // gpio_hold_en((gpio_num_t)BOARD_GPS_TXD);
    // gpio_hold_en((gpio_num_t)BOARD_LORA_RST);
    // gpio_hold_en((gpio_num_t)BOARD_LORA_BUSY);
    gpio_hold_en((gpio_num_t)BOARD_6609_EN);
    gpio_hold_en((gpio_num_t)BOARD_LORA_EN);
    gpio_hold_en((gpio_num_t)BOARD_GPS_EN);
    gpio_hold_en((gpio_num_t)BOARD_1V8_EN);
    gpio_hold_en((gpio_num_t)BOARD_A7682E_PWRKEY);
    gpio_deep_sleep_hold_en();

    
    // esp_sleep_enable_ext0_wakeup((gpio_num_t)ENCODER_KEY, 0);                            
    esp_sleep_enable_ext1_wakeup((1UL << BOARD_BOOT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);   // Hibernate using user keys
    esp_deep_sleep_start();

    // back 
    scr_back_btn_create(parent, "Sleep", scr8_btn_event_cb);
}
static void entry11(void) 
{
    ui_disp_full_refr();
}
static void exit11(void) {
    ui_disp_full_refr();
}
static void destroy11(void) { }

static scr_lifecycle_t screen11 = {
    .create = create11,
    .entry = entry11,
    .exit  = exit11,
    .destroy = destroy11,
};
#endif
//************************************[ UI ENTRY ]******************************************
static lv_obj_t *menu_keypad;
static lv_timer_t *menu_timer = NULL;

static void indev_get_gesture_dir(lv_timer_t *t)
{
    lv_indev_t * touch_indev = lv_indev_get_next(NULL);
    lv_dir_t dir = lv_indev_get_gesture_dir(touch_indev);

    if(dir == LV_DIR_RIGHT) { // right
        ui_get_gesture_dir(LV_DIR_RIGHT);
    } 
    else if(dir == LV_DIR_LEFT) { // left
        ui_get_gesture_dir(LV_DIR_LEFT);
    }
    // Serial.printf("dir=%d\n", dir);
}



static void menu_taskbar_update_timer_cb(lv_timer_t *t)
{
    if (!menu_taskbar) return;
    static int sec = 0;
    sec++;

    // On first boot the time and battery labels start as placeholders (the
    // BQ27220 finishes init asynchronously, and NTP can take seconds to
    // converge). The transition from those boot-time values to real ones is
    // pushed as a partial refresh, which on this e-paper panel can't fully
    // clear pixels that have been settled on screen for several seconds —
    // hence ghosted overlap of the previous digits. Force a single full
    // refresh on each label's first real update; subsequent updates stay on
    // the fast partial path.
    static bool first_battery_update_done = false;
    static bool first_time_update_done = false;

    bool charge = 0;
    bool finish = 0;
    bool wifi = 0;
    int percent = 0;

    // Read every tick. The underlying getters are I2C-rate-limited by a 500 ms
    // cache, and the change-detection below avoids redundant label redraws.
    // Reading only every 10 s let the label sit at 0 for up to ten seconds
    // after BQ27220 init completed.
    finish = ui_battery_27220_get_charge_finish();
    percent = ui_battery_27220_get_percent();

    if(taskbar_statue[TASKBAR_ID_CHARGE_FINISH] != finish)
    {
        if(finish){
            lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_OK);
        } else {
            lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_CHARGE);
        }
        taskbar_statue[TASKBAR_ID_CHARGE_FINISH] = finish;
    }

    if(taskbar_statue[TASKBAR_ID_BATTERY_PERCENT] != percent)
    {
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", percent);
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        taskbar_statue[TASKBAR_ID_BATTERY_PERCENT] = percent;
        if (!first_battery_update_done) {
            first_battery_update_done = true;
            ui_disp_full_refr();
        }
    }

    charge = ui_battery_27220_get_input();
    if(taskbar_statue[TASKBAR_ID_CHARGE] != charge)
    {
        if(charge) {
            lv_obj_clear_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_CHARGE] = charge;
    }

    bool wifi_en = ui_wifi_get_enabled();
    if(taskbar_statue[TASKBAR_ID_WIFI] != (int)wifi_en)
    {
        if(wifi_en) {
            lv_obj_clear_flag(menu_taskbar_wifi, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_wifi, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_WIFI] = (int)wifi_en;
    }

    // Clock. Only render once SNTP has produced a plausible year — before that
    // the system epoch is 1970 which is uglier than the placeholder. We rewrite
    // only when the displayed minute changes to avoid burning a partial refresh
    // every second on the e-paper.
    if (menu_taskbar_time) {
        struct tm tm_now;
        if (ui_time_get_local(&tm_now)) {
            static int last_min = -1;
            int cur = tm_now.tm_hour * 60 + tm_now.tm_min;
            if (cur != last_min) {
                lv_label_set_text_fmt(menu_taskbar_time, "%02d:%02d",
                                      tm_now.tm_hour, tm_now.tm_min);
                last_min = cur;
                if (!first_time_update_done) {
                    first_time_update_done = true;
                    ui_disp_full_refr();
                }
            }
        }
    }
}

static int cursor = 0;
#define SCREEN_POP -1

int menu_buf[] = {
    SCREEN1_ID, SCREEN1_1_ID, SCREEN_POP, SCREEN1_2_ID, SCREEN_POP, 0,
    SCREEN2_ID, 0,
    SCREEN3_ID, 0,
    SCREEN4_ID, SCREEN4_1_ID, SCREEN_POP, SCREEN4_2_ID, SCREEN_POP, 0,
    SCREEN5_ID, 0,
    SCREEN6_ID, SCREEN6_1_ID, SCREEN_POP, SCREEN6_2_ID, SCREEN_POP, 0,
    SCREEN7_ID, 0,
    // SCREEN8_ID, SCREEN8_1_ID, SCREEN_POP, SCREEN8_2_ID, SCREEN_POP, 0,
    // SCREEN9_ID, 0,
    // SCREEN10_ID, 0,
    // SCREEN11_ID, 0,
};

void ui_auto_timer_cb(lv_timer_t *t)
{
    if(menu_buf[cursor] != 0 && menu_buf[cursor] != -1) {
        scr_mgr_push(menu_buf[cursor], false);
        printf("push = %d\n", menu_buf[cursor]);
    } else if(menu_buf[cursor] == -1) {
        scr_mgr_pop(false);
        printf("pop\n");
    } else {
        scr_mgr_switch(SCREEN0_ID, false);
        printf("back\n");
    }

    cursor++;
    if(cursor > (sizeof(menu_buf)/sizeof(menu_buf[0]) - 1)) {
        cursor = 0;
    }
}



// Lightweight toast: a centered overlay label that auto-removes after a few
// seconds. We park it on lv_layer_top so it floats above whatever screen is
// active. Single instance — a new toast replaces any in-flight one.
static lv_obj_t *ui_toast_obj = NULL;
static lv_timer_t *ui_toast_timer = NULL;

static void ui_toast_dismiss_cb(lv_timer_t *t)
{
    if (ui_toast_obj)   { lv_obj_del(ui_toast_obj);     ui_toast_obj   = NULL; }
    if (ui_toast_timer) { lv_timer_del(ui_toast_timer); ui_toast_timer = NULL; }
    ui_disp_full_refr();
}

static void ui_toast_show(const char *text, uint32_t ms)
{
    if (ui_toast_obj)   { lv_obj_del(ui_toast_obj);     ui_toast_obj   = NULL; }
    if (ui_toast_timer) { lv_timer_del(ui_toast_timer); ui_toast_timer = NULL; }

    ui_toast_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ui_toast_obj, lv_pct(80), LV_SIZE_CONTENT);
    lv_obj_align(ui_toast_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ui_toast_obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_toast_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_toast_obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_toast_obj, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_toast_obj, 8, LV_PART_MAIN);
    lv_obj_clear_flag(ui_toast_obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lab = lv_label_create(ui_toast_obj);
    lv_obj_set_width(lab, lv_pct(100));
    lv_obj_set_style_text_font(lab, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lab, text);
    lv_obj_center(lab);

    ui_toast_timer = lv_timer_create(ui_toast_dismiss_cb, ms, NULL);
    lv_timer_set_repeat_count(ui_toast_timer, 1);
}

static void ui_do_ping_1111(void)
{
    Serial.println("[ping] 'p' pressed, starting ping to 1.1.1.1");
    // Show "Pinging..." first, then run ping. esp_ping blocks the calling
    // thread until the session ends, so we need a placeholder to confirm the
    // keypress was received and to give the e-paper something to draw before
    // the 2s wait.
    ui_toast_show("Pinging 1.1.1.1...", 30000);
    ui_disp_full_refr();
    lv_refr_now(NULL);

    int rtt = -1;
    bool ok = ui_ping("1.1.1.1", 2000, &rtt);
    char buf[96];
    if (ok) {
        // Internet reachable -> kick off NTP. SNTP is async, so we just
        // request the resync and report local time on the next tick if it
        // already had a recent sample.
        ui_wifi_set_tz("<-03>3");
        ui_ntp_resync();
        delay(500);
        if (ui_time_is_synced()) {
            struct tm tm_now;
            ui_time_get_local(&tm_now);
            lv_snprintf(buf, sizeof(buf),
                        "Ping 1.1.1.1\nOK  %d ms\nTime %02d:%02d:%02d",
                        rtt, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        } else {
            lv_snprintf(buf, sizeof(buf),
                        "Ping 1.1.1.1\nOK  %d ms\nNTP syncing...", rtt);
        }
    } else {
        lv_snprintf(buf, sizeof(buf), "Ping 1.1.1.1\nFAIL");
    }
    Serial.printf("[ping] result: %s\n", buf);
    ui_toast_show(buf, 3000);
    ui_disp_full_refr();
}

static void ui_do_ntp_sync(void)
{
    Serial.println("[ntp] 'z' pressed, starting NTP sync for Americas/Sao_Paulo");
    if (ui_wifi_get_status() != UI_WIFI_STATUS_CONNECTED) {
        ui_toast_show("WiFi not connected!", 3000);
        return;
    }
    
    ui_toast_show("Syncing time (Sao Paulo)...", 30000);
    ui_disp_full_refr();
    lv_refr_now(NULL);

    ui_wifi_set_tz("<-03>3");
    ui_ntp_resync();
    
    // Wait a bit for NTP to potentially update (non-blocking in reality but gives feedback)
    delay(500);
    
    if (ui_time_is_synced()) {
        struct tm tm_now;
        ui_time_get_local(&tm_now);
        char buf[64];
        lv_snprintf(buf, sizeof(buf), "Time Synced!\n%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        ui_toast_show(buf, 3000);
    } else {
        ui_toast_show("NTP Request Sent\nWaiting for sync...", 3000);
    }
    ui_disp_full_refr();
}

// Reader-screen statics are defined here (rather than alongside the rest of
// the reader code below) because the home-screen 'c' shortcut in
// menu_keypay_get_event needs to stage a resume target before pushing
// SCREEN13_1, and C++ doesn't allow two file-scope static definitions.
static char   reader_selected_file[32] = {0};
// Set true by 'c' on the home screen; entry13_1 consumes it to seed
// reader_page_offsets[0] so the first render is the user's last-read page.
static bool   reader_resume_pending = false;
static size_t reader_resume_offset  = 0;

static void menu_keypay_get_event(lv_timer_t *timer)
{
    uint16_t curr_id = scr_mgr_get_curr_scr_id();
    if (curr_id == SCREEN0_ID) {
        char key_val;
        if (ui_input_get_keypad_val(&key_val)) {
            Serial.printf("[home] key=0x%02x ('%c')\n",
                          (unsigned char)key_val,
                          (key_val >= 32 && key_val < 127) ? key_val : '?');
            switch (key_val) {
                case 's':
                    scr_mgr_push(SCREEN2_ID, false);
                    break;
                case 'r':
                    scr_mgr_push(SCREEN13_ID, false);
                    break;
                case 'c': {
                    // Resume reading the last book at its saved byte
                    // offset. Skip if no bookmark exists.
                    char saved[32] = {0};
                    size_t saved_off = 0;
                    if (ui_reader_resume_get(saved, sizeof(saved), &saved_off)
                        && saved[0]) {
                        strncpy(reader_selected_file, saved,
                                sizeof(reader_selected_file) - 1);
                        reader_selected_file[sizeof(reader_selected_file) - 1] = '\0';
                        reader_resume_offset = saved_off;
                        reader_resume_pending = true;
                        scr_mgr_push(SCREEN13_1_ID, false);
                    }
                    break;
                }
                case 'n':
                    notes_selected_file[0] = '\0';
                    scr_mgr_push(SCREEN12_1_ID, false);
                    break;
                case 't':
                    scr_mgr_push(SCREEN_DICT_ID, false);
                    break;
                case 'q':
                    scr_mgr_push(SCREEN_USB_MSC_ID, false);
                    break;
                case 'l':
                    scr_mgr_push(SCREEN_LOCK_ID, false);
                    break;
                case 'd':
                    ui_disp_white_clear();
                    scr_mgr_push(SCREEN11_ID, false);
                    break;
                case 'w':
                    ui_wifi_set_enabled(!ui_wifi_get_enabled());
                    break;
                case 'p':
                    ui_do_ping_1111();
                    break;
                case 'z':
                    ui_do_ntp_sync();
                    break;
                case 'b':
                    ui_topbar_show_battery_set(!ui_topbar_show_battery_get());
                    ui_taskbar_apply_battery_visibility();
                    ui_disp_full_refr();
                    break;
                case 0x1B: // Esc
                    ui_disp_hard_refr();
                    break;
            }
            ui_input_set_keypad_flag(); // Consume the key
        }
    } else if (curr_id == SCREEN13_ID || curr_id == SCREEN13_1_ID) {
        // The reader file list (SCREEN13) and the reader page view
        // (SCREEN13_1) each run their own keypad timer that handles j/k/E
        // and Esc. If we unconditionally consumed the key here we'd race
        // with those timers and eat half the presses — making j/k feel
        // like they need two taps to advance one page. Only consume on
        // keys this branch actually acts on.
        char key_val;
        if (ui_input_get_keypad_val(&key_val)) {
            if (key_val == 'q') {
                scr_mgr_switch(SCREEN0_ID, false);
                ui_input_set_keypad_flag();
            }
        }
    } else if (curr_id == SCREEN2_ID || curr_id == SCREEN5_ID || curr_id == SCREEN8_ID || curr_id == SCREEN10_ID) {
        // These paginated menus have their own local kb_timers. 
        // Don't consume here or we'll race and drop half the key events.
        char key_val;
        if (ui_input_get_keypad_val(&key_val)) {
            if (key_val == 'q') {
                scr_mgr_switch(SCREEN0_ID, false);
                ui_input_set_keypad_flag();
            }
        }
    } else if (curr_id == SCREEN2_1_ID) {
        char key_val;
        if (ui_input_get_keypad_val(&key_val)) {
            if (key_val == 0x1B) {
                scr_mgr_switch(SCREEN0_ID, false);
            }
            ui_input_set_keypad_flag();
        }
    } else if (curr_id == SCREEN_USB_MSC_ID) {
        char key_val;
        if (ui_input_get_keypad_val(&key_val)) {
            if (key_val == 0x1B) {
                scr_mgr_switch(SCREEN0_ID, false);
            }
            ui_input_set_keypad_flag();
        }
    } else if (curr_id == SCREEN_LOCK_ID) {
        char key_val;
        if (ui_input_get_keypad_val(&key_val)) {
            lock_handle_key(key_val);
            ui_input_set_keypad_flag();
        }
    }
}

extern scr_lifecycle_t screen12;
extern scr_lifecycle_t screen12_1;
extern scr_lifecycle_t screen13;
extern scr_lifecycle_t screen13_1;
extern scr_lifecycle_t screen13_2;
extern scr_lifecycle_t screen_dict;

void ui_deckpro_entry(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    disp->theme = lv_theme_mono_init(disp, false, LV_FONT_DEFAULT);

    touch_chk_timer = lv_timer_create(indev_get_gesture_dir, LV_INDEV_DEF_READ_PERIOD, NULL);
    lv_timer_pause(touch_chk_timer);

    taskbar_update_timer = lv_timer_create(menu_taskbar_update_timer_cb, 1000, NULL);
    lv_timer_pause(taskbar_update_timer);

    low_voltage_popup_create();
    low_voltage_timer = lv_timer_create(low_voltage_timer_cb, LOW_VOLTAGE_POLL_MS, NULL);

    // auto test
    // lv_timer_create(ui_auto_timer_cb, 3000, NULL);

    scr_mgr_init();

    scr_mgr_register(SCREEN0_ID,    &screen0);      // menu
    scr_mgr_register(SCREEN1_ID,    &screen1);      // Lora
    scr_mgr_register(SCREEN1_1_ID,  &screen1_1);    // - Auto send
    scr_mgr_register(SCREEN1_2_ID,  &screen1_2);    // - Lora Setting
    scr_mgr_register(SCREEN2_ID,    &screen2);      // Setting
    scr_mgr_register(SCREEN2_1_ID,  &screen2_1);    //  - About System
    scr_mgr_register(SCREEN2_2_ID,  &screen2_2);    //  - Hidden Apps
    scr_mgr_register(SCREEN3_ID,    &screen3);      // 
    scr_mgr_register(SCREEN4_ID,    &screen4);      // WIFI
    scr_mgr_register(SCREEN4_1_ID,  &screen4_1);    //  - WIFI Config
    scr_mgr_register(SCREEN4_2_ID,  &screen4_2);    //  - WIFI Scan
    scr_mgr_register(SCREEN5_ID,    &screen5);      // 
    scr_mgr_register(SCREEN6_ID,    &screen6);      // Battery
    scr_mgr_register(SCREEN6_1_ID,  &screen6_1);    //  - BQ25896
    scr_mgr_register(SCREEN6_2_ID,  &screen6_2);    //  - BQ27220
    scr_mgr_register(SCREEN7_ID,    &screen7);      // 
    scr_mgr_register(SCREEN8_ID,    &screen8);      // A7682E
    scr_mgr_register(SCREEN8_1_ID,  &screen8_1);    //  - Call test
    scr_mgr_register(SCREEN8_2_ID,  &screen8_2);    //  - AT test
    scr_mgr_register(SCREEN9_ID,    &screen9);      // Shutdown
    scr_mgr_register(SCREEN10_ID,   &screen10);     // PCM5102
    scr_mgr_register(SCREEN11_ID,   &screen11);
    scr_mgr_register(SCREEN12_ID,   &screen12);
    scr_mgr_register(SCREEN12_1_ID, &screen12_1);
    scr_mgr_register(SCREEN13_ID, &screen13);
    scr_mgr_register(SCREEN13_1_ID, &screen13_1);
    scr_mgr_register(SCREEN13_2_ID, &screen13_2);
    scr_mgr_register(SCREEN_USB_MSC_ID, &screen_usb_msc);
    scr_mgr_register(SCREEN_LOCK_ID, &screen_lock);
    scr_mgr_register(SCREEN_DICT_ID, &screen_dict);


    scr_mgr_switch(SCREEN0_ID, false); // set root screen
    scr_mgr_set_anim(LV_SCR_LOAD_ANIM_OVER_LEFT, LV_SCR_LOAD_ANIM_OVER_LEFT, LV_SCR_LOAD_ANIM_OVER_LEFT);

    // menu_keypad = lv_label_create(lv_layer_top());
    // lv_obj_set_style_text_font(menu_keypad, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    // lv_label_set_text(menu_keypad, " ");
    // lv_obj_align(menu_keypad, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    menu_timer = lv_timer_create(menu_keypay_get_event, 40, NULL);
}
//************************************[ screen 12 ]***************************************** Notes List
#if 1

static void notes_list_update(void)
{
    lv_obj_clean(notes_list_obj);
    char list[UI_NOTES_MAX_COUNT][32];
    int count = 0;
    ui_notes_get_list(notes_use_sd, list, &count);

    for (int i = 0; i < count; i++) {
        lv_obj_t * btn = lv_list_add_btn(notes_list_obj, NULL, list[i]);
        lv_obj_set_style_text_font(btn, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        
        struct note_info {
            char name[32];
        };
        // We can't easily pass strings in user_data without allocation, 
        // so we'll just use the button text in the event handler.
    }
}

static void notes_list_btn_event_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    const char * name = lv_list_get_btn_text(notes_list_obj, obj);
    if (name) {
        strncpy(notes_selected_file, name, 31);
        scr_mgr_push(SCREEN12_1_ID, false);
    }
}

static void notes_storage_sw_event_cb(lv_event_t * e)
{
    lv_obj_t * sw = lv_event_get_target(e);
    notes_use_sd = lv_obj_has_state(sw, LV_STATE_CHECKED);
    notes_list_update();
}

static void notes_new_btn_event_cb(lv_event_t * e)
{
    notes_selected_file[0] = '\0'; // Empty name means new file
    scr_mgr_push(SCREEN12_1_ID, false);
}

static void scr12_back_btn_event_cb(lv_event_t * e)
{
    scr_mgr_pop(false);
}

static void create12(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;

    lv_obj_t * sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 40, 20);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -10, status_bar_height + 5);
    if (notes_use_sd) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, notes_storage_sw_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * sw_label = lv_label_create(parent);
    lv_label_set_text(sw_label, "SD");
    lv_obj_set_style_text_font(sw_label, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_align_to(sw_label, sw, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    notes_list_obj = lv_list_create(parent);
    lv_obj_set_size(notes_list_obj, lv_pct(100), LV_VER_RES - status_bar_height - 60);
    lv_obj_align(notes_list_obj, LV_ALIGN_TOP_MID, 0, status_bar_height + 30);
    lv_obj_add_event_cb(notes_list_obj, notes_list_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * new_btn = lv_btn_create(parent);
    lv_obj_set_size(new_btn, 80, 25);
    lv_obj_align(new_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_t * new_label = lv_label_create(new_btn);
    lv_label_set_text(new_label, "New Note");
    lv_obj_center(new_label);
    lv_obj_add_event_cb(new_btn, notes_new_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

static void entry12(void)
{
    notes_list_update();
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}

static void exit12(void) { 
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr(); 
}

static void destroy12(void) {
    if(menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
}

scr_lifecycle_t screen12 = {
    .create = create12,
    .entry = entry12,
    .exit  = exit12,
    .destroy = destroy12,
};
#endif

//************************************[ screen 12.1 ]*************************************** Notes Editor
#if 1
static lv_obj_t *notes_editor_ta;
static lv_timer_t *notes_editor_kb_timer = NULL;

typedef enum {
    NOTES_SAVE_OK,
    NOTES_SAVE_EMPTY,
    NOTES_SAVE_NO_TIME,
    NOTES_SAVE_ERR,
} notes_save_result_t;

// Build a YYYYMMDD_HHMMSS.txt filename from system time, falling back to
// GPS time. Returns false if neither source has a plausible date — callers
// must surface that to the user rather than inventing a filename.
static bool notes_make_timestamp_name(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    if (now > 1700000000 && localtime_r(&now, &tm_now) != NULL) {
        strftime(out, out_len, "%Y%m%d_%H%M%S.txt", &tm_now);
        return true;
    }

    uint16_t y = 0;
    uint8_t  mo = 0, d = 0, h = 0, mi = 0, s = 0;
    ui_gps_get_data(&y, &mo, &d);
    ui_gps_get_time(&h, &mi, &s);
    if (y >= 2024) {
        snprintf(out, out_len, "%04u%02u%02u_%02u%02u%02u.txt",
                 (unsigned)y, mo, d, h, mi, s);
        return true;
    }

    return false;
}

// Save the current buffer under a fresh timestamp filename. Empty buffers
// are skipped so backing out of an untouched editor doesn't litter the FS.
// If no time source is available, refuses the save and shows a toast — the
// user must sync NTP (press 'z') first so the filename can be timestamped.
static notes_save_result_t notes_save_current(void)
{
    if (!notes_editor_ta) return NOTES_SAVE_ERR;
    const char *content = lv_textarea_get_text(notes_editor_ta);
    if (!content || content[0] == '\0') {
        Serial.println("notes_save: empty, skipping");
        return NOTES_SAVE_EMPTY;
    }

    char fname[40];
    if (!notes_make_timestamp_name(fname, sizeof(fname))) {
        Serial.println("notes_save: no time source, refusing to save");
        ui_toast_show("Time not set\nPress 'z' to NTP sync\nthen try again", 4000);
        return NOTES_SAVE_NO_TIME;
    }
    Serial.printf("notes_save: writing '%s' (%u bytes)\n",
                  fname, (unsigned)strlen(content));

    bool ok = ui_notes_write(notes_use_sd, fname, content);
    if (!ok) {
        Serial.println("notes_save: write failed");
        ui_toast_show("Save failed", 3000);
        return NOTES_SAVE_ERR;
    }
    return NOTES_SAVE_OK;
}

static void notes_editor_kb_timer_cb(lv_timer_t *t)
{
    // Drain every buffered key per tick so a burst of presses lands in one
    // textarea update rather than one-per-tick (and one-per-redraw on this
    // e-paper, which would feel painfully slow).
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 'E') { // Enter
            lv_textarea_add_char(notes_editor_ta, '\n');
        } else if (key == 0x08) { // Backspace
            lv_textarea_del_char(notes_editor_ta);
        } else if (key == 0x1B) { // Esc
            notes_save_result_t r = notes_save_current();
            if (r == NOTES_SAVE_NO_TIME || r == NOTES_SAVE_ERR) {
                // Stay in the editor so the user doesn't lose the buffer.
                return;
            }
            scr_mgr_switch(SCREEN0_ID, false);
            return;
        } else if (key >= 32 && key <= 126) {
            lv_textarea_add_char(notes_editor_ta, key);
        }
    }
}

static void notes_save_btn_event_cb(lv_event_t * e)
{
    notes_save_result_t r = notes_save_current();
    if (r == NOTES_SAVE_NO_TIME || r == NOTES_SAVE_ERR) return;
    scr_mgr_pop(false);
}

static void notes_editor_back_btn_event_cb(lv_event_t * e)
{
    notes_save_result_t r = notes_save_current();
    if (r == NOTES_SAVE_NO_TIME || r == NOTES_SAVE_ERR) return;
    scr_mgr_pop(false);
}

static void create12_1(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;

    notes_editor_ta = lv_textarea_create(parent);
    lv_obj_set_size(notes_editor_ta, lv_pct(100), LV_VER_RES - status_bar_height);
    lv_obj_align(notes_editor_ta, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(notes_editor_ta, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_textarea_set_text(notes_editor_ta, "");

    lv_group_t * g = lv_group_get_default();
    if (!g) {
        g = lv_group_create();
        lv_group_set_default(g);
    }
    lv_group_add_obj(g, notes_editor_ta);
    lv_group_focus_obj(notes_editor_ta);
}

static void entry12_1(void)
{
    // Poll the keypad buffer often. The buffer absorbs bursts already, but a
    // tight tick keeps perceived latency low between keypress and the next
    // partial e-paper refresh.
    notes_editor_kb_timer = lv_timer_create(notes_editor_kb_timer_cb, 20, NULL);

    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }

    ui_disp_full_refr();
}

static void exit12_1(void)
{
    if (notes_editor_kb_timer) {
        lv_timer_del(notes_editor_kb_timer);
        notes_editor_kb_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}

static void destroy12_1(void) {
    if(menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
}

scr_lifecycle_t screen12_1 = {
    .create = create12_1,
    .entry = entry12_1,
    .exit  = exit12_1,
    .destroy = destroy12_1,
};
#endif

//************************************[ screen dict ]*************************************** Dictionary (EN -> PT-BR)
#if 1
static lv_obj_t  *dict_query_ta    = NULL;
static lv_obj_t  *dict_result_lab  = NULL;
static lv_timer_t *dict_kb_timer   = NULL;

static void dict_do_lookup(void)
{
    if (!dict_query_ta || !dict_result_lab) return;
    const char *q = lv_textarea_get_text(dict_query_ta);
    if (!q || !*q) return;

    // Trim trailing whitespace/newlines from the textarea before lookup.
    char trimmed[64] = {0};
    size_t out = 0;
    for (size_t i = 0; q[i] && out < sizeof(trimmed) - 1; i++) {
        unsigned char c = (unsigned char)q[i];
        if (c >= 32 && c < 127) trimmed[out++] = (char)c;
    }
    while (out > 0 && trimmed[out - 1] == ' ') trimmed[--out] = '\0';
    if (out == 0) return;

    char *def = ui_dict_lookup(trimmed);
    if (def) {
        lv_label_set_text(dict_result_lab, def);
        free(def);
    } else if (!ui_dict_available()) {
        lv_label_set_text(dict_result_lab, "Dictionary file missing.\n\nPlace eng-pob.tsv in /dict/\non the SD card.");
    } else {
        lv_label_set_text(dict_result_lab, "Word not found.");
    }
    ui_disp_full_refr();
}

static void dict_kb_timer_cb(lv_timer_t *t)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 'E') {                       // Enter -> look up
            dict_do_lookup();
        } else if (key == 0x08) {               // Backspace
            lv_textarea_del_char(dict_query_ta);
        } else if (key == 0x1B) {               // Esc -> back home
            scr_mgr_switch(SCREEN0_ID, false);
            return;
        } else if (key >= 32 && key <= 126) {
            lv_textarea_add_char(dict_query_ta, key);
        }
    }
}

static void create_dict(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;

    dict_query_ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(dict_query_ta, true);
    lv_textarea_set_placeholder_text(dict_query_ta, "type word, press Enter");
    lv_obj_set_size(dict_query_ta, lv_pct(100), 36);
    lv_obj_align(dict_query_ta, LV_ALIGN_TOP_MID, 0, status_bar_height + 2);
    lv_obj_set_style_text_font(dict_query_ta, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_textarea_set_text(dict_query_ta, "");

    lv_group_t *g = lv_group_get_default();
    if (!g) {
        g = lv_group_create();
        lv_group_set_default(g);
    }
    lv_group_add_obj(g, dict_query_ta);
    lv_group_focus_obj(dict_query_ta);

    dict_result_lab = lv_label_create(parent);
    lv_obj_set_size(dict_result_lab, lv_pct(100) - 8, LV_VER_RES - status_bar_height - 50);
    lv_obj_align(dict_result_lab, LV_ALIGN_TOP_MID, 0, status_bar_height + 44);
    lv_obj_set_style_text_font(dict_result_lab, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_long_mode(dict_result_lab, LV_LABEL_LONG_WRAP);
    lv_label_set_text(dict_result_lab, "EN -> PT-BR\n\nType a word and press Enter.");
}

static void entry_dict(void)
{
    dict_kb_timer = lv_timer_create(dict_kb_timer_cb, 20, NULL);
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}

static void exit_dict(void)
{
    if (dict_kb_timer) {
        lv_timer_del(dict_kb_timer);
        dict_kb_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}

static void destroy_dict(void)
{
    if (menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
    dict_query_ta = NULL;
    dict_result_lab = NULL;
}

scr_lifecycle_t screen_dict = {
    .create  = create_dict,
    .entry   = entry_dict,
    .exit    = exit_dict,
    .destroy = destroy_dict,
};
#endif

//************************************[ screen 13 ]***************************************** Ebook Reader List
#if 1

static bool ends_with(const char *str, const char *suffix) {
    if (!str || !suffix)
        return false;
    size_t len_str = strlen(str);
    size_t len_suffix = strlen(suffix);
    if (len_suffix > len_str)
        return false;
    return strncmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
}

static lv_obj_t *reader_list_obj;
static bool reader_use_sd = true; // Always use SD for ebooks
// reader_selected_file, reader_resume_pending, and reader_resume_offset are
// defined earlier in the file so the home-screen 'c' shortcut can reach them.
static lv_group_t *reader_group = NULL;
static lv_timer_t *reader_kb_timer = NULL;

static void reader_apply_focus_style(lv_obj_t *btn)
{
    // Inverted (black bg + white text) when the row is focused via j/k.
    lv_obj_set_style_bg_color(btn, lv_color_black(), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn,   LV_OPA_COVER,    LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, lv_color_white(), LV_PART_MAIN | LV_STATE_FOCUSED);
}

static void reader_list_update(void)
{
    lv_obj_clean(reader_list_obj);
    if (reader_group) lv_group_remove_all_objs(reader_group);

    char list[UI_READER_MAX_COUNT][32];
    int count = 0;
    ui_reader_get_list(reader_use_sd, list, &count);

    for (int i = 0; i < count; i++) {
        if (ends_with(list[i], ".txt") || ends_with(list[i], ".TXT")) {
            lv_obj_t * btn = lv_list_add_btn(reader_list_obj, NULL, list[i]);
            lv_obj_set_style_text_font(btn, FONT_BOLD_SIZE_14, LV_PART_MAIN);
            reader_apply_focus_style(btn);
            if (reader_group) lv_group_add_obj(reader_group, btn);
        }
    }
}

// Pulls the per-file bookmark for `name` and seeds the resume state so
// SCREEN13_1 lands on the saved page instead of page 1.
static void reader_seed_resume_for(const char *name)
{
    strncpy(reader_selected_file, name, 31);
    reader_selected_file[31] = '\0';
    size_t saved_off = 0;
    if (ui_reader_bookmark_get(reader_selected_file, &saved_off)) {
        reader_resume_offset = saved_off;
        reader_resume_pending = true;
    } else {
        reader_resume_offset = 0;
        reader_resume_pending = false;
    }
}

static void reader_list_btn_event_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    const char * name = lv_list_get_btn_text(reader_list_obj, obj);
    if (name) {
        reader_seed_resume_for(name);
        scr_mgr_push(SCREEN13_1_ID, false);
    }
}

static void scr13_back_btn_event_cb(lv_event_t * e)
{
    scr_mgr_pop(false);
}

static void reader_kb_timer_cb(lv_timer_t *t)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (!reader_group) continue;
        if (key == 'j') {
            lv_group_focus_next(reader_group);
            lv_obj_t *f = lv_group_get_focused(reader_group);
            if (f) lv_obj_scroll_to_view(f, LV_ANIM_OFF);
        } else if (key == 'k') {
            lv_group_focus_prev(reader_group);
            lv_obj_t *f = lv_group_get_focused(reader_group);
            if (f) lv_obj_scroll_to_view(f, LV_ANIM_OFF);
        } else if (key == 'E') {
            lv_obj_t *f = lv_group_get_focused(reader_group);
            if (f) {
                const char *name = lv_list_get_btn_text(reader_list_obj, f);
                if (name && name[0]) {
                    reader_seed_resume_for(name);
                    scr_mgr_push(SCREEN13_1_ID, false);
                    return;
                }
            }
        } else if (key == 0x1B) {
            scr_mgr_pop(false);
            return;
        }
    }
}

static void create13(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;

    reader_list_obj = lv_list_create(parent);
    lv_obj_set_size(reader_list_obj, lv_pct(100), LV_VER_RES - status_bar_height);
    lv_obj_align(reader_list_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(reader_list_obj, reader_list_btn_event_cb, LV_EVENT_CLICKED, NULL);

    if (!reader_group) reader_group = lv_group_create();
}

static void entry13(void)
{
    reader_list_update();

    // Focus first item so the selector is visible immediately.
    if (reader_group) {
        lv_obj_t *first = lv_obj_get_child(reader_list_obj, 0);
        if (first) lv_group_focus_obj(first);
    }

    if (!reader_kb_timer) reader_kb_timer = lv_timer_create(reader_kb_timer_cb, 20, NULL);

    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
}

static void exit13(void) {
    if (reader_kb_timer) {
        lv_timer_del(reader_kb_timer);
        reader_kb_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
}

static void destroy13(void) {
    if(menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
}

scr_lifecycle_t screen13 = {
    .create = create13,
    .entry = entry13,
    .exit  = exit13,
    .destroy = destroy13,
};
#endif

//************************************[ screen 13.1 ]*************************************** Ebook Reader View
#if 1

static lv_obj_t *reader_view_label;
static lv_obj_t *reader_view_status;
static lv_timer_t *reader_view_kb_timer = NULL;

#define READER_PAGE_BYTES   1024
#define READER_MAX_PAGES    2048

static char  reader_page_buf[READER_PAGE_BYTES + 1];
static size_t reader_file_size = 0;
static size_t reader_page_offsets[READER_MAX_PAGES];
static int    reader_pages_known = 0;   // number of valid offsets in the array
static int    reader_page_idx = 0;

// Reader font catalog. Each face has its own size list (lengths can differ
// because LVGL's bitmap UNSCII only ships at 8 and 16 px, etc).
//
// E-ink notes:
//  * Mono   = JetBrains Mono Bold (project-bundled). Heavy weight, monospace,
//             reads cleanly down to 14 px on e-paper.
//  * Sans   = LVGL Montserrat. Proportional sans-serif — classic ebook look.
//  * Serif  = LVGL Montserrat Medium at larger sizes used as a contrast face.
//             (LVGL doesn't ship a true serif; this slot holds a bigger,
//             lighter Montserrat range so users have a visibly different
//             second proportional option than "Sans".)
//  * Pixel  = LVGL UNSCII bitmap. Pixel-perfect, no anti-aliasing — looks
//             fantastic on e-paper at very small sizes.
typedef struct {
    const char       *name;
    int               n_sizes;
    const lv_font_t **fonts;
    const int        *pts;
} reader_face_t;

static const lv_font_t *reader_mono_fonts[] = {
    &Font_Mono_Bold_14, &Font_Mono_Bold_16, &Font_Mono_Bold_18, &Font_Mono_Bold_20
};
static const int reader_mono_pts[] = {14, 16, 18, 20};

static const lv_font_t *reader_sans_fonts[] = {
    &lv_font_montserrat_12, &lv_font_montserrat_14,
    &lv_font_montserrat_16, &lv_font_montserrat_18
};
static const int reader_sans_pts[] = {12, 14, 16, 18};

static const lv_font_t *reader_serif_fonts[] = {
    &lv_font_montserrat_14, &lv_font_montserrat_18, &lv_font_montserrat_20
};
static const int reader_serif_pts[] = {14, 18, 20};

static const lv_font_t *reader_pixel_fonts[] = {
    &lv_font_unscii_8, &lv_font_unscii_16
};
static const int reader_pixel_pts[] = {8, 16};

// Spleen — modern pixel font designed for HiDPI / small displays. BSD.
static const lv_font_t *reader_spleen_fonts[] = {
    &lv_font_spleen_5x8, &lv_font_spleen_8x16,
    &lv_font_spleen_12x24, &lv_font_spleen_16x32
};
static const int reader_spleen_pts[] = {8, 16, 24, 32};

// Tamzen — sharp programmer pixel font. MIT-style.
static const lv_font_t *reader_tamzen_fonts[] = {
    &lv_font_tamzen_6x12, &lv_font_tamzen_8x16, &lv_font_tamzen_10x20
};
static const int reader_tamzen_pts[] = {12, 16, 20};

// Tiny — Tom Thumb 3x5, public domain. The smallest legible Latin font.
static const lv_font_t *reader_tiny_fonts[] = {&lv_font_tom_thumb};
static const int reader_tiny_pts[] = {6};

static const reader_face_t reader_faces[] = {
    {"Mono",   4, reader_mono_fonts,   reader_mono_pts},
    {"Sans",   4, reader_sans_fonts,   reader_sans_pts},
    {"Serif",  3, reader_serif_fonts,  reader_serif_pts},
    {"Pixel",  2, reader_pixel_fonts,  reader_pixel_pts},
    {"Spleen", 4, reader_spleen_fonts, reader_spleen_pts},
    {"Tamzen", 3, reader_tamzen_fonts, reader_tamzen_pts},
    {"Tiny",   1, reader_tiny_fonts,   reader_tiny_pts},
};
#define READER_FACE_COUNT ((int)(sizeof(reader_faces)/sizeof(reader_faces[0])))

static const reader_face_t* reader_face_for_slot(int slot)
{
    int f = ui_font_face_get(slot);
    if (f < 0 || f >= READER_FACE_COUNT) f = 0;
    return &reader_faces[f];
}

static int reader_size_clamped_slot(int slot)
{
    const reader_face_t *fc = reader_face_for_slot(slot);
    int s = ui_font_size_get(slot);
    if (s < 0) s = 0;
    if (s >= fc->n_sizes) s = fc->n_sizes - 1;
    return s;
}

static const lv_font_t* reader_font_for_slot(int slot)
{
    return reader_face_for_slot(slot)->fonts[reader_size_clamped_slot(slot)];
}

// Convenience accessors for each slot's resolved font.
static const lv_font_t* topbar_font_get(void)        { return reader_font_for_slot(UI_FONT_SLOT_TOPBAR); }
static const lv_font_t* reader_body_font_get(void)   { return reader_font_for_slot(UI_FONT_SLOT_READER_BODY); }
static const lv_font_t* reader_footer_font_get(void) { return reader_font_for_slot(UI_FONT_SLOT_READER_FOOTER); }

static const lv_font_t* ui_get_font(int pt, bool force_mono)
{
    if (force_mono) {
        // Find the best Mono Bold font for the requested size.
        // We have 14, 15, 16, 17, 18, 19, 20.
        if (pt <= 14) return &Font_Mono_Bold_14;
        if (pt == 15) return &Font_Mono_Bold_15;
        if (pt == 16) return &Font_Mono_Bold_16;
        if (pt == 17) return &Font_Mono_Bold_17;
        if (pt == 18) return &Font_Mono_Bold_18;
        if (pt == 19) return &Font_Mono_Bold_19;
        return &Font_Mono_Bold_20;
    }

    // Non-mono: every general-UI label resolves through here, so route them
    // all through the General slot. The `pt` argument becomes a hint that the
    // selected size index can override — this preserves the prior behavior
    // that all general labels follow one face/size choice.
    return reader_font_for_slot(UI_FONT_SLOT_GENERAL);
}

static const char* reader_font_face_label(int slot)
{
    return reader_face_for_slot(slot)->name;
}

static int reader_font_size_pt(int slot)
{
    return reader_face_for_slot(slot)->pts[reader_size_clamped_slot(slot)];
}

static void reader_font_step_face(int slot, int dir)
{
    int f = (ui_font_face_get(slot) + dir + READER_FACE_COUNT) % READER_FACE_COUNT;
    ui_font_face_set(slot, f);
    // Clamp size to new face's range so we don't index past the new size list.
    int max = reader_faces[f].n_sizes - 1;
    if (ui_font_size_get(slot) > max) ui_font_size_set(slot, max);
}
static void reader_font_step_size(int slot, int dir)
{
    int n = reader_face_for_slot(slot)->n_sizes;
    ui_font_size_set(slot, (ui_font_size_get(slot) + dir + n) % n);
}
static void reader_font_cycle_face(int slot) { reader_font_step_face(slot, +1); }
static void reader_font_cycle_size(int slot) { reader_font_step_size(slot, +1); }

static void scr13_1_back_btn_event_cb(lv_event_t * e)
{
    scr_mgr_pop(false);
}

// The fixed-size byte buffer can hold more text than the label can show at
// the current font/size/orientation — without this clipping the surplus
// bytes were being silently skipped (next_off jumped past unread text).
// Walks the buffer line-by-line using LVGL's wrap routine and returns the
// byte index just past the last line that fully fits in the label.
static size_t reader_clip_to_visible(size_t got)
{
    if (got == 0 || reader_view_label == NULL) return got;
    const lv_font_t *font = reader_body_font_get();
    lv_coord_t line_h = lv_font_get_line_height(font);
    lv_coord_t letter_space = lv_obj_get_style_text_letter_space(reader_view_label, LV_PART_MAIN);
    lv_coord_t line_space = lv_obj_get_style_text_line_space(reader_view_label, LV_PART_MAIN);
    lv_obj_update_layout(reader_view_label);
    lv_coord_t max_w = lv_obj_get_content_width(reader_view_label);
    lv_coord_t avail_h = lv_obj_get_content_height(reader_view_label);
    if (max_w <= 0 || avail_h <= 0 || line_h <= 0) return got;

    // _lv_txt_get_next_line scans a NUL-terminated string; make sure the
    // buffer is terminated at `got` so it doesn't read past valid data.
    reader_page_buf[got] = '\0';

    size_t pos = 0;
    lv_coord_t y = 0;
    while (pos < got) {
        uint32_t consumed = _lv_txt_get_next_line(reader_page_buf + pos, font,
                                                  letter_space, max_w, NULL,
                                                  LV_TEXT_FLAG_NONE);
        if (consumed == 0) break;
        if (y + line_h > avail_h) {
            // This line wouldn't fit fully — push it (and everything after)
            // to the next page by truncating here.
            return pos;
        }
        pos += consumed;
        y += line_h + line_space;
    }
    return got;
}

// Reads and trims one page starting at `off` into reader_page_buf, applying
// the same word-boundary and visible-line-fit rules as
// reader_load_current_page. Returns the byte count consumed (== the page
// size) or 0 at EOF. Used both by the renderer and by reader_seek_to_offset
// to silently walk page boundaries.
static size_t reader_read_one_page(size_t off)
{
    size_t got = ui_reader_read_range(reader_use_sd, reader_selected_file,
                                      off, reader_page_buf, sizeof(reader_page_buf));
    if (got == 0) return 0;

    // If we filled the buffer and aren't at EOF, back up to the last whitespace
    // so we don't cut a word in half.
    bool at_eof = (off + got >= reader_file_size);
    if (!at_eof && got > 64) {
        for (size_t i = got - 1; i > got / 2; i--) {
            char c = reader_page_buf[i];
            if (c == '\n' || c == ' ' || c == '\t') {
                reader_page_buf[i] = '\0';
                got = i;
                break;
            }
        }
    }

    // Trim to the last fully-visible line so the partial bottom row that
    // would otherwise clip against the footer rolls onto the next page.
    size_t visible_got = reader_clip_to_visible(got);
    if (visible_got < got) {
        got = visible_got;
        reader_page_buf[got] = '\0';
    }

    return got;
}

// The bundled fonts (Tamzen, Spleen, UNSCII, Montserrat subset, Mono Bold,
// Tom Thumb) only carry ASCII / basic Latin glyphs, so common ebook
// typography (curly quotes, em/en dashes, ellipsis, NBSP, ...) renders as
// LVGL "tofu" boxes. Fold the well-known codepoints to ASCII at display
// time; pass-through unknown UTF-8 so any font that *does* have the glyph
// (or its tofu) still works.
//
// Substitutions are all same-length-or-shorter than the source bytes, so
// READER_PAGE_BYTES is a safe upper bound for the destination.
static size_t reader_normalize_for_display(const char *src, size_t src_len,
                                           char *dst, size_t dst_size)
{
    size_t i = 0, j = 0;
    if (dst_size == 0) return 0;
    while (i < src_len && j + 1 < dst_size) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) { dst[j++] = src[i++]; continue; }

        const char *rep = NULL;
        size_t seq = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < src_len) {
            seq = 2;
            uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)src[i+1] & 0x3F);
            switch (cp) {
                case 0x00A0: rep = " ";  break; // NBSP
                case 0x00AD: rep = "";   break; // soft hyphen
                case 0x00AB: rep = "<<"; break; // «
                case 0x00BB: rep = ">>"; break; // »
                case 0x00B7: rep = ".";  break; // middle dot
            }
        } else if ((c & 0xF0) == 0xE0 && i + 2 < src_len) {
            seq = 3;
            uint32_t cp = ((c & 0x0F) << 12)
                        | (((unsigned char)src[i+1] & 0x3F) << 6)
                        |  ((unsigned char)src[i+2] & 0x3F);
            switch (cp) {
                case 0x2018: case 0x2019: case 0x201B:
                case 0x2032: case 0x2035:           rep = "'";   break;
                case 0x201A:                        rep = ",";   break;
                case 0x201C: case 0x201D: case 0x201F:
                case 0x2033: case 0x2036:           rep = "\"";  break;
                case 0x201E:                        rep = ",,";  break;
                case 0x2010: case 0x2011: case 0x2012:
                case 0x2013: case 0x2212:           rep = "-";   break;
                case 0x2014: case 0x2015:           rep = "--";  break;
                case 0x2022: case 0x25CF:           rep = "*";   break;
                case 0x2026:                        rep = "..."; break;
                case 0x00A0:                        rep = " ";   break;
            }
        } else if ((c & 0xF8) == 0xF0) {
            seq = 4;
        }

        if (rep) {
            while (*rep && j + 1 < dst_size) dst[j++] = *rep++;
            i += seq;
        } else {
            // Pass UTF-8 sequence through untouched so fonts that do carry
            // the glyph still render it.
            size_t end = i + seq;
            if (end > src_len) end = src_len;
            while (i < end && j + 1 < dst_size) dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
    return j;
}

// Loads the chunk at reader_page_offsets[reader_page_idx], trims to a clean
// word boundary, and records the next page's offset.
static void reader_load_current_page(void)
{
    size_t off = reader_page_offsets[reader_page_idx];
    size_t got = reader_read_one_page(off);
    if (got == 0) {
        lv_label_set_text(reader_view_label, "(end of file)");
        return;
    }

    // Record the next page's start offset if it's the first time we see it.
    // Note: we record the raw file-byte count (`got`) so pagination matches
    // the on-disk text, then normalize a separate display copy below.
    int next_idx = reader_page_idx + 1;
    if (next_idx >= reader_pages_known && reader_pages_known < READER_MAX_PAGES) {
        reader_page_offsets[reader_pages_known] = off + got;
        reader_pages_known = next_idx + 1;
    }

    static char reader_display_buf[READER_PAGE_BYTES + 1];
    reader_normalize_for_display(reader_page_buf, got,
                                 reader_display_buf, sizeof(reader_display_buf));
    lv_label_set_text(reader_view_label, reader_display_buf);

    // Status footer: page number + byte progress
    if (reader_view_status) {
        int pct = reader_file_size ? (int)((100ULL * (off + got)) / reader_file_size) : 0;
        lv_label_set_text_fmt(reader_view_status, "p.%d  %d%%",
                              reader_page_idx + 1, pct);
    }
}

// Walks page boundaries forward from offset 0 using the same trimming rules
// as the renderer, populating reader_page_offsets[0..N] until reaching the
// page that contains `target`. Sets reader_page_idx and reader_pages_known
// so the resumed reader has a correct page number and can navigate back to
// every page leading up to the target. Falls through gracefully at EOF and
// when target falls slightly off a page boundary (e.g., font changed since
// the bookmark was saved).
static void reader_seek_to_offset(size_t target)
{
    reader_page_offsets[0] = 0;
    reader_pages_known = 1;
    reader_page_idx = 0;

    if (target == 0 || reader_file_size == 0) return;

    int idx = 0;
    while (idx + 1 < READER_MAX_PAGES) {
        size_t off = reader_page_offsets[idx];
        size_t got = reader_read_one_page(off);
        if (got == 0) {
            // Hit EOF before reaching the saved offset (file shrank or
            // pagination drifted). Land on the last valid page.
            reader_page_idx = idx;
            return;
        }
        size_t next_off = off + got;
        if (next_off > target) {
            // Target falls inside page `idx`.
            reader_page_idx = idx;
            return;
        }
        idx++;
        reader_page_offsets[idx] = next_off;
        reader_pages_known = idx + 1;
        if (next_off == target) {
            // Target is exactly the start of page `idx`.
            reader_page_idx = idx;
            return;
        }
    }
    // Walked the whole array without reaching target; stick at the last slot.
    reader_page_idx = idx;
}

static void reader_apply_font_and_reflow(void)
{
    lv_obj_set_style_text_font(reader_view_label, reader_body_font_get(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(reader_view_label, ui_reader_line_space_get(), LV_PART_MAIN);
    if (reader_view_status) {
        lv_obj_set_style_text_font(reader_view_status, reader_footer_font_get(), LV_PART_MAIN);
    }
    // Page byte boundaries depend on file content, not font, so we keep the
    // current offset but invalidate any pre-computed page after this one —
    // the *visible* page changes (different font = different wrap) but the
    // start offset is still valid. Forward pages will be re-recorded as the
    // user advances.
    reader_pages_known = reader_page_idx + 1;
    reader_load_current_page();
}

static void reader_apply_rotation(void)
{
    ui_set_reader_landscape(ui_reader_rotation_get() == 1);
}

// Resize page label + status footer for the current orientation. Called on
// entry and on the 'r' rotation toggle: the label height is in pixels, so
// it does not auto-adapt when LV_VER_RES flips between 240 and 320.
static void reader_apply_layout(void)
{
    int status_bar_height = ui_reader_bars_hidden_get() ? 14 : 25;
    int footer_height     = ui_reader_bars_hidden_get() ? 8 : 24;
    int pad_left = 10;
    if (menu_taskbar) {
        if (ui_reader_bars_hidden_get()) lv_obj_add_flag(menu_taskbar, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_clear_flag(menu_taskbar, LV_OBJ_FLAG_HIDDEN);
    }
    if (reader_view_label) {
        lv_obj_set_size(reader_view_label, lv_pct(100),
                        LV_VER_RES - status_bar_height - footer_height);
        lv_obj_align(reader_view_label, LV_ALIGN_TOP_MID, 0, status_bar_height);
        lv_obj_set_style_pad_left(reader_view_label, pad_left, LV_PART_MAIN);
    }
    if (reader_view_status) {
        if (ui_reader_bars_hidden_get()) {
            lv_obj_add_flag(reader_view_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(reader_view_status, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(reader_view_status, lv_pct(100), footer_height);
            lv_obj_align(reader_view_status, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_pad_left(reader_view_status, pad_left, LV_PART_MAIN);
        }
    }
}

static void reader_view_kb_timer_cb(lv_timer_t *t)
{
    char key;
    bool changed = false;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to list
            scr_mgr_pop(false);
            return;
        } else if (key == 'j') { // next page
            size_t next_off = (reader_page_idx + 1 < reader_pages_known)
                                ? reader_page_offsets[reader_page_idx + 1] : 0;
            if (reader_page_idx + 1 < reader_pages_known && next_off < reader_file_size) {
                reader_page_idx++;
                changed = true;
            }
        } else if (key == 'k') { // prev page
            if (reader_page_idx > 0) {
                reader_page_idx--;
                changed = true;
            }
        } else if (key == 'f') { // cycle reader body font face
            reader_font_cycle_face(UI_FONT_SLOT_READER_BODY);
            reader_apply_font_and_reflow();
            ui_disp_full_refr();
        } else if (key == 's') { // cycle reader body font size
            reader_font_cycle_size(UI_FONT_SLOT_READER_BODY);
            reader_apply_font_and_reflow();
            ui_disp_full_refr();
        } else if (key == 'b') { // toggle topbar battery icon + %
            ui_topbar_show_battery_set(!ui_topbar_show_battery_get());
            ui_taskbar_apply_battery_visibility();
            ui_disp_full_refr();
        } else if (key == 'h') { // toggle top/bottom bars for distraction-free reading
            ui_reader_bars_hidden_set(!ui_reader_bars_hidden_get());
            reader_apply_layout();
            // Body region just grew/shrank, so forward page boundaries no
            // longer line up with the new visible height — invalidate them
            // and reflow the current page against the new clip rect.
            reader_apply_font_and_reflow();
            ui_disp_full_refr();
        } else if (key == 'r') { // toggle portrait/landscape
            // The TCA8418 occasionally emits two press events for a single
            // physical key tap (no software debounce in peri_keypad), and
            // because this loop drains all queued keys per tick, two 'r's
            // would toggle rotation back to the original — causing a
            // visible mid-tick portrait flush followed by a landscape one.
            // Debounce: ignore further 'r' presses within 300 ms.
            static uint32_t last_rot_ms = 0;
            uint32_t now = lv_tick_get();
            if (now - last_rot_ms < 300) continue;
            last_rot_ms = now;

            ui_reader_rotation_set(!ui_reader_rotation_get());
            reader_apply_rotation();
            // The label height is fixed in pixels, so it does not adapt
            // to the new LV_VER_RES on its own — re-run the layout.
            reader_apply_layout();
            // After rotation the label width changes, so forward page
            // boundaries no longer line up — invalidate them and reload.
            reader_apply_font_and_reflow();
            ui_disp_full_refr();
        }
    }
    if (changed) {
        reader_load_current_page();
        // Page flips just swap text; let LVGL push them via the panel's fast
        // partial refresh (no clean/flicker cycle). Ghost pixels accumulate,
        // so do one clean full refresh every N pages to clear them.
        static int partial_pages_since_full = 0;
        const int FULL_REFRESH_EVERY = 8;
        if (++partial_pages_since_full >= FULL_REFRESH_EVERY) {
            partial_pages_since_full = 0;
            ui_disp_full_refr();
        }
    }
}

static void create13_1(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;
    int footer_height = 24;

    reader_view_label = lv_label_create(parent);
    lv_obj_set_size(reader_view_label, lv_pct(100),
                    LV_VER_RES - status_bar_height - footer_height);
    lv_obj_align(reader_view_label, LV_ALIGN_TOP_MID, 0, status_bar_height);
    lv_obj_set_style_text_font(reader_view_label, reader_body_font_get(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(reader_view_label, ui_reader_line_space_get(), LV_PART_MAIN);
    lv_label_set_long_mode(reader_view_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(reader_view_label, "");

    reader_view_status = lv_label_create(parent);
    lv_obj_set_size(reader_view_status, lv_pct(100), footer_height);
    lv_obj_align(reader_view_status, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(reader_view_status, reader_footer_font_get(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(reader_view_status, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(reader_view_status, LV_OPA_COVER, LV_PART_MAIN);
    lv_label_set_text(reader_view_status, "");
}

static void entry13_1(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }

    // Apply rotation first; it affects screen dims that the layout reads.
    reader_apply_rotation();

    // The taskbar was built in create13_1 while still in portrait, so its
    // width was captured at 240. After rotating to landscape (320) the
    // right-aligned status cluster (battery, charge, wifi) would otherwise
    // sit ~80 px shy of the true right edge.
    if (menu_taskbar) {
        lv_obj_set_width(menu_taskbar, LV_HOR_RES);
    }

    // Same story for the page label and footer: their heights were sized
    // off LV_VER_RES at create time (portrait 320) and would overflow in
    // landscape (240). reader_apply_layout re-clamps them and applies the
    // landscape left padding.
    reader_apply_layout();

    // Re-apply font in case it was changed in settings since last visit.
    lv_obj_set_style_text_font(reader_view_label, reader_body_font_get(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(reader_view_label, ui_reader_line_space_get(), LV_PART_MAIN);
    if (reader_view_status) {
        lv_obj_set_style_text_font(reader_view_status, reader_footer_font_get(), LV_PART_MAIN);
    }

    // Open the file once and cache its handle for the lifetime of the view.
    // Every page read (resume seek, j/k page flip, font reflow) now reuses
    // this handle instead of reopening, which used to dominate cold-load time
    // on long books — the resume walk reads one ~1 KB chunk per page from 0
    // to the saved offset, and reopening through SD on the shared SPI bus
    // for each of those was the bottleneck.
    reader_file_size = ui_reader_open_view(reader_use_sd, reader_selected_file);
    // If the home-screen 'c' shortcut launched us with a saved offset for
    // this file, walk page boundaries from offset 0 up to that point so
    // both the page number and back-navigation are correct from the very
    // first render. (Previously we just seeded offsets[0] with the saved
    // offset, which made every resumed session look like "page 1" with no
    // history.)
    size_t target_off = 0;
    if (reader_resume_pending && reader_resume_offset < reader_file_size) {
        target_off = reader_resume_offset;
    }
    reader_resume_pending = false;
    reader_resume_offset = 0;
    reader_seek_to_offset(target_off);

    if (reader_file_size == 0) {
        lv_label_set_text_fmt(reader_view_label,
            "Could not open file.\n\nFile: %s",
            reader_selected_file);
        if (reader_view_status) lv_label_set_text(reader_view_status, "");
    } else {
        reader_load_current_page();
    }

    if (!reader_view_kb_timer) {
        reader_view_kb_timer = lv_timer_create(reader_view_kb_timer_cb, 20, NULL);
    }
    ui_disp_full_refr();
}

static void exit13_1(void)
{
    // Persist the page the user was on so the home-screen 'c' shortcut can
    // resume here next time. Save the byte offset of the *current* page
    // start, not the next page, so 'c' lands on the same page they left.
    if (reader_selected_file[0] && reader_file_size > 0 &&
        reader_page_idx >= 0 && reader_page_idx < reader_pages_known) {
        ui_reader_resume_set(reader_selected_file,
                             reader_page_offsets[reader_page_idx]);
    }

    lv_timer_pause(taskbar_update_timer);
    if (reader_view_kb_timer) {
        lv_timer_del(reader_view_kb_timer);
        reader_view_kb_timer = NULL;
    }
    ui_reader_close_view();
    // Other screens are laid out for portrait — restore on the way out so
    // the rest of the UI doesn't render rotated.
    ui_set_reader_landscape(false);
    ui_disp_full_refr();
}

static void destroy13_1(void) {
    if(menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
}

scr_lifecycle_t screen13_1 = {
    .create = create13_1,
    .entry = entry13_1,
    .exit  = exit13_1,
    .destroy = destroy13_1,
};
#endif

//************************************[ screen 13.2 ]*************************************** Reader Font
#if 1

static lv_obj_t *system_font_slot_btn = NULL;
static lv_obj_t *system_font_face_btn = NULL;
static lv_obj_t *system_font_size_btn = NULL;
static lv_obj_t *system_font_lsp_btn  = NULL;
static lv_obj_t *system_font_preview = NULL;
static lv_timer_t *system_font_kb_timer = NULL;

// j/k navigates between the four buttons (Slot, Face, Size, Line space).
// Enter toggles "edit mode" on the focused button — while editing, j/k
// cycles that button's value instead of moving focus. Esc exits edit mode
// if active, otherwise pops the screen. The Slot button picks which font
// slot the Face/Size buttons (and the live preview) operate on. The Line
// space button is reader-only and adjusts the gap between text lines in
// the reader body label.
typedef enum {
    SF_FOCUS_SLOT = 0,
    SF_FOCUS_FACE = 1,
    SF_FOCUS_SIZE = 2,
    SF_FOCUS_LSP  = 3,
    SF_FOCUS_COUNT = 4,
} system_font_focus_t;
static system_font_focus_t system_font_focus = SF_FOCUS_SLOT;
static int  system_font_slot = UI_FONT_SLOT_GENERAL;
static bool system_font_editing = false;

static const char* system_font_slot_label(int slot)
{
    switch (slot) {
        case UI_FONT_SLOT_GENERAL:       return "General";
        case UI_FONT_SLOT_TOPBAR:        return "Top bar";
        case UI_FONT_SLOT_READER_BODY:   return "Reader body";
        case UI_FONT_SLOT_READER_FOOTER: return "Reader footer";
        default:                         return "?";
    }
}

static void system_font_step_slot(int dir)
{
    int s = (system_font_slot + dir + UI_FONT_SLOT_COUNT) % UI_FONT_SLOT_COUNT;
    system_font_slot = s;
}

static void system_font_apply_focus_style(void)
{
    if (!system_font_slot_btn || !system_font_face_btn ||
        !system_font_size_btn || !system_font_lsp_btn) return;

    lv_obj_t *btns[SF_FOCUS_COUNT] = {
        system_font_slot_btn, system_font_face_btn,
        system_font_size_btn, system_font_lsp_btn
    };
    for (int i = 0; i < SF_FOCUS_COUNT; i++) {
        bool focused = (i == (int)system_font_focus);
        bool editing = focused && system_font_editing;
        if (editing) {
            // Inverted: black bg, white text — clearly distinct from focused.
            lv_obj_set_style_bg_color(btns[i], DECKPRO_COLOR_FG, LV_PART_MAIN);
            lv_obj_set_style_text_color(btns[i], DECKPRO_COLOR_BG, LV_PART_MAIN);
            lv_obj_set_style_border_width(btns[i], 3, LV_PART_MAIN);
        } else if (focused) {
            lv_obj_set_style_bg_color(btns[i], DECKPRO_COLOR_BG, LV_PART_MAIN);
            lv_obj_set_style_text_color(btns[i], DECKPRO_COLOR_FG, LV_PART_MAIN);
            lv_obj_set_style_border_width(btns[i], 3, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(btns[i], DECKPRO_COLOR_BG, LV_PART_MAIN);
            lv_obj_set_style_text_color(btns[i], DECKPRO_COLOR_FG, LV_PART_MAIN);
            lv_obj_set_style_border_width(btns[i], 1, LV_PART_MAIN);
        }
    }
}

static void system_font_refresh_labels(void)
{
    int slot = system_font_slot;
    if (system_font_slot_btn) {
        lv_obj_t *lab = lv_obj_get_child(system_font_slot_btn, 0);
        if (lab) lv_label_set_text_fmt(lab, "Slot: %s", system_font_slot_label(slot));
    }
    if (system_font_face_btn) {
        lv_obj_t *lab = lv_obj_get_child(system_font_face_btn, 0);
        if (lab) lv_label_set_text_fmt(lab, "Face: %s", reader_font_face_label(slot));
    }
    if (system_font_size_btn) {
        lv_obj_t *lab = lv_obj_get_child(system_font_size_btn, 0);
        if (lab) lv_label_set_text_fmt(lab, "Size: %d", reader_font_size_pt(slot));
    }
    if (system_font_lsp_btn) {
        lv_obj_t *lab = lv_obj_get_child(system_font_lsp_btn, 0);
        if (lab) lv_label_set_text_fmt(lab, "Line space: %d px", ui_reader_line_space_get());
    }
    if (system_font_preview) {
        lv_obj_set_style_text_font(system_font_preview, reader_font_for_slot(slot), LV_PART_MAIN);
        lv_obj_set_style_text_line_space(system_font_preview, ui_reader_line_space_get(), LV_PART_MAIN);
    }
    system_font_apply_focus_style();
}

// Step the reader line spacing in 2-px increments and wrap within [0, 16].
// 2 px keeps the option count small while still giving a visible difference
// on the e-paper at the reader body's font sizes.
static void system_font_step_line_space(int dir)
{
    int step = 2;
    int v = ui_reader_line_space_get() + dir * step;
    if (v > 16) v = 0;
    if (v < 0)  v = 16;
    ui_reader_line_space_set(v);
}

static void system_font_step_focused(int dir)
{
    int slot = system_font_slot;
    if      (system_font_focus == SF_FOCUS_SLOT) system_font_step_slot(dir);
    else if (system_font_focus == SF_FOCUS_FACE) reader_font_step_face(slot, dir);
    else if (system_font_focus == SF_FOCUS_SIZE) reader_font_step_size(slot, dir);
    else                                         system_font_step_line_space(dir);
}

static void system_font_kb_timer_cb(lv_timer_t *t)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc
            if (system_font_editing) {
                system_font_editing = false;
                system_font_apply_focus_style();
            } else {
                scr_mgr_pop(false);
                return;
            }
        } else if (key == 'E') { // Enter -> toggle edit mode
            system_font_editing = !system_font_editing;
            system_font_apply_focus_style();
        } else if (key == 'j') {
            if (system_font_editing) {
                system_font_step_focused(+1);
                system_font_refresh_labels();
            } else if ((int)system_font_focus < SF_FOCUS_COUNT - 1) {
                system_font_focus = (system_font_focus_t)((int)system_font_focus + 1);
                system_font_apply_focus_style();
            }
        } else if (key == 'k') {
            if (system_font_editing) {
                system_font_step_focused(-1);
                system_font_refresh_labels();
            } else if ((int)system_font_focus > 0) {
                system_font_focus = (system_font_focus_t)((int)system_font_focus - 1);
                system_font_apply_focus_style();
            }
        }
    }
}

static void system_font_slot_event_cb(lv_event_t *e)
{
    if (e->code != LV_EVENT_CLICKED) return;
    system_font_step_slot(+1);
    system_font_refresh_labels();
}

static void system_font_face_event_cb(lv_event_t *e)
{
    if (e->code != LV_EVENT_CLICKED) return;
    reader_font_cycle_face(system_font_slot);
    system_font_refresh_labels();
}

static void system_font_size_event_cb(lv_event_t *e)
{
    if (e->code != LV_EVENT_CLICKED) return;
    reader_font_cycle_size(system_font_slot);
    system_font_refresh_labels();
}

static void system_font_lsp_event_cb(lv_event_t *e)
{
    if (e->code != LV_EVENT_CLICKED) return;
    system_font_step_line_space(+1);
    system_font_refresh_labels();
}

static void create13_2(lv_obj_t *parent)
{
    ui_taskbar_create(parent);
    int status_bar_height = 25;

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), LV_VER_RES - status_bar_height);
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(cont, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cont, DECKPRO_COLOR_BG, LV_PART_MAIN);

    system_font_slot_btn = lv_btn_create(cont);
    lv_obj_set_width(system_font_slot_btn, lv_pct(100));
    lv_obj_set_style_bg_color(system_font_slot_btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(system_font_slot_btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(system_font_slot_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(system_font_slot_btn, 5, LV_PART_MAIN);
    lv_obj_t *sloblbl = lv_label_create(system_font_slot_btn);
    lv_obj_set_style_text_font(sloblbl, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(sloblbl);
    lv_obj_add_event_cb(system_font_slot_btn, system_font_slot_event_cb, LV_EVENT_CLICKED, NULL);

    system_font_face_btn = lv_btn_create(cont);
    lv_obj_set_width(system_font_face_btn, lv_pct(100));
    lv_obj_set_style_bg_color(system_font_face_btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(system_font_face_btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(system_font_face_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(system_font_face_btn, 5, LV_PART_MAIN);
    lv_obj_t *fl = lv_label_create(system_font_face_btn);
    lv_obj_set_style_text_font(fl, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(fl);
    lv_obj_add_event_cb(system_font_face_btn, system_font_face_event_cb, LV_EVENT_CLICKED, NULL);

    system_font_size_btn = lv_btn_create(cont);
    lv_obj_set_width(system_font_size_btn, lv_pct(100));
    lv_obj_set_style_bg_color(system_font_size_btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(system_font_size_btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(system_font_size_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(system_font_size_btn, 5, LV_PART_MAIN);
    lv_obj_t *sl = lv_label_create(system_font_size_btn);
    lv_obj_set_style_text_font(sl, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(sl);
    lv_obj_add_event_cb(system_font_size_btn, system_font_size_event_cb, LV_EVENT_CLICKED, NULL);

    system_font_lsp_btn = lv_btn_create(cont);
    lv_obj_set_width(system_font_lsp_btn, lv_pct(100));
    lv_obj_set_style_bg_color(system_font_lsp_btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(system_font_lsp_btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(system_font_lsp_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(system_font_lsp_btn, 5, LV_PART_MAIN);
    lv_obj_t *lsl = lv_label_create(system_font_lsp_btn);
    lv_obj_set_style_text_font(lsl, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(lsl);
    lv_obj_add_event_cb(system_font_lsp_btn, system_font_lsp_event_cb, LV_EVENT_CLICKED, NULL);

    system_font_preview = lv_label_create(cont);
    lv_obj_set_width(system_font_preview, lv_pct(100));
    lv_label_set_long_mode(system_font_preview, LV_LABEL_LONG_WRAP);
    lv_label_set_text(system_font_preview,
        "The quick brown fox jumps over the lazy dog. 0123456789");
}

static void entry13_2(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    system_font_focus = SF_FOCUS_SLOT;
    system_font_slot = UI_FONT_SLOT_GENERAL;
    system_font_editing = false;
    system_font_refresh_labels();
    if (!system_font_kb_timer)
        system_font_kb_timer = lv_timer_create(system_font_kb_timer_cb, 20, NULL);
}

static void exit13_2(void)
{
    if (system_font_kb_timer) {
        lv_timer_del(system_font_kb_timer);
        system_font_kb_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
}

static void destroy13_2(void) {
    if (menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
    system_font_slot_btn = NULL;
    system_font_face_btn = NULL;
    system_font_size_btn = NULL;
    system_font_lsp_btn  = NULL;
    system_font_preview = NULL;
}

scr_lifecycle_t screen13_2 = {
    .create  = create13_2,
    .entry   = entry13_2,
    .exit    = exit13_2,
    .destroy = destroy13_2,
};
#endif



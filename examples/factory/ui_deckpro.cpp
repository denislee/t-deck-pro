
/* ui_deckpro_int.h brings in common includes, FONT_BOLD_* macros, and
 * extern declarations for cross-TU state (taskbar objects, reader/notes
 * selection state, ui_get_font / topbar_font_get declarations). */
#include "ui_deckpro_int.h"

#define SETTING_PAGE_MAX_ITEM 15
/* GET_BUFF_LEN comes from ui_deckpro_int.h. */

/* Forward declarations for per-screen on_key() handlers still in this TU. */
static void on_key_usb_msc(void);
static void global_kb_timer_cb(lv_timer_t *t);

#define GLOBAL_BUF_LEN 30
#define LOW_VOLTAGE_THRESHOLD_MV 3300
#define LOW_VOLTAGE_SOC_THRESHOLD 5
#define LOW_VOLTAGE_SHUTDOWN_DELAY_MS 20000
#define LOW_VOLTAGE_POLL_MS 2000
static char global_buf[GLOBAL_BUF_LEN];

/* notes_list_obj and notes_use_sd are defined in ui_notes.cpp (the only TU
 * that uses them).  notes_selected_file is shared with the home-screen
 * handler here, so it has external linkage (declared extern in
 * ui_deckpro_int.h). */
char notes_selected_file[32] = {0};

static lv_timer_t *touch_chk_timer = NULL;
/* taskbar_update_timer has external linkage: all split screen TUs pause /
 * resume it on entry/exit (declared extern in ui_deckpro_int.h). */
lv_timer_t *taskbar_update_timer = NULL;
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

// USB MSC screen is defined further down (after the taskbar globals) so its
// entry function can populate menu_taskbar_battery on enter — otherwise the
// top bar shows blank glyphs until a battery-percent change happens to fire.


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

/* menu_taskbar, menu_taskbar_battery, menu_taskbar_battery_percent have
 * external linkage: split screen TUs use them to populate battery labels on
 * entry and to destroy the taskbar on exit (declared extern in
 * ui_deckpro_int.h).  menu_taskbar_time, _charge, _wifi stay static since
 * no split TU references them directly. */
lv_obj_t * menu_taskbar = NULL;
static lv_obj_t * menu_taskbar_time = NULL;
static lv_obj_t * menu_taskbar_charge = NULL;
lv_obj_t * menu_taskbar_battery = NULL;
lv_obj_t * menu_taskbar_battery_percent = NULL;
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

/* ui_taskbar_apply_battery_visibility and ui_taskbar_create have external
 * linkage: every split screen TU that creates a taskbar needs them
 * (declared in ui_deckpro_int.h). */
void ui_taskbar_apply_battery_visibility(void);

void ui_taskbar_create(lv_obj_t *parent)
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

void ui_taskbar_apply_battery_visibility(void)
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

// ******************************** [ screen USB MSC ] ********************************
#if 1
static lv_obj_t *usb_msc_label;

static void create_usb_msc(lv_obj_t *parent)
{
    // Home-screen clock/battery bar instead of the "USB SD Mount <" title.
    // ESC on the keypad pops back to home (see SCREEN_USB_MSC_ID branch in
    // the global keypad handler).
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_color(info, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "\n\nUSB Mass Storage Mode\n\nSD Card is now mounted\non your computer.\n\nDO NOT unplug while\ntransferring files!");
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, status_bar_height + 15);

    usb_msc_label = lv_label_create(parent);
    lv_obj_set_style_text_font(usb_msc_label, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_align(usb_msc_label, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(usb_msc_label, "Status: Active");
}

static void entry_usb_msc(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_usb_msc_begin();
    ui_disp_full_refr();
}

static void exit_usb_msc(void)
{
    lv_timer_pause(taskbar_update_timer);
    ui_usb_msc_end();
    ui_disp_full_refr();
}

static void destroy_usb_msc(void) {}


/* USB MSC screen: only ESC is handled — switch back to home. */
static void on_key_usb_msc(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) {
            scr_mgr_switch(SCREEN0_ID, false);
            return;
        }
    }
}

static scr_lifecycle_t screen_usb_msc = {
    .create = create_usb_msc,
    .entry = entry_usb_msc,
    .exit  = exit_usb_msc,
    .destroy = destroy_usb_msc,
    .on_key    = on_key_usb_msc,
};
#endif

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

    // Only run the gesture-direction timer when the touch controller is actually
    // enabled.  On this board (CST328 hard-disabled at boot) touch_status is
    // false, so the timer would otherwise fire 100×/s reading a gesture that
    // can never be set.
    if (ui_setting_get_touch_status()) {
        lv_timer_resume(touch_chk_timer);
    }
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
//************************************[ screen 2 ]****************************************** Setting
// --------------------- screen 2.1 --------------------- About System
#if 1
static lv_obj_t *scr2_1_cont;

static void create2_1(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "About System <" title bar. ESC on the keypad pops back
    // to Settings via its on_key() handler.
    ui_taskbar_create(parent);

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
}

static void on_key_scr2_1(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to Settings
            scr_mgr_pop(false);
            return;
        }
    }
}

static void entry2_1(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}
static void exit2_1(void) {
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}
static void destroy2_1(void) { }

static scr_lifecycle_t screen2_1 = {
    .create = create2_1,
    .entry = entry2_1,
    .exit  = exit2_1,
    .destroy = destroy2_1,
    .on_key    = on_key_scr2_1,
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
    {.name = "WIFI Setup",      .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN4_ID, .shortcut = 'w'},
    {.name = "System Font",     .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN13_2_ID, .shortcut = 'f'},
    {.name = "Red LED",         .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_red_led,      .get_cb = ui_setting_get_red_led, .shortcut = 'r'},
    {.name = "Keypad Backlight",.type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_keypad_light, .get_cb = ui_setting_get_keypad_light, .shortcut = 'b'},
    {.name = "Motor Status",    .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_motor_status, .get_cb = ui_setting_get_motor_status, .shortcut = 'm'},
    {.name = "Power GPS",       .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gps_status,   .get_cb = ui_setting_get_gps_status, .shortcut = 'g'},
    {.name = "Power Lora",      .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_lora_status,  .get_cb = ui_setting_get_lora_status, .shortcut = 'l'},
    {.name = "Power Gyro",      .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gyro_status,  .get_cb = ui_setting_get_gyro_status, .shortcut = 'y'},
    {.name = "Power A7682",     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_a7682_status, .get_cb = ui_setting_get_a7682_status, .shortcut = 'a'},
    {.name = "Touchscreen",     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_touch_status, .get_cb = ui_setting_get_touch_status, .shortcut = 't'},
    {.name = "Lora",            .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN1_ID, .shortcut = 's'},
    {.name = "GPS",             .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN3_ID, .shortcut = 'p'},
    {.name = "Test",            .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN5_ID, .shortcut = 'x'},
    {.name = "Battery",         .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN6_ID, .shortcut = 'u'},
    {.name = "Input",           .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN7_ID, .shortcut = 'i'},
    {.name = "A7682E",          .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN8_ID, .shortcut = 'e'},
    {.name = "PCM5102",         .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN10_ID, .shortcut = 'c'},
    {.name = "USB SD Mount",    .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN_USB_MSC_ID, .shortcut = 'v'},
    {.name = "Shutdown",        .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN9_ID, .shortcut = 'h'},
    {.name = "Sleep",           .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN11_ID, .shortcut = 'd'},
    {.name = "About System",    .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_1_ID, .shortcut = 'z'},
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
static void on_key_settings(void) {
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
    ui_disp_full_refr();
}
static void exit2(void) {
    lv_timer_pause(taskbar_update_timer);
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
    .on_key    = on_key_settings,
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

static void create3(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "GPS <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler.
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

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

    // Tick counter — kept on this screen but pushed below the taskbar so it
    // doesn't collide with the battery indicator on the right.
    scr3_cnt_lab = lv_label_create(parent);
    lv_obj_set_style_text_font(scr3_cnt_lab, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr3_cnt_lab, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr3_cnt_lab, 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr3_cnt_lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(scr3_cnt_lab, " %05d ", 0);
    lv_obj_align(scr3_cnt_lab, LV_ALIGN_TOP_RIGHT, -10, status_bar_height + 4);
}
static void on_key_scr3(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to settings
            scr_mgr_pop(false);
            return;
        }
    }
}

static void entry3(void)
{
    scr3_GPS_updata();

    ui_gps_task_resume();

    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }

    GPS_loop_timer = lv_timer_create(GPS_loop_timer_event, 3000, NULL);
    ui_disp_full_refr();
}
static void exit3(void) {
    // Keep GPS running in the background so the topbar clock stays sourced
    // from satellite time after the user leaves this screen — suspending it
    // here defeats the no-WiFi time fallback.
    if(GPS_loop_timer) {
        lv_timer_del(GPS_loop_timer);
        GPS_loop_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}
static void destroy3(void) { }

static scr_lifecycle_t screen3 = {
    .create = create3,
    .entry = entry3,
    .exit  = exit3,
    .destroy = destroy3,
    .on_key    = on_key_scr3,
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
    scr4_set_status("Scanning... (esc to cancel)");
    ui_disp_full_refr();
    ui_wifi_scan_start_async();
    if (!scr4_scan_timer) scr4_scan_timer = lv_timer_create(scr4_scan_timer_cb, 200, NULL);
}

static void on_key_scr4(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) {
            // Cancel the in-flight scan if any so the radio doesn't keep
            // chewing CPU after we've left the screen.
            if (scr4_scan_timer) { lv_timer_del(scr4_scan_timer); scr4_scan_timer = NULL; }
            ui_wifi_scan_cancel();
            scr_mgr_pop(false);
            return;
        }
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

// Poll the async WiFi scan started in scr4_start_scan(). We tick on a 200 ms
// cadence so the kb timer keeps running alongside us — that's what lets Esc
// interrupt mid-scan, which a sync WiFi.scanNetworks() can't do.
static void scr4_scan_timer_cb(lv_timer_t *t)
{
    int n = ui_wifi_scan_poll(scr4_results, UI_WIFI_SCAN_ITEM_MAX);
    if (n == -1) return; // still scanning

    lv_timer_del(scr4_scan_timer);
    scr4_scan_timer = NULL;

    if (n == -2) {
        scr4_count = 0;
        scr4_set_status("Scan failed (r retry, esc back)");
        scr4_focus = 0;
        scr4_render_list();
        ui_disp_full_refr();
        return;
    }

    scr4_count = n;

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
    // Use the standard home-screen taskbar (clock + battery + wifi icon)
    // instead of the per-screen "WiFi <" title bar. ESC on the keypad still
    // pops back to settings via its on_key() handler.
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

    scr4_list = lv_list_create(parent);
    lv_obj_set_size(scr4_list, lv_pct(100), lv_pct(80));
    lv_obj_align(scr4_list, LV_ALIGN_TOP_MID, 0, status_bar_height + 4);
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
}

static void entry4(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    scr4_start_scan();
}

static void exit4(void)
{
    if (scr4_scan_timer) { lv_timer_del(scr4_scan_timer); scr4_scan_timer = NULL; }
    // If we're leaving mid-scan (e.g. via Esc), make sure the radio is
    // actually idle and the result buffer is freed.
    ui_wifi_scan_cancel();
    lv_timer_pause(taskbar_update_timer);
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
    .on_key    = on_key_scr4,
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

static void on_key_scr4_1(void)
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
    if (!scr4_1_status_timer) scr4_1_status_timer = lv_timer_create(scr4_1_status_timer_cb, 1000, NULL);
}
static void exit4_1(void) {
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
    .on_key    = on_key_scr4_1,
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
static int test_num = 0;
static int test_page_num = 0;
static int test_curr_page = 0;

static ui_test_handle test_handle_list[] = {
    { .name="Lora",       .peri_id=E_PERI_LORA       , .cb=ui_test_get, .shortcut='l' },
    { .name="Touch",      .peri_id=E_PERI_TOUCH      , .cb=ui_test_get, .shortcut='t' },
    { .name="BQ25896",    .peri_id=E_PERI_BQ25896    , .cb=ui_test_get, .shortcut='5' },
    { .name="BQ27220",    .peri_id=E_PERI_BQ27220    , .cb=ui_test_get, .shortcut='7' },
    { .name="SD Card",    .peri_id=E_PERI_SD         , .cb=ui_test_get, .shortcut='d' },
    { .name="A7682E",     .peri_id=E_PERI_A7682E     , .cb=ui_test_get, .shortcut='a' },
    { .name="PCM5102A",   .peri_id=E_PERI_PCM5102A   , .cb=ui_test_get, .shortcut='p' },
    { .name="Keypad",     .peri_id=E_PERI_KYEPAD     , .cb=ui_test_get, .shortcut='y' },
    { .name="GPS",        .peri_id=E_PERI_GPS        , .cb=ui_test_get, .shortcut='g' },
    { .name="BHI260AP",   .peri_id=E_PERI_BHI260AP   , .cb=ui_test_get, .shortcut='b' },
    { .name="LTR_553ALS", .peri_id=E_PERI_LTR_553ALS , .cb=ui_test_get, .shortcut='r' },
    { .name="INK_SCREEN", .peri_id=E_PERI_INK_SCREEN , .cb=ui_test_get, .shortcut='i' },
};

static void test_item_create(int curr_apge);

static lv_group_t *test_group = NULL;

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
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Test <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler; per-item letter shortcuts focus the
    // matching test row.
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

    if (!test_group) {
        test_group = lv_group_create();
        lv_group_set_wrap(test_group, false);
    }
    test_list = lv_list_create(parent);
    lv_obj_set_size(test_list, LV_HOR_RES, LV_VER_RES - status_bar_height - 8);
    lv_obj_align(test_list, LV_ALIGN_TOP_MID, 0, status_bar_height + 4);
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
}
static void on_key_test(void) {
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
        } else {
            // Per-item shortcut: jump focus to the matching test row.
            for (int i = 0; i < test_num; i++) {
                if (test_handle_list[i].shortcut == key) {
                    int target_page = i / SETTING_PAGE_MAX_ITEM;
                    if (target_page != test_curr_page) {
                        // page navigation expects 'n' = decrement, 'p' = increment
                        while (test_curr_page != target_page) {
                            test_page_switch_internal(test_curr_page < target_page ? 'p' : 'n');
                        }
                    }
                    int idx_on_page = i % SETTING_PAGE_MAX_ITEM;
                    lv_obj_t *obj = lv_obj_get_child(test_list, idx_on_page);
                    if (obj) {
                        lv_group_focus_obj(obj);
                        lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
                    }
                    return;
                }
            }
        }
    }
}
static void entry5(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}
static void exit5(void) {
    lv_timer_pause(taskbar_update_timer);
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
    .on_key    = on_key_test,
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

            if(strcmp("c  BQ25896", str) == 0)
            {
                scr_mgr_push(SCREEN6_1_ID, false);
            }
            if(strcmp("g  BQ27220", str) == 0)
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

static void create6(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Battery <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler. Letter prefixes on the items double as
    // keypad shortcuts handled by the same kb timer.
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

    scr6_list = lv_list_create(parent);
    lv_obj_set_size(scr6_list, lv_pct(93), LV_VER_RES - status_bar_height - 8);
    lv_obj_align(scr6_list, LV_ALIGN_TOP_MID, 0, status_bar_height + 4);
    lv_obj_set_style_pad_top(scr6_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr6_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr6_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr6_list, 0, LV_PART_MAIN);

    scr6_item_create("c  BQ25896", scr6_list_event);
    scr6_item_create("g  BQ27220", scr6_list_event);
}

static void on_key_scr6(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to Settings
            scr_mgr_pop(false);
            return;
        }
        if (key == 'c') { // Charger IC
            scr_mgr_push(SCREEN6_1_ID, false);
            return;
        }
        if (key == 'g') { // Fuel gauge IC
            scr_mgr_push(SCREEN6_2_ID, false);
            return;
        }
    }
}

static void entry6(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}
static void exit6(void) {
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}
static void destroy6(void) { }

static scr_lifecycle_t screen6 = {
    .create = create6,
    .entry = entry6,
    .exit  = exit6,
    .destroy = destroy6,
    .on_key    = on_key_scr6,
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

static void create6_1(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "BQ25896 <" title bar. ESC on the keypad pops back to
    // the Battery menu via its on_key() handler.
    ui_taskbar_create(parent);

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
}

static void on_key_scr6_1(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to Battery menu
            scr_mgr_pop(false);
            return;
        }
    }
}

static void entry6_1(void)
{
    scr6_1_battert_updata();
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
    batt_6_1_timer = lv_timer_create(batt_6_1_updata_timer_event, 5000, NULL);
}
static void exit6_1(void) {
    if(batt_6_1_timer) {
        lv_timer_del(batt_6_1_timer);
        batt_6_1_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}
static void destroy6_1(void) { }

static scr_lifecycle_t screen6_1 = {
    .create = create6_1,
    .entry = entry6_1,
    .exit  = exit6_1,
    .destroy = destroy6_1,
    .on_key    = on_key_scr6_1,
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

static void create6_2(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "BQ27220 <" title bar. ESC on the keypad pops back to
    // the Battery menu via its on_key() handler.
    ui_taskbar_create(parent);

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
}

static void on_key_scr6_2(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == 0x1B) { // Esc -> back to Battery menu
            scr_mgr_pop(false);
            return;
        }
    }
}

static void entry6_2(void)
{
    scr6_2_battert_updata();
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
    batt_6_2_timer = lv_timer_create(batt_6_2_updata_timer_event, 5000, NULL);
}
static void exit6_2(void) {
    if(batt_6_2_timer) {
        lv_timer_del(batt_6_2_timer);
        batt_6_2_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
    ui_disp_full_refr();
}

static void destroy6_2(void) { }

static scr_lifecycle_t screen6_2 = {
    .create = create6_2,
    .entry = entry6_2,
    .exit  = exit6_2,
    .destroy = destroy6_2,
    .on_key    = on_key_scr6_2,
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
        // Esc still owns its usual "pop back" role even on the input visualizer,
        // since this screen owns the keypad queue and a separate kb timer would
        // race us for it.
        if (keypay_v == 0x1B) {
            scr_mgr_pop(false);
            return;
        }
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
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Other <" title bar. ESC on the keypad pops back to
    // Settings — handled inside input_timer_event so we don't fight the
    // visualizer for the keypad queue.
    ui_taskbar_create(parent);

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

}
static void entry7(void)
{
    memcpy(keypad_str, "Keypad: \n", 9);
    keypad_str[9] = '\0';
    keypad_str_len = 9;
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
    input_timer = lv_timer_create(input_timer_event, 50, NULL);
}
static void exit7(void) {
    if(input_timer)
    {
        lv_timer_del(input_timer);
        input_timer = NULL;
    }
    lv_timer_pause(taskbar_update_timer);
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
    {"A7682 Audio", NULL, NULL, ui_a7682_at_cb,        'u'},
    {"Call test",   NULL, NULL, ui_a7682_call_test,    'c'},
    {"AT test",     NULL, NULL, ui_a7682_at_test,      't'},
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

static void on_key_a7682(void) {
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
        } else {
            // Per-item shortcut: focus + click the matching row.
            for (int i = 0; i < a7682_num; i++) {
                if (a7682_handle_list[i].shortcut == key) {
                    int target_page = i / SETTING_PAGE_MAX_ITEM;
                    if (target_page != a7682_curr_page) {
                        while (a7682_curr_page != target_page) {
                            a7682_page_switch_internal(a7682_curr_page < target_page ? 'p' : 'n');
                        }
                    }
                    int idx_on_page = i % SETTING_PAGE_MAX_ITEM;
                    lv_obj_t *obj = lv_obj_get_child(a7682_list, idx_on_page);
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

// scr8_btn_event_cb is kept for the Shutdown/Sleep sub-screens that still
// use the per-screen back button.
static void scr8_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create8(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "A7682E <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler; letter shortcuts pick a row.
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

    if (!a7682_group) {
        a7682_group = lv_group_create();
        lv_group_set_wrap(a7682_group, false);
    }
    a7682_list = lv_list_create(parent);
    lv_obj_set_size(a7682_list, LV_HOR_RES, LV_VER_RES - status_bar_height - 8);
    lv_obj_align(a7682_list, LV_ALIGN_TOP_MID, 0, status_bar_height + 4);
    lv_obj_set_style_bg_color(a7682_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(a7682_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(a7682_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(a7682_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(a7682_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(a7682_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(a7682_list, 0, LV_PART_MAIN);

    a7682_num = sizeof(a7682_handle_list) / sizeof(a7682_handle_list[0]);
    a7682_page_num = a7682_num / SETTING_PAGE_MAX_ITEM;
    a7682_item_create(a7682_curr_page);
}
static void entry8(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}
static void exit8(void) {
    lv_timer_pause(taskbar_update_timer);
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
    .on_key    = on_key_a7682,
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
    {"PCM5102 Audio", NULL, NULL, ui_pcm5102_cb, 'u'},
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

static void on_key_pcm5102(void) {
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
        } else {
            // Per-item shortcut: focus + click the matching row.
            for (int i = 0; i < pcm5102_num; i++) {
                if (pcm5102_handle_list[i].shortcut == key) {
                    int target_page = i / SETTING_PAGE_MAX_ITEM;
                    if (target_page != pcm5102_curr_page) {
                        while (pcm5102_curr_page != target_page) {
                            pcm5102_page_switch_internal(pcm5102_curr_page < target_page ? 'p' : 'n');
                        }
                    }
                    int idx_on_page = i % SETTING_PAGE_MAX_ITEM;
                    lv_obj_t *obj = lv_obj_get_child(pcm5102_list, idx_on_page);
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

static void create10(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "PCM5102 <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler; letter shortcuts pick a row.
    ui_taskbar_create(parent);
    const int status_bar_height = 25;

    if (!pcm5102_group) {
        pcm5102_group = lv_group_create();
        lv_group_set_wrap(pcm5102_group, false);
    }
    pcm5102_list = lv_list_create(parent);
    lv_obj_set_size(pcm5102_list, LV_HOR_RES, LV_VER_RES - status_bar_height - 8);
    lv_obj_align(pcm5102_list, LV_ALIGN_TOP_MID, 0, status_bar_height + 4);
    lv_obj_set_style_bg_color(pcm5102_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(pcm5102_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(pcm5102_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(pcm5102_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(pcm5102_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(pcm5102_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(pcm5102_list, 0, LV_PART_MAIN);

    pcm5102_num = sizeof(pcm5102_handle_list) / sizeof(pcm5102_handle_list[0]);
    pcm5102_page_num = pcm5102_num / SETTING_PAGE_MAX_ITEM;
    pcm5102_item_create(pcm5102_curr_page);
}
static void entry10(void)
{
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}
static void exit10(void)
{
    ui_pcm5102_stop();
    lv_timer_pause(taskbar_update_timer);
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
    .on_key    = on_key_pcm5102,
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

    // Deep sleep never returns, so flush any setting still sitting in the
    // debounced save timer — unlike ui_shutdown_on(), this path does not go
    // through ui_settings_save() on its way down.
    ui_settings_save();

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
                // Stamp every minute-tick so the next cold boot has something
                // recent to restore. NVS writes are wear-level safe at this
                // cadence (~1440/day vs. ~100k cycle endurance).
                ui_time_persist_save();
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

/* ui_toast_show has external linkage: split TUs (reader) show toasts for
 * errors (declared in ui_deckpro_int.h). */
void ui_toast_show(const char *text, uint32_t ms)
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

// ──────────────────────────────────────────────────────────────────────────────
// § 3.10  Non-blocking NTP sync (replaces the delay(500) that froze the UI)
// ──────────────────────────────────────────────────────────────────────────────

// Number of 500 ms polls to attempt before giving up.
#define NTP_POLL_RETRIES 6

static lv_timer_t *s_ntp_poll_timer  = NULL;
static int         s_ntp_retries_left = 0;

static void ntp_check_timer_cb(lv_timer_t *t)
{
    if (ui_time_is_synced() || s_ntp_retries_left <= 0) {
        // Time either synced or we've exhausted retries — show final status.
        if (ui_time_is_synced()) {
            struct tm tm_now;
            ui_time_get_local(&tm_now);
            char buf[64];
            lv_snprintf(buf, sizeof(buf), "Time Synced!\n%02d:%02d:%02d",
                        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
            ui_toast_show(buf, 3000);
        } else {
            ui_toast_show("NTP Request Sent\nWaiting for sync...", 3000);
        }
        ui_disp_full_refr();
        lv_timer_del(t);
        s_ntp_poll_timer  = NULL;
        s_ntp_retries_left = 0;
        return;
    }
    s_ntp_retries_left--;
}

// ──────────────────────────────────────────────────────────────────────────────
// § 3.10  Non-blocking ping (replaces the blocking ui_ping + delay(500))
//
// ui_ping() blocks for up to 2000 ms on an ICMP socket.  Running it on the
// LVGL loop task froze all timers and bq25896_runtime_maintain() for ~2.5 s.
// We offload it to a short-lived FreeRTOS worker task; an lv_timer polls a
// one-item queue for the result and updates the toast from the LVGL context.
// ──────────────────────────────────────────────────────────────────────────────

typedef struct {
    bool ok;
    int  rtt_ms;
} ping_result_t;

static QueueHandle_t s_ping_queue     = NULL;
static lv_timer_t   *s_ping_poll_timer = NULL;
static volatile bool s_ping_active    = false;

static void ping_worker_task(void *param)
{
    // Runs on its own FreeRTOS task — must NOT call any lv_obj_* functions.
    int  rtt = -1;
    bool ok  = ui_ping("1.1.1.1", 2000, &rtt);
    ping_result_t result = {ok, rtt};
    if (s_ping_queue) {
        xQueueSend(s_ping_queue, &result, 0); // non-blocking; queue capacity = 1
    }
    vTaskDelete(NULL); // self-delete
}

static void ping_poll_timer_cb(lv_timer_t *t)
{
    if (!s_ping_queue) {
        // Queue was destroyed (e.g. on re-entry) — abort this timer.
        lv_timer_del(t);
        s_ping_poll_timer = NULL;
        return;
    }

    ping_result_t result;
    if (xQueueReceive(s_ping_queue, &result, 0) != pdTRUE) {
        return; // Worker not done yet — check again next tick.
    }

    // Worker finished — clean up queue and timer.
    vQueueDelete(s_ping_queue);
    s_ping_queue      = NULL;
    s_ping_active     = false;
    lv_timer_del(t);
    s_ping_poll_timer = NULL;

    char buf[96];
    if (result.ok) {
        // Internet reachable — kick off NTP and update the toast.
        ui_wifi_set_tz("<-03>3");
        ui_ntp_resync();
        if (ui_time_is_synced()) {
            struct tm tm_now;
            ui_time_get_local(&tm_now);
            lv_snprintf(buf, sizeof(buf),
                        "Ping 1.1.1.1\nOK  %d ms\nTime %02d:%02d:%02d",
                        result.rtt_ms,
                        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        } else {
            lv_snprintf(buf, sizeof(buf),
                        "Ping 1.1.1.1\nOK  %d ms\nNTP syncing...", result.rtt_ms);
        }
    } else {
        lv_snprintf(buf, sizeof(buf), "Ping 1.1.1.1\nFAIL");
    }
    Serial.printf("[ping] result: %s\n", buf);
    ui_toast_show(buf, 3000);
    ui_disp_full_refr();
}

static void ui_do_ping_1111(void)
{
    Serial.println("[ping] 'p' pressed, launching async ping to 1.1.1.1");

    // Prevent double-launch: if a ping is already in flight, ignore the press.
    if (s_ping_active) {
        Serial.println("[ping] already in flight, ignoring");
        return;
    }

    // Create a one-item result queue before spawning the worker so there is
    // no window where the worker has nowhere to post its result.
    s_ping_queue = xQueueCreate(1, sizeof(ping_result_t));
    if (!s_ping_queue) {
        ui_toast_show("Ping: out of memory", 3000);
        ui_disp_full_refr();
        return;
    }

    // Show the placeholder toast immediately so the e-paper has something to
    // display while the worker runs (up to 2 s).
    ui_toast_show("Pinging 1.1.1.1...", 30000);
    ui_disp_full_refr();

    s_ping_active = true;
    BaseType_t rc = xTaskCreate(ping_worker_task, "ping_work",
                                4096, NULL, 1, NULL);
    if (rc != pdPASS) {
        vQueueDelete(s_ping_queue);
        s_ping_queue  = NULL;
        s_ping_active = false;
        ui_toast_show("Ping: task create failed", 3000);
        ui_disp_full_refr();
        return;
    }

    // Poll for the result every 50 ms; the callback cleans up when done.
    if (s_ping_poll_timer) lv_timer_del(s_ping_poll_timer);
    s_ping_poll_timer = lv_timer_create(ping_poll_timer_cb, 50, NULL);
    lv_timer_set_repeat_count(s_ping_poll_timer, -1); // infinite; self-deletes when result arrives
}

static void ui_do_ntp_sync(void)
{
    Serial.println("[ntp] 'z' pressed, starting NTP sync for Americas/Sao_Paulo");
    if (ui_wifi_get_status() != UI_WIFI_STATUS_CONNECTED) {
        ui_toast_show("WiFi not connected!", 3000);
        return;
    }

    // Cancel any in-flight NTP poll from a previous keypress.
    if (s_ntp_poll_timer) {
        lv_timer_del(s_ntp_poll_timer);
        s_ntp_poll_timer  = NULL;
        s_ntp_retries_left = 0;
    }

    ui_wifi_set_tz("<-03>3");
    ui_ntp_resync();

    // Show a holding toast while the SNTP stack converges.
    ui_toast_show("Syncing time (Sao Paulo)...", 10000);
    ui_disp_full_refr();

    // A repeating timer re-checks ui_time_is_synced() every 500 ms and
    // updates the toast when NTP responds (or after NTP_POLL_RETRIES * 500 ms).
    s_ntp_retries_left = NTP_POLL_RETRIES;
    s_ntp_poll_timer   = lv_timer_create(ntp_check_timer_cb, 500, NULL);
    lv_timer_set_repeat_count(s_ntp_poll_timer, -1); // infinite; self-deletes on completion
}

// Reader resume state is defined here (rather than alongside the reader code
// in ui_reader.cpp) because the home-screen 'c' shortcut in
// menu_keypay_get_event needs to stage a resume target before pushing
// SCREEN13_1.  All three have external linkage so ui_reader.cpp can access
// them (declared extern in ui_deckpro_int.h).
char   reader_selected_file[32] = {0};
// Set true by 'c' on the home screen; entry13_1 consumes it to seed
// reader_page_offsets[0] so the first render is the user's last-read page.
bool   reader_resume_pending = false;
size_t reader_resume_offset  = 0;

/* ---------------------------------------------------------------
 * Global 20 ms keypad dispatch timer  (§5.1 consolidation)
 *
 * One LVGL timer polls the keypad ring-buffer and dispatches each
 * key event to the current screen's on_key() hook.  The hook
 * contains the original while/get/set drain loop verbatim, so all
 * per-key semantics are preserved.  Screens without an on_key hook
 * (SCREEN0, screens handled by menu_keypay_get_event) receive no
 * delivery here — the home-screen timer below handles SCREEN0.
 * --------------------------------------------------------------- */
static lv_timer_t *global_kb_timer = NULL;
static lv_timer_t *menu_timer       = NULL;
static void global_kb_timer_cb(lv_timer_t *t)
{
    scr_lifecycle_t *life = scr_mgr_get_curr_life();
    if (!life || !life->on_key) return;
    /* Snapshot the screen ID so we can detect a navigation event
     * (push/pop/switch) that on_key triggers.  If the screen
     * changes we stop — the next tick will re-fetch the new
     * screen's on_key and continue draining from there. */
    int prev_id = scr_mgr_get_curr_scr_id();
    life->on_key();
    (void)prev_id; /* on_key drains the full buffer internally */
}

// Home-screen keypad handler.  All other screens use the global_kb_timer
// dispatch below (ui_deckpro_entry()) which calls their on_key() hook.
static void menu_keypay_get_event(lv_timer_t *timer)
{
    if (scr_mgr_get_curr_scr_id() != SCREEN0_ID) return;

    char key_val;
    if (!ui_input_get_keypad_val(&key_val)) return;
    Serial.printf("[home] key=0x%02x ('%c')\n",
                  (unsigned char)key_val,
                  (key_val >= 32 && key_val < 127) ? key_val : '?');
    switch (key_val) {
        case 's': scr_mgr_push(SCREEN2_ID,        false); break;
        case 'r': scr_mgr_push(SCREEN13_ID,       false); break;
        case 'c': {
            char saved[32] = {0};
            size_t saved_off = 0;
            if (ui_reader_resume_get(saved, sizeof(saved), &saved_off) && saved[0]) {
                strncpy(reader_selected_file, saved, sizeof(reader_selected_file) - 1);
                reader_selected_file[sizeof(reader_selected_file) - 1] = '\0';
                reader_resume_offset = saved_off;
                reader_resume_pending = true;
                scr_mgr_push(SCREEN13_1_ID, false);
            }
            break;
        }
        case 'n': notes_selected_file[0] = '\0'; scr_mgr_push(SCREEN12_1_ID, false); break;
        case 't': scr_mgr_push(SCREEN_DICT_ID,    false); break;
        case 'q': scr_mgr_push(SCREEN_USB_MSC_ID, false); break;
        case 'l': scr_mgr_push(SCREEN_LOCK_ID,    false); break;
        case 'd': ui_disp_white_clear(); scr_mgr_push(SCREEN11_ID, false); break;
        case 'w': ui_wifi_set_enabled(!ui_wifi_get_enabled()); break;
        case 'p': ui_do_ping_1111(); break;
        case 'z': ui_do_ntp_sync(); break;
        case 'b':
            ui_topbar_show_battery_set(!ui_topbar_show_battery_get());
            ui_taskbar_apply_battery_visibility();
            ui_disp_full_refr();
            break;
        case 0x1B: ui_disp_hard_refr(); break;
    }
    ui_input_set_keypad_flag();
}

extern scr_lifecycle_t screen_lock;   // ui_lockscreen.cpp
extern scr_lifecycle_t screen1;       // ui_lora.cpp
extern scr_lifecycle_t screen1_1;     // ui_lora.cpp
extern scr_lifecycle_t screen1_2;     // ui_lora.cpp
extern scr_lifecycle_t screen12;      // ui_notes.cpp
extern scr_lifecycle_t screen12_1;    // ui_notes.cpp
extern scr_lifecycle_t screen13;      // ui_reader.cpp
extern scr_lifecycle_t screen13_1;    // ui_reader.cpp
extern scr_lifecycle_t screen13_2;    // ui_reader.cpp
extern scr_lifecycle_t screen_dict;   // ui_reader.cpp

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

    global_kb_timer = lv_timer_create(global_kb_timer_cb, 20, NULL);
    menu_timer = lv_timer_create(menu_keypay_get_event, 40, NULL);
}

/*
 * ui_lockscreen.cpp — lock screen (SCREEN_LOCK_ID).
 *
 * Pure move from ui_deckpro.cpp.  No behaviour change.
 */
#include "ui_deckpro_int.h"

/* Forward declaration: on_key_lock is defined below the scr_lifecycle_t
 * initialiser that references it. */
static void on_key_lock(void);

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
static lv_obj_t *lock_wk_label = NULL;
static lv_obj_t *lock_rule = NULL;
static lv_timer_t *lock_clock_timer = NULL;
static int lock_unlock_progress = 0;
static bool lock_saved_wifi_was_enabled = false;
static int lock_last_minute = -1;
static int lock_last_yday = -1;
static bool lock_landscape = false;
// Offset (in months) from today's month for the calendar grid. j/k cycle it
// while the lock screen is up; reset to 0 on entry.
static int lock_month_offset = 0;

// Calendar grid geometry. Portrait uses tamzen_10x20 (3-char cells →
// 30 wide × 20 tall). Landscape switches to spleen_12x24 (3-char cells
// → 36 wide × 24 tall) so the grid is noticeably bigger while still
// fitting 7 cols × 6 rows on the 320×240 panel.
#define LOCK_CAL_CELL_W_P 30
#define LOCK_CAL_CELL_H_P 20
#define LOCK_CAL_Y_P     174   // grid top edge in portrait
#define LOCK_CAL_CELL_W_L 36
#define LOCK_CAL_CELL_H_L 24
#define LOCK_CAL_Y_L      82   // grid top edge in landscape (~12 px bottom margin)
#define LOCK_CAL_X_NUDGE_L 6   // shift the whole calendar block right of true center

static inline int lock_cal_cell_w(void) { return lock_landscape ? LOCK_CAL_CELL_W_L : LOCK_CAL_CELL_W_P; }
static inline int lock_cal_cell_h(void) { return lock_landscape ? LOCK_CAL_CELL_H_L : LOCK_CAL_CELL_H_P; }
static inline int lock_cal_y(void)      { return lock_landscape ? LOCK_CAL_Y_L     : LOCK_CAL_Y_P;     }
static inline int lock_cal_x(void)
{
    int centered = (LV_HOR_RES - 7 * lock_cal_cell_w()) / 2;
    return centered + (lock_landscape ? LOCK_CAL_X_NUDGE_L : 0);
}

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

    int today_year = tm_now->tm_year + 1900;
    int today_mon  = tm_now->tm_mon;        // 0..11
    int today      = tm_now->tm_mday;       // 1..31
    if (today_mon < 0 || today_mon > 11) today_mon = 0;

    // Apply j/k month offset.
    int year = today_year;
    int mon  = today_mon + lock_month_offset;
    while (mon < 0)   { mon += 12; year--; }
    while (mon > 11)  { mon -= 12; year++; }
    bool showing_today_month = (year == today_year) && (mon == today_mon);

    int days_in_month = dim[mon];
    if (mon == 1) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (leap) days_in_month = 29;
    }

    // Weekday of day-1 of the displayed month: mktime fills tm_wday.
    struct tm first = {};
    first.tm_year = year - 1900;
    first.tm_mon  = mon;
    first.tm_mday = 1;
    first.tm_hour = 12;  // avoid DST edge cases at midnight
    mktime(&first);
    int first_wday = first.tm_wday;
    if (first_wday < 0 || first_wday > 6) first_wday = 0;

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
        if (showing_today_month && d == today) { today_col = col; today_row = row; }
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
    if (today_col < 0) {
        if (lock_today_box)   lv_obj_add_flag(lock_today_box, LV_OBJ_FLAG_HIDDEN);
        if (lock_today_label) lv_obj_add_flag(lock_today_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (lock_today_box && lock_today_label && today_col >= 0) {
        int cw = lock_cal_cell_w();
        int ch = lock_cal_cell_h();
        int x = lock_cal_x() + today_col * cw;
        int y = lock_cal_y() + today_row * ch;
        lv_obj_set_size(lock_today_box, cw - 4, ch);
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

    // ── Huge clock (32×64 pixel font) ──
    lock_time_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_time_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_time_label, &lv_font_spleen_32x64, LV_PART_MAIN);
    lv_label_set_text(lock_time_label, "--:--");
    lv_obj_align(lock_time_label, LV_ALIGN_TOP_MID, 0, 16);

    // ── Date subtitle ──
    lock_date_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_date_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_date_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
    lv_label_set_text(lock_date_label, " ");
    lv_obj_align(lock_date_label, LV_ALIGN_TOP_MID, 0, 86);

    // ── Divider above the calendar ──
    lock_rule = lv_obj_create(parent);
    lv_obj_remove_style_all(lock_rule);
    lv_obj_set_size(lock_rule, 200, 1);
    lv_obj_align(lock_rule, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(lock_rule, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lock_rule, LV_OPA_COVER, LV_PART_MAIN);

    // ── Calendar: month name ──
    lock_month_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_month_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_month_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(lock_month_label, 1, LV_PART_MAIN);
    lv_label_set_text(lock_month_label, "");
    lv_obj_align(lock_month_label, LV_ALIGN_TOP_MID, 0, 126);

    // ── Calendar: weekday header (Su Mo Tu We Th Fr Sa) ──
    lock_wk_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_wk_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_wk_label, &lv_font_tamzen_10x20_bold, LV_PART_MAIN);
    lv_label_set_text(lock_wk_label, "Su Mo Tu We Th Fr Sa");
    lv_obj_set_pos(lock_wk_label, lock_cal_x(), lock_cal_y() - 22);

    // ── Today highlight: black box (drawn before label so label sits above) ──
    lock_today_box = lv_obj_create(parent);
    lv_obj_remove_style_all(lock_today_box);
    lv_obj_set_size(lock_today_box, lock_cal_cell_w() - 4, lock_cal_cell_h());
    lv_obj_set_style_bg_color(lock_today_box, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lock_today_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(lock_today_box, LV_OBJ_FLAG_HIDDEN);

    // ── Calendar: date grid ──
    lock_cal_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_cal_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_cal_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
    lv_label_set_text(lock_cal_label, "");
    lv_obj_set_pos(lock_cal_label, lock_cal_x(), lock_cal_y());

    // White today number (drawn above the grid + box)
    lock_today_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_today_label, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_today_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
    lv_label_set_text(lock_today_label, "");
    lv_obj_add_flag(lock_today_label, LV_OBJ_FLAG_HIDDEN);

    // ── Progress dots (intentionally tiny — just a subtle hint) ──
    lock_dots_label = lv_label_create(parent);
    lv_obj_set_style_text_color(lock_dots_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(lock_dots_label, &lv_font_spleen_5x8, LV_PART_MAIN);
    lv_obj_align(lock_dots_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    lock_render_dots();
}

// Lay out the lock screen for the current orientation.
//
// Portrait (240×320): original stacked layout — huge 32×64 clock on top,
//   date subtitle, divider, then the calendar in tamzen_10x20 with 30×20
//   cells.
//
// Landscape (320×240): smaller spleen_16x32 clock at top-left, date
//   right of it on the same row, and a *bigger* spleen_12x24 calendar
//   filling the rest of the panel with 36×24 cells. The divider is
//   dropped and the unlock-progress dots move to the top-right so the
//   6-row grid can extend almost to the panel bottom without overlap.
static void lock_apply_orientation(bool landscape)
{
    if (landscape) {
        if (lock_time_label) {
            lv_obj_set_style_text_font(lock_time_label, &lv_font_spleen_12x24, LV_PART_MAIN);
            lv_obj_align(lock_time_label, LV_ALIGN_TOP_LEFT, 12, 8);
        }
        if (lock_date_label) lv_obj_add_flag(lock_date_label, LV_OBJ_FLAG_HIDDEN);
        if (lock_rule) lv_obj_add_flag(lock_rule, LV_OBJ_FLAG_HIDDEN);

        if (lock_dots_label) lv_obj_align(lock_dots_label, LV_ALIGN_TOP_RIGHT, -12, 12);

        int x = lock_cal_x();
        int grid_w = 7 * lock_cal_cell_w();
        if (lock_month_label) {
            lv_obj_clear_flag(lock_month_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(lock_month_label, &lv_font_spleen_12x24, LV_PART_MAIN);
            // Center the month name over the (nudged) calendar grid, not the
            // panel, so the whole block shifts as one. Fixed-width label +
            // center text alignment is stable across varying month names.
            lv_obj_set_width(lock_month_label, grid_w);
            lv_obj_set_style_text_align(lock_month_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(lock_month_label, LV_ALIGN_TOP_LEFT, x, 36);
        }
        if (lock_wk_label) {
            lv_obj_clear_flag(lock_wk_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(lock_wk_label, &lv_font_terminus_12x24_bold, LV_PART_MAIN);
            lv_obj_set_pos(lock_wk_label, x, lock_cal_y() - 24);
        }
        if (lock_cal_label) {
            lv_obj_clear_flag(lock_cal_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(lock_cal_label, &lv_font_spleen_12x24, LV_PART_MAIN);
            lv_obj_set_pos(lock_cal_label, x, lock_cal_y());
        }
        if (lock_today_label) {
            lv_obj_set_style_text_font(lock_today_label, &lv_font_spleen_12x24, LV_PART_MAIN);
        }
        // today_box size + today_box/today_label positions are reset by
        // lock_render_calendar on the next clock tick (caller invalidates
        // lock_last_yday).
    } else {
        if (lock_time_label) {
            lv_obj_set_style_text_font(lock_time_label, &lv_font_spleen_32x64, LV_PART_MAIN);
            lv_obj_align(lock_time_label, LV_ALIGN_TOP_MID, 0, 16);
        }
        if (lock_date_label) {
            lv_obj_clear_flag(lock_date_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(lock_date_label, LV_ALIGN_TOP_MID, 0, 86);
        }
        if (lock_rule) {
            lv_obj_clear_flag(lock_rule, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(lock_rule, LV_ALIGN_TOP_MID, 0, 118);
        }
        if (lock_dots_label) lv_obj_align(lock_dots_label, LV_ALIGN_BOTTOM_MID, 0, -4);

        int x = lock_cal_x();
        if (lock_month_label) {
            lv_obj_clear_flag(lock_month_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(lock_month_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
            // Undo the fixed-width / centered-text styling landscape applies.
            lv_obj_set_width(lock_month_label, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(lock_month_label, LV_TEXT_ALIGN_AUTO, LV_PART_MAIN);
            lv_obj_align(lock_month_label, LV_ALIGN_TOP_MID, 0, 126);
        }
        if (lock_wk_label) {
            lv_obj_clear_flag(lock_wk_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(lock_wk_label, &lv_font_tamzen_10x20_bold, LV_PART_MAIN);
            lv_obj_set_pos(lock_wk_label, x, lock_cal_y() - 22);
        }
        if (lock_cal_label) {
            lv_obj_clear_flag(lock_cal_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(lock_cal_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
            lv_obj_set_pos(lock_cal_label, x, lock_cal_y());
        }
        if (lock_today_label) {
            lv_obj_set_style_text_font(lock_today_label, &lv_font_tamzen_10x20, LV_PART_MAIN);
        }
    }
}

static void entry_lock(void)
{
    lock_unlock_progress = 0;
    lock_last_minute = -1;
    lock_last_yday = -1;
    lock_month_offset = 0;
    // Restore the last orientation the user toggled to with 'r'. Persisted
    // via Preferences (lk_rot) so a reboot doesn't drop the user back to
    // portrait if they had explicitly switched to landscape.
    lock_landscape = (ui_lock_landscape_get() != 0);
    factory_set_landscape(lock_landscape);
    lock_apply_orientation(lock_landscape);
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

    // Other screens assume portrait — undo any rotation toggled while
    // the lock screen was active before popping back.
    if (lock_landscape) {
        factory_set_landscape(false);
        lock_landscape = false;
    }

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
    lock_wk_label = NULL;
    lock_rule = NULL;
}

scr_lifecycle_t screen_lock = {
    .create = create_lock,
    .entry = entry_lock,
    .exit = exit_lock,
    .destroy = destroy_lock,
    .on_key    = on_key_lock,
};

// Called by the home-screen keypad timer while the lock screen is active.
static void lock_handle_key(char key)
{
    if (key == 'r' || key == 'R') {
        // TCA8418 occasionally emits two press events for one physical
        // tap (see reader-screen 'r' handler) — debounce so a single
        // press doesn't toggle rotation twice.
        static uint32_t last_rot_ms = 0;
        uint32_t now = lv_tick_get();
        if (now - last_rot_ms < 300) return;
        last_rot_ms = now;

        lock_landscape = !lock_landscape;
        factory_set_landscape(lock_landscape);
        lock_apply_orientation(lock_landscape);
        // Persist so the choice survives reboot.
        ui_lock_landscape_set(lock_landscape ? 1 : 0);
        // Force the calendar / today-box positions to be recomputed
        // against the new resolution on the next clock tick.
        lock_last_yday = -1;
        lock_update_clock(true);
        ui_disp_full_refr();
        return;
    }
    if (key == 'j' || key == 'J' || key == 'k' || key == 'K') {
        // j = next month, k = previous month. Clamp to ±120 months so a held
        // key can't run off into ridiculous years.
        int delta = (key == 'j' || key == 'J') ? 1 : -1;
        int next = lock_month_offset + delta;
        if (next < -120) next = -120;
        if (next >  120) next =  120;
        if (next == lock_month_offset) return;
        lock_month_offset = next;
        // Any stray unlock progress is cleared — user is browsing, not unlocking.
        if (lock_unlock_progress != 0) {
            lock_unlock_progress = 0;
            lock_render_dots();
        }
        // Force the renderer to redraw the grid even though tm_yday is unchanged.
        lock_last_yday = -1;
        lock_update_clock(true);
        ui_disp_full_refr();
        return;
    }
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

/* Lock screen on_key hook — delegates to lock_handle_key() which was
 * previously called directly from menu_keypay_get_event(). */
static void on_key_lock(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        lock_handle_key(key);
        /* lock_handle_key may switch screens (unlock); stop draining
         * so we don't deliver keys to the newly active screen here. */
        if (scr_mgr_get_curr_scr_id() != SCREEN_LOCK_ID) break;
    }
}
#endif

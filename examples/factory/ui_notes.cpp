/*
 * ui_notes.cpp — notes list (screen 12) and notes editor (screen 12.1).
 *
 * Pure move from ui_deckpro.cpp.  No behaviour change.
 * notes_list_obj and notes_use_sd are file-local state in this TU.
 * notes_selected_file is defined in ui_deckpro.cpp and accessed here via
 * the extern declaration in ui_deckpro_int.h.
 */
#include "ui_deckpro_int.h"

/* notes_list_obj and notes_use_sd are only needed in this TU. */
static lv_obj_t *notes_list_obj;
static bool notes_use_sd = true;

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

static void on_key_notes_editor(void)
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

    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }

    ui_disp_full_refr();
}

static void exit12_1(void)
{
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
    .on_key    = on_key_notes_editor,
};
#endif


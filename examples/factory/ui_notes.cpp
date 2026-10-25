/*
 * ui_notes.cpp — notes editor (screen 12.1).
 */
#include "ui_deckpro_int.h"

//************************************[ screen 12.1 ]*************************************** Notes Editor
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

    bool ok = ui_notes_write(fname, content);
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
        if (key == KEY_ENTER) { // Enter
            lv_textarea_add_char(notes_editor_ta, '\n');
        } else if (key == KEY_BACKSPACE) { // Backspace
            lv_textarea_del_char(notes_editor_ta);
        } else if (key == KEY_ESC) { // Esc
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

static void create12_1(lv_obj_t *parent)
{
    ui_taskbar_create(parent);

    notes_editor_ta = lv_textarea_create(parent);
    lv_obj_set_size(notes_editor_ta, lv_pct(100), LV_VER_RES - UI_TASKBAR_H);
    lv_obj_align(notes_editor_ta, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(notes_editor_ta, UI_FONT, LV_PART_MAIN);
    lv_textarea_set_text(notes_editor_ta, "");

    lv_group_t * g = ui_default_group();
    lv_group_add_obj(g, notes_editor_ta);
    lv_group_focus_obj(notes_editor_ta);
}

static void entry12_1(void)
{
    ui_taskbar_enter();
    ui_disp_full_refr();
}

static void exit12_1(void)
{
    ui_taskbar_leave();
    ui_disp_full_refr();
}

scr_lifecycle_t screen12_1 = {
    .create = create12_1,
    .entry = entry12_1,
    .exit  = exit12_1,
    .on_key    = on_key_notes_editor,
};

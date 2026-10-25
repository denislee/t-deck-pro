#pragma once
/*
 * ui_deckpro_int.h — internal shared header for ui_deckpro split TUs.
 *
 * This header is NOT part of the public ui_deckpro.h surface.  It is
 * included by ui_reader.cpp, ui_notes.cpp, ui_lora.cpp, ui_lockscreen.cpp
 * and (transitively) the main ui_deckpro.cpp to provide:
 *
 *   • the common includes shared by all split TUs,
 *   • the UI_FONT / UI_FONT_MONO() macros and the shared UI_TASKBAR_H
 *     constant,
 *   • ui_obj_set_hidden(),
 *   • extern declarations for cross-TU state owned by ui_deckpro.cpp,
 *   • the screen lifecycles registered by ui_deckpro_entry(), and
 *   • function declarations for helpers defined in ui_deckpro.cpp and
 *     ui_reader.cpp.
 */

#include "ui_deckpro.h"
#include "src/assets.h"
#include "stdio.h"
#include "ui_deckpro_port.h"
#include "Arduino.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ── Font helpers ────────────────────────────────────────────────────────────
 * Defined in ui_reader.cpp (where the font-face catalog lives).
 * Split TUs that use the UI_FONT macros resolve through these. */
#ifdef __cplusplus
extern "C" {
#endif
const lv_font_t *ui_get_font(int pt, bool force_mono);
const lv_font_t *topbar_font_get(void);
#ifdef __cplusplus
}
#endif

#define GET_BUFF_LEN(a) (sizeof(a)/sizeof(a[0]))

/* ── Shared UI constants ─────────────────────────────────────────────────── */
#define UI_TASKBAR_H     25          /* height of the top-bar / taskbar strip */

/* UI_FONT / UI_FONT_MONO() — every split TU can use these macros.
 * ui_get_font() ignores `pt` for proportional faces, so all general-UI labels
 * share one resolved font; monospace callers pick a point size. */
#define UI_FONT           ui_get_font(0, false)
#define UI_FONT_MONO(pt)  ui_get_font((pt), true)

/* Show or hide a widget (adds / clears LV_OBJ_FLAG_HIDDEN). */
static inline void ui_obj_set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ── Taskbar shared state ────────────────────────────────────────────────────
 * Owned by ui_deckpro.cpp (screen-0 section).
 * Only menu_taskbar itself is shared: ui_reader.cpp toggles its hidden flag
 * when the reader chrome is hidden.  The remaining taskbar widgets and the
 * update timer stay private to ui_deckpro.cpp — split TUs drive them through
 * ui_taskbar_enter() / ui_taskbar_leave(). */
extern lv_obj_t   *menu_taskbar;

/* on_key() for screens whose only key is ESC = back (defined in ui_deckpro.cpp). */
void on_key_esc_pop(void);

/* ── Reader resume state ─────────────────────────────────────────────────────
 * Owned by ui_deckpro.cpp (placed there so the home-screen 'c' shortcut can
 * stage a resume target before pushing SCREEN13_1).
 * ui_reader.cpp reads and writes these in entry13_1 / exit13_1. */
extern char   reader_selected_file[32];
extern bool   reader_resume_pending;
extern size_t reader_resume_offset;

/* ── Taskbar helper functions ────────────────────────────────────────────────
 * Defined in ui_deckpro.cpp.  Every split screen TU that creates a taskbar
 * calls ui_taskbar_create() in its create() function and
 * ui_taskbar_apply_battery_visibility() via on_key_reader_view().
 * ui_taskbar_enter() / ui_taskbar_leave(): call from entry() / exit() of every
 * screen; enter re-points the shared globals at the incoming screen's own
 * taskbar. */
void ui_taskbar_create(lv_obj_t *parent);
void ui_taskbar_apply_battery_visibility(void);
void ui_taskbar_enter(void);
void ui_taskbar_leave(void);

/* ── Shared UI helpers ───────────────────────────────────────────────────────
 * Defined in ui_deckpro.cpp; used by the menu screens in ui_lora.cpp. */

/* Big bordered list row; clicking it pushes screen_id. */
void ui_menu_item_create(lv_obj_t *list, const char *name, int screen_id);

/* 3-state styling for the edit-mode screens (LoRa params, System Font):
 * editing = inverted + 3 px border, focused = 3 px border, idle = 1 px. */
void ui_edit_focus_style(lv_obj_t *obj, bool focused, bool editing);

/* ── Toast notification ──────────────────────────────────────────────────────
 * Defined in ui_deckpro.cpp. ui_reader.cpp calls this to report errors
 * (e.g. PSRAM allocation failure). */
void ui_toast_show(const char *text, uint32_t ms);

/* ── Timing / input-group helpers ────────────────────────────────────────────
 * Defined in ui_deckpro.cpp; used by the split screen TUs that debounce a
 * repeating key ('r') or need the shared default LVGL group. */

/* True if at least ms elapsed since *last_ms (and updates it); false = debounce this event. */
bool ui_debounce(uint32_t *last_ms, uint32_t ms);
/* The default LVGL input group, created on first use. */
lv_group_t *ui_default_group(void);

/* ── Screen lifecycles ───────────────────────────────────────────────────────
 * Owned by the split TUs, registered in ui_deckpro_entry(). */
extern scr_lifecycle_t screen_lock;   // ui_lockscreen.cpp
extern scr_lifecycle_t screen1;       // ui_lora.cpp
extern scr_lifecycle_t screen1_1;     // ui_lora.cpp
extern scr_lifecycle_t screen1_2;     // ui_lora.cpp
extern scr_lifecycle_t screen12_1;    // ui_notes.cpp
extern scr_lifecycle_t screen13;      // ui_reader.cpp
extern scr_lifecycle_t screen13_1;    // ui_reader.cpp
extern scr_lifecycle_t screen13_2;    // ui_reader.cpp
extern scr_lifecycle_t screen_dict;   // ui_reader.cpp

#pragma once
/*
 * ui_deckpro_int.h — internal shared header for ui_deckpro split TUs.
 *
 * This header is NOT part of the public ui_deckpro.h surface.  It is
 * included by ui_reader.cpp, ui_notes.cpp, ui_lora.cpp, ui_lockscreen.cpp
 * and (transitively) the main ui_deckpro.cpp to provide:
 *
 *   • the common includes shared by all split TUs,
 *   • the FONT_BOLD_SIZE_* / FONT_BOLD_MONO_SIZE_* macros,
 *   • extern declarations for cross-TU state owned by ui_deckpro.cpp, and
 *   • function declarations for font helpers defined in ui_reader.cpp.
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
 * Split TUs that use the FONT_BOLD_* macros resolve through these. */
#ifdef __cplusplus
extern "C" {
#endif
const lv_font_t *ui_get_font(int pt, bool force_mono);
const lv_font_t *topbar_font_get(void);
#ifdef __cplusplus
}
#endif

#define GET_BUFF_LEN(a) (sizeof(a)/sizeof(a[0]))

/* FONT_BOLD_SIZE_* — every split TU can use these macros. */
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

/* ── Taskbar shared state ────────────────────────────────────────────────────
 * Owned by ui_deckpro.cpp (screen-0 section).
 * Split screen TUs access these to resume / pause the update timer and to
 * populate the battery labels on screen entry — the same pattern that was
 * used when all code lived in one TU. */
extern lv_obj_t   *menu_taskbar;
extern lv_obj_t   *menu_taskbar_battery;
extern lv_obj_t   *menu_taskbar_battery_percent;
extern lv_timer_t *taskbar_update_timer;

/* ── Reader resume state ─────────────────────────────────────────────────────
 * Owned by ui_deckpro.cpp (placed there so the home-screen 'c' shortcut can
 * stage a resume target before pushing SCREEN13_1).
 * ui_reader.cpp reads and writes these in entry13_1 / exit13_1. */
extern char   reader_selected_file[32];
extern bool   reader_resume_pending;
extern size_t reader_resume_offset;

/* ── Notes selection ─────────────────────────────────────────────────────────
 * Owned by ui_deckpro.cpp; set by the home-screen 'n' shortcut, consumed
 * by the notes entry function. */
extern char notes_selected_file[32];

/* ── Taskbar helper functions ────────────────────────────────────────────────
 * Defined in ui_deckpro.cpp.  Every split screen TU that creates a taskbar
 * calls ui_taskbar_create() in its create() function and
 * ui_taskbar_apply_battery_visibility() via on_key_reader_view(). */
void ui_taskbar_create(lv_obj_t *parent);
void ui_taskbar_apply_battery_visibility(void);

/* ── Toast notification ──────────────────────────────────────────────────────
 * Defined in ui_deckpro.cpp. ui_reader.cpp calls this to report errors
 * (e.g. PSRAM allocation failure). */
void ui_toast_show(const char *text, uint32_t ms);

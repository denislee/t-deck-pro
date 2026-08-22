/*
 * ui_reader.cpp — ebook reader (screens 13, 13.1, 13.2) and
 *                 dictionary screen (screen_dict).
 *
 * Also contains the reader font catalog (reader_faces[]) and the two
 * cross-TU font helpers that all split TUs share:
 *   ui_get_font()     — resolves FONT_BOLD_SIZE_* macros everywhere
 *   topbar_font_get() — used by ui_taskbar_create() in ui_deckpro.cpp
 *
 * Pure move from ui_deckpro.cpp.  No behaviour change.
 */
#include "ui_deckpro_int.h"


//************************************[ screen dict ]*************************************** Dictionary (EN -> PT-BR)
#if 1
static lv_obj_t  *dict_query_ta    = NULL;
static lv_obj_t  *dict_result_lab  = NULL;

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

static void on_key_dict(void)
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
    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
    ui_disp_full_refr();
}

static void exit_dict(void)
{
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
    .on_key    = on_key_dict,
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

static void on_key_reader(void)
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


    lv_timer_resume(taskbar_update_timer);
    if (menu_taskbar) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }
}

static void exit13(void) {
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
    .on_key    = on_key_reader,
};
#endif

//************************************[ screen 13.1 ]*************************************** Ebook Reader View
#if 1

static lv_obj_t *reader_view_label;
static lv_obj_t *reader_view_status;

#define READER_PAGE_BYTES   1024
// Cap on the number of page offsets tracked per session.  Raised to 8192
// (was 2048) now that the array lives in PSRAM rather than internal DRAM.
// At READER_PAGE_BYTES == 1024 bytes/page this covers books up to ~8 MB.
#define READER_MAX_PAGES    8192

// ── § 2.3: reader buffers live in PSRAM, not internal DRAM ───────────────
// Allocated in entry13_1, freed in exit13_1.  Nothing outside that window
// should reference these pointers.
static char   *reader_page_buf     = NULL;  // READER_PAGE_BYTES + 1
static char   *reader_display_buf  = NULL;  // READER_PAGE_BYTES + 1 (was fn-static)
static size_t *reader_page_offsets = NULL;  // READER_MAX_PAGES elements

static size_t reader_file_size   = 0;
static int    reader_pages_known = 0;   // number of valid offsets in the array
static int    reader_page_idx    = 0;

// ── § 3.5 fix 2: page-base for O(1) resume display ───────────────────────
// When resuming via the fast path (reader_page_offsets[0] seeded with the
// saved offset instead of byte 0), reader_page_idx is relative to that
// window, not to the start of the file.  This addend corrects the displayed
// page number and is reset to 0 whenever a full walk from byte 0 runs.
static int    reader_resume_page_base = 0;

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
/* topbar_font_get and ui_get_font have external linkage: they are declared in
 * ui_deckpro_int.h and called from ui_deckpro.cpp and other split TUs. */
const lv_font_t* topbar_font_get(void)        { return reader_font_for_slot(UI_FONT_SLOT_TOPBAR); }
static const lv_font_t* reader_body_font_get(void)   { return reader_font_for_slot(UI_FONT_SLOT_READER_BODY); }
static const lv_font_t* reader_footer_font_get(void) { return reader_font_for_slot(UI_FONT_SLOT_READER_FOOTER); }

const lv_font_t* ui_get_font(int pt, bool force_mono)
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

// ── § 3.5 fix 1: cached layout geometry ──────────────────────────────────────
// max_w / avail_h / line_h / letter_space / line_space only change when the
// font, rotation, or bar visibility changes.  Computing them per page via
// lv_obj_update_layout() caused ~400 full LVGL layout passes when resuming at
// 60% of a 500 KB book.  We now compute them once into s_reader_geo and
// re-compute only on the events that can change these values:
//   • reader_apply_font_and_reflow  (font change)
//   • reader_apply_layout           (label resize on bar toggle / rotation)
//   • entry13_1                     (fresh screen entry)
// Every batch of reader_read_one_page calls is preceded by one
// reader_geo_refresh() call; reader_clip_to_visible uses the cached struct.
typedef struct {
    const lv_font_t *font;
    lv_coord_t       max_w;
    lv_coord_t       avail_h;
    lv_coord_t       line_h;
    lv_coord_t       letter_space;
    lv_coord_t       line_space;
    bool             valid;
} reader_geo_t;

static reader_geo_t s_reader_geo = {NULL, 0, 0, 0, 0, 0, false};

static void reader_geo_invalidate(void)
{
    s_reader_geo.valid = false;
}

// Runs one lv_obj_update_layout pass and caches the resulting geometry.
// Returns true when the cached values are usable.
static bool reader_geo_refresh(void)
{
    if (!reader_view_label) {
        s_reader_geo.valid = false;
        return false;
    }
    lv_obj_update_layout(reader_view_label);
    s_reader_geo.font         = reader_body_font_get();
    s_reader_geo.max_w        = lv_obj_get_content_width(reader_view_label);
    s_reader_geo.avail_h      = lv_obj_get_content_height(reader_view_label);
    s_reader_geo.line_h       = lv_font_get_line_height(s_reader_geo.font);
    s_reader_geo.letter_space = lv_obj_get_style_text_letter_space(reader_view_label, LV_PART_MAIN);
    s_reader_geo.line_space   = lv_obj_get_style_text_line_space(reader_view_label, LV_PART_MAIN);
    s_reader_geo.valid        = (s_reader_geo.max_w  > 0 &&
                                 s_reader_geo.avail_h > 0 &&
                                 s_reader_geo.line_h  > 0);
    return s_reader_geo.valid;
}

// The fixed-size byte buffer can hold more text than the label can show at
// the current font/size/orientation — without this clipping the surplus
// bytes were being silently skipped (next_off jumped past unread text).
// Walks the buffer line-by-line using LVGL's wrap routine and returns the
// byte index just past the last line that fully fits in the label.
//
// § 3.5 fix 1: uses the pre-computed s_reader_geo instead of calling
// lv_obj_update_layout() every invocation.  Caller must ensure
// reader_geo_refresh() was called at least once since the last geometry
// invalidation before starting a batch of reader_read_one_page calls.
static size_t reader_clip_to_visible(size_t got)
{
    if (got == 0 || !s_reader_geo.valid || !reader_page_buf) return got;
    const reader_geo_t *g = &s_reader_geo;

    // _lv_txt_get_next_line scans a NUL-terminated string; make sure the
    // buffer is terminated at `got` so it doesn't read past valid data.
    reader_page_buf[got] = '\0';

    size_t pos = 0;
    lv_coord_t y = 0;
    while (pos < got) {
        uint32_t consumed = _lv_txt_get_next_line(reader_page_buf + pos, g->font,
                                                  g->letter_space, g->max_w, NULL,
                                                  LV_TEXT_FLAG_NONE);
        if (consumed == 0) break;
        if (y + g->line_h > g->avail_h) {
            // This line wouldn't fit fully — push it (and everything after)
            // to the next page by truncating here.
            return pos;
        }
        pos += consumed;
        y += g->line_h + g->line_space;
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
    if (!reader_page_buf) return 0; // guard: PSRAM not yet allocated
    size_t got = ui_reader_read_range(reader_use_sd, reader_selected_file,
                                      off, reader_page_buf, READER_PAGE_BYTES + 1);
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
    if (!reader_page_offsets || !reader_page_buf || !reader_display_buf) return;

    // § 3.5 fix 1: refresh geometry once before the single reader_read_one_page
    // call so reader_clip_to_visible uses a current cached value.
    reader_geo_refresh();

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

    // § 2.3: reader_display_buf is now a PSRAM-allocated file-scope pointer
    // (was a function-static array in internal DRAM).
    reader_normalize_for_display(reader_page_buf, got,
                                 reader_display_buf, READER_PAGE_BYTES + 1);
    lv_label_set_text(reader_view_label, reader_display_buf);

    // Status footer: page number + byte progress.
    // § 3.5 fix 2: add reader_resume_page_base so the displayed page number
    // is accurate even when the O(1) fast path seeds offsets[0] mid-book.
    if (reader_view_status) {
        int pct = reader_file_size ? (int)((100ULL * (off + got)) / reader_file_size) : 0;
        lv_label_set_text_fmt(reader_view_status, "p.%d  %d%%",
                              reader_page_idx + 1 + reader_resume_page_base, pct);
    }
}

// Walks page boundaries forward from offset 0 using the same trimming rules
// as the renderer, populating reader_page_offsets[0..N] until reaching the
// page that contains `target`. Sets reader_page_idx and reader_pages_known
// so the resumed reader has a correct page number and can navigate back to
// every page leading up to the target. Falls through gracefully at EOF and
// when target falls slightly off a page boundary (e.g., font changed since
// the bookmark was saved).
//
// § 3.5 fix 1: reader_geo_refresh() is called once here so the ~N subsequent
// reader_read_one_page → reader_clip_to_visible calls use the cached geometry
// instead of each running a full lv_obj_update_layout pass.
static void reader_seek_to_offset(size_t target)
{
    if (!reader_page_offsets) return;
    reader_page_offsets[0] = 0;
    reader_pages_known = 1;
    reader_page_idx = 0;
    reader_resume_page_base = 0; // full walk from byte 0 — reset the base

    if (target == 0 || reader_file_size == 0) return;

    // One layout pass for the whole seek loop.
    reader_geo_refresh();

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
    // § 3.5 fix 1: font change invalidates line metrics in the geometry cache.
    reader_geo_invalidate();
    reader_load_current_page(); // will call reader_geo_refresh() internally
}

static void reader_apply_rotation(void)
{
    ui_set_reader_landscape(ui_reader_rotation_get() == 1);
    // Rotation changes LV_HOR_RES / LV_VER_RES → label size → cached geometry.
    // reader_apply_layout() must be called next; it will also invalidate.
    reader_geo_invalidate();
}

// Resize page label + status footer for the current orientation. Called on
// entry and on the 'r' rotation toggle: the label height is in pixels, so
// it does not auto-adapt when LV_VER_RES flips between 240 and 320.
// § 3.5 fix 1: label geometry changes here → invalidate the cached layout so
// the next reader_geo_refresh() picks up the new dimensions.
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
    // Label size just changed; the cached geometry (max_w, avail_h) is now
    // stale.  The next reader_geo_refresh() will re-measure.
    reader_geo_invalidate();
}

static void on_key_reader_view(void)
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
            } else if (reader_resume_page_base > 0 &&
                       reader_page_offsets && reader_page_offsets[0] > 0) {
                // § 3.5 fix 2: O(1) resume window — we're at the start of the
                // window but there are pages before it in the file.  Fall back
                // to a full walk from byte 0 so the user can navigate back.
                // This is O(pages from 0 to window start), but it only
                // triggers if the user explicitly pages back past the resume
                // point, which is the correct trade-off.
                size_t before = reader_page_offsets[0] - 1;
                reader_seek_to_offset(before); // resets reader_resume_page_base to 0
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

// ── § 3.5 fix 2: O(1) resume helpers ─────────────────────────────────────────
// ui_reader_bookmark_page_get / ui_reader_bookmark_page_set in ui_deckpro_port.cpp
// persist the per-file page index.  A geometry cookie guards against using a
// saved index that was computed under different font/rotation/bar settings.
//
// The cookie is a compact encoding of every setting that affects page wrapping.
// It is stored in a file-scope static: within the same session this is exact;
// across a reboot the cookie is always 0 (initial value), which will never
// match a non-zero saved cookie (they differ unless ALL settings are at their
// default values).  A mismatch falls back to the full O(N) walk — correct but
// slow.  For full cross-reboot cookie persistence a "c_%08x" NVS key alongside
// the "p_%08x" page key in ui_deckpro_port.cpp would be needed; see the
// note in ui_deckpro_port.h near ui_reader_bookmark_page_set() for the slot.
// ─────────────────────────────────────────────────────────────────────────────

// Last geo cookie written at exit13_1; 0 means "unknown / not yet set".
static uint32_t s_last_saved_cookie = 0;

// Computes a 32-bit cookie that captures every setting that affects page
// boundaries (font face, font size, rotation, bar visibility, line spacing).
// A mismatch between the saved cookie and the current one means the saved
// page index was computed under different geometry and must not be trusted.
static uint32_t reader_geo_cookie(void)
{
    uint32_t c = 0;
    c |= ((uint32_t)(ui_font_face_get(UI_FONT_SLOT_READER_BODY) & 0x0F));
    c |= ((uint32_t)(ui_font_size_get(UI_FONT_SLOT_READER_BODY) & 0x0F)) << 4;
    c |= ((uint32_t)(ui_reader_rotation_get() ? 1u : 0u)) << 8;
    c |= ((uint32_t)(ui_reader_bars_hidden_get() ? 1u : 0u)) << 9;
    c |= ((uint32_t)((uint32_t)ui_reader_line_space_get() & 0x3Fu)) << 10;
    return c;
}

static void entry13_1(void)
{
    // ── § 2.3: allocate reader buffers from PSRAM ─────────────────────────
    // Guard against double-allocation (re-entry without exit in between).
    if (!reader_page_buf) {
        reader_page_buf = (char *)ps_malloc(READER_PAGE_BYTES + 1);
    }
    if (!reader_display_buf) {
        reader_display_buf = (char *)ps_malloc(READER_PAGE_BYTES + 1);
    }
    if (!reader_page_offsets) {
        reader_page_offsets = (size_t *)ps_malloc(READER_MAX_PAGES * sizeof(size_t));
    }
    if (!reader_page_buf || !reader_display_buf || !reader_page_offsets) {
        // PSRAM allocation failed — free whatever succeeded, show toast, abort.
        free(reader_page_buf);     reader_page_buf     = NULL;
        free(reader_display_buf);  reader_display_buf  = NULL;
        free(reader_page_offsets); reader_page_offsets = NULL;
        Serial.println("[reader] PSRAM alloc failed — refusing to open reader");
        ui_toast_show("Reader: not enough PSRAM", 4000);
        ui_disp_full_refr();
        // The screen is already pushed; pop it so we don't land on a blank view.
        scr_mgr_pop(false);
        return;
    }
    // ──────────────────────────────────────────────────────────────────────

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
    // landscape left padding.  Also invalidates the geometry cache.
    reader_apply_layout();

    // Re-apply font in case it was changed in settings since last visit.
    lv_obj_set_style_text_font(reader_view_label, reader_body_font_get(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(reader_view_label, ui_reader_line_space_get(), LV_PART_MAIN);
    if (reader_view_status) {
        lv_obj_set_style_text_font(reader_view_status, reader_footer_font_get(), LV_PART_MAIN);
    }
    // Font styles were just (re-)applied — geometry cache is stale.
    reader_geo_invalidate();

    // Open the file once and cache its handle for the lifetime of the view.
    // Every page read (resume seek, j/k page flip, font reflow) now reuses
    // this handle instead of reopening.
    reader_file_size = ui_reader_open_view(reader_use_sd, reader_selected_file);

    if (reader_resume_pending && reader_resume_offset < reader_file_size) {
        // ── § 3.5 fix 2: O(1) resume fast path ───────────────────────────
        // Use the saved page index (ui_reader_bookmark_page_get) to seed the
        // offset array at the resume position, skipping the O(N) walk.
        // The geometry cookie guards against a saved index that was computed
        // under different font/rotation/bar settings — on mismatch we fall
        // back to the correct-but-slow full walk.
        int      saved_page   = -1;
        uint32_t curr_cookie  = reader_geo_cookie();
        bool     page_ok      = ui_reader_bookmark_page_get(reader_selected_file,
                                                            &saved_page);

        if (page_ok && saved_page >= 0 && curr_cookie == s_last_saved_cookie) {
            // Geometry matches the session that saved this index.  Seed the
            // window directly at the resume offset; the user can still read
            // forward normally.  Pressing 'k' past slot 0 triggers a lazy
            // full walk from byte 0 (handled in the reader view on_key() handler).
            reader_page_offsets[0] = reader_resume_offset;
            reader_pages_known     = 1;
            reader_page_idx        = 0;
            reader_resume_page_base = saved_page; // accurate page-number display
            Serial.printf("[reader] O(1) resume: page %d, offset %u, cookie 0x%08x\n",
                          saved_page, (unsigned)reader_resume_offset, curr_cookie);
        } else {
            // No saved index, or the geometry changed since the index was
            // captured: full walk from byte 0 (O(N) but always correct).
            Serial.printf("[reader] full-walk resume to offset %u (%s)\n",
                          (unsigned)reader_resume_offset,
                          !page_ok  ? "no saved page" :
                          saved_page < 0 ? "invalid page" : "cookie mismatch");
            reader_seek_to_offset(reader_resume_offset);
        }
        // ─────────────────────────────────────────────────────────────────
    } else {
        // No resume pending — start from the beginning of the file.
        reader_seek_to_offset(0);
    }

    reader_resume_pending = false;
    reader_resume_offset  = 0;

    if (reader_file_size == 0) {
        lv_label_set_text_fmt(reader_view_label,
            "Could not open file.\n\nFile: %s",
            reader_selected_file);
        if (reader_view_status) lv_label_set_text(reader_view_status, "");
    } else {
        reader_load_current_page();
    }

    ui_disp_full_refr();
}

static void exit13_1(void)
{
    // ── § 3.5 fix 2: persist page index + geo cookie for O(1) resume ─────
    if (reader_selected_file[0] && reader_file_size > 0 &&
        reader_page_offsets &&
        reader_page_idx >= 0 && reader_page_idx < reader_pages_known) {
        size_t   off       = reader_page_offsets[reader_page_idx];
        int      abs_page  = reader_page_idx + reader_resume_page_base;
        uint32_t cookie    = reader_geo_cookie();
        // ui_reader_resume_set persists filename + byte offset (unchanged).
        ui_reader_resume_set(reader_selected_file, off);
        // ui_reader_bookmark_page_set persists the absolute page index so
        // next entry can seed offsets[0] at the resume offset without walking.
        ui_reader_bookmark_page_set(reader_selected_file, abs_page);
        // Remember the geometry under which this index was valid; the next
        // entry13_1 compares this cookie against the current settings.
        s_last_saved_cookie = cookie;
    }
    reader_resume_page_base = 0;
    // ─────────────────────────────────────────────────────────────────────

    lv_timer_pause(taskbar_update_timer);
    ui_reader_close_view();
    // Other screens are laid out for portrait — restore on the way out so
    // the rest of the UI doesn't render rotated.
    ui_set_reader_landscape(false);

    // ── § 2.3: free PSRAM buffers — they are not needed between sessions ──
    free(reader_page_buf);     reader_page_buf     = NULL;
    free(reader_display_buf);  reader_display_buf  = NULL;
    free(reader_page_offsets); reader_page_offsets = NULL;
    // ─────────────────────────────────────────────────────────────────────

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
    .on_key    = on_key_reader_view,
};
#endif

//************************************[ screen 13.2 ]*************************************** Reader Font
#if 1

static lv_obj_t *system_font_slot_btn = NULL;
static lv_obj_t *system_font_face_btn = NULL;
static lv_obj_t *system_font_size_btn = NULL;
static lv_obj_t *system_font_lsp_btn  = NULL;
static lv_obj_t *system_font_preview = NULL;

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

static void on_key_system_font(void)
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
}

static void exit13_2(void)
{
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
    .on_key    = on_key_system_font,
};
#endif



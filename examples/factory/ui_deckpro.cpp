
/* ui_deckpro_int.h brings in common includes, the UI_FONT / UI_FONT_MONO()
 * macros, and extern declarations for cross-TU state (taskbar objects,
 * reader selection state, ui_get_font / topbar_font_get declarations). */
#include "ui_deckpro_int.h"
#include "driver/rtc_io.h"

#define SETTING_PAGE_MAX_ITEM 15
/* GET_BUFF_LEN comes from ui_deckpro_int.h. */

#define KV_LINE_MAX 23
#define LOW_VOLTAGE_THRESHOLD_MV 3300
#define LOW_VOLTAGE_SHUTDOWN_DELAY_MS 20000
#define LOW_VOLTAGE_POLL_MS 10000
// Lock the device after this long without a keypress.
#define AUTO_LOCK_IDLE_MS (10UL * 60UL * 1000UL)

/* Private to this TU: split screens pause / resume it through
 * ui_taskbar_leave() / ui_taskbar_enter(). */
static lv_timer_t *taskbar_update_timer = NULL;
static lv_timer_t *low_voltage_timer = NULL;
static lv_obj_t *label_list[10] = {0};
static int taskbar_state[TASKBAR_ID_MAX] = {0};
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

    ui_obj_set_hidden(low_voltage_popup, !visible);
    if (visible) {
        lv_obj_move_foreground(low_voltage_popup);
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
    lv_obj_set_style_text_font(title, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(title, "LOW VOLTAGE");

    low_voltage_countdown_label = lv_label_create(low_voltage_popup);
    lv_obj_set_width(low_voltage_countdown_label, lv_pct(100));
    lv_obj_set_style_text_font(low_voltage_countdown_label, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(low_voltage_countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(low_voltage_countdown_label, LV_LABEL_LONG_WRAP);
    low_voltage_popup_update(LOW_VOLTAGE_SHUTDOWN_DELAY_MS / 1000);

    low_voltage_popup_set_visible(false);
}

static bool low_voltage_should_latch(void)
{
    bool voltage_low = false;
    bool gauge_low = false;

    if (ui_test_get(E_PERI_BQ27220)) {
        uint16_t vbat_mv = (uint16_t)ui_battery_27220_get_voltage();
        voltage_low = (vbat_mv <= LOW_VOLTAGE_THRESHOLD_MV);
    }

    if (ui_battery_27220_is_vaild()) {
        gauge_low = ui_battery_27220_is_low_alarm();
    }

    return voltage_low || gauge_low;
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
    if (ui_battery_is_external_power_present()) {
        lv_timer_set_period(t, LOW_VOLTAGE_POLL_MS);
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

    lv_timer_set_period(t, 1000);  // tick the countdown once a second while latched

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
/* Screen title with a back arrow; ESC (on_key) is what actually goes back. */
static lv_obj_t *scr_title_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label2 = lv_label_create(parent);
    lv_obj_align(label2, LV_ALIGN_TOP_LEFT, 3, 10);
    lv_obj_set_style_text_color(label2, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label2, LV_SYMBOL_LEFT);

    lv_obj_t *label = lv_label_create(parent);
    lv_obj_align_to(label, label2, LV_ALIGN_OUT_RIGHT_MID, 5, -1);
    lv_obj_set_style_text_font(label, UI_FONT_MONO(15), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label, text);

    return label;
}

/* on_key for screens whose only key is ESC = back. */
void on_key_esc_pop(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) {
            scr_mgr_pop();
            return;
        }
    }
}

// USB MSC screen is defined further down (after the taskbar globals) so its
// entry function can populate menu_taskbar_battery on enter — otherwise the
// top bar shows blank glyphs until a battery-percent change happens to fire.


/* ── Key/value info screens (GPS, BQ25896, BQ27220) ───────────────────────── */

/* Left-align k, then v so the value column ends one char before the screen edge. */
static void kv_format(char *out, size_t n, int width, const char *k, const char *v)
{
    lv_snprintf(out, n, "%-*s%s", width - (int)strlen(v), k, v);
}

static void kv_set_line(lv_obj_t *label, const char *k, const char *v)
{
    int w2 = strlen(v);
    int w1 = KV_LINE_MAX - w2;
    lv_label_set_text_fmt(label, "%-*s%-*s", w1, k, w2, v);
}

static lv_obj_t * kv_label_create(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, UI_FONT_MONO(15), LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

/* Bottom-anchored column of label_list[] rows shared by the info screens. */
static lv_obj_t *kv_screen_create(lv_obj_t *parent)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(cont, 0, LV_PART_MAIN);
    lv_obj_set_align(cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = kv_label_create(cont);
        lv_label_set_text(label_list[i], " ");
    }
    return cont;
}

/* ── Timing / input-group helpers ─────────────────────────────────────────── */

/* True if at least ms elapsed since *last_ms (and updates it); false = debounce this event. */
bool ui_debounce(uint32_t *last_ms, uint32_t ms)
{
    uint32_t now = lv_tick_get();
    if (now - *last_ms < ms) return false;
    *last_ms = now;
    return true;
}

/* The default LVGL input group, created on first use. */
lv_group_t *ui_default_group(void)
{
    lv_group_t *g = lv_group_get_default();
    if (!g) {
        g = lv_group_create();
        lv_group_set_default(g);
    }
    return g;
}

/* ── Shared styles ─────────────────────────────────────────────────────────
 * Every lv_obj_set_style_*() allocates a local style entry in the LVGL heap for
 * that one object, and the list screens build dozens of rows that all repeat
 * the same handful of properties.  These three lv_style_t live for the whole
 * run and are added by reference (lv_obj_add_style), so N rows cost one style
 * each instead of one style entry per property.
 *
 * text_font stays out of them: UI_FONT changes at runtime with the System Font
 * setting, so it must remain a per-object local style.  Same for height/size,
 * which differ per row. */
static lv_style_t s_style_row;        /* list row base look */
static lv_style_t s_style_focus_inv;  /* focused row: inverted */
static lv_style_t s_style_menu_item;  /* big menu buttons */

static void ui_styles_init(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    lv_style_init(&s_style_row);
    lv_style_set_pad_left(&s_style_row, 5);
    lv_style_set_pad_right(&s_style_row, 5);
    lv_style_set_bg_color(&s_style_row, DECKPRO_COLOR_BG);
    lv_style_set_text_color(&s_style_row, DECKPRO_COLOR_FG);
    lv_style_set_border_width(&s_style_row, 0);   // No border for cleaner look
    lv_style_set_radius(&s_style_row, 0);         // Square edges for list items

    /* Inverted row when focused via j/k.  The reader list also relies on the
     * opaque background here; every list button already paints LV_OPA_COVER
     * from the theme, so carrying it over keeps the reader look explicit. */
    lv_style_init(&s_style_focus_inv);
    lv_style_set_bg_color(&s_style_focus_inv, DECKPRO_COLOR_FG);
    lv_style_set_bg_opa(&s_style_focus_inv, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_focus_inv, DECKPRO_COLOR_BG);
    lv_style_set_outline_width(&s_style_focus_inv, 0);

    lv_style_init(&s_style_menu_item);
    lv_style_set_border_width(&s_style_menu_item, 1);
    lv_style_set_radius(&s_style_menu_item, 10);
}

/* Add the shared inverted-focus look (LV_PART_MAIN | LV_STATE_FOCUSED) to obj. */
void ui_style_add_focus_inverted(lv_obj_t *obj)
{
    if (!obj) return;
    ui_styles_init();
    lv_obj_add_style(obj, &s_style_focus_inv, LV_PART_MAIN | LV_STATE_FOCUSED);
}

/* ── Shared list helpers (Settings / Test / A7682 / PCM5102) ──────────────── */

/* Create the group if needed (wrap off). */
static lv_group_t *ui_list_group_get(lv_group_t **g)
{
    if (!*g) {
        *g = lv_group_create();
        lv_group_set_wrap(*g, false); // Disable wrap to detect boundaries
    }
    return *g;
}

/* Full-width list below the taskbar, used by the Settings/Test/A7682/PCM5102 screens. */
static lv_obj_t *ui_list_create(lv_obj_t *parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, LV_HOR_RES, h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list, 0, LV_PART_MAIN);
    return list;
}

/* 18 px row: pads, font, bg/fg colours, border 0, radius 0, focus style, add to group. */
static void ui_list_style_row(lv_obj_t *btn, lv_group_t *g)
{
    ui_styles_init();
    lv_obj_set_height(btn, 18); // Ultra-compact height
    lv_obj_set_style_text_font(btn, UI_FONT, LV_PART_MAIN); // runtime font setting: stays per-object
    lv_obj_add_style(btn, &s_style_row, LV_PART_MAIN);

    ui_style_add_focus_inverted(btn);
    if (g) lv_group_add_obj(g, btn);
}

/* j/k move focus inside g (scroll into view), Enter clicks the focused row.
 * Returns true if the key was one of those. Does not page. */
static bool ui_list_nav_key(lv_group_t *g, char key)
{
    if (key != 'j' && key != 'k' && key != KEY_ENTER) return false;
    if (!g) return true;

    if (key == KEY_ENTER) {
        lv_obj_t *f = lv_group_get_focused(g);
        if (f) lv_event_send(f, LV_EVENT_CLICKED, NULL);
        return true;
    }

    if (key == 'j') lv_group_focus_next(g);
    else            lv_group_focus_prev(g);

    lv_obj_t *new_f = lv_group_get_focused(g);
    if (new_f) lv_obj_scroll_to_view(new_f, LV_ANIM_OFF);
    return true;
}

/* ── Submenu rows ─────────────────────────────────────────────────────────── */

static void ui_menu_item_event_cb(lv_event_t *e)
{
    scr_mgr_push((int)(intptr_t)lv_event_get_user_data(e));
}

/* Big bordered list row; clicking it pushes screen_id. */
void ui_menu_item_create(lv_obj_t *list, const char *name, int screen_id)
{
    lv_obj_t * obj = lv_obj_class_create_obj(&lv_list_btn_class, list);
    lv_obj_class_init_obj(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_set_height(obj, LV_VER_RES / 6);
    lv_obj_set_style_text_font(obj, UI_FONT, LV_PART_MAIN); // runtime font setting: stays per-object
    ui_styles_init();
    lv_obj_add_style(obj, &s_style_menu_item, LV_PART_MAIN);
    // PRESSED never happens without an input device; keep it local so the
    // pressed look stays exactly as before.
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_add_event_cb(obj, ui_menu_item_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)screen_id);
}

/* 3-state edit styling: editing = inverted + 3 px border, focused = 3 px
 * border, idle = 1 px border. */
void ui_edit_focus_style(lv_obj_t *obj, bool focused, bool editing)
{
    if (editing) {
        // Inverted: black bg, white text — clearly distinct from focused.
        lv_obj_set_style_bg_color(obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 3, LV_PART_MAIN);
    } else if (focused) {
        lv_obj_set_style_bg_color(obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 3, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    }
}

/* j/k + Enter + Esc navigation shared by the LoRa Setting and System Font
 * screens.  Returns the action the caller must take for `key`; *dir is set
 * only for UI_EDIT_STEP. */
int ui_edit_nav_key(ui_edit_nav_t *nav, char key, int *dir)
{
    if (!nav) return UI_EDIT_NONE;

    switch (key) {
    case KEY_ESC:
        if (nav->editing) {
            nav->editing = false;
            return UI_EDIT_RESTYLE;
        }
        return UI_EDIT_POP;
    case KEY_ENTER:
        nav->editing = !nav->editing;
        return UI_EDIT_RESTYLE;
    case 'j':
        if (nav->editing) {
            if (dir) *dir = +1;
            return UI_EDIT_STEP;
        }
        if (nav->focus < nav->count - 1) {
            nav->focus++;
            return UI_EDIT_RESTYLE;
        }
        return UI_EDIT_NONE;
    case 'k':
        if (nav->editing) {
            if (dir) *dir = -1;
            return UI_EDIT_STEP;
        }
        if (nav->focus > 0) {
            nav->focus--;
            return UI_EDIT_RESTYLE;
        }
        return UI_EDIT_NONE;
    default:
        return UI_EDIT_NONE;
    }
}

/* List of 'k  Label' rows; the same table drives the key handler. */
lv_obj_t *ui_shortcut_menu_create(lv_obj_t *parent, const ui_shortcut_item_t *items, int n)
{
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, lv_pct(93), LV_VER_RES - UI_TASKBAR_H - 8);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, UI_TASKBAR_H + 4);
    lv_obj_set_style_pad_top(list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list, 0, LV_PART_MAIN);

    for (int i = 0; i < n; i++) {
        // ui_menu_item_create copies the text (lv_label_set_text), so a local
        // buffer is safe here.
        char text[64];
        lv_snprintf(text, sizeof(text), "%c  %s", items[i].key, items[i].label);
        ui_menu_item_create(list, text, items[i].screen_id);
    }
    return list;
}

void ui_shortcut_menu_on_key(const ui_shortcut_item_t *items, int n)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) { // Esc -> back to the parent screen
            scr_mgr_pop();
            return;
        }
        for (int i = 0; i < n; i++) {
            if (key == items[i].key) {
                scr_mgr_push(items[i].screen_id);
                return;
            }
        }
    }
}

/* The screen-transition refresh policy lives in ui_disp_full_refr(); most
 * screens just need this pair. */
void ui_entry_taskbar(void)
{
    ui_taskbar_enter();
    ui_disp_full_refr();
}

void ui_exit_taskbar(void)
{
    ui_taskbar_leave();
    ui_disp_full_refr();
}

//************************************[ screen 0 ]****************************************** menu
/* menu_taskbar has external linkage: ui_reader.cpp toggles its hidden flag
 * (declared extern in ui_deckpro_int.h).  The remaining taskbar widgets are
 * private to this TU — split screens reach them through ui_taskbar_*(). */
lv_obj_t * menu_taskbar = NULL;
static lv_obj_t * menu_taskbar_time = NULL;
static lv_obj_t * menu_taskbar_charge = NULL;
static lv_obj_t * menu_taskbar_battery = NULL;
static lv_obj_t * menu_taskbar_battery_percent = NULL;
static lv_obj_t * menu_taskbar_wifi = NULL;

/* Every screen owns its own taskbar.  ui_taskbar_create() registers the
 * widgets here, keyed by the screen object that owns them, so entry can
 * re-point the shared globals above at the incoming screen's own taskbar
 * instead of leaving them dangling on a popped screen's freed widgets. */
typedef struct {
    lv_obj_t *scr;      /* screen object that owns this taskbar */
    lv_obj_t *bar, *time, *charge, *battery, *percent, *wifi;
} ui_taskbar_t;
#define UI_TASKBAR_SLOTS 8          /* > max screen-stack depth */
static ui_taskbar_t s_taskbars[UI_TASKBAR_SLOTS];

/* ui_taskbar_create, ui_taskbar_apply_battery_visibility, ui_taskbar_enter
 * and ui_taskbar_leave have external linkage: every split screen TU that
 * creates a taskbar needs them (declared in ui_deckpro_int.h). */

/* Point the shared top-bar globals at tb's widgets (NULL tb clears them). */
static void ui_taskbar_point_globals(const ui_taskbar_t *tb)
{
    menu_taskbar = tb ? tb->bar : NULL;
    menu_taskbar_time = tb ? tb->time : NULL;
    menu_taskbar_charge = tb ? tb->charge : NULL;
    menu_taskbar_battery = tb ? tb->battery : NULL;
    menu_taskbar_battery_percent = tb ? tb->percent : NULL;
    menu_taskbar_wifi = tb ? tb->wifi : NULL;
}

/* LV_EVENT_DELETE on the bar: its owning screen is going away, so never
 * leave the globals or the registry slot pointing at freed widgets. */
static void ui_taskbar_delete_cb(lv_event_t *e)
{
    lv_obj_t *bar = (lv_obj_t *)lv_event_get_target(e);

    if (menu_taskbar == bar)
        ui_taskbar_point_globals(NULL);

    for (int i = 0; i < UI_TASKBAR_SLOTS; i++) {
        if (s_taskbars[i].bar == bar) {
            memset(&s_taskbars[i], 0, sizeof(s_taskbars[i]));
            break;
        }
    }
}

void ui_taskbar_create(lv_obj_t *parent)
{
    menu_taskbar = lv_obj_create(parent);
    lv_obj_set_size(menu_taskbar, LV_HOR_RES, UI_TASKBAR_H);
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
    lv_obj_set_size(status_parent, lv_pct(80)-2, UI_TASKBAR_H-2);
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
    ui_obj_set_hidden(menu_taskbar_wifi, !taskbar_state[TASKBAR_ID_WIFI]);

    menu_taskbar_charge = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(menu_taskbar_charge, tb_font, LV_PART_MAIN);
    ui_obj_set_hidden(menu_taskbar_charge, !taskbar_state[TASKBAR_ID_CHARGE]);

    menu_taskbar_battery = lv_label_create(status_parent);
    lv_obj_set_style_text_font(menu_taskbar_battery, tb_font, LV_PART_MAIN);

    menu_taskbar_battery_percent = lv_label_create(status_parent);
    lv_obj_set_style_text_font(menu_taskbar_battery_percent, tb_font, LV_PART_MAIN);

    ui_taskbar_apply_battery_visibility();

    /* Register this taskbar under the screen that owns it, so entry() can
     * re-point the globals after a push/pop. */
    bool registered = false;
    for (int i = 0; i < UI_TASKBAR_SLOTS; i++) {
        if (s_taskbars[i].bar == NULL) {
            s_taskbars[i].scr = lv_obj_get_screen(parent);
            s_taskbars[i].bar = menu_taskbar;
            s_taskbars[i].time = menu_taskbar_time;
            s_taskbars[i].charge = menu_taskbar_charge;
            s_taskbars[i].battery = menu_taskbar_battery;
            s_taskbars[i].percent = menu_taskbar_battery_percent;
            s_taskbars[i].wifi = menu_taskbar_wifi;
            registered = true;
            break;
        }
    }
    if (!registered)
        printf("ui_taskbar_create: no free slot (%d)\n", UI_TASKBAR_SLOTS);

    lv_obj_add_event_cb(menu_taskbar, ui_taskbar_delete_cb, LV_EVENT_DELETE, NULL);
}

void ui_taskbar_apply_battery_visibility(void)
{
    bool show = ui_topbar_show_battery_get();
    ui_obj_set_hidden(menu_taskbar_battery, !show);
    ui_obj_set_hidden(menu_taskbar_battery_percent, !show);
}

/* Called from every screen's entry(): re-point the shared globals at the
 * incoming screen's own taskbar and refresh the values the labels show
 * before the update timer's next tick. */
void ui_taskbar_enter(void)
{
    lv_obj_t *scr = scr_mgr_get_curr_obj();
    ui_taskbar_t *tb = NULL;

    for (int i = 0; i < UI_TASKBAR_SLOTS; i++) {
        if (s_taskbars[i].bar && s_taskbars[i].scr == scr) {
            tb = &s_taskbars[i];
            break;
        }
    }
    ui_taskbar_point_globals(tb);

    if (menu_taskbar_battery) {
        lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
        lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
    }

    if (menu_taskbar_charge) {
        lv_label_set_text_fmt(menu_taskbar_charge, "%s",
                              taskbar_state[TASKBAR_ID_CHARGE_FINISH] ? LV_SYMBOL_OK : LV_SYMBOL_CHARGE);
        ui_obj_set_hidden(menu_taskbar_charge, !taskbar_state[TASKBAR_ID_CHARGE]);
    }
    if (menu_taskbar_wifi) {
        ui_obj_set_hidden(menu_taskbar_wifi, !taskbar_state[TASKBAR_ID_WIFI]);
    }

    struct tm tm_now;
    if (menu_taskbar_time && ui_time_get_local(&tm_now)) {
        lv_label_set_text_fmt(menu_taskbar_time, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    }

    ui_taskbar_apply_battery_visibility();
    lv_timer_resume(taskbar_update_timer);
}

/* Called from every screen's exit(): stop the shared timer while its
 * globals may point into a taskbar that is about to be freed. */
void ui_taskbar_leave(void)
{
    lv_timer_pause(taskbar_update_timer);
}

// ******************************** [ screen USB MSC ] ********************************
static lv_obj_t *usb_msc_label;

static void create_usb_msc(lv_obj_t *parent)
{
    // Home-screen clock/battery bar instead of the "USB SD Mount <" title.
    // ESC on the keypad pops back to home (see SCREEN_USB_MSC_ID branch in
    // the global keypad handler).
    ui_taskbar_create(parent);

    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_color(info, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "\n\nUSB Mass Storage Mode\n\nSD Card is now mounted\non your computer.\n\nDO NOT unplug while\ntransferring files!");
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, UI_TASKBAR_H + 15);

    usb_msc_label = lv_label_create(parent);
    lv_obj_set_style_text_font(usb_msc_label, UI_FONT, LV_PART_MAIN);
    lv_obj_align(usb_msc_label, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(usb_msc_label, "Status: Active");
}

static void entry_usb_msc(void)
{
    ui_taskbar_enter();
    ui_usb_msc_begin();
    ui_disp_full_refr();
}

static void exit_usb_msc(void)
{
    ui_taskbar_leave();
    ui_usb_msc_end();
    ui_disp_full_refr();
}


/* USB MSC screen: only ESC is handled — switch back to home. */
static void on_key_usb_msc(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) {
            scr_mgr_switch(SCREEN0_ID);
            return;
        }
    }
}

static scr_lifecycle_t screen_usb_msc = {
    .create = create_usb_msc,
    .entry = entry_usb_msc,
    .exit  = exit_usb_msc,
    .on_key    = on_key_usb_msc,
};

static void create0(lv_obj_t *parent)
{
    ui_taskbar_create(parent);

    // === Top divider line under the taskbar ===
    lv_obj_t *top_rule = lv_obj_create(parent);
    lv_obj_remove_style_all(top_rule);
    lv_obj_set_size(top_rule, LV_HOR_RES, 1);
    lv_obj_align(top_rule, LV_ALIGN_TOP_MID, 0, UI_TASKBAR_H);
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
    ui_taskbar_enter();
}
static void exit0(void) {
    ui_taskbar_leave();
}


static void on_key_home(void);

static scr_lifecycle_t screen0 = {
    .create = create0,
    .entry = entry0,
    .exit  = exit0,
    .on_key = on_key_home,
};
//************************************[ screen 2 ]****************************************** Setting
// --------------------- screen 2.1 --------------------- About System
static void create2_1(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "About System <" title bar. ESC on the keypad pops back
    // to Settings via its on_key() handler.
    ui_taskbar_create(parent);

    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_color(info, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    // Build into a fixed stack buffer instead of an Arduino String — String +=
    // reallocs on every concatenation, fragmenting the heap.
    char str[512];
    char buf[32];
    char row[32];
    uint64_t total = 0, used = 0;
    ui_setting_get_sd_capacity(&total, &used);

    size_t off = 0;
    off += lv_snprintf(str + off, sizeof(str) - off, "                           \n");
    kv_format(row, sizeof(row), 27, "SF Version:", ui_setting_get_sf_ver());
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n", row);
    kv_format(row, sizeof(row), 27, "HD Version:", ui_setting_get_hd_ver());
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n", row);

    lv_snprintf(buf, sizeof(buf), "%lluMB", total);
    kv_format(row, sizeof(row), 27, "SD total:", buf);
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n", row);

    lv_snprintf(buf, sizeof(buf), "%lluMB", used);
    kv_format(row, sizeof(row), 27, "SD used:", buf);
    off += lv_snprintf(str + off, sizeof(str) - off, "%s\n                           \n", row);

    lv_label_set_text_fmt(info, "%s", str);
    
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 35);
}

static scr_lifecycle_t screen2_1 = {
    .create = create2_1,
    .entry = ui_entry_taskbar,
    .exit  = ui_exit_taskbar,
    .on_key    = on_key_esc_pop,
};
// --------------------- screen 2 --------------------- Setting
static lv_obj_t *setting_list;
static lv_group_t *setting_group = NULL;
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
#ifdef BOARD_HAS_BHI260AP
    {.name = "Power Gyro",      .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gyro_status,  .get_cb = ui_setting_get_gyro_status, .shortcut = 'y'},
#endif
    {.name = "Power A7682",     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_a7682_status, .get_cb = ui_setting_get_a7682_status, .shortcut = 'a'},
    {.name = "Lora",            .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN1_ID, .shortcut = 's'},
    {.name = "GPS",             .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN3_ID, .shortcut = 'p'},
    {.name = "Test",            .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN5_ID, .shortcut = 'x'},
    {.name = "Battery",         .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN6_ID, .shortcut = 'u'},
    {.name = "Input",           .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN7_ID, .shortcut = 'i'},
    {.name = "A7682E",          .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN8_ID, .shortcut = 'e'},
#ifdef BOARD_HAS_PCM5102A
    {.name = "PCM5102",         .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN10_ID, .shortcut = 'c'},
#endif
    {.name = "USB SD Mount",    .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN_USB_MSC_ID, .shortcut = 'v'},
    {.name = "Shutdown",        .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN9_ID, .shortcut = 'h'},
    {.name = "Sleep",           .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN11_ID, .shortcut = 'd'},
    {.name = "About System",    .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_1_ID, .shortcut = 'z'},
};

static void setting_item_create(int curr_apge);

static void setting_scr_event(lv_event_t *e)
{
    ui_setting_handle *h = (ui_setting_handle *)e->user_data;

    if(e->code == LV_EVENT_CLICKED) {
        switch (h->type)
        {
        case UI_SETTING_TYPE_SW:
            h->set_cb(!h->get_cb());
            lv_label_set_text_fmt(h->st, "%s", (h->get_cb() ? "ON" : "OFF"));
            break;
        case UI_SETTING_TYPE_SUB:
            scr_mgr_push(h->sub_id);
            break;
        default:
            break;
        }
    }
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

static void setting_item_create(int curr_apge)
{
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > setting_num) end = setting_num;

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
            lv_obj_set_style_text_font(h->st, UI_FONT, LV_PART_MAIN);
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
        lv_obj_add_event_cb(h->obj, setting_scr_event, LV_EVENT_CLICKED, (void *)h);
        ui_list_style_row(h->obj, setting_group);
    }
    if (setting_group) {
        lv_obj_t *first = lv_obj_get_child(setting_list, 0);
        if (first) lv_group_focus_obj(first);
    }
}

static void create2(lv_obj_t *parent) 
{
    ui_taskbar_create(parent);

    ui_list_group_get(&setting_group);

    setting_list = ui_list_create(parent, UI_TASKBAR_H, LV_VER_RES - UI_TASKBAR_H);

    setting_num = sizeof(setting_handle_list) / sizeof(setting_handle_list[0]);
    setting_page_num = setting_num / SETTING_PAGE_MAX_ITEM;

    setting_item_create(setting_curr_page);
}
static void on_key_settings(void) {
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) { // Esc -> back to home
            scr_mgr_pop();
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
        } else if (key == KEY_ENTER) {
            ui_list_nav_key(setting_group, key);
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
static void destroy2(void) {
    if (setting_group) {
        lv_group_del(setting_group);
        setting_group = NULL;
    }
}

static scr_lifecycle_t screen2 = {
    .create = create2,
    .entry = ui_entry_taskbar,
    .exit  = ui_exit_taskbar,
    .destroy = destroy2,
    .on_key    = on_key_settings,
};
//************************************[ screen 3 ]****************************************** GPS
static lv_obj_t *scr3_cont;
static lv_obj_t *scr3_cnt_lab;
static lv_timer_t *GPS_loop_timer = NULL;

static void scr3_GPS_updata(void)
{
    double lat      = 0; // Latitude
    double lon      = 0; // Longitude
    double speed    = 0; // Speed over ground
    uint32_t   vsat     = 0; // Visible Satellites
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

    ui_fmt_fixed(buf, 16, (float)lat, 1, "");
    kv_set_line(label_list[0], "Latitude:", buf);

    ui_fmt_fixed(buf, 16, (float)lon, 1, "");
    kv_set_line(label_list[1], "Longitude:", buf);

    ui_fmt_fixed(buf, 16, (float)speed, 3, "kmph");
    kv_set_line(label_list[2], "Speed:", buf);

    lv_snprintf(buf, 16, "%d", vsat);
    kv_set_line(label_list[3], "vsat:", buf);
    
    lv_snprintf(buf, 16, "%d", year);
    kv_set_line(label_list[4], "year:", buf);

    lv_snprintf(buf, 16, "%d", month);
    kv_set_line(label_list[5], "month:", buf);

    lv_snprintf(buf, 16, "%d", day);
    kv_set_line(label_list[6], "day:", buf);

    lv_snprintf(buf, 16, "%02d:%02d:%02d", hour, min, sec);
    kv_set_line(label_list[7], "time:", buf);
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

    scr3_cont = kv_screen_create(parent);

    // Tick counter — kept on this screen but pushed below the taskbar so it
    // doesn't collide with the battery indicator on the right.
    scr3_cnt_lab = lv_label_create(parent);
    lv_obj_set_style_text_font(scr3_cnt_lab, UI_FONT_MONO(15), LV_PART_MAIN);
    lv_obj_set_style_radius(scr3_cnt_lab, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr3_cnt_lab, 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr3_cnt_lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(scr3_cnt_lab, " %05d ", 0);
    lv_obj_align(scr3_cnt_lab, LV_ALIGN_TOP_RIGHT, -10, UI_TASKBAR_H + 4);
}
static void entry3(void)
{
    scr3_GPS_updata();

    ui_gps_task_resume();
    ui_gps_set_continuous(true);

    ui_taskbar_enter();

    GPS_loop_timer = lv_timer_create(GPS_loop_timer_event, 3000, NULL);
    ui_disp_full_refr();
}
static void exit3(void) {
    // Back to duty-cycled mode: the GPS task keeps the clock synced with an
    // hourly fix and powers the module down in between.
    ui_gps_set_continuous(false);
    if(GPS_loop_timer) {
        lv_timer_del(GPS_loop_timer);
        GPS_loop_timer = NULL;
    }
    ui_taskbar_leave();
    ui_disp_full_refr();
}
static scr_lifecycle_t screen3 = {
    .create = create3,
    .entry = entry3,
    .exit  = exit3,
    .on_key    = on_key_esc_pop,
};

//************************************[ screen 4 ]****************************************** Wifi Scan + Pick
// --------------------- screen 4 --------------------- WIFI: scan-and-select picker
//
// Flow: settings -> WIFI -> SCREEN4 (this screen) shows scanned networks.
// j/k cycles selection, Enter selects:
//   - open network: connect immediately, pop back to settings.
//   - encrypted network: push SCREEN4_1 (password prompt).
// Esc backs out. The list is rescanned when the screen entry runs (with a
// brief "Scanning..." placeholder while the blocking call returns).
static lv_obj_t *scr4_list = NULL;
static lv_obj_t *scr4_status_lab = NULL;
static lv_timer_t *scr4_scan_timer = NULL;
static int scr4_focus = 0;
static int scr4_count = 0;
static ui_wifi_scan_info_t scr4_results[UI_WIFI_SCAN_ITEM_MAX];

// Selection handed off to SCREEN4_1 when the user picks an encrypted SSID.
static char scr4_selected_ssid[33] = {0};

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
        lv_obj_set_style_text_font(btn, UI_FONT, LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < scr4_count; i++) {
        char buf[48];
        lv_snprintf(buf, sizeof(buf), "%s%-20.20s %4d",
                    scr4_results[i].open ? " " : "*",
                    scr4_results[i].name, scr4_results[i].rssi);
        lv_obj_t *btn = lv_list_add_btn(scr4_list, NULL, buf);
        lv_obj_set_style_text_font(btn, UI_FONT_MONO(14), LV_PART_MAIN);
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
        scr_mgr_pop();
    } else {
        scr_mgr_push(SCREEN4_1_ID);
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
        if (key == KEY_ESC) {
            // Cancel the in-flight scan if any so the radio doesn't keep
            // chewing CPU after we've left the screen.
            if (scr4_scan_timer) { lv_timer_del(scr4_scan_timer); scr4_scan_timer = NULL; }
            ui_wifi_scan_cancel();
            scr_mgr_pop();
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
        } else if (key == KEY_ENTER) {
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
    lv_snprintf(status, sizeof(status), "%d nets  j/k move  ent pick  r rescan", scr4_count);
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

    scr4_list = lv_list_create(parent);
    lv_obj_set_size(scr4_list, lv_pct(100), lv_pct(80));
    lv_obj_align(scr4_list, LV_ALIGN_TOP_MID, 0, UI_TASKBAR_H + 4);
    lv_obj_set_style_pad_top(scr4_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr4_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(scr4_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr4_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr4_list, 0, LV_PART_MAIN);

    scr4_status_lab = lv_label_create(parent);
    lv_obj_set_width(scr4_status_lab, lv_pct(100));
    lv_obj_set_style_text_font(scr4_status_lab, UI_FONT, LV_PART_MAIN);
    lv_obj_align(scr4_status_lab, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_label_set_text(scr4_status_lab, "Scanning...");
}

static void entry4(void)
{
    ui_taskbar_enter();
    scr4_start_scan();
}

static void exit4(void)
{
    if (scr4_scan_timer) { lv_timer_del(scr4_scan_timer); scr4_scan_timer = NULL; }
    // If we're leaving mid-scan (e.g. via Esc), make sure the radio is
    // actually idle and the result buffer is freed.
    ui_wifi_scan_cancel();
    ui_taskbar_leave();
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

// --------------------- screen 4.1 --------------------- Wifi password prompt
//
// Reached only from the scan picker (SCREEN4) when the user picks an
// encrypted network. SCREEN4 stashes the chosen SSID in scr4_selected_ssid
// before pushing here. We just collect the password and connect.
//
// Layout: SSID readout (top), password textarea, status line, Connect button.
// Enter on the password field or button triggers connect-and-pop.
// Esc backs out to the scan list.
static lv_obj_t *scr4_1_ssid_lab = NULL;
static lv_obj_t *scr4_1_pass_ta = NULL;
static lv_obj_t *scr4_1_conn_btn = NULL;
static lv_obj_t *scr4_1_conn_btn_lab = NULL;
static lv_obj_t *scr4_1_status_lab = NULL;
static lv_timer_t *scr4_1_status_timer = NULL;

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
    if (scr4_1_pass_ta) {
        lv_obj_set_style_border_width(scr4_1_pass_ta, 3, LV_PART_MAIN);
        lv_obj_add_state(scr4_1_pass_ta, LV_STATE_FOCUSED);
    }
    if (scr4_1_conn_btn) {
        lv_obj_set_style_border_width(scr4_1_conn_btn, 1, LV_PART_MAIN);
        lv_obj_clear_state(scr4_1_conn_btn, LV_STATE_FOCUSED);
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
        scr_mgr_pop();
    }
}

static void scr4_1_save_and_connect(void)
{
    const char *ssid = scr4_selected_ssid;
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

        if (key == KEY_ESC) { // Esc -> back to scan list
            scr_mgr_pop();
            return;
        }

        // Enter on the password field commits and connects (single-line UX so
        // we treat enter as submit). Enter on the button does the same.
        if (key == KEY_ENTER) {
            scr4_1_save_and_connect();
            continue;
        }

        int len = (int)strlen(scr4_1_pass_buf);
        int max_len = (int)sizeof(scr4_1_pass_buf) - 1;
        if (key == KEY_BACKSPACE) {
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

    const char *ssid = scr4_selected_ssid;

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
    lv_obj_set_style_text_font(scr4_1_ssid_lab, UI_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(scr4_1_ssid_lab, LV_LABEL_LONG_DOT);
    char ssid_buf[64];
    lv_snprintf(ssid_buf, sizeof(ssid_buf), "SSID: %s", (ssid && ssid[0]) ? ssid : "(none)");
    lv_label_set_text(scr4_1_ssid_lab, ssid_buf);

    // Password textarea
    scr4_1_pass_ta = lv_textarea_create(cont);
    lv_obj_set_style_anim_time(scr4_1_pass_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);  // solid cursor: a blinking one keeps the e-paper refreshing nonstop
    lv_textarea_set_one_line(scr4_1_pass_ta, true);
    lv_textarea_set_password_mode(scr4_1_pass_ta, true);
    lv_obj_set_width(scr4_1_pass_ta, lv_pct(100));
    lv_obj_set_height(scr4_1_pass_ta, 32);
    lv_obj_set_style_text_font(scr4_1_pass_ta, UI_FONT_MONO(14), LV_PART_MAIN);
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
    lv_obj_set_style_text_font(scr4_1_status_lab, UI_FONT_MONO(14), LV_PART_MAIN);
    lv_label_set_long_mode(scr4_1_status_lab, LV_LABEL_LONG_WRAP);
    lv_label_set_text(scr4_1_status_lab, "");

    scr_title_create(parent, "Password");
}

static void entry4_1(void)
{
    ui_disp_full_refr();
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
//************************************[ screen 5 ]****************************************** Test
static lv_obj_t *test_list;
static int test_num = 0;

static ui_test_handle test_handle_list[] = {
    { .name="Lora",       .peri_id=E_PERI_LORA       , .shortcut='l' },
    { .name="BQ25896",    .peri_id=E_PERI_BQ25896    , .shortcut='5' },
    { .name="BQ27220",    .peri_id=E_PERI_BQ27220    , .shortcut='7' },
    { .name="SD Card",    .peri_id=E_PERI_SD         , .shortcut='d' },
    { .name="A7682E",     .peri_id=E_PERI_A7682E     , .shortcut='a' },
#ifdef BOARD_HAS_PCM5102A
    { .name="PCM5102A",   .peri_id=E_PERI_PCM5102A   , .shortcut='p' },
#endif
    { .name="Keypad",     .peri_id=E_PERI_KYEPAD     , .shortcut='y' },
    { .name="GPS",        .peri_id=E_PERI_GPS        , .shortcut='g' },
#ifdef BOARD_HAS_BHI260AP
    { .name="BHI260AP",   .peri_id=E_PERI_BHI260AP   , .shortcut='b' },
#endif
    { .name="LTR_553ALS", .peri_id=E_PERI_LTR_553ALS , .shortcut='r' },
    { .name="INK_SCREEN", .peri_id=E_PERI_INK_SCREEN , .shortcut='i' },
};

static lv_group_t *test_group = NULL;

static void test_item_create(void)
{
    for(int i = 0; i < test_num; i++) {
        ui_test_handle *h = &test_handle_list[i];
        lv_obj_t *obj = lv_list_add_btn(test_list, NULL, h->name);
        h->st = lv_label_create(obj);
        lv_obj_set_style_text_font(h->st, UI_FONT, LV_PART_MAIN);
        lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_label_set_text_fmt(h->st, "%s", (ui_test_get(h->peri_id) ? "PASS" : "----"));
        // Ensure status label also changes color on focus
        lv_obj_set_style_text_color(h->st, DECKPRO_COLOR_BG, LV_PART_MAIN | LV_STATE_FOCUSED);

        ui_list_style_row(obj, test_group);
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

    ui_list_group_get(&test_group);
    test_list = ui_list_create(parent, UI_TASKBAR_H + 4, LV_VER_RES - UI_TASKBAR_H - 8);

    test_num = sizeof(test_handle_list) / sizeof(test_handle_list[0]);
    test_item_create();
}
static void on_key_test(void) {
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) { // Esc -> back to settings
            scr_mgr_pop();
            return;
        }
        if (ui_list_nav_key(test_group, key)) continue;
        // Per-item shortcut: jump focus to the matching test row.
        for (int i = 0; i < test_num; i++) {
            if (test_handle_list[i].shortcut == key) {
                lv_obj_t *obj = lv_obj_get_child(test_list, i);
                if (obj) {
                    lv_group_focus_obj(obj);
                    lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
                }
                return;
            }
        }
    }
}
static void destroy5(void) {
    if (test_group) {
        lv_group_del(test_group);
        test_group = NULL;
    }
}

static scr_lifecycle_t screen5 = {
    .create = create5,
    .entry = ui_entry_taskbar,
    .exit  = ui_exit_taskbar,
    .destroy = destroy5,
    .on_key    = on_key_test,
};
//************************************[ screen 6 ]****************************************** Battery
// --------------------- screen 6 --------------------- Battery
static const ui_shortcut_item_t scr6_shortcuts[] = {
    { 'c', "BQ25896", SCREEN6_1_ID },
    { 'g', "BQ27220", SCREEN6_2_ID },
};
#define SCR6_SHORTCUT_N (int)(sizeof(scr6_shortcuts) / sizeof(scr6_shortcuts[0]))

static void create6(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Battery <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler. Letter prefixes on the items double as
    // keypad shortcuts handled by the same table.
    ui_taskbar_create(parent);

    ui_shortcut_menu_create(parent, scr6_shortcuts, SCR6_SHORTCUT_N);
}

static void on_key_scr6(void)
{
    ui_shortcut_menu_on_key(scr6_shortcuts, SCR6_SHORTCUT_N);
}

static scr_lifecycle_t screen6 = {
    .create = create6,
    .entry = ui_entry_taskbar,
    .exit  = ui_exit_taskbar,
    .on_key    = on_key_scr6,
};
// --------------------- screen 6.1 --------------------- BQ25896

static lv_timer_t *batt_6_1_timer = NULL;

static void scr6_1_battert_updata(void)
{
    char buf[KV_LINE_MAX];

    kv_set_line(label_list[0], "Charging:", (ui_batt_25896_is_chg() == true ? "Charging" : "Not charged"));

    ui_fmt_fixed(buf, KV_LINE_MAX, ui_batt_25896_get_vbus(), 2, "V");
    kv_set_line(label_list[1], "VBUS:", buf);

    ui_fmt_fixed(buf, KV_LINE_MAX, ui_batt_25896_get_vsys(), 2, "V");
    kv_set_line(label_list[2], "VSYS:", buf);

    ui_fmt_fixed(buf, KV_LINE_MAX, ui_batt_25896_get_vbat(), 2, "V");
    kv_set_line(label_list[3], "VBAT:", buf);

    ui_fmt_fixed(buf, KV_LINE_MAX, ui_batt_25896_get_volt_targ(), 2, "v");
    kv_set_line(label_list[4], "VOLT Target:", buf);

    ui_fmt_fixed(buf, KV_LINE_MAX, ui_batt_25896_get_chg_curr(), 2, "mA");
    kv_set_line(label_list[5], "Charge Curr:", buf);

    ui_fmt_fixed(buf, KV_LINE_MAX, ui_batt_25896_get_pre_curr(), 2, "mA");
    kv_set_line(label_list[6], "Prechg Curr:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%s", ui_batt_25896_get_chg_st());
    kv_set_line(label_list[7], "CHG ST:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%s", ui_batt_25896_get_vbus_st());
    kv_set_line(label_list[8], "VBUS Status:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%s", ui_batt_25896_get_ntc_st());
    kv_set_line(label_list[9], " ", buf);
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

    kv_screen_create(parent);
}

static void entry6_1(void)
{
    ui_batt_25896_set_adc_continuous(true);
    scr6_1_battert_updata();
    ui_taskbar_enter();
    ui_disp_full_refr();
    batt_6_1_timer = lv_timer_create(batt_6_1_updata_timer_event, 5000, NULL);
}
static void exit6_1(void) {
    ui_batt_25896_set_adc_continuous(false);
    if(batt_6_1_timer) {
        lv_timer_del(batt_6_1_timer);
        batt_6_1_timer = NULL;
    }
    ui_taskbar_leave();
    ui_disp_full_refr();
}
static scr_lifecycle_t screen6_1 = {
    .create = create6_1,
    .entry = entry6_1,
    .exit  = exit6_1,
    .on_key    = on_key_esc_pop,
};

// --------------------- screen 6.2 --------------------- BQ27220

static lv_timer_t *batt_6_2_timer = NULL;

static void scr6_2_battert_updata(void)
{
    char buf[KV_LINE_MAX];

    kv_set_line(label_list[0],"VBUS ST::", (ui_battery_is_external_power_present() == true ? "Connected" : "Disonnected"));

    if(ui_battery_is_external_power_present() == true ){
        lv_snprintf(buf, KV_LINE_MAX, "%s", (ui_battery_27220_get_charge_finish()? "Finsish":"Charging"));
    } else {
        lv_snprintf(buf, KV_LINE_MAX, "%s", "Discharge");
    }
    kv_set_line(label_list[1],"Charing ST:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "0x%x", ui_battery_27220_get_status());
    kv_set_line(label_list[2],"Battery ST:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%dmV", ui_battery_27220_get_voltage());
    kv_set_line(label_list[3], "Voltage:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%dmA", ui_battery_27220_get_current());
    kv_set_line(label_list[4], "Current:", buf);

    ui_fmt_fixed(buf, KV_LINE_MAX, (float)(ui_battery_27220_get_temperature() / 10.0f - 273.0f), 2, "C");
    kv_set_line(label_list[5], "Temperature:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%dmAh", ui_battery_27220_get_remain_capacity());
    kv_set_line(label_list[6], "Cap Remain:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%dmAh", ui_battery_27220_get_full_capacity());
    kv_set_line(label_list[7], "Cap Full:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%d%%", ui_battery_27220_get_percent());
    kv_set_line(label_list[8], "Cap Percent:", buf);

    lv_snprintf(buf, KV_LINE_MAX, "%d%%", ui_battery_27220_get_health());
    kv_set_line(label_list[9], "CapHealth:", buf);
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

    kv_screen_create(parent);
}

static void entry6_2(void)
{
    scr6_2_battert_updata();
    ui_taskbar_enter();
    ui_disp_full_refr();
    batt_6_2_timer = lv_timer_create(batt_6_2_updata_timer_event, 5000, NULL);
}
static void exit6_2(void) {
    if(batt_6_2_timer) {
        lv_timer_del(batt_6_2_timer);
        batt_6_2_timer = NULL;
    }
    ui_taskbar_leave();
    ui_disp_full_refr();
}

static scr_lifecycle_t screen6_2 = {
    .create = create6_2,
    .entry = entry6_2,
    .exit  = exit6_2,
    .on_key    = on_key_esc_pop,
};
//************************************[ screen 7 ]****************************************** Other
static lv_obj_t *scr7_cont;
static lv_obj_t *input_keypad;
#ifdef BOARD_HAS_BHI260AP
static lv_obj_t *gyroscope;
#endif
static lv_timer_t *input_timer;
// Fixed buffer so each keypress is an O(1) append instead of a String realloc.
// 128 bytes holds the "Keypad: \n" header plus ~118 typed chars before
// rolling.
#define KEYPAD_STR_CAP 128
static char keypad_str[KEYPAD_STR_CAP] = "Keypad: \n";
static size_t keypad_str_len = 9; // strlen("Keypad: \n")

static void input_timer_event(lv_timer_t *t)
{
#ifdef BOARD_HAS_BHI260AP
    static int sec = 0;
    float gyro_x, gyro_y, gyro_z;
#endif
    char keypay_v;

    int ret = ui_input_get_keypad_val(&keypay_v);
    if(ret == 1)
    {
        ui_input_set_keypad_flag();
        // Esc still owns its usual "pop back" role even on the input visualizer,
        // since this screen owns the keypad queue and a separate kb timer would
        // race us for it.
        if (keypay_v == KEY_ESC) {
            scr_mgr_pop();
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

#ifdef BOARD_HAS_BHI260AP
        sec = 0;
#endif
    }

#ifdef BOARD_HAS_BHI260AP
    sec++;
    if(sec > 60) // ~3 s (60 x 50 ms)
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
#endif
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

    input_keypad = lv_label_create(scr7_cont);
    lv_obj_set_width(input_keypad, lv_pct(95));
    lv_obj_set_style_pad_all(input_keypad, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(input_keypad, UI_FONT_MONO(15), LV_PART_MAIN);
    lv_label_set_long_mode(input_keypad, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(input_keypad, "Keypad: ");

#ifdef BOARD_HAS_BHI260AP
    lv_obj_t *lab2 = lv_label_create(scr7_cont);
    lv_obj_set_style_text_font(lab2, UI_FONT_MONO(15), LV_PART_MAIN);
    lv_label_set_text(lab2, "gyroscope");

    gyroscope = lv_label_create(scr7_cont);
    lv_obj_set_width(gyroscope, lv_pct(95));
    lv_obj_set_style_pad_all(gyroscope, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(gyroscope, UI_FONT_MONO(15), LV_PART_MAIN);
    lv_label_set_long_mode(gyroscope, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(gyroscope,    "   gyros_x: 000\n"
                                        "   gyros_y: 000\n"
                                        "   gyros_z: 000");
#endif

}
static void entry7(void)
{
    memcpy(keypad_str, "Keypad: \n", 9);
    keypad_str[9] = '\0';
    keypad_str_len = 9;
    ui_taskbar_enter();
    ui_disp_full_refr();
    input_timer = lv_timer_create(input_timer_event, 50, NULL);
}
static void exit7(void) {
    if(input_timer)
    {
        lv_timer_del(input_timer);
        input_timer = NULL;
    }
    ui_taskbar_leave();
    ui_disp_full_refr();
}
static scr_lifecycle_t screen7 = {
    .create = create7,
    .entry = entry7,
    .exit  = exit7,
};
//************************************[ screen 8 ]****************************************** A7682E
/* Shared A7682 / PCM5102 submenu helpers: a click runs cb(name); ESC pops;
 * j/k/Enter navigate; a row shortcut focuses, clicks and scrolls to that row. */
static void action_scr_event(lv_event_t *e)
{
    ui_action_handle *h = (ui_action_handle *)e->user_data;

    if(e->code == LV_EVENT_CLICKED) {
        if(h->cb)
            h->cb(h->name);
    }
}

static void action_items_create(lv_obj_t *list, lv_group_t *group, ui_action_handle *items, int n)
{
    for(int i = 0; i < n; i++) {
        ui_action_handle *h = &items[i];
        h->obj = lv_list_add_btn(list, NULL, h->name);
        lv_obj_add_event_cb(h->obj, action_scr_event, LV_EVENT_CLICKED, (void *)h);
        ui_list_style_row(h->obj, group);
    }
    if (group) {
        lv_obj_t *first = lv_obj_get_child(list, 0);
        if (first) lv_group_focus_obj(first);
    }
}

static void action_on_key(lv_group_t *group, lv_obj_t *list, ui_action_handle *items, int n)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) { // Esc -> back to settings
            scr_mgr_pop();
            return;
        }
        if (ui_list_nav_key(group, key)) continue;
        // Per-item shortcut: focus + click the matching row.
        for (int i = 0; i < n; i++) {
            if (items[i].shortcut == key) {
                lv_obj_t *obj = lv_obj_get_child(list, i);
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

// --------------------- screen 8 --------------------- A7682E
static lv_obj_t *a7682_list;
static int a7682_num = 0;

static bool ui_a7682_call_test(const char *param)
{
    scr_mgr_push(SCREEN8_1_ID);
    return true;
}

static bool ui_a7682_at_test(const char *param)
{
    scr_mgr_push(SCREEN8_2_ID);
    return true;
}

static ui_action_handle a7682_handle_list[] =
{
    {"A7682 Audio", NULL, ui_a7682_at_cb,        'u'},
    {"Call test",   NULL, ui_a7682_call_test,    'c'},
    {"AT test",     NULL, ui_a7682_at_test,      't'},
};

static lv_group_t *a7682_group = NULL;

static void a7682_item_create(void)
{
    action_items_create(a7682_list, a7682_group, a7682_handle_list, a7682_num);
}

static void on_key_a7682(void) {
    action_on_key(a7682_group, a7682_list, a7682_handle_list, a7682_num);
}

static void create8(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "A7682E <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler; letter shortcuts pick a row.
    ui_taskbar_create(parent);

    ui_list_group_get(&a7682_group);
    a7682_list = ui_list_create(parent, UI_TASKBAR_H + 4, LV_VER_RES - UI_TASKBAR_H - 8);

    a7682_num = sizeof(a7682_handle_list) / sizeof(a7682_handle_list[0]);
    a7682_item_create();
}
static void destroy8(void) {
    if (a7682_group) {
        lv_group_del(a7682_group);
        a7682_group = NULL;
    }
}
static scr_lifecycle_t screen8 = {
    .create = create8,
    .entry = ui_entry_taskbar,
    .exit  = ui_exit_taskbar,
    .destroy = destroy8,
    .on_key    = on_key_a7682,
};
// --------------------- screen 8.1 --------------------- Call test
static void event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * ta =  (lv_obj_t *)lv_event_get_user_data(e);

    if(code == LV_EVENT_VALUE_CHANGED) {
        uint32_t id = lv_btnmatrix_get_selected_btn(obj);
        const char * txt = lv_btnmatrix_get_btn_text(obj, id);
 
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


static void create8_1(lv_obj_t *parent) 
{
    lv_obj_t * ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, lv_pct(98));
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, lv_pct(20));
    lv_obj_set_style_text_font(ta, &Font_Mono_Bold_20, LV_PART_MAIN);
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_anim_time(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);  // solid cursor: a blinking one keeps the e-paper refreshing nonstop
    lv_obj_set_style_text_letter_space(ta, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * btnm1 = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(btnm1, btnm_map);
    lv_obj_set_size(btnm1, lv_pct(100)-2, lv_pct(60));
    lv_obj_set_style_border_width(btnm1, 0, 0);
    lv_obj_align(btnm1, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(btnm1, event_handler, LV_EVENT_VALUE_CHANGED, ta);
    
    scr_title_create(parent, ("Call"));
}
static void entry8_1(void) 
{
    // No serial bridge here: the call screen talks to the modem through
    // TinyGSM and a bridge would steal its responses.
    ui_disp_full_refr();
}
static void exit8_1(void) {
    ui_disp_full_refr();
}
static scr_lifecycle_t screen8_1 = {
    .create = create8_1,
    .entry = entry8_1,
    .exit  = exit8_1,
    .on_key    = on_key_esc_pop,
};
// --------------------- screen 8.2 --------------------- AT test
static void create8_2(lv_obj_t *parent) 
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_width(lab, lv_pct(95));
    lv_obj_set_style_text_font(lab, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(lab, "Open the serial port, set the baud rate to 115200, "
                            "and send the AT command of A7682E to test the function.");
    lv_obj_center(lab);
    
    scr_title_create(parent, ("AT test"));
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
static scr_lifecycle_t screen8_2 = {
    .create = create8_2,
    .entry = entry8_2,
    .exit  = exit8_2,
    .on_key    = on_key_esc_pop,
};
//************************************[ screen 9 ]****************************************** Shutdown
// Pending 2 s shutdown timer, or NULL. Kept here so destroy9() can cancel it
// when the user hits ESC during the countdown.
static lv_timer_t *shutdown_timer = NULL;

static void shutdown_timer_event(lv_timer_t* t)
{
    ui_shutdown_on();
    lv_timer_del(t);
    shutdown_timer = NULL;
}

static void create9(lv_obj_t *parent)
{
    if(ui_battery_25896_is_vbus_in()) 
    {
        lv_obj_t * label = lv_label_create(parent);
        lv_obj_set_width(label, lv_pct(95));
        lv_obj_set_style_text_font(label, UI_FONT, LV_PART_MAIN);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, "The shutdown function can only be used when the "
                            "battery is connected alone, and cannot be shut down when connected to USB.");
        lv_obj_center(label);

        // back 
        scr_title_create(parent, "Shutdown");
    } 
    else 
    {
        lv_obj_t * img = lv_img_create(parent);
        lv_img_set_src(img, &img_start);
        lv_obj_center(img);

        shutdown_timer = lv_timer_create(shutdown_timer_event, 2000, (void *)parent);
    }
}
static void entry9(void) 
{
    ui_disp_full_refr();
}
static void exit9(void) {
    ui_disp_full_refr();
}
static void destroy9(void) {
    // ESC during the countdown pops this screen: without this the timer would
    // still fire and shut the device down after the user backed out.
    if (shutdown_timer) {
        lv_timer_del(shutdown_timer);
        shutdown_timer = NULL;
    }
}
static scr_lifecycle_t screen9 = {
    .create = create9,
    .entry = entry9,
    .exit  = exit9,
    .destroy = destroy9,
    .on_key    = on_key_esc_pop,
};
//************************************[ screen 10 ]***************************************** pcm5102
#ifdef BOARD_HAS_PCM5102A
static lv_obj_t *pcm5102_list;
static int pcm5102_num = 0;

static ui_action_handle pcm5102_handle_list[] =
{
    {"PCM5102 Audio", NULL, ui_pcm5102_cb, 'u'},
};

static lv_group_t *pcm5102_group = NULL;

static void pcm5102_item_create(void)
{
    action_items_create(pcm5102_list, pcm5102_group, pcm5102_handle_list, pcm5102_num);
}

static void on_key_pcm5102(void) {
    action_on_key(pcm5102_group, pcm5102_list, pcm5102_handle_list, pcm5102_num);
}

static void create10(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "PCM5102 <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler; letter shortcuts pick a row.
    ui_taskbar_create(parent);

    ui_list_group_get(&pcm5102_group);
    pcm5102_list = ui_list_create(parent, UI_TASKBAR_H + 4, LV_VER_RES - UI_TASKBAR_H - 8);

    pcm5102_num = sizeof(pcm5102_handle_list) / sizeof(pcm5102_handle_list[0]);
    pcm5102_item_create();
}
static void exit10(void)
{
    ui_pcm5102_stop();
    ui_taskbar_leave();
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
    .entry = ui_entry_taskbar,
    .exit  = exit10,
    .destroy = destroy10,
    .on_key    = on_key_pcm5102,
};
#endif /* BOARD_HAS_PCM5102A */
//************************************[ screen 11 ]****************************************** Sleep
static void create11(lv_obj_t *parent)
{
    // Deep sleep never returns, so stamp the clock and flush any setting
    // still sitting in the debounced save timer — unlike ui_shutdown_on(),
    // this path does not go through ui_settings_save() on its way down.
    ui_time_persist_save();
    ui_settings_save();

    lora_sleep();

    SerialGPS.end();
    
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

    board_rails_hold(true);

    
    // Wake on BOOT or on any key: the TCA8418 stays powered and holds INT low
    // while a key event is queued. The INT line needs a pull-up in deep sleep
    // (the GPIO pull-up set by pinMode does not survive), and it must be idle
    // high now or the chip would wake immediately.
    uint64_t wake_mask = (1ULL << BOARD_BOOT_PIN);
    if (digitalRead(BOARD_KEYBOARD_INT) == HIGH) {
        rtc_gpio_pullup_en((gpio_num_t)BOARD_KEYBOARD_INT);
        rtc_gpio_pulldown_dis((gpio_num_t)BOARD_KEYBOARD_INT);
        wake_mask |= (1ULL << BOARD_KEYBOARD_INT);
    }
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}
static scr_lifecycle_t screen11 = {
    .create = create11,
};
//************************************[ UI ENTRY ]******************************************
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
    int percent = 0;

    // Until the gauge has produced its first real value, poll every tick; after
    // that the state of charge moves slowly, so read it every 30 s and the
    // charger presence every 5 s. Each read is an I2C transaction that keeps the
    // CPU and bus awake.
    if (!first_battery_update_done || sec % 30 == 0) {
        finish = ui_battery_27220_get_charge_finish();
        percent = ui_battery_27220_get_percent();

        if(taskbar_state[TASKBAR_ID_CHARGE_FINISH] != finish)
        {
            if(finish){
                lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_OK);
            } else {
                lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_CHARGE);
            }
            taskbar_state[TASKBAR_ID_CHARGE_FINISH] = finish;
        }

        if(taskbar_state[TASKBAR_ID_BATTERY_PERCENT] != percent)
        {
            lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", percent);
            lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
            taskbar_state[TASKBAR_ID_BATTERY_PERCENT] = percent;
            if (!first_battery_update_done) {
                first_battery_update_done = true;
                ui_disp_force_full_refr();
            }
        }
    }

    if (!first_battery_update_done || sec % 5 == 0) {
        charge = ui_battery_is_external_power_present();
        if(taskbar_state[TASKBAR_ID_CHARGE] != charge)
        {
            ui_obj_set_hidden(menu_taskbar_charge, !charge);
            taskbar_state[TASKBAR_ID_CHARGE] = charge;
        }
    }

    bool wifi_en = ui_wifi_get_enabled();
    if(taskbar_state[TASKBAR_ID_WIFI] != (int)wifi_en)
    {
        ui_obj_set_hidden(menu_taskbar_wifi, !wifi_en);
        taskbar_state[TASKBAR_ID_WIFI] = (int)wifi_en;
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
                    ui_disp_force_full_refr();
                }
                // Stamp every 15 minutes (each NVS write can stall the flash
                // cache for tens of ms); lock, deep sleep and shutdown also
                // stamp.
                if (tm_now.tm_min % 15 == 0) {
                    ui_time_persist_save();
                }
            }
        }
    }
}

// Lightweight toast: a centered overlay label that auto-removes after a few
// seconds. We park it on lv_layer_top so it floats above whatever screen is
// active. Single instance — a new toast replaces any in-flight one.
static lv_obj_t *ui_toast_obj = NULL;
static lv_timer_t *ui_toast_timer = NULL;

static void ui_toast_clear(void)
{
    if (ui_toast_obj)   { lv_obj_del(ui_toast_obj);     ui_toast_obj   = NULL; }
    if (ui_toast_timer) { lv_timer_del(ui_toast_timer); ui_toast_timer = NULL; }
}

static void ui_toast_dismiss_cb(lv_timer_t *t)
{
    ui_toast_clear();
    ui_disp_full_refr();
}

/* ui_toast_show has external linkage: split TUs (reader) show toasts for
 * errors (declared in ui_deckpro_int.h). */
void ui_toast_show(const char *text, uint32_t ms)
{
    ui_toast_clear();

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
    lv_obj_set_style_text_font(lab, UI_FONT, LV_PART_MAIN);
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
        ui_wifi_set_tz(UI_DEFAULT_TZ);
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

    ui_wifi_set_tz(UI_DEFAULT_TZ);
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
// in ui_reader.cpp) because the home-screen 'c' shortcut in on_key_home()
// needs to stage a resume target before pushing SCREEN13_1.  All three have
// external linkage so ui_reader.cpp can access them (declared extern in
// ui_deckpro_int.h).
char   reader_selected_file[UI_READER_NAME_MAX] = {0};
// Set true by 'c' on the home screen; entry13_1 consumes it to seed
// reader_page_offsets[0] so the first render is the user's last-read page.
bool   reader_resume_pending = false;
size_t reader_resume_offset  = 0;

/* ---------------------------------------------------------------
 * Keypad dispatch  (§5.1 consolidation)
 *
 * There is no polling timer any more: the keypad task notifies
 * the loop task after every key burst, and loop() calls
 * ui_deckpro_dispatch_keys(), which runs the current screen's
 * on_key() hook.  The home screen is just another screen with an
 * on_key hook (on_key_home below), so there is one dispatch path.
 * Every hook drains the keypad ring-buffer itself with the
 * original while/get/set loop, so all per-key semantics are
 * preserved.
 * --------------------------------------------------------------- */

// Called by loop() each time it wakes: the keypad task notifies the loop
// task after every burst, so keys are dispatched with no polling timer.
void ui_deckpro_dispatch_keys(void)
{
    scr_lifecycle_t *life = scr_mgr_get_curr_life();
    if (life && life->on_key) life->on_key();
}

// Home-screen key handler: drains the ring buffer and runs each
// single-key shortcut.  Stops draining as soon as a key switched
// screens, so the new screen does not get the rest of the burst.
static void on_key_home(void)
{
    char key_val;
    while (ui_input_get_keypad_val(&key_val)) {
        ui_input_set_keypad_flag();
        Serial.printf("[home] key=0x%02x ('%c')\n",
                      (unsigned char)key_val,
                      (key_val >= 32 && key_val < 127) ? key_val : '?');
        switch (key_val) {
            case 's': scr_mgr_push(SCREEN2_ID); break;
            case 'r': scr_mgr_push(SCREEN13_ID); break;
            case 'c': {
                char saved[UI_READER_NAME_MAX] = {0};
                size_t saved_off = 0;
                if (ui_reader_resume_get(saved, sizeof(saved), &saved_off) && saved[0]) {
                    strncpy(reader_selected_file, saved, sizeof(reader_selected_file) - 1);
                    reader_selected_file[sizeof(reader_selected_file) - 1] = '\0';
                    reader_resume_offset = saved_off;
                    reader_resume_pending = true;
                    scr_mgr_push(SCREEN13_1_ID);
                }
                break;
            }
            case 'n': scr_mgr_push(SCREEN12_1_ID); break;
            case 't': scr_mgr_push(SCREEN_DICT_ID); break;
            case 'q': scr_mgr_push(SCREEN_USB_MSC_ID); break;
            case 'l': scr_mgr_push(SCREEN_LOCK_ID); break;
            case 'd': ui_disp_white_clear(); scr_mgr_push(SCREEN11_ID); break;
            case 'w': ui_wifi_set_enabled(!ui_wifi_get_enabled()); break;
            case 'p': ui_do_ping_1111(); break;
            case 'z': ui_do_ntp_sync(); break;
            case 'b':
                ui_topbar_show_battery_set(!ui_topbar_show_battery_get());
                ui_taskbar_apply_battery_visibility();
                ui_disp_full_refr();
                break;
            case KEY_ESC: ui_disp_hard_refr(); break;
        }
        if (scr_mgr_get_curr_scr_id() != SCREEN0_ID) break;
    }
}

// Lock after 10 minutes without a key. Skipped on screens that are
// legitimately idle (USB export, AT/LoRa tests) and in the reader, whose
// entry() would restart the book from page 1 after the lock pops.
static void auto_lock_timer_cb(lv_timer_t *)
{
    int id = scr_mgr_get_curr_scr_id();
    if (id == SCREEN_LOCK_ID || id == SCREEN_USB_MSC_ID || id == SCREEN8_2_ID ||
        id == SCREEN1_1_ID || id == SCREEN13_1_ID || id == SCREEN11_ID) {
        return;
    }
    if (ui_input_idle_ms() >= AUTO_LOCK_IDLE_MS) {
        scr_mgr_push(SCREEN_LOCK_ID);
    }
}

void ui_deckpro_entry(void)
{
    ui_styles_init();

    lv_disp_t *disp = lv_disp_get_default();
    disp->theme = lv_theme_mono_init(disp, false, LV_FONT_DEFAULT);

    taskbar_update_timer = lv_timer_create(menu_taskbar_update_timer_cb, 1000, NULL);
    lv_timer_pause(taskbar_update_timer);

    low_voltage_popup_create();
    low_voltage_timer = lv_timer_create(low_voltage_timer_cb, LOW_VOLTAGE_POLL_MS, NULL);

    scr_mgr_init();

    scr_mgr_register(SCREEN0_ID,    &screen0);      // menu
    scr_mgr_register(SCREEN1_ID,    &screen1);      // Lora
    scr_mgr_register(SCREEN1_1_ID,  &screen1_1);    // - Auto send
    scr_mgr_register(SCREEN1_2_ID,  &screen1_2);    // - Lora Setting
    scr_mgr_register(SCREEN2_ID,    &screen2);      // Setting
    scr_mgr_register(SCREEN2_1_ID,  &screen2_1);    //  - About System
    scr_mgr_register(SCREEN3_ID,    &screen3);      // 
    scr_mgr_register(SCREEN4_ID,    &screen4);      // WIFI
    scr_mgr_register(SCREEN4_1_ID,  &screen4_1);    //  - WIFI Config
    scr_mgr_register(SCREEN5_ID,    &screen5);      // 
    scr_mgr_register(SCREEN6_ID,    &screen6);      // Battery
    scr_mgr_register(SCREEN6_1_ID,  &screen6_1);    //  - BQ25896
    scr_mgr_register(SCREEN6_2_ID,  &screen6_2);    //  - BQ27220
    scr_mgr_register(SCREEN7_ID,    &screen7);      // 
    scr_mgr_register(SCREEN8_ID,    &screen8);      // A7682E
    scr_mgr_register(SCREEN8_1_ID,  &screen8_1);    //  - Call test
    scr_mgr_register(SCREEN8_2_ID,  &screen8_2);    //  - AT test
    scr_mgr_register(SCREEN9_ID,    &screen9);      // Shutdown
#ifdef BOARD_HAS_PCM5102A
    scr_mgr_register(SCREEN10_ID,   &screen10);     // PCM5102
#endif
    scr_mgr_register(SCREEN11_ID,   &screen11);
    scr_mgr_register(SCREEN12_1_ID, &screen12_1);
    scr_mgr_register(SCREEN13_ID, &screen13);
    scr_mgr_register(SCREEN13_1_ID, &screen13_1);
    scr_mgr_register(SCREEN13_2_ID, &screen13_2);
    scr_mgr_register(SCREEN_USB_MSC_ID, &screen_usb_msc);
    scr_mgr_register(SCREEN_LOCK_ID, &screen_lock);
    scr_mgr_register(SCREEN_DICT_ID, &screen_dict);


    scr_mgr_switch(SCREEN0_ID); // set root screen

    // Keys arrive via the keypad task's notification (ui_deckpro_dispatch_keys
    // from loop()); this timer only implements the idle auto-lock.
    lv_timer_create(auto_lock_timer_cb, 5000, NULL);
}

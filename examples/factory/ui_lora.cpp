/*
 * ui_lora.cpp — LoRa screens: lora list (screen 1), auto-send (screen 1.1),
 *               and LoRa settings (screen 1.2).
 */
#include "ui_deckpro_int.h"

//************************************[ screen 1 ]****************************************** lora
// --------------------- screen 1 --------------------- lora
static lv_obj_t * scr1_list;

static void create1(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Lora <" title bar. ESC on the keypad pops back to
    // Settings via its on_key() handler.
    ui_taskbar_create(parent);

    scr1_list = lv_list_create(parent);
    lv_obj_set_size(scr1_list, lv_pct(93), LV_VER_RES - UI_TASKBAR_H - 8);
    lv_obj_align(scr1_list, LV_ALIGN_TOP_MID, 0, UI_TASKBAR_H + 4);
    lv_obj_set_style_pad_top(scr1_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr1_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr1_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr1_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr1_list, 0, LV_PART_MAIN);

    ui_menu_item_create(scr1_list, "a  Auto Test", SCREEN1_1_ID);
    ui_menu_item_create(scr1_list, "s  Lora Setting", SCREEN1_2_ID);
}

static void on_key_scr1(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) { // Esc -> back to Settings
            scr_mgr_pop(false);
            return;
        }
        if (key == 'a') { // Auto Test
            scr_mgr_push(SCREEN1_1_ID, false);
            return;
        }
        if (key == 's') { // Lora Setting
            scr_mgr_push(SCREEN1_2_ID, false);
            return;
        }
    }
}

static void entry1(void)
{
    ui_taskbar_enter();
    ui_disp_full_refr();
}
static void exit1(void) {
    ui_taskbar_leave();
    ui_disp_full_refr();
}
scr_lifecycle_t screen1 = {
    .create = create1,
    .entry = entry1,
    .exit  = exit1,
    .on_key    = on_key_scr1,
};
// --------------------- screen 1.1 --------------------- Auto Send
static lv_obj_t *scr1_1_cont;
static lv_obj_t *lora_lab_buf[11] = {0};
static lv_obj_t *lora_sw_btn;
static lv_obj_t *lora_sw_btn_info;
static lv_timer_t *lora_RT_timer = NULL;
static lv_timer_t *lora_recv_timer = NULL;
static int lora_cnt = 0;

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
    lv_obj_set_style_text_font(label, UI_FONT, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}
static void create1_1(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Lora <" title bar. ESC on the keypad pops back to the
    // Lora menu via its on_key() handler.
    ui_taskbar_create(parent);

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
    lv_obj_set_style_text_font(lora_sw_btn_info, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(lora_sw_btn_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lora_sw_btn_info, "Send");
    lv_obj_center(lora_sw_btn_info);
    // Sit below the 25-px taskbar so we don't cover the clock / battery.
    lv_obj_align(lora_sw_btn, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_event_cb(lora_sw_btn, lora_mode_sw_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_style_text_font(lab, UI_FONT, LV_PART_MAIN);
    lv_label_set_text_fmt(lab, "%.1fM", ui_lora_get_freq());
    lv_obj_align(lab, LV_ALIGN_TOP_RIGHT, -10, 33);

    ui_lora_set_mode(LORA_MODE_SEND);
    lora_cnt = 0;
}

static void entry1_1(void)
{
    ui_taskbar_enter();
    ui_disp_full_refr();
    lora_RT_timer = lv_timer_create(lora_RT_timer_event, 2000, NULL);
    lora_recv_timer = lv_timer_create(lora_recv_loop_event, 400, NULL);
}
static void exit1_1(void) {
    ui_taskbar_leave();
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
scr_lifecycle_t screen1_1 = {
    .create = create1_1,
    .entry = entry1_1,
    .exit  = exit1_1,
    .on_key    = on_key_esc_pop,
};
// --------------------- screen 1.2 --------------------- Lora Setting

static float lora_freq_list[] = {433.0, 850.0, 868.0, 915.0, 920.0};
static float lora_band_list[] = {125.0, 250.0, 500.0};
static float lora_power_list[] = {10.0, 22.0};

static lv_obj_t *scr1_2_cont;
static lv_obj_t *dropdown_freq;
static lv_obj_t *dropdown_band;
static lv_obj_t *dropdown_power;

// Keyboard nav mirrors the System Font screen (screen13_2): j/k moves
// focus between the dropdowns, Enter toggles edit mode, in edit mode j/k
// cycles the focused dropdown's selection, Esc exits edit mode (or pops
// the screen back to Settings when not editing).
typedef enum {
    LS_FOCUS_FREQ  = 0,
    LS_FOCUS_BAND  = 1,
    LS_FOCUS_POWER = 2,
    LS_FOCUS_COUNT = 3,
} scr1_2_focus_t;
static scr1_2_focus_t scr1_2_focus = LS_FOCUS_FREQ;
static bool scr1_2_editing = false;

static lv_obj_t *scr1_2_focused_dropdown(void)
{
    if (scr1_2_focus == LS_FOCUS_FREQ)  return dropdown_freq;
    if (scr1_2_focus == LS_FOCUS_BAND)  return dropdown_band;
    return dropdown_power;
}

static void scr1_2_apply_focus_style(void)
{
    lv_obj_t *dds[LS_FOCUS_COUNT] = { dropdown_freq, dropdown_band, dropdown_power };
    for (int i = 0; i < LS_FOCUS_COUNT; i++) {
        if (!dds[i]) continue;
        bool focused = (i == (int)scr1_2_focus);
        ui_edit_focus_style(dds[i], focused, focused && scr1_2_editing);
    }
}

static void scr1_2_step_focused(int dir)
{
    lv_obj_t *dd = scr1_2_focused_dropdown();
    if (!dd) return;
    uint16_t cnt = lv_dropdown_get_option_cnt(dd);
    if (cnt == 0) return;
    int sel = (int)lv_dropdown_get_selected(dd) + dir;
    if (sel < 0)            sel = cnt - 1;
    if (sel >= (int)cnt)    sel = 0;
    lv_dropdown_set_selected(dd, sel);
    // Reuse the existing change handler so the radio actually gets updated.
    lv_event_send(dd, LV_EVENT_VALUE_CHANGED, NULL);
}

static void on_key_scr1_2(void)
{
    char key;
    while (ui_input_get_keypad_val(&key)) {
        ui_input_set_keypad_flag();
        if (key == KEY_ESC) { // Esc
            if (scr1_2_editing) {
                scr1_2_editing = false;
                scr1_2_apply_focus_style();
            } else {
                scr_mgr_pop(false);
                return;
            }
        } else if (key == KEY_ENTER) { // Enter -> toggle edit mode
            scr1_2_editing = !scr1_2_editing;
            scr1_2_apply_focus_style();
        } else if (key == 'j') {
            if (scr1_2_editing) {
                scr1_2_step_focused(+1);
            } else if ((int)scr1_2_focus < LS_FOCUS_COUNT - 1) {
                scr1_2_focus = (scr1_2_focus_t)((int)scr1_2_focus + 1);
                scr1_2_apply_focus_style();
            }
        } else if (key == 'k') {
            if (scr1_2_editing) {
                scr1_2_step_focused(-1);
            } else if ((int)scr1_2_focus > 0) {
                scr1_2_focus = (scr1_2_focus_t)((int)scr1_2_focus - 1);
                scr1_2_apply_focus_style();
            }
        }
    }
}

static void lora_setting_event_handler(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    const char *flag = ( const char *)lv_event_get_user_data(e);
    int select = lv_dropdown_get_selected(obj);

    switch (*flag)
    {
    case 'f':
        if (select < 0 || select >= (int)GET_BUFF_LEN(lora_freq_list)) return;
        printf("set freq %.1fMHz\n", lora_freq_list[select]);
        ui_lora_set_freq(lora_freq_list[select]);
        break;
    case 'b':
        if (select < 0 || select >= (int)GET_BUFF_LEN(lora_band_list)) return;
        printf("set bandwidth %dKhz\n", (int)lora_band_list[select]);
        ui_lora_set_bandwidth(lora_band_list[select]);
        break;
    case 'p':
        if (select < 0 || select >= (int)GET_BUFF_LEN(lora_power_list)) return;
        printf("set power %ddBm\n", (int)lora_power_list[select]);
        ui_lora_set_power(lora_power_list[select]);
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
    lv_obj_set_style_text_font(ui_Label14, UI_FONT_MONO(15), LV_PART_MAIN);   

    lv_obj_t *ui_Dropdown1 = lv_dropdown_create(ui_Container1);
    lv_obj_set_width(ui_Dropdown1, lv_pct(60));
    lv_obj_set_height(ui_Dropdown1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_Dropdown1, 19);
    lv_obj_set_y(ui_Dropdown1, -1);
    lv_obj_add_flag(ui_Dropdown1, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags

    lv_obj_set_style_border_width(ui_Dropdown1, 1, LV_PART_MAIN | LV_STATE_PRESSED);

    return ui_Dropdown1;
}

/* Select the option whose value matches cur (no-op when nothing matches). */
static void dd_select_matching(lv_obj_t *dd, const float *vals, int n, float cur)
{
    for (int i = 0; i < n; i++) {
        if (cur == vals[i]) lv_dropdown_set_selected(dd, i);
    }
}

/* Build the option list from vals, one line each, formatted with fmt. */
static void dd_set_options_fmt(lv_obj_t *dd, const float *vals, int n, const char *fmt)
{
    char opts[128];
    size_t off = 0;
    opts[0] = '\0';
    for (int i = 0; i < n; i++) {
        char item[16];
        lv_snprintf(item, sizeof(item), fmt, vals[i]);
        off += lv_snprintf(opts + off, sizeof(opts) - off, "%s%s", i ? "\n" : "", item);
    }
    lv_dropdown_set_options(dd, opts);
}

static void create1_2(lv_obj_t *parent)
{
    // Use the home-screen taskbar (clock + battery + wifi icon) instead of
    // the per-screen "Lora Setting <" title bar. ESC pops to Settings via
    // its on_key() handler; j/k + Enter mirror System Font's navigation.
    ui_taskbar_create(parent);

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
    lv_obj_set_align(scr1_2_cont, LV_ALIGN_BOTTOM_MID);

    dropdown_freq = scr1_2_lora_setting_create(scr1_2_cont, "Freq: ");
    dd_set_options_fmt(dropdown_freq, lora_freq_list, GET_BUFF_LEN(lora_freq_list), "%.0fMHz");
    dd_select_matching(dropdown_freq, lora_freq_list, GET_BUFF_LEN(lora_freq_list), ui_lora_get_freq());

    dropdown_band = scr1_2_lora_setting_create(scr1_2_cont, "Band: ");
    dd_set_options_fmt(dropdown_band, lora_band_list, GET_BUFF_LEN(lora_band_list), "%.0fKHz");
    dd_select_matching(dropdown_band, lora_band_list, GET_BUFF_LEN(lora_band_list), ui_lora_get_bandwidth());

    dropdown_power = scr1_2_lora_setting_create(scr1_2_cont, "Power:");
    dd_set_options_fmt(dropdown_power, lora_power_list, GET_BUFF_LEN(lora_power_list), "%.0fdBm");
    dd_select_matching(dropdown_power, lora_power_list, GET_BUFF_LEN(lora_power_list), ui_lora_get_power());
    static const char freq_flag = 'f';
    static const char band_flag = 'b';
    static const char power_flag = 'p';
    lv_obj_add_event_cb(dropdown_freq, lora_setting_event_handler, LV_EVENT_VALUE_CHANGED, (void *)&freq_flag);
    lv_obj_add_event_cb(dropdown_band, lora_setting_event_handler, LV_EVENT_VALUE_CHANGED, (void *)&band_flag);
    lv_obj_add_event_cb(dropdown_power, lora_setting_event_handler, LV_EVENT_VALUE_CHANGED, (void *)&power_flag);
}
static void entry1_2(void)
{
    ui_taskbar_enter();
    scr1_2_focus = LS_FOCUS_FREQ;
    scr1_2_editing = false;
    scr1_2_apply_focus_style();
    ui_disp_full_refr();
}
static void exit1_2(void) {
    ui_taskbar_leave();
    ui_disp_full_refr();
    ui_lora_param_set();
}
scr_lifecycle_t screen1_2 = {
    .create = create1_2,
    .entry = entry1_2,
    .exit  = exit1_2,
    .on_key    = on_key_scr1_2,
};

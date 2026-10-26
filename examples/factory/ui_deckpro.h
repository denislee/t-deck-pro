/************************************************************************
 * FilePath     : ui_base.h
 * Author       : GX.Duan
 * LastEditors  : ShallowGreen123 2608653986@qq.com
 * Copyright (c): 2022 by GX.Duan, All Rights Reserved.
 * Github       : https://github.com/ShallowGreen123/lvgl_examples.git
 ************************************************************************/
#ifndef __UI_DECKPRO_H__
#define __UI_DECKPRO_H__

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include "lvgl.h"
#include "ui_scr_mrg.h"

/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
/*********************************************************************************
 *                                   MACROS
 * *******************************************************************************/
#define DECKPRO_COLOR_BG        lv_color_white()
#define DECKPRO_COLOR_FG        lv_color_black()
#define UI_WIFI_SCAN_ITEM_MAX   13

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/
enum {
    SCREEN0_ID = 0,
    SCREEN1_ID,
    SCREEN1_1_ID,
    SCREEN1_2_ID,
    SCREEN2_ID,
    SCREEN2_1_ID,
    SCREEN3_ID,
    SCREEN4_ID,
    SCREEN4_1_ID,
    SCREEN5_ID,
    SCREEN6_ID,
    SCREEN6_1_ID,
    SCREEN6_2_ID,
    SCREEN7_ID,
    SCREEN8_ID,
    SCREEN8_1_ID,
    SCREEN8_2_ID,
    SCREEN9_ID,
    SCREEN10_ID,
    SCREEN11_ID,
    SCREEN12_1_ID,
    SCREEN13_ID,
    SCREEN13_1_ID,
    SCREEN13_2_ID,        // Reader font settings
    SCREEN_USB_MSC_ID,
    SCREEN_LOCK_ID,
    SCREEN_DICT_ID,
};

enum {
    TASKBAR_ID_CHARGE,
    TASKBAR_ID_CHARGE_FINISH,
    TASKBAR_ID_BATTERY_PERCENT,
    TASKBAR_ID_WIFI,
    TASKBAR_ID_MAX,
};

enum{
    UI_SETTING_TYPE_SW,
    UI_SETTING_TYPE_SUB,
};

typedef struct _ui_setting
{
    const char *name;
    int type;
    void (*set_cb)(bool);
    bool (*get_cb)(void);
    int sub_id;
    char shortcut;
    lv_obj_t *obj;
    lv_obj_t *st;
} ui_setting_handle;

typedef struct _ui_test {
    const char *name;
    int peri_id;
    lv_obj_t *st;
    char shortcut;
} ui_test_handle;

/* A7682 / PCM5102 submenu row: click runs cb(name). */
typedef struct _ui_action {
    const char *name;
    lv_obj_t *obj;
    bool (*cb)(const char *arg);
    char shortcut;
} ui_action_handle;


typedef struct {
    char name[33];
    int  rssi;
    bool open;   // true = open network (no auth), false = encrypted
}ui_wifi_scan_info_t;
/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void ui_deckpro_entry(void);
// Dispatch the current screen's on_key() hook. Called from loop() each time the
// keypad task wakes it (event-driven; no polling timer).
void ui_deckpro_dispatch_keys(void);
// Refresh the lock-screen clock after a timed light-sleep wake, which may land
// before the 15 s clock timer is due.
void ui_lock_clock_refresh(void);
// True while the low-voltage shutdown countdown runs; the lock screen must not
// light-sleep through it.
bool ui_low_voltage_latched(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif /* __UI_EPD47H__ */
